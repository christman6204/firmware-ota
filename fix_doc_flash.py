import re

with open('docs/ota-design.md', 'rb') as f:
    d = f.read()

# Show current layout section
idx = d.find(b'0x0800_0000  Bootloader')
end = d.find(b'```', idx + 50)
print("=== CURRENT LAYOUT ===")
print(d[idx:end].decode('latin-1'))

# Find exact pattern to replace - look for the full layout block between ``` markers
# The layout block starts with 0x0800_0000 and ends with the closing ```
# Let's find the opening and closing ``` around it
block_start = d.rfind(b'```', 0, idx)
block_end = d.find(b'```', end) + 3

print(f"\nBlock: {block_start}-{block_end} ({block_end-block_start} bytes)")

# Build new layout
new_layout = b"""```
0x0800_0000  Bootloader   (48KB = 24页)  永不升级，出厂烧录
0x0800_C000  加密ID标记区  (2KB = 1页)   首次上电标记 (0x1234)
0x0800_C800  加密ID区      (2KB = 1页)   UID绑定生成的设备指纹 (16B)
0x0800_D000  保留区1      (12KB = 6页)   预留
0x0801_0000  App          (288KB = 144页) OTA 目标
0x0805_8000  参数区        (96KB = 48页)  状态机 + 版本 + 升级标志
0x0807_0000  保留区2       (64KB = 32页)  预留
```"""

d2 = d[:block_start] + new_layout + d[block_end:]

# Also fix the summary note
old_summary = b'> \xe5\x90\x88\xe8\xae\xa1 48 + 256 + 4 = 308KB'  # 合计 48 + 256 + 4 = 308KB
new_summary = b'> \xe5\x90\x88\xe8\xae\xa1 48+2+2+12+288+96+64 = 512KB\xef\xbc\x8c\xe5\xa1\xab\xe6\xbb\xa1 STM32F103VE \xe7\x89\x87\xe5\x86\x85 flash\xe3\x80\x82App \xe5\x8c\xba\xe4\xb8\x8a\xe9\x99\x90 288KB\xe3\x80\x82\xe5\x8a\xa0\xe5\xaf\x86ID\xe6\xa0\x87\xe8\xae\xb0\xe5\x8c\xba\xe5\x92\x8c\xe5\x8a\xa0\xe5\xaf\x86ID\xe5\x8c\xba\xe7\xb4\xa7\xe6\x8e\xa5 Bootloader \xe4\xb9\x8b\xe5\x90\x8e\xef\xbc\x8c\xe9\x81\xbf\xe5\x85\x8d high-address \xe5\xaf\xbc\xe8\x87\xb4\xe7\x9a\x84\xe5\x9b\xba\xe4\xbb\xb6 hex \xe8\x86\xa8\xe8\x83\x80\xe3\x80\x82'
if old_summary in d2:
    # Find from this marker onwards to the next block marker
    summary_idx = d2.find(old_summary)
    # Look for the end of this line
    line_end = d2.find(b'\n', summary_idx)
    # Also remove the next line if it exists (old second summary line)
    next_line_start = line_end + 1
    next_line_end = d2.find(b'\n', next_line_start)
    d2 = d2[:summary_idx] + new_summary + d2[next_line_end:]
    print("Fixed summary note")
else:
    print("Summary pattern not found, searching for partial...")
    # Check for partial match
    partial = b'48 + 256 + 4 = 308KB'
    if partial in d2:
        print("Found partial summary text")
        # Just search and fix context around it
    else:
        print("No summary text found at all")

with open('docs/ota-design.md', 'wb') as f:
    f.write(d2)

# Verify
with open('docs/ota-design.md', 'rb') as f:
    v = f.read()

print("\n=== VERIFICATION ===")
for addr, desc in [
    (b'0x08010000', 'App start'),
    (b'0x08058000', 'Param addr'),
    (b'0x0800C000', 'UID flag'),
    (b'0x0800C800', 'UID ID'),
    (b'288KB', 'App size'),
    (b'96KB', 'Param size'),
    (b'64KB', 'Reserve2 size'),
]:
    c = v.count(addr)
    print(f'  {"OK" if c else "MISS"} ({c}x): {desc} ({addr.decode()})')

for addr in [b'0x0807F000', b'0x0807F800', b'0x0804C000']:
    c = v.count(addr)
    if c:
        print(f'  STALE! ({c}x): {addr.decode()}')
