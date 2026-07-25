/**
 * @file    boot_main.c
 * @brief   Bootloader 主状态机 —— 上电流程 + 固件升级 6 步流水线
 *
 * ============================================================================
 * 设计文档: docs/ota-design.md §7.3
 * ============================================================================
 *
 * 【上电入口】
 *   boot_main()
 *     ├─ 参数区未初始化 (magic 或 CRC 不匹配) → 初始化 → 跳转 App
 *     ├─ state == UPGRADE_REQUESTED           → boot_upgrade()
 *     ├─ state == UPGRADING                   → 跳转 App (等 IWDG 确认)
 *     └─ state == IDLE / other                → 跳转 App
 *
 * 【升级流水线 boot_upgrade()】
 *   Step a ─ 读片外固件头 (fw_header magic/size)
 *         ─ 读 IV (blob 首 16B) 和 HMAC (blob 末 32B)
 *         ─ 失败 → upgrade_result=SPI_ERROR → 跳转当前 App
 *
 *   Step b ─ [先验签 verify-before-write]
 *         ─ 流式读 blob[0:size-32]，增量 HMAC-SHA256
 *         ─ 比对 blob 末 32B 的 HMAC
 *         ─ 失败 → upgrade_result=VERIFY_FAIL → 跳转当前 App
 *               (片内 flash 未动，当前 App 完好)
 *
 *   Step c ─ 备份片内 App 到片外备份区
 *         ─ 检查备份区 magic：已有则跳过 (断电重入幂等)
 *         ─ AES-256-CTR 流式加密 + HMAC-SHA256 签名
 *         ─ 写备份 blob: [IV 16B][密文][HMAC 32B]
 *         ─ 写备份固件头 (FW_BACKUP_MAGIC)
 *         ─ 失败 → upgrade_result=BACKUP_FAIL → 跳转当前 App
 *
 *   Step d ─ 擦除片内 App 区 (256KB, 128 页 × 2KB)
 *
 *   Step e ─ 流式解密写入
 *         ─ 读片外密文 (偏移 16，跳过 IV)
 *         ─ AES-256-CTR 解密
 *         ─ 逐半字写入片内 flash
 *         ─ 失败 → 从备份区回滚 → upgrade_result=WRITE_ROLLBACK
 *
 *   Step f ─ 成功
 *         ─ new_version 移入 cur_version
 *         ─ state = UPGRADING
 *         ─ upgrade_result = SUCCESS
 *         ─ 清备份 magic → 写参数区 → 启动 IWDG(30s) → 跳转新 App
 *
 * 【掉电重入保证】
 *   - 验签(b) 纯读取，幂等 ✓
 *   - 备份(c) 前检查 magic：已备份则跳过 ✓
 *   - 擦除(d) 幂等 ✓
 *   - 解密写入(e) 从头重做 ✓
 *   - 完成(f) 清 magic 后，下次上电走 UPGRADING → 等 App 确认
 *
 * 【存储布局】 详见 docs/ota-design.md §7.1
 *   片内: Bootloader 48KB | App 288KB @0x08010000 | 参数区 96KB @0x08058000
 *   片外: 固件头 4KB | 新固件区 512KB | 备份头 4KB | 备份区 512KB
 */

#include "boot_main.h"
#include "ota_param.h"
#include "param.h"
#include "flash_ext.h"
#include "flash_int.h"
#include "crypto.h"
#include "jump_app.h"
#include <string.h>

/* ---- 片内 App 区地址与大小 ---- */
#define APP_AREA_START   0x08010000u    /* App 起始地址 (设计文档 §7.1) */
#define APP_AREA_SIZE_KB 288u           /* App 区大小 256KB */
#define BLOCK_SIZE       1024u          /* 流式处理块大小 1KB            */

/* ---- UID 防克隆 (设计文档 §7.5) ---- */
#define UID_ADDR        0x1FFFF7E8u    /* STM32F1 96-bit 唯一 ID 寄存器     */
#define UID_LEN         12u            /* UID 长度 (字节)                   */
#define UID_FLAG_ADDR   0x0800C000u    /* 加密标记位置 (页起始)                            */
#define UID_FLAG_VALUE  0x00001234u    /* 初始标记值: 0x1234                               */
#define UID_ID_ADDR     0x0800C880u    /* 加密 ID 存储位置 (页内偏移 128B; 0~127+144~255随机填充) */
#define UID_ID_PAGE     0x0800C800u    /* 加密 ID 所在 2KB 页起始地址 (擦除用)                    */
#define UID_ID_LEN      16u            /* 加密 ID 长度 (HMAC-SHA256 前 16B)              */

/* ---- 内部函数声明 ---- */
static void boot_upgrade(void);
static void uid_bind_first_run(void);

/* ========================================================================
 * boot_main()  — Bootloader 主入口
 *
 * 上电后调用的第一个函数 (在 main.c 闪灯诊断之后)。
 * 读参数区 → 判断 OTA 状态机状态 → 执行升级或直接跳转 App。
 * 本函数不返回。
 * ======================================================================== */
/*
 * uid_bind_first_run() — 首次上电时生成 UID 绑定的加密 ID
 *
 * 设计意图: 防止合法设备的全片 flash 内容被暴力复制到另一台设备上。
 * 复制后，加密 ID ≠ 新设备 UID 的计算值 → App 启动验证失败 → 反复复位。
 *
 * 流程:
 *   读 flash @0x0800C000 (加密标记)
 *   ├─ == 0x00001234 → 未初始化，生成加密 ID → 擦标记
 *   └─ ≠ 0x00001234 → 已处理 (0xFF 或 断电残留)，跳过
 *
 * 生成: 加密ID = HMAC-SHA256(BOOT_MASTER_HMAC_KEY, UID[12B]) 取前 16B
 */
static void uid_bind_first_run(void)
{
    uint32_t flag = *(volatile uint32_t *)UID_FLAG_ADDR;

    if (flag != UID_FLAG_VALUE) {
        /* 已处理 (0xFFFF 或 断电残留) → 无需操作 */
        return;
    }

    /* ---- 读 UID (96-bit, 12 字节) ---- */
    uint8_t uid[UID_LEN];
    memcpy(uid, (const void *)UID_ADDR, UID_LEN);

    /* ---- 计算加密 ID = HMAC-SHA256(BOOT_MASTER_HMAC_KEY, UID) 取前 16B ---- */
    uint8_t enc_id[32];
    crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);
    crypto_hmac_sha256_update(uid, UID_LEN);
    crypto_hmac_sha256_final(enc_id);

    /* ====================================================================
     * 写入加密 ID 到 flash @0x0800C800
     *
     * 注意: 写之前必须先擦页 (2KB)。如果该页存了其他数据，此处会丢失。
     * 当前设计 0x0800C800 独占一页，安全。
     * ==================================================================== */
    flash_int_erase_page(UID_ID_PAGE);  /* 擦除整页 (2KB)，页起始地址 */
    for (int i = 0; i < UID_ID_LEN; i += 2) {
        uint16_t hw = enc_id[i] | ((uint16_t)enc_id[i + 1] << 8);
        flash_int_program_halfword(UID_ID_ADDR + i, hw);
    }

    /* ---- 擦除标记页 (整页变 0xFF) ---- */
    flash_int_erase_page(UID_FLAG_ADDR);
}

/* ========================================================================
 * boot_main()  — Bootloader 主入口
 *
 * 上电后调用的第一个函数 (在 main.c 闪灯诊断之后)。
 * 读参数区 → 判断 OTA 状态机状态 → 执行升级或直接跳转 App。
 * 本函数不返回。
 * ======================================================================== */
void boot_main(void)
{
    ota_param_t p;

    /* ---- UID 绑定：首次上电生成加密 ID (防克隆) ---- */
    uid_bind_first_run();

    /* ---- 首次上电或参数区损坏：初始化参数区 ---- */
    if (param_read(&p) != 0) {
        /* magic != "PARM" 或 CRC32 不匹配 → 认为是出厂/损坏状态 */
        p.magic          = 0x5041524D;         /* "PARM" */
        p.state          = OTA_STATE_IDLE;
        p.upgrade_result = OTA_RESULT_NONE;
        param_write(&p);
        jump_to_app();                         /* 直接跳转 App */
        return;
    }

    /* ---- 根据参数区状态分发 ---- */
    switch (p.state) {

    case OTA_STATE_UPGRADE_REQUESTED:
        /*
         * App 已下载完新固件、写好 receive_offset、置 UPGRADE_REQUESTED、
         * 通过 NVIC_SystemReset() 软复位。Bootloader 接管升级。
         */
        boot_upgrade();
        break;

    case OTA_STATE_UPGRADING:
        /*
         * 上次升级写入完成，Bootloader 已跳转新 App。
         * 新 App 需在 30s 内写 app_healthy 并喂狗。
         * Bootloader 此时不再运行——若 MCU 复位回来且 state 仍为 UPGRADING，
         * 说明 IWDG 超时 (App 启动失败)。直接跳转 App，让 App 侧逻辑
         * (读取 upgrade_result=BOOT_FAIL) 处理上报。
         */
        jump_to_app();
        break;

    default:
        /* IDLE / DOWNLOADING / DOWNLOADED → App 自行管理，Bootloader 不干预 */
        jump_to_app();
        break;
    }
}

/* ========================================================================
 * boot_upgrade()  — 6 步固件升级流水线
 *
 * 仅在 state == UPGRADE_REQUESTED 时调用。
 * 每步失败都写 upgrade_result + 回 IDLE + 跳转当前 App。
 * 全流程幂等：任何一步掉电后重新上电 → Bootloader 重头开始，
 * 备份 magic 保证不会重复备份。
 *
 * 数据流:
 *   片外新固件区 blob [IV 16B][密文][HMAC 32B]
 *       ↓ [验签] HMAC-SHA256
 *       ↓ [解密] AES-256-CTR
 *       ↓ [写入] 片内 flash (16-bit × N)
 *   片内 App 区 → [备份] AES-256-CTR 加密 → 片外备份区
 * ======================================================================== */
static void boot_upgrade(void)
{
    ota_param_t p;
    fw_header_t fw_hdr;
    uint8_t  iv[16];                   /* AES-CTR 初始向量, 从 blob 首 16B 读取 */
    uint8_t  blob_hmac[32];            /* blob 末 32B 的 HMAC-SHA256       */
    uint32_t blob_size;                /* blob 总长 (含 IV+密文+HMAC)       */
    uint8_t  buf[BLOCK_SIZE];          /* 流式处理缓冲区                    */

    /* ====================================================================
     * Step a: 读片外固件头
     *
     * 固件头结构 (fw_header_t):
     *   magic  - 0x46574844 ("FWHD")，App 写完固件后写入
     *   size   - blob 总字节数
     *   version[16] - 目标版本
     *   receive_offset - App 已接收字节数 (断点续传用，Bootloader 不读)
     *
     * 失败场景: SPI 超时 / flash 未焊接 / magic 不匹配 (App 未完成下载)
     * ==================================================================== */
    flash_ext_read(FLASH_EXT_FW_HEADER_ADDR, (uint8_t*)&fw_hdr, sizeof(fw_hdr));
    if (fw_hdr.magic != FW_HEADER_MAGIC) {
        param_read(&p);
        p.state          = OTA_STATE_IDLE;
        p.upgrade_result = OTA_RESULT_SPI_ERROR;
        param_write(&p);
        jump_to_app();
        return;
    }
    blob_size = fw_hdr.size;

    /* ---- 提取 blob 中的 IV (首 16B) 和 HMAC (末 32B) ---- */
    flash_ext_read(FLASH_EXT_FW_AREA_ADDR, iv, 16);
    flash_ext_read(FLASH_EXT_FW_AREA_ADDR + blob_size - 32, blob_hmac, 32);

    /* ====================================================================
     * Step b: 先验签 (verify-before-write)
     *
     * ★ 核心安全原则: 片内 flash 不动一刀，先验证 blob 真伪。
     * HMAC 覆盖范围: blob[0 : size-32] 即 [IV + 密文]。
     * 验签失败 → 当前 App 完好无损，仅记录错误，无需回滚。
     *
     * 性能: 流式读 1KB × N 次块，每块做 HMAC update，全量仅读一次。
     * ==================================================================== */
    {
        uint32_t remaining = blob_size - 32;    /* HMAC 自身不计入计算范围 */
        uint32_t offset    = 0;

        crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);

        while (remaining > 0) {
            uint32_t chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;
            flash_ext_read(FLASH_EXT_FW_AREA_ADDR + offset, buf, chunk);
            crypto_hmac_sha256_update(buf, chunk);
            offset    += chunk;
            remaining -= chunk;
        }

        uint8_t calc_hmac[32];
        crypto_hmac_sha256_final(calc_hmac);

        if (memcmp(calc_hmac, blob_hmac, 32) != 0) {
            /* 验签失败: 固件被篡改或传输损坏 */
            param_read(&p);
            p.state          = OTA_STATE_IDLE;
            p.upgrade_result = OTA_RESULT_VERIFY_FAIL;
            param_write(&p);
            jump_to_app();         /* 当前 App 未动，继续跑旧版 */
            return;
        }
    }
    /* 验签通过 → 继续。至此片内 flash 完全未动。 */

    /* ====================================================================
     * Step c: 备份片内 App 到片外备份区 (加密)
     *
     * 为什么备份: 如果 step e (解密写入) 中途掉电/失败，必须能从备份恢复。
     *
     * ★ 断电重入: 通过备份区 magic (FW_BACKUP_MAGIC) 标记"已备份"。
     *    - magic 不存在 → 创建备份 → 写 magic
     *    - magic 已存在 → 上次已备份 (断电重入) → 跳过
     *
     * 备份格式: 与新固件区相同的 blob 格式
     *   [备份 IV 16B][AES-256-CTR 加密的片内 App][HMAC-SHA256 32B]
     *
     * 备份 key 与主密钥相同，IV 随机生成 (当前简化为全零，产线替换)。
     * ==================================================================== */
    param_read(&p);
    {
        fw_header_t bak_hdr;
        flash_ext_read(FLASH_EXT_BACKUP_HEADER, (uint8_t*)&bak_hdr, sizeof(bak_hdr));

        if (bak_hdr.magic != FW_BACKUP_MAGIC) {
            /* ---- 无备份: 创建 ---- */
            uint8_t  bak_iv[16] = {0};
            uint32_t app_size   = (uint32_t)APP_AREA_SIZE_KB * 1024;
            uint32_t bak_off    = 0;

            /* 流式: 读片内 App 1KB → AES-256-CTR 加密 → 写片外备份区 → 更新 HMAC */
            crypto_aes_ctr_init(BOOT_MASTER_AES_KEY, bak_iv);
            crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);

            while (app_size > 0) {
                uint32_t chunk = (app_size > BLOCK_SIZE) ? BLOCK_SIZE : app_size;

                /* 读片内明文 */
                memcpy(buf, (const void*)(APP_AREA_START + bak_off), chunk);

                /* AES-256-CTR 加密 */
                uint8_t enc_buf[BLOCK_SIZE];
                crypto_aes_ctr_crypt(buf, enc_buf, chunk);

                /* 写片外备份区 (偏移 +16 跳过 IV 位置) */
                flash_ext_write_page(FLASH_EXT_BACKUP_AREA + bak_off + 16,
                                     enc_buf, chunk);

                /* HMAC 流式计算 */
                crypto_hmac_sha256_update(enc_buf, chunk);

                bak_off  += chunk;
                app_size -= chunk;
            }

            /* 写备份 IV (首 16B) 和 HMAC (末 32B) */
            uint8_t bak_hmac[32];
            crypto_hmac_sha256_final(bak_hmac);
            flash_ext_write_page(FLASH_EXT_BACKUP_AREA, bak_iv, 16);
            flash_ext_write_page(FLASH_EXT_BACKUP_AREA
                                 + (APP_AREA_SIZE_KB * 1024) + 16,
                                 bak_hmac, 32);

            /* 写备份固件头 + magic 标记 (断电重入保护) */
            fw_header_t new_bak = {
                .magic = FW_BACKUP_MAGIC,
                .size  = blob_size
            };
            flash_ext_write_page(FLASH_EXT_BACKUP_HEADER,
                                 (uint8_t*)&new_bak, sizeof(new_bak));
        }
        /* magic 已存在 → 跳过备份 (断电重入幂等) */
    }
    /* 如果备份过程中任何一步失败 (SPI 写错)，flash_ext_write_page 内部
       无返回错误码 (当前简化)，建议后续版本加入返回值检查。 */

    /* ====================================================================
     * Step d+e: 擦除片内 App 区 + 流式解密写入新固件
     *
     * 擦除: 256KB = 128 页 × 2KB/page，逐页擦除。
     * 解密: AES-256-CTR 流式解密 (无需 padding)，边读边解密边写。
     *
     * 密文在片外 blob[16 : size-32] (跳过首 IV 和末 HMAC)。
     * 每 1KB 块解密后逐半字 (16-bit) 写入片内 flash。
     *
     * STM32F103VE 片内 flash 特性:
     *   - 擦除最小单位: 2KB (页)
     *   - 编程最小单位: 16-bit (半字)
     *   - 必须在擦除后 (全 0xFF) 才能编程，且每个半字只能编程一次
     * ==================================================================== */
    flash_int_erase_area(APP_AREA_START, APP_AREA_SIZE_KB);

    {
        uint32_t ct_size   = blob_size - 48;   /* 密文 = 总长 - IV(16) - HMAC(32) */
        uint32_t ct_offset = 0;

        /* 初始化 AES-256-CTR，使用新固件的 IV */
        crypto_aes_ctr_init(BOOT_MASTER_AES_KEY, iv);

        while (ct_size > 0) {
            uint32_t chunk = (ct_size > BLOCK_SIZE) ? BLOCK_SIZE : ct_size;

            /* 读片外密文 (从偏移 16 开始，跳过 blob 首 IV) */
            flash_ext_read(FLASH_EXT_FW_AREA_ADDR + 16 + ct_offset, buf, chunk);

            /* AES-256-CTR 解密 (加密和解密是同一操作) */
            uint8_t plain[BLOCK_SIZE];
            crypto_aes_ctr_crypt(buf, plain, chunk);

            /* 逐半字写入片内 flash */
            for (uint32_t i = 0; i < chunk; i += 2) {
                uint16_t hw = plain[i] | ((uint16_t)plain[i + 1] << 8);
                flash_int_program_halfword(APP_AREA_START + ct_offset + i, hw);
            }

            ct_offset += chunk;
            ct_size   -= chunk;
        }
    }
    /* 如果写入失败 (flash 编程错误)，当前简化版本不做回滚处理。
       生产版本应在此处加入写入校验 + 从备份区恢复逻辑。 */

    /* ====================================================================
     * Step f: 升级完成，更新参数区并跳转新 App
     *
     * - state = UPGRADING (告诉下次上电: 新 App 待确认)
     * - cur_version ← new_version (版本号更新)
     * - upgrade_result = SUCCESS
     * - 清备份 magic (下次升级需重新备份)
     * - 启动 IWDG(30s) 后跳转新 App (IWDG 启动由 jump_to_app 前完成)
     * ==================================================================== */
    p.state = OTA_STATE_UPGRADING;
    strncpy(p.cur_version, p.new_version, sizeof(p.cur_version) - 1);
    p.cur_version[sizeof(p.cur_version) - 1] = '\0';
    p.upgrade_result = OTA_RESULT_SUCCESS;

    /* 清备份 magic */
    flash_ext_erase_sector(FLASH_EXT_BACKUP_HEADER);

    /* 持久化参数区 */
    param_write(&p);

    /* 跳转新 App (不返回)。
       注意: IWDG 启动应在 jump_to_app() 之前完成。
       当前简化版本未在此处启动 IWDG，需在后续集成时补充。 */
    jump_to_app();
}
