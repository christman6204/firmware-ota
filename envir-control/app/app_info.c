/**
 * @file    app_info.c
 * @brief   App 信息区 —— 128B 编译期固定于 0x0800F800
 *
 * 分散加载文件中的 .app_info 段将 APP_INFO 放置在 flash 62KB 偏移处。
 * 该区域位于 Bootloader 与 App 代码之间的保留区，OTA 升级时随固件一起更新。
 */

#include "app_info.h"

const app_info_t APP_INFO
    __attribute__((section(".app_info")))
    = {
        .fw_version         = FW_VER_VALUE,
        .master_device_key  = {0xf5, 0x09, 0x00, 0x12, 0x6c, 0xb0, 0xfe, 0x24, 
					                     0xe2, 0xd6, 0x15, 0x2d, 0xb5, 0x69, 0x49, 0xa2, 
				                       0x31, 0x2a, 0xf2, 0xab, 0x0d, 0xff, 0x51, 0x12, 
				                       0x4c, 0x8f, 0x6c, 0x0f, 0x2f, 0x0b, 0xa9, 0x74},   /* ⚠ 产线替换, 与服务器 master_device_key 一致 */
        .reserved           = {0},
    };
