#ifndef OTA_PARAM_H
#define OTA_PARAM_H

#include <stdint.h>

/* 参数区地址 */
#define OTA_PARAM_ADDR          0x0804C000u
#define OTA_PARAM_PAGE_SIZE     2048u    /* STM32F103VE 页大小 2KB */
#define OTA_PARAM_SIZE          4096u    /* 分配 4KB = 2 页 */

/* OTA 状态机 */
#define OTA_STATE_IDLE              0u
#define OTA_STATE_DOWNLOADING       1u
#define OTA_STATE_DOWNLOADED        2u
#define OTA_STATE_UPGRADE_REQUESTED 3u
#define OTA_STATE_UPGRADING         4u

/* Bootloader 升级结果码 */
#define OTA_RESULT_NONE             0u
#define OTA_RESULT_SUCCESS          1u
#define OTA_RESULT_VERIFY_FAIL      2u
#define OTA_RESULT_BACKUP_FAIL      3u
#define OTA_RESULT_SPI_ERROR        4u
#define OTA_RESULT_WRITE_ROLLBACK   5u
#define OTA_RESULT_BOOT_FAIL        6u

/* 参数区结构体（64 字节，与设计文档一致） */
typedef struct {
    uint32_t magic;           /* 0x5041524D ("PARM") */
    uint32_t dev_id;          /* 设备 ID */
    uint8_t  state;           /* OTA 状态机 */
    uint8_t  app_healthy;     /* 启动确认标志 */
    uint8_t  upgrade_flag;    /* 触发升级标志 */
    uint8_t  upgrade_result;  /* Bootloader 写入的结果码 */
    uint8_t  task_id[16];     /* OTA 任务 ID */
    char     cur_version[16]; /* 当前 App 版本 */
    char     new_version[16]; /* 升级目标版本 */
    uint32_t crc32;           /* 结构体 CRC32 */
} ota_param_t;

#endif
