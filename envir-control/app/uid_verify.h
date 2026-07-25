/**
 * @file    uid_verify.h
 * @brief   设备防克隆 —— App 侧 UID 验证
 *
 * 设计文档: docs/ota-design.md §7.5
 *
 * App 每次启动时必须调用 uid_verify()。该函数读取 STM32 片内 UID，
 * 重新计算 HMAC-SHA256(主密钥, UID)，与 Bootloader 首次上电时写入
 * 的加密 ID (位于 0x0800C900，页内偏移 256B) 比对。不匹配则 NVIC_SystemReset()。
 *
 * 注意: 主密钥 BOOT_MASTER_HMAC_KEY 与 Bootloader 中必须完全相同。
 */
#ifndef UID_VERIFY_H
#define UID_VERIFY_H

void uid_verify(void);

#endif
