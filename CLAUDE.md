# CLAUDE.md

本项目是 **STM32F103VE + 4G模块 OTA 升级 & 数据采集系统**。

完整设计文档：`docs/ota-design.md`（Part A OTA + Part B 数据平台）

## 工程目录结构

```
bootloader/          STM32F103VE 裸机 Bootloader（Keil Bootloader.uvprojx）
envir-control/       uC/OS-III App 工程（Keil Fire_uCOS.uvprojx）
docs/                设计文档 + 实施计划
```

---

## 重要：操作 Keil .uvprojx 文件的规则

**.uvprojx（XML）路径包含反斜杠，禁止用 Python 普通字符串操作——必须全程 `rb` 二进制模式。**

### ❌ 错误示例

```python
# 这些全部会损坏 XML——\a、\b、\f 等被 Python 解释为控制字符
open(p, encoding='utf-8').read()
s.replace('..\\app\\app.c', '..\\src\\app.c')
re.sub(r'..\\app\\(.*)', r'..\\src\\\1', s)
```

### ✅ 正确示例

```python
# 读
with open(p, 'rb') as f:
    data = f.read()

# 替换——bytes 不解析任何转义
data = data.replace(b'..\\app\\app.c', b'..\\src\\app.c')
data = data.replace(b'..\\bsp\\bsp.c', b'..\\src\\bsp.c')

# 写
with open(p, 'wb') as f:
    f.write(data)
```

### 为什么

Python 字符串中 `\a`=BEL(0x07)、`\b`=BS(0x08)、`\f`=FF(0x0C)、`\n`=LF(0x0A)、`\r`=CR(0x0D)、`\t`=TAB(0x09)、`\v`=VT(0x0B)。Windows 路径如 `..\app`、`..\bsp`、`\aes.c` 中的 `\a` 和 `\b` 会被静默转义成不可见的控制字符，表面看起来正常，实际上 XML 已经损坏。

### 验证方法

**修改 .uvprojx 后必须运行验证脚本（最后一步，不可跳过）：**

```bash
python verify_uvprojx.py
# 输出: CLEAN + VALID = 通过。任何 CORRUPT/INVALID = 立即修复。
```

脚本功能：全量扫描所有 .uvprojx 文件，检查①控制字符(BEL 0x07/BS 0x08)残留 ②XML 合法性。只改一个文件也要跑——历史教训表明残留控制字符可能藏在你没改的行里。

### 历史踩坑记录

| 日期 | 文件 | 损坏原因 | 修复方式 |
|---|---|---|---|
| 2026-07-25 | `Fire_uCOS.uvprojx` | `\app`→BEL+pp, `\bsp`→BS+sp | 逐字节替换 0x07→0x5C+0x61 |
| 2026-07-25 | `Bootloader.uvprojx` | `\aes.c`→BEL+es.c | 逐字节替换 + regex 修复 |
