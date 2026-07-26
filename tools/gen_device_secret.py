#!/usr/bin/env python3
"""
派生单台设备的认证密钥

公式: secret = HMAC-SHA256(master_device_key, dev_id) 取前 16 字节 hex

用法:
    # 单台设备
    python gen_device_secret.py 10001 --keys keys.json

    # 批量生成 dev_id 10001~10100
    python gen_device_secret.py 10001-10100 --keys keys.json

    # 自动写入服务器 devices 表
    python gen_device_secret.py 10001-10100 --keys keys.json --sql

输出:
    dev_id=10001  secret=<16B hex>
"""

import sys
import json
import hmac
import hashlib
import argparse
import struct


def derive_secret(master_key: bytes, dev_id: int) -> str:
    """HMAC-SHA256(master_key, dev_id_le)[:16] → hex"""
    dev_bytes = struct.pack("<I", dev_id)
    sig = hmac.digest(master_key, dev_bytes, hashlib.sha256)
    return sig[:16].hex()


def parse_range(s: str):
    """'10001-10100' → range(10001, 10101)"""
    if "-" in s:
        lo, hi = s.split("-")
        return range(int(lo), int(hi) + 1)
    return [int(s)]


def main():
    parser = argparse.ArgumentParser(description="派生设备认证密钥")
    parser.add_argument("dev_id", help="设备 ID, 如 10001 或 10001-10100")
    parser.add_argument("--keys", "-k", required=True, help="keys.json 路径")
    parser.add_argument("--sql", action="store_true", help="生成 INSERT SQL")
    args = parser.parse_args()

    with open(args.keys, encoding="utf-8") as f:
        keys = json.load(f)
    mk = bytes.fromhex(keys["master_device_key"])

    ids = parse_range(args.dev_id)

    if args.sql:
        print("-- 设备认证密钥批量 INSERT")
        print("INSERT INTO devices (dev_id, secret_hash) VALUES")

    for i, dev_id in enumerate(ids):
        s = derive_secret(mk, dev_id)
        if args.sql:
            comma = "," if i < len(ids) - 1 else ";"
            print(f"  ({dev_id}, '{s}'){comma}")
        else:
            print(f"dev_id={dev_id:5d}  secret={s}")


if __name__ == "__main__":
    main()
