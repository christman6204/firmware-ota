/**
 * @file    device_secret.c
 * @brief   设备认证 secret 派生实现
 *
 * master_device_key 存储在 APP_INFO 中 (scatter 定位 0x0800F800 offset 2),
 * 编译期写入, RDP L1 保护, 不出 STM32。
 */

#include "device_secret.h"
#include "mbedtls/md.h"

/**
 * @brief  APP_INFO 中的 master_device_key, 地址 = 0x0800F800 + 2
 *
 * 直接从 flash 地址读取, 不需要 extern 声明。
 * app_info_t 前 2 字节是 fw_version, 紧接着 32 字节是 master_device_key。
 */
#define MASTER_DEVICE_KEY_ADDR  ((const uint8_t *)0x0800F802u)

void device_secret_gen(uint32_t dev_id, uint8_t secret[16])
{
    uint8_t dev_le[4];
    uint8_t hmac_out[32];
    mbedtls_md_context_t ctx;

    /* dev_id 小端编码 */
    dev_le[0] = (uint8_t)(dev_id);
    dev_le[1] = (uint8_t)(dev_id >> 8);
    dev_le[2] = (uint8_t)(dev_id >> 16);
    dev_le[3] = (uint8_t)(dev_id >> 24);

    /* HMAC-SHA256(master_device_key, dev_id_le) */
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, MASTER_DEVICE_KEY_ADDR, 32);
    mbedtls_md_hmac_update(&ctx, dev_le, 4);
    mbedtls_md_hmac_finish(&ctx, hmac_out);
    mbedtls_md_free(&ctx);

    /* 取前 16 字节 */
    for (int i = 0; i < 16; i++) {
        secret[i] = hmac_out[i];
    }
}
