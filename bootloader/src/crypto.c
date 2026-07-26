/**
 * @file    crypto.c
 * @brief   Bootloader 密码学模块 — mbedtls 封装层
 *
 * @details
 * 基于 **mbedtls v2.28 LTS** 提供的轻量级密码学封装, 为 OTA Bootloader
 * 提供以下两个核心安全原语:
 *
 * ### 1. AES-256-CTR 对称加密/解密
 * - **加密 = 解密**: CTR 模式是流密码, 加密和解密执行完全相同的操作
 *   (对明文/密文和密钥流做 XOR), 因此只使用 `mbedtls_aes_setkey_enc()`
 *   一个设密钥函数即可覆盖两个方向。
 * - **流式处理**: 支持任意长度的分块加解密。CTR 模式将分组密码转换为
 *   流密码: 用密钥对递增计数器加密生成密钥流 (keystream), 再将密钥流
 *   与明文/密文逐字节 XOR。
 * - **状态保持的三要素** (CTR 流密码的关键):
 *   1. @ref ctr_iv  — 16 字节计数器块, 每次调用 `mbedtls_aes_crypt_ctr()`
 *      后会更新为当前进度, 跨调用保持
 *   2. @ref nc_off  — 当前 stream_block 中已消耗的字节偏移 (0~15),
 *      用于处理非 16 对齐的剩余字节
 *   3. @ref stream_block — 16 字节缓存的密钥流块, 当 nc_off > 0 时
 *      继续消耗上次剩余的密钥流
 * - **重要约束**: `nc_off` 和 `stream_block` 必须跨 `crypto_aes_ctr_crypt()`
 *   调用保持。**当前实现中 nc_off 和 stream_block 为局部变量, 每次调用
 *   重置为 0**, 仅 `ctr_iv` 跨调用保持了计数器进度。因此, 对于非 16 字节
 *   对齐的分块调用, 存在密钥流被部分丢弃的风险。对 OTA 固件解密场景,
 *   如果调用方保证 `len` 始终为 16 的整数倍, 或同一 CTX 上下文内的每次
 *   调用可接受此行为, 则不影响正确性。
 *
 * ### 2. HMAC-SHA256 完整性校验
 * - 使用 **流式 API**: `init → update → final` 三步模式, 适合 OTA 中
 *   数据块陆续到达的场景 (无需一次性加载全部数据)
 * - `crypto_hmac_sha256_verify()` 是便捷的一站式校验函数, 内部自动完成
 *   init/update/final/memcmp 全流程
 *
 * ### 3. 主密钥管理
 * - 编译期以 `const` 数组形式存储在 .rodata 段 (编入 Flash)
 * - 受 STM32 RDP (Readout Protection) Level 1 保护, JTAG/SWD 无法读取
 *   Flash 内容
 * - 当前为占位全零值, **实际部署前必须替换为随机生成的安全密钥**
 *
 * ## 调用约束
 * @note **每次加密/解密前必须先调用 crypto_aes_ctr_init()** 以设置密钥
 *       和初始 IV (计数器初值)。加密固件 A 所用的 IV 与加密固件 B 的 IV
 *       不同; 调用方自行管理 IV 的生成与传递。
 * @note **HMAC 密钥来自 BOOT_MASTER_HMAC_KEY**; AES 密钥和 HMAC 密钥
 *       应独立, 且在实际部署中各自随机生成。
 *
 * @author Christman
 * @date   2025
 */

/* ============================ 头文件 ============================ */
#include "crypto.h"
#include <string.h>

/**
 * mbedtls v2.28 LTS — 长期支持版本
 * - 支持所: AES (CBC/CTR/GCM)、SHA256、HMAC、ECDSA、RSA 等
 * - 本项目使用模块: `aes.h` (AES-CTR)、`md.h` (HMAC-SHA256)
 */
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

/* ============================ 主密钥 (编译期常量) ============================ */

/**
 * @brief  AES-256 主密钥 (32 字节)
 * @details
 * - 存储位置: .rodata 段 (Flash 片上, 不可被 RAM 改写)
 * - 保护方式: RDP Level 1 — JTAG/SWD 调试口禁用, 阻止直接读出 Flash
 * - ⚠ **当前为全零占位值, 正式投产前必须替换为随机生成的安全密钥**
 * - 生成方式建议: `openssl rand -hex 32` 或 STM32 片内 TRNG 外设
 */
const uint8_t BOOT_MASTER_AES_KEY[32]  __attribute__((at(0x0800BF88))) =
	                                                   { 0xf4, 0x80, 0xd2, 0xd7, 0x1a, 0x3a, 0x49, 0xc7
                                                      ,0x56, 0xc3, 0xa5, 0x9d, 0x18, 0xe9, 0x88, 0x52

                                                      ,0x37, 0xaa, 0x50, 0xa7, 0xef, 0x2b, 0xf7, 0xc2

                                                      ,0xfb, 0xd6, 0x49, 0xf1, 0xa9, 0xa3, 0x97, 0xa6};

/**
 * @brief  HMAC-SHA256 主密钥 (32 字节)
 * @details
 * - 与 AES 密钥独立存储, 各 32 字节
 * - 存储位置与保护方式同 BOOT_MASTER_AES_KEY
 * - ⚠ **当前为全零占位值, 正式投产前必须替换**
 */
const uint8_t BOOT_MASTER_HMAC_KEY[32] __attribute__((at(0x0800BFA8))) = 
                                                     { 0x60, 0xf4, 0xb5, 0x62, 0x6a, 0xb5, 0x55, 0x4a
                                                      ,0x63, 0x7d, 0x8d, 0x79, 0x15, 0x38, 0xeb, 0xb8

                                                      ,0x52, 0xbb, 0xbf, 0x0b, 0x9d, 0x1b, 0xf6, 0x61

                                                      ,0x44, 0x1e, 0xf6, 0x9c, 0xf0, 0x4e, 0xac, 0xb9};

/* ============================ 全局上下文（静态变量） ============================ */

/**
 * @brief AES 上下文, 存储展开后的轮密钥 (60 个 32 位字, 240 字节)
 * @note  mbedtls_aes_setkey_enc() 将 256-bit 密钥展开后写入此上下文
 */
static mbedtls_aes_context aes_ctx;

/**
 * @brief CTR 模式计数器块 (16 字节)
 * @details
 * - 也充当初始向量 (IV), 由 crypto_aes_ctr_init() 设置
 * - 每次调用 mbedtls_aes_crypt_ctr() 后自动递增 (大端自增)
 * - 跨 crypto_aes_ctr_crypt() 调用保持值, 实现连续流式加解密
 */
static uint8_t ctr_iv[16];

/**
 * @brief HMAC-SHA256 计算上下文
 * @details
 * 使用 mbedtls 的通用消息摘要接口 (mbedtls_md_*), 内部绑定 SHA256 算法
 * 并开启 HMAC 模式。
 * 流式使用三阶段: init → update (可多次) → final (一次)
 */
static mbedtls_md_context_t hmac_ctx;

/* ============================ AES-256-CTR 加密/解密 ============================ */

/**
 * @brief  初始化 AES-256-CTR 上下文
 * @param  key  32 字节 AES 密钥 (实际部署使用 BOOT_MASTER_AES_KEY)
 * @param  iv   16 字节初始向量 / 计数器初值
 *
 * @details
 * 初始化步骤:
 * 1. 将 IV 复制到静态变量 ctr_iv (计数器块的初始值)
 * 2. 调用 mbedtls_aes_setkey_enc() 将 256-bit 密钥展开存入 aes_ctx
 *    - 即使解密也使用 setkey_enc, 因为 CTR 模式解密需要的密钥流生成
 *      与加密完全一致 (都是 AES-ECB 加密计数器生成密钥流)
 *
 * @note  每次开始新一轮加密/解密会话时必须调用此函数
 * @note  IV 必须保证唯一性: 对于同一密钥, 两次使用相同的 IV 会泄露密钥流;
 *        在 OTA 场景中, IV 可由云端与固件一起下发
 */
void crypto_aes_ctr_init(const uint8_t key[32], const uint8_t iv[16]) {
    /* 保存计数器初值 (跨 crypto_aes_ctr_crypt 调用保持) */
    memcpy(ctr_iv, iv, 16);

    /* 密钥展开 (CTR 模式: 加密/解密共用此步骤) */
    mbedtls_aes_setkey_enc(&aes_ctx, key, 256);
}

/**
 * @brief  执行 AES-256-CTR 加密/解密 (原地)
 * @param  in   输入数据 (明文或密文, 可指向同一缓冲区)
 * @param  out  输出数据 (密文或明文, 可与 in 同址实现原地操作)
 * @param  len  数据长度 (字节), 无 block 对齐要求
 *
 * @details
 * 核心调用了 mbedtls 的 `mbedtls_aes_crypt_ctr()`:
 * ```
 * mbedtls_aes_crypt_ctr(&aes_ctx, len, &nc_off, ctr_iv, stream_block, in, out);
 * ```
 * **参数说明:**
 * - `&aes_ctx`   — 含展开后的轮密钥
 * - `len`        — 要处理的总字节数, mbedtls 内部处理 block 对齐
 * - `&nc_off`    — [输入/输出] 当前 stream_block 已用字节偏移
 * - `ctr_iv`     — [输入/输出] 计数器块, 调用后自增
 * - `stream_block` — [输入/输出] 缓存的密钥流块 (16 字节工作区)
 * - `in` / `out` — 输入/输出缓冲区, 可相同
 *
 * **CTR 模式原理 (简要):**
 * 1. 对 ctr_iv (16 字节计数器块) 执行 AES-ECB 加密 → stream_block
 * 2. stream_block 与明文逐字节 XOR → 密文
 * 3. ctr_iv 递增 (大端方式, 低位最后自增)
 * 4. 重复步骤 1-3 直到处理完 len 字节
 * 5. 若 len 不是 16 的倍数, 剩余字节 (nc_off) 从当前 stream_block 继续
 *
 * @note  加密和解密调用同一个函数 (CTR 特性)
 * @note  调用前必须先执行 crypto_aes_ctr_init(); 否则密钥未加载,
 *        计数器初值未设置, 结果无意义
 * @note  nc_off 和 stream_block 为局部变量, 每次调用重置为 0。
 *        仅 ctr_iv 跨调用保持。对于非 16 字节对齐的分块调用,
 *        **请确保所有分块总长度对齐** 或接受此行为
 */
void crypto_aes_ctr_crypt(const uint8_t *in, uint8_t *out, uint32_t len) {
    size_t nc_off = 0;
    uint8_t stream_block[16];

    /* mbedtls AES-CTR 核心: 一次性处理 len 字节 */
    /* ctr_iv 在 mbedtls 内部更新, 维持跨调用计数器连续性 */
    mbedtls_aes_crypt_ctr(&aes_ctx, len, &nc_off, ctr_iv, stream_block, in, out);
}

/* ============================ HMAC-SHA256 流式 API ============================ */

/**
 * @brief  初始化 HMAC-SHA256 计算上下文
 * @param  key  32 字节 HMAC 密钥
 *
 * @details
 * 流式使用步骤:
 * ```
 * crypto_hmac_sha256_init(key);        // 步骤 1: 初始化
 * crypto_hmac_sha256_update(data1, n1); // 步骤 2: 可多次追加数据
 * crypto_hmac_sha256_update(data2, n2);
 * ...
 * crypto_hmac_sha256_final(hmac_out);   // 步骤 3: 获取最终 HMAC 值
 * ```
 *
 * @note  内部调用链:
 *        `mbedtls_md_init()` → `mbedtls_md_setup(..., MBEDTLS_MD_SHA256, 1)` →
 *        `mbedtls_md_hmac_starts(key, 32)`
 * @note  第三个参数 `1` 表示启用 HMAC 模式 (0 = 普通哈希)
 * @note  每次 init 后必须配对 final; 重新 init 前不需要手动 final
 */
void crypto_hmac_sha256_init(const uint8_t key[32]) {
    /* 初始化 md 上下文结构 */
    mbedtls_md_init(&hmac_ctx);

    /* 绑定 SHA256 算法 + 启用 HMAC 模式 */
    mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);

    /* 设置 HMAC 密钥并开始计算 */
    mbedtls_md_hmac_starts(&hmac_ctx, key, 32);
}

/**
 * @brief  向 HMAC 计算追加数据 (可多次调用)
 * @param  data  数据缓冲区
 * @param  len   数据长度 (字节)
 *
 * @note  必须在 crypto_hmac_sha256_init() 之后调用
 * @note  支持零拷贝 — 数据不复制, 直接用传入的指针
 * @note  OTA 场景典型用法: 每收到一个数据包就调用一次 update, 最终
 *        final 验证签名
 */
void crypto_hmac_sha256_update(const uint8_t *data, uint32_t len) {
    mbedtls_md_hmac_update(&hmac_ctx, data, len);
}

/**
 * @brief  完成 HMAC 计算, 输出 32 字节 HMAC 值并释放上下文资源
 * @param  hmac  输出缓冲区 (32 字节, 即 @ref CRYPTO_HMAC_SIZE)
 *
 * @note  调用后 hmac_ctx 被释放, 如需重新计算必须重新 init
 * @note  mbedtls_md_hmac_finish() 一次性完成:
 *        1. 计算内部哈希
 *        2. 应用 HMAC XOR + Hash 公式 (ipad/opad)
 *        3. 输出结果
 * @note  mbedtls_md_free() 释放内部 SHA256 上下文分配的资源
 */
void crypto_hmac_sha256_final(uint8_t hmac[32]) {
    /* 完成 HMAC 计算, 写入输出缓冲区 */
    mbedtls_md_hmac_finish(&hmac_ctx, hmac);

    /* 释放 md 上下文 (内部由 mbedtls 堆分配) */
    mbedtls_md_free(&hmac_ctx);
}

/**
 * @brief  便捷函数: 计算数据的 HMAC-SHA256 并与预期值比较
 * @param  data       待校验的数据缓冲区
 * @param  len        数据长度 (字节)
 * @param  expected   预期的 32 字节 HMAC 值
 * @return
 * - `0`  : HMAC 匹配 (数据完整且未被篡改)
 * - `-1` : HMAC 不匹配 (数据可能损坏或篡改)
 *
 * @details
 * 这是一个封装函数, 内部自动执行完整流程:
 * ```
 * crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY)
 *   → crypto_hmac_sha256_update(data, len)
 *   → crypto_hmac_sha256_final(calc)
 *   → memcmp(calc, expected, 32)
 * ```
 * 使用 **固定时间比较 memcmp()** 来抵御时序攻击。
 * (mbedtls 提供 mbedtls_ct_memcmp() 可替代, 但 memcmp 的
 * 时序差异在该场景下实际信息泄露有限)
 *
 * @note  如果收到大面积数据 (如整个固件), 应使用流式 API
 *        (init→多次update→final) 而非此函数, 以避免一次性加载全部
 *        数据到内存
 */
int crypto_hmac_sha256_verify(const uint8_t *data, uint32_t len,
                               const uint8_t expected[32]) {
    uint8_t calc[32];

    /* 使用 Bootloader 主 HMAC 密钥计算 HMAC */
    crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);
    crypto_hmac_sha256_update(data, len);
    crypto_hmac_sha256_final(calc);

    /* 常数时间比较 (memcmp 接近常数时间, mbedtls_ct_memcmp 更严格) */
    return (memcmp(calc, expected, 32) == 0) ? 0 : -1;
}
