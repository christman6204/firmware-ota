_POLY = 0xEDB88320


def _build_table():
    table = []
    for byte in range(256):
        crc = byte
        for _ in range(8):
            crc = (crc >> 1) ^ _POLY if crc & 1 else crc >> 1
        table.append(crc)
    return table


_TABLE = _build_table()


def crc32(data: bytes, crc: int = 0) -> int:
    crc ^= 0xFFFFFFFF
    for byte in data:
        crc = (crc >> 8) ^ _TABLE[(crc ^ byte) & 0xFF]
    return crc ^ 0xFFFFFFFF


if __name__ == "__main__":
    import zlib
    for sample in [b"", b"abc", b"hello world", b"\x00\x01\x02\xff"]:
        assert crc32(sample) == zlib.crc32(sample)
    print("ok")
