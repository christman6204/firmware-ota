/**
 * @file    flash_ext.h
 * @brief   片外 SPI Flash 操作接口与布局定义
 *
 * @details 定义片外 Flash 的存储布局、固件头部结构体 (fw_header_t)
 *          以及操作函数接口。
 *
 * @attention 本模块是硬件无关的接口层，具体 SPI 驱动实现在对应 .c 中。
 */

#ifndef FLASH_EXT_H
#define FLASH_EXT_H

#include <stdint.h>

/* ================================================================
 *  片外 Flash 存储布局
 *  ================================================================
 *
 *  +---------------------------+ 0x00000000
 *  |  固件头部 (4 KB)           |
 *  |  fw_header_t + padding    |
 *  +---------------------------+ 0x00001000
 *  |  固件数据区               |
 *  |  (固件二进制本体)          |
 *  |  ...                      |
 *  +---------------------------+ 0x00081000
 *  |  备份固件头部 (4 KB)       |
 *  +---------------------------+ 0x00082000
 *  |  备份固件数据区            |
 *  |  ...                      |
 *  +---------------------------+
 */

/** 固件头部所在的起始地址 */
#define FLASH_EXT_FW_HEADER_ADDR   0x00000000u
/** 固件数据区的起始地址 */
#define FLASH_EXT_FW_AREA_ADDR     0x00001000u
/** 备份固件头部的起始地址 */
#define FLASH_EXT_BACKUP_HEADER    0x00081000u
/** 备份固件数据区的起始地址 */
#define FLASH_EXT_BACKUP_AREA      0x00082000u
/** 片外 Flash 扇区大小 (4 KB) */
#define FLASH_EXT_SECTOR_SIZE      4096u

/* ================================================================
 *  Magic 常量
 *  ================================================================ */

/**
 * @brief 主固件头部的 Magic 标识
 * @note  值: 0x46574844, ASCII 为 "FWHD" (FirmWare HeaDer)
 */
#define FW_HEADER_MAGIC            0x46574844u

/**
 * @brief 备份固件头部的 Magic 标识
 * @note  值: 0x424B5550, ASCII 为 "BKUP" (BacKUP)
 */
#define FW_BACKUP_MAGIC            0x424B5550u

/* ================================================================
 *  固件头部结构体
 *  ================================================================ */

/**
 * @brief 固件头部结构体 (fw_header_t)
 *
 * 存储在片外 Flash 的头部区域 (4 KB 扇区首部)，
 * 用于描述固件的基本信息，Bootloader 据此判断固件的完整性与有效性。
 */
typedef struct {
    /** Magic 标识，用于校验头部有效性 (FW_HEADER_MAGIC 或 FW_BACKUP_MAGIC) */
    uint32_t magic;

    /** 固件总大小 (字节)，不含头部 */
    uint32_t size;

    /** 固件版本字符串 (以 '\0' 结尾，最多 15 个有效字符) */
    char     version[16];

    /** OTA 接收进度偏移量 (已接收的字节数)，用于断点续传 */
    uint32_t receive_offset;
} fw_header_t;

/* ================================================================
 *  操作接口
 *  ================================================================ */

/**
 * @brief   初始化片外 SPI Flash (GPIO/SPI 时钟使能、引脚配置)
 */
void      flash_ext_init(void);

/**
 * @brief   从片外 Flash 读取数据
 * @param   addr  片外 Flash 中的起始地址
 * @param   buf   接收缓冲区指针
 * @param   len   要读取的字节数
 */
void      flash_ext_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief   向片外 Flash 写入一页 (256 字节)
 * @param   addr  目标地址
 * @param   buf   源数据缓冲区
 * @param   len   要写入的字节数 (不大于 256)
 */
void      flash_ext_write_page(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
 * @brief   擦除片外 Flash 的一个扇区 (4 KB)
 * @param   addr  扇区内任意地址
 */
void      flash_ext_erase_sector(uint32_t addr);

/**
 * @brief   擦除片外 Flash 的 4 KB 区域
 * @param   addr  起始地址
 * @note    与 flash_ext_erase_sector 功能相同，提供命名别名
 */
void      flash_ext_erase_4k(uint32_t addr);

/**
 * @brief   读取片外 Flash 的 JEDEC 制造商/设备 ID
 * @return  3 字节 ID (高 8 位为 0，低 24 位为实际 ID)
 */
uint32_t  flash_ext_read_id(void);

#endif
