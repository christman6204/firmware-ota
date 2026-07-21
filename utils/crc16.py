_POLY = 0xA001


def _build_table():
    table = []
    for byte in range(256):
        crc = byte
        for _ in range(8):
            crc = (crc >> 1) ^ _POLY if crc & 1 else crc >> 1
        table.append(crc)
    return table


_TABLE = _build_table()


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for byte in data:
        crc = (crc >> 8) ^ _TABLE[(crc ^ byte) & 0xFF]
    return crc


if __name__ == "__main__":
    # CRC-16/MODBUS check value from the catalog (reveng):
    for sample, expected in [
        (b"", 0xFFFF),
        (b"123456789", 0x4B37),
    ]:
        got = crc16(sample)
        assert got == expected, f"crc16({sample!r}) = 0x{got:04X}, expected 0x{expected:04X}"
    print("ok")
