"""
验证所有 .uvprojx 文件的 XML 合法性和控制字符残留。
CLAUDE.md 规则：任何 .uvprojx 修改后必须运行本脚本。
用法: python verify_uvprojx.py
"""
import xml.etree.ElementTree as ET
import os, sys

base = os.path.dirname(os.path.abspath(__file__))
projects = [
    'bootloader/RVMDK/Bootloader.uvprojx',
    'envir-control/RVMDK/Fire_uCOS.uvprojx',
]

errors = 0
for p in projects:
    path = os.path.join(base, p)
    if not os.path.exists(path):
        print(f'MISSING: {p}')
        errors += 1
        continue

    with open(path, 'rb') as f:
        data = f.read()

    bad = [hex(b) for b in data if b in (0x07, 0x08)]
    if bad:
        print(f'CORRUPT: {p} — {len(bad)} control chars found: {set(bad)}')
        errors += 1
    else:
        print(f'CLEAN:   {p} — 0 control chars')

    try:
        ET.parse(path)
        print(f'VALID:   {p}')
    except ET.ParseError as e:
        print(f'INVALID: {p} — XML parse error: {e}')
        errors += 1

sys.exit(errors)
