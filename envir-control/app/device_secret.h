/**
 * @file    device_secret.h
 * @brief   设备认证 secret 派生 — 读取 APP_INFO.master_device_key 计算
 *
 * secret = HMAC-SHA256(master_device_key, dev_id_le)[:16]
 */

#ifndef DEVICE_SECRET_H
#define DEVICE_SECRET_H

#include <stdint.h>

/** 派生设备认证 secret (16 字节), 结果直接用作 hex 比对或转 hex 字符串 */
void device_secret_gen(uint32_t dev_id, uint8_t secret[16]);

#endif
