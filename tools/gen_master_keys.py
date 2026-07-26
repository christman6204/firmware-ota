#!/usr/bin/env python3
"""
生成 OTA 主密钥对 (AES-256 + HMAC-SHA256)

用法:
    python gen_master_keys.py                    # 交互式，屏幕输出 hex
    python gen_master_keys.py --output keys.json # 写入 JSON 文件

安全注意事项:
    - 产线隔离环境运行，禁止联网
    - 生成后一份烧录到 STM32 Bootloader (crypto.c)，一份本地离线保管
    - NEVER 提交 keys.json 到 git
"""

import os
import sys
import json
import argparse


def gen_keys():
    aes_key     = os.urandom(32)
    hmac_key    = os.urandom(32)
    device_key  = os.urandom(32)
    return {
        "aes_key":           aes_key.hex(),
        "hmac_key":          hmac_key.hex(),
        "master_device_key": device_key.hex(),
    }


def main():
    parser = argparse.ArgumentParser(description="生成 OTA 主密钥对")
    parser.add_argument("--output", "-o", help="输出 JSON 文件路径")
    parser.add_argument("--quiet", "-q", action="store_true", help="仅输出 JSON")
    args = parser.parse_args()

    keys = gen_keys()

    # 写入文件 (先写，避免 --quiet 提前 return 跳过)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            json.dump(keys, f, indent=2)

    if args.quiet:
        print(json.dumps(keys))
        return

    print("=" * 60)
    print("  OTA 主密钥组 (AES + HMAC + Device Auth)")
    print("=" * 60)
    print()
    print(f"  AES_KEY            (hex): {keys['aes_key']}")
    print(f"  HMAC_KEY           (hex): {keys['hmac_key']}")
    print(f"  master_device_key  (hex): {keys['master_device_key']}")
    print()
    print("  固件密钥 (AES+HMAC) 所有设备共用，设备认证密钥一机一派生")
    print()
    print("  【下一步】")
    print(f"  1. AES_KEY, HMAC_KEY → bootloader/src/crypto.c")
    print(f"  2. master_device_key → 服务器环境变量 (ARK_MASTER_DEVICE_KEY)")
    print(f"  3. master_device_key → 产线烧录工具 (派生每台设备 secret)")
    print(f"  4. keys.json → 离线加密保管，NEVER 提交 git")
    print(f"  5. 加密固件: python encrypt_firmware.py app.bin --keys keys.json")
    print(f"  6. 派生设备密钥: python gen_device_secret.py <dev_id> --keys keys.json")

    if args.output:
        print(f"\n  ✓ 已写入: {args.output}")
        print(f"  ⚠ 请立即将 keys.json 移到离线存储，不要留在工程目录里")


if __name__ == "__main__":
    main()
