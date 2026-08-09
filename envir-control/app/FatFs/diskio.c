/**
 * @file    diskio.c
 * @brief   FatFs 底层磁盘接口 - 桥接到 sd_lib/sd_spi 驱动
 *
 * @details
 * FatFs 通过 diskio.h 定义的 5 个函数访问物理磁盘:
 *   disk_initialize() -> SD_SPI_Init()
 *   disk_status()     -> 就绪标志
 *   disk_read()       -> SD_SPI_ReadBlock()
 *   disk_write()      -> SD_SPI_WriteBlock()
 *   disk_ioctl()      -> 容量/扇区信息
 *
 * 本适配层移植自 downloader 工程（经硬件验证），并扩展为支持写入。
 * 底层驱动 (sd_lib/sd_spi.c) 带 uC/OS-III 临界区保护。
 *
 * 物理驱动号 pdrv = 0 对应 SD 卡 (SPI2)。
 *
 * @author Christman
 * @date   2026
 */

#include "ff.h"          /* FatFs 头文件 */
#include "diskio.h"      /* FatFs 磁盘接口定义 */
#include "sd_spi.h"      /* SD 卡 SPI 驱动 (sd_lib/sd_spi.h) */

/* 单卷状态：b7 = 已初始化标志 */
static DSTATUS g_sd_stat = STA_NOINIT;

/* ============================ 磁盘状态 ============================ */

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NODISK;
    return g_sd_stat;
}

/* ============================ 磁盘初始化 ============================ */

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NODISK;

    /* 1. SPI2 硬件初始化 (GPIO + SPI 外设配置) */
    BSP_SPI2_Init();

    /* 2. SD 卡 SPI 模式初始化 (CMD0->CMD8->ACMD41)。返回 0=成功 */
    if (SD_SPI_Init() != 0u) {
        g_sd_stat |= STA_NOINIT;
        return g_sd_stat;
    }

    g_sd_stat &= ~STA_NOINIT;
    return g_sd_stat;
}

/* ============================ 扇区读 ============================ */

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (g_sd_stat & STA_NOINIT) return RES_NOTRDY;
    if (!buff || count == 0u) return RES_PARERR;

    for (UINT i = 0u; i < count; i++) {
        /* SDHC: 直接传扇区号; SDSC: 驱动内部自动 ×512 */
        if (SD_SPI_ReadBlock((uint32_t)(sector + i),
                             buff + (i * SD_BLOCK_SIZE)) != 0u) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

/* ============================ 扇区写 ============================ */

#if !FF_FS_READONLY
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (g_sd_stat & STA_NOINIT) return RES_NOTRDY;
    if (!buff || count == 0u) return RES_PARERR;

    for (UINT i = 0u; i < count; i++) {
        if (SD_SPI_WriteBlock((uint32_t)(sector + i),
                              buff + (i * SD_BLOCK_SIZE)) != 0u) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}
#endif

/* ============================ IO 控制 ============================ */

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    if (g_sd_stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        /* SD_SPI_WriteBlock 内部已等 busy 完成, 无需额外同步 */
        break;

    case GET_SECTOR_COUNT:
        /* 通过 CSD 计算容量 */
        {
            uint8_t csd[16];
            if (SD_SPI_ReadCSD(csd) != 0u) {
                return RES_ERROR;
            }
            uint8_t csd_ver = (csd[0] >> 6) & 0x03u;
            uint32_t capacity_bytes = 0u;
            if (csd_ver == 0u) {
                /* CSD v1.0 (SDSC) */
                uint32_t read_bl_len = csd[5] & 0x0Fu;
                uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10)
                                | ((uint32_t)csd[7] << 2)
                                | ((uint32_t)csd[8] >> 6);
                uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03u) << 1)
                                     | ((uint32_t)csd[10] >> 7);
                uint32_t block_len = 1u << read_bl_len;
                uint32_t mult = 1u << (c_size_mult + 2u);
                capacity_bytes = (c_size + 1u) * mult * block_len;
            } else if (csd_ver == 1u) {
                /* CSD v2.0 (SDHC/SDXC) */
                uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16)
                                | ((uint32_t)csd[8] << 8)
                                | (uint32_t)csd[9];
                capacity_bytes = (c_size + 1u) * 512u * 1024u;
            }
            if (capacity_bytes > 0u) {
                *(LBA_t *)buff = capacity_bytes / SD_BLOCK_SIZE;
            } else {
                return RES_ERROR;
            }
        }
        break;

    case GET_SECTOR_SIZE:
        *(WORD *)buff = SD_BLOCK_SIZE;
        break;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1u;
        break;

    default:
        return RES_PARERR;
    }
    return RES_OK;
}
