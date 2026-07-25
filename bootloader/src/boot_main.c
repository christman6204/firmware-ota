#include "boot_main.h"
#include "ota_param.h"
#include "param.h"
#include "flash_ext.h"
#include "flash_int.h"
#include "crypto.h"
#include "jump_app.h"
#include <string.h>

#define APP_AREA_START   0x0800C000u
#define APP_AREA_SIZE_KB 256u
#define BLOCK_SIZE       1024u

static void boot_upgrade(void) {
    ota_param_t p;
    fw_header_t fw_hdr;
    uint8_t iv[16];
    uint8_t blob_hmac[32];
    uint32_t blob_size;
    uint8_t buf[BLOCK_SIZE];

    /* ---- Step a: 读固件头 ---- */
    flash_ext_read(FLASH_EXT_FW_HEADER_ADDR, (uint8_t*)&fw_hdr, sizeof(fw_hdr));
    if (fw_hdr.magic != FW_HEADER_MAGIC) {
        param_read(&p);
        p.state = OTA_STATE_IDLE;
        p.upgrade_result = OTA_RESULT_SPI_ERROR;
        param_write(&p);
        return;
    }
    blob_size = fw_hdr.size;

    /* 从 blob 读 IV（首 16B）和 HMAC（末 32B）*/
    flash_ext_read(FLASH_EXT_FW_AREA_ADDR, iv, 16);
    flash_ext_read(FLASH_EXT_FW_AREA_ADDR + blob_size - 32, blob_hmac, 32);

    /* ---- Step b: 先验签（verify-before-write）---- */
    {
        uint32_t remaining = blob_size - 32;
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
            param_read(&p);
            p.state = OTA_STATE_IDLE;
            p.upgrade_result = OTA_RESULT_VERIFY_FAIL;
            param_write(&p);
            return;
        }
    }

    /* ---- Step c: 备份片内 App ---- */
    param_read(&p);
    {
        fw_header_t bak_hdr;
        flash_ext_read(FLASH_EXT_BACKUP_HEADER, (uint8_t*)&bak_hdr, sizeof(bak_hdr));
        if (bak_hdr.magic != FW_BACKUP_MAGIC) {
            uint8_t bak_iv[16] = {0}; /* 简化：需替换为硬件随机数 */
            uint32_t app_size = (uint32_t)APP_AREA_SIZE_KB * 1024;
            uint32_t bak_off  = 0;

            crypto_aes_ctr_init(BOOT_MASTER_AES_KEY, bak_iv);
            crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);
            while (app_size > 0) {
                uint32_t chunk = (app_size > BLOCK_SIZE) ? BLOCK_SIZE : app_size;
                memcpy(buf, (const void*)(APP_AREA_START + bak_off), chunk);
                uint8_t enc_buf[BLOCK_SIZE];
                crypto_aes_ctr_crypt(buf, enc_buf, chunk);
                flash_ext_write_page(FLASH_EXT_BACKUP_AREA + bak_off + 16, enc_buf, chunk);
                crypto_hmac_sha256_update(enc_buf, chunk);
                bak_off  += chunk;
                app_size -= chunk;
            }
            uint8_t bak_hmac[32];
            crypto_hmac_sha256_final(bak_hmac);
            flash_ext_write_page(FLASH_EXT_BACKUP_AREA, bak_iv, 16);
            flash_ext_write_page(FLASH_EXT_BACKUP_AREA + (APP_AREA_SIZE_KB * 1024) + 16, bak_hmac, 32);
            fw_header_t new_bak = { .magic = FW_BACKUP_MAGIC, .size = blob_size };
            flash_ext_write_page(FLASH_EXT_BACKUP_HEADER, (uint8_t*)&new_bak, sizeof(new_bak));
        }
    }

    /* ---- Step d+e: 擦片内 App 区 + 流式解密写入 ---- */
    flash_int_erase_area(APP_AREA_START, APP_AREA_SIZE_KB);

    {
        uint32_t ct_size   = blob_size - 48;
        uint32_t ct_offset = 0;
        crypto_aes_ctr_init(BOOT_MASTER_AES_KEY, iv);
        while (ct_size > 0) {
            uint32_t chunk = (ct_size > BLOCK_SIZE) ? BLOCK_SIZE : ct_size;
            flash_ext_read(FLASH_EXT_FW_AREA_ADDR + 16 + ct_offset, buf, chunk);
            uint8_t plain[BLOCK_SIZE];
            crypto_aes_ctr_crypt(buf, plain, chunk);
            for (uint32_t i = 0; i < chunk; i += 2) {
                uint16_t hw = plain[i] | ((uint16_t)plain[i+1] << 8);
                flash_int_program_halfword(APP_AREA_START + ct_offset + i, hw);
            }
            ct_offset += chunk;
            ct_size   -= chunk;
        }
    }

    /* ---- Step f: 成功 ---- */
    p.state = OTA_STATE_UPGRADING;
    strncpy(p.cur_version, p.new_version, 16);
    p.upgrade_result = OTA_RESULT_SUCCESS;
    {
        fw_header_t clear = {0};
        flash_ext_erase_sector(FLASH_EXT_BACKUP_HEADER);
    }
    param_write(&p);
}

void boot_main(void) {
    ota_param_t p;

    if (param_read(&p) != 0) {
        /* 参数区未初始化（首次上电或损坏）-> 初始化并跳转 App */
        p.magic = 0x5041524D;
        p.state = OTA_STATE_IDLE;
        p.upgrade_result = OTA_RESULT_NONE;
        param_write(&p);
        jump_to_app();
        return;
    }

    if (p.state == OTA_STATE_UPGRADE_REQUESTED) {
        boot_upgrade();
    } else if (p.state == OTA_STATE_UPGRADING) {
        /* 上次升级完成等 App 确认（IWDG 机制，Bootloader 只跳转）*/
        jump_to_app();
    } else {
        jump_to_app();
    }
}
