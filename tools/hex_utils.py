"""
Intel HEX 解析 & 合并工具库

Intel HEX 格式:
  :BBAAAATTDDDD...DDCC

  BB = 数据字节数 (1 字节 hex)
  AAAA = 地址 (2 字节 hex, 大端)
  TT = 记录类型:
       00 - 数据
       01 - 文件结束
       02 - 扩展段地址
       04 - 扩展线性地址 (高 16 位)
       05 - 起始线性地址
  DD...DD = 数据 (BB 字节)
  CC = 校验和

支持的项目:
  - STM32F103VE 512KB flash: 0x08000000 - 0x0807FFFF
  - Bootloader: 0x08000000 - 0x0800BFFF (48KB)
  - App:        0x08010000 - 0x08057FFF (288KB)
"""

from dataclasses import dataclass
from typing import List, Tuple


# ============================================================
# Flash 布局常量
# ============================================================

FLASH_BASE      = 0x08000000   # STM32F103VE flash 基址
FLASH_SIZE      = 512 * 1024   # 总大小 512KB
BOOTLOADER_END  = 0x0800BFFF   # Bootloader 结束 (相对偏移 0x0C000-1)
APP_START       = 0x08010000   # App 起始
APP_END         = 0x08057FFF   # App 结束
APP_SIZE        = 288 * 1024   # App 大小 288KB


@dataclass
class HexSegment:
    """一段连续的 flash 数据"""
    address: int    # 绝对地址 (如 0x08000000)
    data: bytes     # 原始字节


# ============================================================
# HEX 解析
# ============================================================

def hex_to_segments(path: str) -> List[HexSegment]:
    """
    解析 Intel HEX 文件，返回有序的不重叠地址-数据段列表。

    处理:
      - 扩展线性地址 (type 04), 用于 STM32 >64KB 地址空间
      - 校验和验证 (不匹配报错)
      - 连续数据自动合并为一段
      - 按地址升序排列
    """
    blocks: List[Tuple[int, bytes]] = []
    ext_linear_addr = 0
    line_no = 0

    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line_no += 1
            line = line.strip()
            if not line or not line.startswith(":"):
                continue

            byte_count = int(line[1:3], 16)
            addr_low   = int(line[3:7], 16)
            rec_type   = int(line[7:9], 16)

            # 校验 checksum
            csum = 0
            for i in range(1, len(line) - 2, 2):
                csum += int(line[i:i+2], 16)
            csum = (-csum) & 0xFF
            if csum != int(line[-2:], 16):
                raise ValueError(
                    f"{path}:{line_no}: checksum error "
                    f"(expected 0x{int(line[-2:], 16):02X}, "
                    f"calculated 0x{csum:02X})"
                )

            if rec_type == 0x00:
                # 数据记录: 地址 = (ext_linear << 16) | addr_low
                abs_addr = (ext_linear_addr << 16) | addr_low
                data = bytes.fromhex(line[9:9 + byte_count * 2])
                blocks.append((abs_addr, data))

            elif rec_type == 0x01:
                # EOF
                break

            elif rec_type == 0x04:
                # 扩展线性地址 (高 16 位)
                ext_linear_addr = int(line[9:9 + byte_count * 2], 16)

            # type 02/03/05 忽略 (ARM Cortex-M 不使用)

    if not blocks:
        raise ValueError(f"{path}: no data records found")

    # 按地址排序
    blocks.sort(key=lambda x: x[0])

    # 合并连续块
    segments: List[HexSegment] = []
    seg_addr = blocks[0][0]
    seg_data = bytearray(blocks[0][1])

    for addr, data in blocks[1:]:
        expected_next = seg_addr + len(seg_data)
        if addr == expected_next:
            seg_data.extend(data)
        else:
            segments.append(HexSegment(address=seg_addr, data=bytes(seg_data)))
            seg_addr = addr
            seg_data = bytearray(data)

    segments.append(HexSegment(address=seg_addr, data=bytes(seg_data)))
    return segments


def hex_to_binary(path: str) -> Tuple[int, bytes]:
    """
    将 Intel HEX 文件转换为单个连续的二进制数据。

    返回 (起始地址, 数据)。
    假设 hex 文件描述的地址空间是连续的。
    """
    segments = hex_to_segments(path)

    if len(segments) == 0:
        raise ValueError(f"{path}: no data to convert")

    start = segments[0].address

    # 检查连续性
    expected = start
    for seg in segments:
        if seg.address != expected:
            raise ValueError(
                f"{path}: address gap at 0x{expected:08X} -> 0x{seg.address:08X}. "
                f"File may contain multiple non-contiguous images."
            )
        expected = seg.address + len(seg.data)

    # 拼接
    total_size = sum(len(s.data) for s in segments)
    result = bytearray(total_size)
    offset = 0
    for seg in segments:
        result[offset:offset + len(seg.data)] = seg.data
        offset += len(seg.data)

    return start, bytes(result)


# ============================================================
# 固件版本读取
# ============================================================

FW_VERSION_ADDR = 0x0800F800   # app_info_t.fw_version 地址 (flash 62KB 偏移)


def read_fw_version_from_hex(path: str) -> tuple:
    """
    从 HEX 文件中读取固件版本号 (地址 0x0800F800, 2 字节)

    返回:
      (major, minor) — 如 (1, 21) 表示 v1.21
      (0, 0) — 地址处无数据 (版本未定义或填充 0xFF)

    格式: 高 8 位 = 主版本, 低 8 位 = 次版本
    """
    import struct

    segments = hex_to_segments(path)
    for seg in segments:
        if seg.address <= FW_VERSION_ADDR < seg.address + len(seg.data):
            offset = FW_VERSION_ADDR - seg.address
            val = struct.unpack_from("<H", seg.data, offset)[0]
            major = (val >> 8) & 0xFF
            minor = val & 0xFF
            return major, minor

    return 0, 0


# ============================================================
# HEX 合并 → factory.bin
# ============================================================

def merge_hex_to_hex(
    bootloader_hex: str,
    app_hex: str,
    output_hex: str,
) -> int:
    """
    合并 bootloader.hex + app.hex → 单个 HEX 文件（最小体积，跳过间隙）

    HEX 格式原生支持非连续地址段，因此无需填充间隙。
    适合产线烧录工具（ST-Link / J-Flash 均支持多段 HEX）。

    返回:
      合并后的数据段数量
    """
    bl_segments = hex_to_segments(bootloader_hex)
    app_segments = hex_to_segments(app_hex)

    all_segments = bl_segments + app_segments
    all_segments.sort(key=lambda s: s.address)

    with open(output_hex, "w") as f:
        current_ext_addr = -1

        for seg in all_segments:
            pos = 0
            while pos < len(seg.data):
                addr = seg.address + pos
                addr_hi = (addr >> 16) & 0xFFFF
                addr_lo = addr & 0xFFFF

                # 写扩展线性地址（地址高 16 位变化时）
                if addr_hi != current_ext_addr:
                    line = f"02000004{addr_hi:04X}"
                    csum = (-sum(int(line[i:i+2], 16) for i in range(0, len(line), 2))) & 0xFF
                    f.write(f":{line}{csum:02X}\n")
                    current_ext_addr = addr_hi

                # 每行最多 16 字节
                chunk_len = min(16, len(seg.data) - pos)
                chunk = seg.data[pos:pos + chunk_len]
                pos += chunk_len

                line = f"{chunk_len:02X}{addr_lo:04X}00{chunk.hex().upper()}"
                csum = (-sum(int(line[i:i+2], 16) for i in range(0, len(line), 2))) & 0xFF
                f.write(f":{line}{csum:02X}\n")

        # EOF
        f.write(":00000001FF\n")

    return len(all_segments)


def merge_hex_files(
    bootloader_hex: str,
    app_hex: str,
    output_bin: str,
    fill_byte: int = 0xFF,
    full_flash: bool = False,
) -> Tuple[int, List[str]]:
    """
    合并 bootloader.hex + app.hex → factory.bin

    factory.bin 布局 (full_flash=False, ~352KB):
      Offset      Size    Content
      0x00000     48KB    Bootloader
      0x0C000     16KB    填充 (加密ID区 + 保留1)
      0x10000     288KB   App

    factory.bin 布局 (full_flash=True, 512KB):
      ...同上... + 160KB 填充 (参数区 + 保留2) 到 0x0807FFFF

    参数:
      bootloader_hex:  bootloader hex 文件路径
      app_hex:         app hex 文件路径
      output_bin:      输出 factory.bin 路径
      fill_byte:       填充字节 (默认 0xFF = flash 擦除态)
      full_flash:      默认 False = 仅到 App 结束; True = 完整 512KB

    返回:
      (文件大小, 填充区域描述列表)
    """
    bl_segments = hex_to_segments(bootloader_hex)
    app_segments = hex_to_segments(app_hex)

    bl_start = min(s.address for s in bl_segments)
    bl_end   = max(s.address + len(s.data) - 1 for s in bl_segments)
    app_start = min(s.address for s in app_segments)
    app_end   = max(s.address + len(s.data) - 1 for s in app_segments)

    # 有效性检查
    if bl_start != FLASH_BASE:
        raise ValueError(
            f"Bootloader start address 0x{bl_start:08X} != 0x{FLASH_BASE:08X}. "
            f"Check Bootloader project settings."
        )

    if app_start < 0x0800F000:
        raise ValueError(
            f"App data starts too low: 0x{app_start:08X}. "
            f"Check VECT_TAB_OFFSET."
        )

    # 确定镜像范围
    if full_flash:
        image_end = FLASH_BASE + FLASH_SIZE - 1   # 0x0807FFFF
    else:
        image_end = max(bl_end, app_end)

    image_size = image_end - FLASH_BASE + 1

    # 创建缓冲区, 全部填充 fill_byte
    buffer = bytearray([fill_byte] * image_size)

    # 写入 bootloader 数据
    for seg in bl_segments:
        offset = seg.address - FLASH_BASE
        buffer[offset:offset + len(seg.data)] = seg.data

    # 写入 app 数据
    for seg in app_segments:
        offset = seg.address - FLASH_BASE
        buffer[offset:offset + len(seg.data)] = seg.data

    # 统计填充区域
    gap_descs = []
    gaps = [
        (0x0C000, 0x0FFFF, "EncryptedID + Reserved1"),
    ]
    if full_flash:
        gaps.append((0x58000, 0x7FFFF, "Parameters + Reserved2"))

    for gap_start, gap_end, desc in gaps:
        actual_end = min(gap_end, image_size - 1)
        if actual_end >= gap_start:
            gap_descs.append(f"{desc} ({gap_end - gap_start + 1} bytes @ 0x{gap_start:05X})")

    # 写入文件
    with open(output_bin, "wb") as f:
        f.write(buffer)

    return len(buffer), gap_descs


def verify_merged_bin(path: str) -> dict:
    """
    验证 factory.bin 内容完整性。

    返回 dict:
      size, bl_size, app_size, bl_empty, app_empty, gaps_filled
    """
    with open(path, "rb") as f:
        data = f.read()

    info = {
        "size": len(data),
        "bl_size": BOOTLOADER_END - FLASH_BASE + 1,
        "app_size": APP_SIZE,
    }

    # Bootloader 非空检查 (前 256B)
    bl_data = data[0:min(256, len(data))]
    info["bl_empty"] = all(b == 0xFF for b in bl_data)

    # App 非空检查
    app_offset = APP_START - FLASH_BASE
    if len(data) > app_offset + 16:
        app_data = data[app_offset:app_offset + min(256, len(data) - app_offset)]
        info["app_empty"] = all(b == 0xFF for b in app_data)
    else:
        info["app_empty"] = True  # App 区域不在镜像范围内

    # 间隙填充检查 (加密ID区)
    gap_start = 0x0C000
    gap_end   = min(0x10000, len(data))
    gap_data  = data[gap_start:gap_end]
    info["gaps_filled"] = all(b == 0xFF for b in gap_data)

    return info
