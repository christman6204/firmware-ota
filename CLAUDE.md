# CLAUDE.md

本项目是 **STM32F103VE + ESP-07S OTA 升级 & 数据采集系统**。

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

修改后必须验证：
```python
import xml.etree.ElementTree as ET
ET.parse('project.uvprojx')  # 抛异常 = 已损坏

# 检查残留控制字符
with open('project.uvprojx', 'rb') as f:
    d = f.read()
bad = sum(1 for b in d if b in (0x07, 0x08))
assert bad == 0, f'{bad} control chars found'
```

### 历史踩坑记录

| 日期 | 文件 | 损坏原因 | 修复方式 |
|---|---|---|---|
| 2026-07-25 | `Fire_uCOS.uvprojx` | `\app`→BEL+pp, `\bsp`→BS+sp | 逐字节替换 0x07→0x5C+0x61 |
| 2026-07-25 | `Bootloader.uvprojx` | `\aes.c`→BEL+es.c | 逐字节替换 + regex 修复 |
