/*
 * mbedtls V2.28 LTS — Bootloader 最小配置
 * 仅启用 AES-256-CTR + HMAC-SHA256 所需模块，最小化 ROM 占用
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* 系统支持 */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* AES-256-CTR */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CTR

/* HMAC-SHA256 */
#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C

/* 禁用不需要的特性（减小体积） */
#undef MBEDTLS_AES_ROM_TABLES        /* 使用运行时生成的 S-box */

#include "mbedtls/check_config.h"
#endif /* MBEDTLS_CONFIG_H */
