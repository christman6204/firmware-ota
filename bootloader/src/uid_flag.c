/**
 * @file    uid_flag.c
 * @brief   UID 绑定首次上电标记 — 编译期固定在 0x0800C000
 *
 * 设计意图:
 *   - 产线烧录的 factory.bin 中自动包含此标记 (bootloader.hex 生成时就有)
 *   - 首次上电时, boot_main.c 的 uid_bind_first_run() 检测到此标记
 *     → 读取 UID → HMAC-SHA256 生成加密 ID → 写入 0x0800C800
 *     → 擦除此标记所在页 (整页变 0xFF)
 *   - 后续上电: 标记已为 0xFFFFFFFF → 跳过 UID 绑定
 *
 * 存储位置: 加密ID标记区 @0x0800C000 (2KB 页起始)
 * 值: 0x00001234 (little-endian)
 */

#include <stdint.h>

const uint32_t UID_FLAG
    __attribute__((at(0x0800C000)))
    = 0x00001234;
