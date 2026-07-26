/**
 * @file    app_info.h
 * @brief   App 信息区结构体 (128 字节, 固定在 0x0800F800)
 *
 * 布局:
 *   [0..1]    fw_version        uint16_t      固件版本 (高8主, 低8次)
 *   [2..33]   master_device_key  uint8_t[32]  设备认证主密钥
 *   [34..127] reserved           uint8_t[94]  预留扩展
 *
 * 分散加载: LR_APP_INFO 0x0800F800 0x80, section ".app_info"
 */

#ifndef APP_INFO_H
#define APP_INFO_H

#include <stdint.h>

#define APP_INFO_ADDR        0x0800F800u
#define APP_INFO_SIZE        128u

/* ---- 版本宏 (编译期修改) ---- */
#define FW_VER_MAJOR  1u
#define FW_VER_MINOR  0u
#define FW_VER_VALUE  ((uint16_t)(((FW_VER_MAJOR) << 8) | (FW_VER_MINOR)))
#define FW_VER_MAJOR_FROM(v)  ((uint8_t)((v) >> 8))
#define FW_VER_MINOR_FROM(v)  ((uint8_t)(v))

typedef struct {
    uint16_t fw_version;                    /* [0..1]   固件版本 */
    uint8_t  master_device_key[32];         /* [2..33]  设备认证主密钥 */
    uint8_t  reserved[94];                  /* [34..127] 预留 */
} app_info_t;

/** 编译期固定的 App 信息, scatter 定位在 0x0800F800 */
extern const app_info_t APP_INFO;

#endif /* APP_INFO_H */
