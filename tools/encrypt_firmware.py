#!/usr/bin/env python3
"""
本地固件加密工具 —— 将明文 app.bin 加密为 OTA 下发 blob

用法:
    # 指定密钥文件
    python encrypt_firmware.py app_v1.2.bin --keys keys.json

    # 指定输出路径
    python encrypt_firmware.py app_v1.2.bin --keys keys.json -o app_v1.2_blob.bin

    # 命令行直接传入密钥 (CI/自动化)
    python encrypt_firmware.py app_v1.2.bin --aes-key <hex> --hmac-key <hex>

输出:
    - 加密 blob: [IV 16B][AES-256-CTR 密文][HMAC-SHA256 32B]
    - 控制台打印 IV / HMAC / file_size (用于录入 MySQL firmwares 表)

安全设计:
    - 加密在本地离线执行，服务器从不持有主密钥和明文
    - 服务器只负责存储 blob + 转发给设备
    - 密钥文件 (keys.json) 禁止提交到 git
"""

import os
import sys
import json
import argparse

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import hmac, hashes


def encrypt_firmware(plaintext: bytes, aes_key: bytes, hmac_key: bytes) -> tuple:
    """
    加密固件，返回 (blob, iv, signature)

    blob 格式: [IV 16B] [密文] [HMAC-SHA256 32B]
    HMAC 覆盖: IV + 密文
    """
    # 生成随机 IV (每个版本仅生成一次)
    iv = os.urandom(16)

    # AES-256-CTR 加密
    cipher = Cipher(algorithms.AES(aes_key), modes.CTR(iv))
    encryptor = cipher.encryptor()
    ciphertext = encryptor.update(plaintext) + encryptor.finalize()

    # IV + 密文 (HMAC 计算范围)
    iv_ct = iv + ciphertext

    # HMAC-SHA256 签名
    h = hmac.HMAC(hmac_key, hashes.SHA256())
    h.update(iv_ct)
    signature = h.finalize()

    # 拼接 blob
    blob = iv_ct + signature

    return blob, iv, signature


def load_keys_from_file(path: str) -> tuple:
    """从 JSON 文件加载密钥"""
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    aes_key  = bytes.fromhex(data["aes_key"])
    hmac_key = bytes.fromhex(data["hmac_key"])
    if len(aes_key) != 32 or len(hmac_key) != 32:
        raise ValueError("密钥长度错误: AES_KEY 和 HMAC_KEY 必须各为 32 字节 (64 hex 字符)")
    return aes_key, hmac_key


def main():
    parser = argparse.ArgumentParser(
        description="本地加密固件 → OTA blob (AES-256-CTR + HMAC-SHA256)",
        epilog="服务器永不持有主密钥，加密完全在本地离线完成。"
    )
    parser.add_argument("bin", help="明文固件 .bin 文件")
    parser.add_argument("--keys", "-k", help="密钥 JSON 文件 (含 aes_key + hmac_key hex)")
    parser.add_argument("--aes-key", help="AES-256 hex 密钥 (64 hex chars)")
    parser.add_argument("--hmac-key", help="HMAC-SHA256 hex 密钥 (64 hex chars)")
    parser.add_argument("--output", "-o", help="输出 blob 路径 (默认 <bin>_blob.bin)")
    parser.add_argument("--quiet", "-q", action="store_true", help="仅输出文件路径")
    args = parser.parse_args()

    # ---- 加载密钥 ----
    if args.keys:
        aes_key, hmac_key = load_keys_from_file(args.keys)
    elif args.aes_key and args.hmac_key:
        aes_key  = bytes.fromhex(args.aes_key)
        hmac_key = bytes.fromhex(args.hmac_key)
        if len(aes_key) != 32 or len(hmac_key) != 32:
            print("错误: 密钥长度必须为 32 字节 (64 hex chars)", file=sys.stderr)
            sys.exit(1)
    else:
        print("错误: 请指定 --keys 或同时指定 --aes-key + --hmac-key", file=sys.stderr)
        sys.exit(1)

    # ---- 读明文 ----
    with open(args.bin, "rb") as f:
        plaintext = f.read()

    if len(plaintext) == 0:
        print("错误: 固件文件为空", file=sys.stderr)
        sys.exit(1)

    # ---- 加密 ----
    blob, iv, signature = encrypt_firmware(plaintext, aes_key, hmac_key)

    # ---- 输出文件 ----
    output_path = args.output or f"{os.path.splitext(args.bin)[0]}_blob.bin"
    with open(output_path, "wb") as f:
        f.write(blob)

    if args.quiet:
        print(output_path)
        return

    # ---- 打印信息 ----
    print("=" * 60)
    print("  固件加密完成")
    print("=" * 60)
    print()
    print(f"  输入:      {args.bin}")
    print(f"  输出:      {output_path}")
    print(f"  明文大小:  {len(plaintext):,} bytes")
    print(f"  Blob 大小: {len(blob):,} bytes  (+{len(blob)-len(plaintext)} = IV 16 + HMAC 32)")
    print(f"  IV (hex):  {iv.hex()}")
    print(f"  HMAC (hex):{signature.hex()}")
    print()
    print("  【下一步】")
    print(f"  1. 上传 {output_path} 到 OSS")
    print(f"  2. 在管理后台创建固件记录，填入:")
    print(f"     version    = <版本号>")
    print(f"     oss_key    = <OSS 对象路径>")
    print(f"     file_size  = {len(blob)}")
    print(f"     iv         = {iv.hex()}")
    print(f"     hmac       = {signature.hex()}")


if __name__ == "__main__":
    main()
