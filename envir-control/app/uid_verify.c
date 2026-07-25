/**
 * @file    uid_verify.c
 * @brief   设备防克隆 —— App 侧 UID 验证实现
 *
 * 设计文档: docs/ota-design.md §7.5
 *
 * 调用时机: App 启动的最早阶段 (在 main 函数或第一个任务中，
 *           在任何业务逻辑之前)。失败则 NVIC_SystemReset()。
 *
 * 注意: BOOT_MASTER_HMAC_KEY 必须与 bootloader/src/crypto.c 中完全一致。
 *       开发阶段为全零密钥；量产前统一替换。
 */

#include "uid_verify.h"
#include <string.h>
#include "stm32f10x.h"
#include "mbedtls/md.h"

/* ---- 常量 (与 bootloader/src/boot_main.c 完全一致) ---- */
#define UID_ADDR         0x1FFFF7E8u
#define UID_LEN          12u
#define UID_ID_ADDR      0x0807F800u
#define UID_ID_LEN       16u

/* ---- 主密钥 (必须与 bootloader/src/crypto.c 完全一致) ---- */
static const uint8_t MASTER_HMAC_KEY[32] = {0};  /* 全零占位，量产替换 */

void uid_verify(void)
{
    uint8_t  uid[UID_LEN];                         /* 96-bit 芯片唯一 ID     */
    uint8_t  calc[32];                             /* HMAC-SHA256 计算结果   */
    uint8_t  stored[UID_ID_LEN];                   /* bootloader 写入的加密ID */
    uint8_t  i;
    mbedtls_md_context_t  ctx;                     /* mbedtls HMAC 上下文    */

    /* ---- Step 1: 读 UID 并计算 HMAC-SHA256(主密钥, UID) ---- */
    memcpy(uid, (const void *)UID_ADDR, UID_LEN);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, MASTER_HMAC_KEY, 32);
    mbedtls_md_hmac_update(&ctx, uid, UID_LEN);
    mbedtls_md_hmac_finish(&ctx, calc);
    mbedtls_md_free(&ctx);

    /* ---- Step 2: 读 bootloader 写入的加密 ID (0x0807F800, 16B) ---- */
    memcpy(stored, (const void *)UID_ID_ADDR, UID_ID_LEN);

    /* ---- Step 3: 比对前 16 字节 ---- */
    for (i = 0; i < UID_ID_LEN; i++) {
        if (calc[i] != stored[i]) {
            /* 校验失败 —— 固件被复制到其他设备。
               反复复位拒绝运行，不给攻击者任何可用的运行状态。 */
            NVIC_SystemReset();
        }
    }
    /* 校验通过 —— UID 匹配，正常启动。 */
}
