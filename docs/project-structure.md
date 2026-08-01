# 环境监控系统 - 项目结构描述文档

> 版本: v1.0 | 更新: 2026-07-29

---

## 1. 项目概述

面向 10,000 台设备的远程环境监测与调控系统，基于 STM32F103VE + 4G 模块 + 阿里云。

**设备端 6 大功能：**

| # | 功能 | 说明 |
|---|------|------|
| 1 | 环境信息检测 | 8 路 DS18B20 温度采集 |
| 2 | 环境调控 | 10 路电磁阀自动控制（逻辑待定） |
| 3 | 人机交互 | 串口屏(RS232) + 3 按键 + 11 LED + 蜂鸣器 |
| 4 | 本地数据保存 | SD 卡存储，断网断电不丢失，90 天回溯 |
| 5 | 数据上传 | 4G 模块远程上传至后台服务器 |
| 6 | 固件升级 | 网页端 OTA，AES-256-CTR 加密 |

---

## 2. 顶层目录结构

```
514/                                项目根目录
├── CLAUDE.md                       项目指引（.uvprojx 操作铁律、验证脚本）
├── verify_uvprojx.py               .uvprojx 文件完整性验证脚本
├── .gitignore                      Git 忽略规则
│
├── bootloader/                     STM32 Bootloader 工程（裸机，48KB）
├── envir-control/                  STM32 App 工程（uC/OS-III，288KB）
├── tools/                          产线工具 + 加密脚本（Python）
├── docs/                           设计文档
├── Libraries/                      ST 标准外设库（FWlib + CMSIS）
├── uC-OS3/                         uC/OS-III 内核源码（子模块）
└── old/                            归档的旧产品代码 + 参考资料料
```

---

## 3. Bootloader 工程 (`bootloader/`) — Keil: Bootloader.uvprojx

**独立 Keil 工程**，裸机程序（无 RTOS），1 个 Target。

### Flash 分布（Bootloader Target）

```
地址            大小     内容
0x08000000      48KB    Bootloader 代码
  ├─ main.c          入口 + 闪灯诊断
  ├─ boot_main.c     状态机 + 6步升级 + 回滚
  ├─ crypto.c        AES + HMAC（含密钥 @0x0800BF88/BFA8）
  ├─ flash_ext.c     W25Q64 SPI 驱动（SPI1, CS=PA4）
  ├─ flash_int.c     片内 flash 驱动
  ├─ param.c         参数区读写 + CRC32
  ├─ uid_flag.c      UID 标记 0x1234 @0x0800C000
  └─ jump_app.c      App 跳转
```

### 3.1 目录结构

```
bootloader/
├── src/
│   ├── main.c                      入口：BSP_Init + 闪灯诊断 + boot_main()
│   ├── boot_main.c                 ★ 主状态机 + 6步升级流水线 + 回滚
│   ├── boot_main.h
│   ├── crypto.c                    ★ mbedtls 封装：AES-256-CTR + HMAC-SHA256
│   ├── crypto.h                    含 BOOT_MASTER_AES_KEY / HMAC_KEY (__at 编译期)
│   ├── flash_int.c                 片内 flash 驱动（2KB页擦除, 16bit编程）
│   ├── flash_int.h
│   ├── flash_ext.c                 W25Q64 片外 flash 驱动（SPI1）
│   ├── flash_ext.h                 固件头结构体 + 存储布局定义
│   ├── param.c                     参数区读写 + CRC32 校验
│   ├── param.h
│   ├── ota_param.h                 ★ OTA 状态机/结果码宏 + ota_param_t 结构体
│   ├── jump_app.c                  App 跳转（栈/入口/VTOR）
│   ├── jump_app.h
│   ├── uid_flag.c                  ★ UID 标记 0x1234 @0x0800C000（编译期 __at）
│   ├── bsp.c                       PE0 LED + 时钟验证
│   ├── bsp.h
│   ├── stm32f10x_it.c              中断处理
│   └── stm32f10x_it.h
├── RVMDK/
│   ├── Bootloader.uvprojx          Keil 工程（⚠️ 二进制模式操作）
│   └── bootloader.sct              分散加载文件
└── Libraries/mbedtls/              mbedtls V2.28 LTS（独立副本）
```

### 3.2 核心模块说明

| 模块 | 职责 |
|------|------|
| `boot_main.c` | 上电状态机分发 + `boot_upgrade()` 6 步流水线 + `boot_rollback()` 回滚 |
| `crypto.c` | AES-256-CTR 加解密 + HMAC-SHA256 流式验签 + 主密钥存储 |
| `flash_ext.c` | W25Q64 SPI 读写擦（固件区/备份区/固件头） |
| `param.c` | 参数区 magic+CRC32 校验 + 持久化读写 |
| `uid_flag.c` | 编译期放置 0x1234 标记，首次上电触发 UID 绑定 |

### 3.3 Flash 布局（Bootloader + App 全部烧录后的完整系统布局）

```
地址            大小      内容                        来源工程
0x08000000      48KB     Bootloader 代码             Bootloader 工程
  └─ 0x0800BF00  256B    密钥区                      Bootloader 工程
     ├─ @BF88           AES_KEY (32B)
     ├─ @BFA8           HMAC_KEY (32B)
     └─ 其余             随机填充
0x0800C000      2KB      UID 标记 (0x1234)           Bootloader 工程 (uid_flag.c 编译期)
0x0800C800      2KB      加密 ID                     Bootloader 首次上电生成 (HMAC-SHA256)
0x0800F800      128B     APP_INFO                    App 工程 (app_info.c, scatter 定位)
  ├─ @0               fw_version (2B)
  └─ @2               master_device_key (32B)
0x08010000      288KB    App 代码 + 向量表           App 工程
0x08058000      96KB     参数区                      Bootloader 写入 (ota_param_t + dev_id + secret)
0x08070000      64KB     保留                        -
```

> **注意**：此布局是 Bootloader 工程 + App 工程（updata_app target）**全部烧录后**的完整系统 Flash 分布。
> Bootloader 工程只烧录 0x08000000~0x0800BFFF 范围（48KB + 密钥区），
> App 工程（updata_app target）烧录 0x0800F800~0x08057FFF 范围（APP_INFO + App 代码）。
> 两者合并即为出厂烧录镜像（factory.bin），由 factory_tool 生成。

---

## 4. App 工程 (`envir-control/`) — Keil: Fire_uCOS.uvprojx

**独立 Keil 工程**，uC/OS-III 实时操作系统，**2 个 Target**（debug + updata_app）。

### 4.1 两个 Target 的 Flash 分布对比

**Target: debug（调试用，3 段分散加载）**

```
地址            大小      分散加载段          内容
0x08000000      1KB      LR_VECTOR          向量表 (RESET + InRoot)
                                          ↑ 调试时独立运行，无需 Bootloader
0x0800F800      128B     LR_APP_INFO        app_info_t (fw_version + master_device_key)
0x08010000      288KB    LR_IROM1           代码 + RO + XO + RW
```

- VECT_TAB_OFFSET = **0x0000**（向量表在 0x08000000）
- 调试时直接烧录整个工程到芯片，独立运行
- 包含向量表，可脱离 Bootloader 直接运行
- 输出文件：`IL_800_debug.axf`

**Target: updata_app（升级固件用，2 段分散加载）**

```
地址            大小      分散加载段          内容
0x0800F800      128B     LR_APP_INFO        app_info_t (fw_version + master_device_key)
0x08010000      288KB    LR_IROM1           向量表(RESET) + 代码 + RO + XO + RW
                                          ↑ 向量表在 App 代码起始处
```

- VECT_TAB_OFFSET = **0x10000**（向量表在 0x08010000）
- 输出文件：`APP_型号_版本.hex`（factory_tool 加密输入）
- **不含 0x08000000 处的向量表**，由 Bootloader 跳转到此

**两个 Target 的关键区别：**

| 对比项 | debug | updata_app |
|--------|-------|-----------|
| 分散加载段数 | 3 段 | 2 段 |
| 向量表位置 | 0x08000000 (LR_VECTOR) | 0x08010000 (LR_IROM1 内) |
| VECT_TAB_OFFSET | 0x0000 | 0x10000 |
| 是否需要 Bootloader | 否（独立运行） | 是（Bootloader 跳转） |
| 输出文件 | .axf (调试) | .hex (OTA 升级) |
| factory_tool 处理 | 不参与 | 加密 + 合并出厂镜像 |

### 4.1 目录结构

```
envir-control/
├── app/
│   ├── app.c                       ★ 主入口：uid_verify + rtc_init + 任务创建
│   ├── app_cfg.h                   ★ 任务优先级/栈大小定义
│   ├── includes.h                  全局头文件包含链
│   ├── app_info.c                  ★ APP_INFO 结构体（fw_version + master_device_key）
│   ├── app_info.h                  scatter 定位 @0x0800F800
│   ├── device_secret.c             ★ 设备认证 secret 派生（HMAC-SHA256）
│   ├── device_secret.h
│   ├── uid_verify.c                UID 防克隆验证（App 启动时）
│   ├── uid_verify.h
│   ├── rtc.c                       ★ RTC 驱动（LSE + VBAT + BKP + Unix转换）
│   ├── rtc.h
│   ├── sd_spi.c                    ★ SD 卡 SPI2 驱动
│   ├── sd_spi.h
│   ├── sd_storage.c                ★ SD 卡数据存储（4KB缓冲 + 日文件 + 90天清理）
│   ├── sd_storage.h
│   ├── FatFs/                      FatFs R0.16 文件系统
│   │   ├── ff.c                    FatFs 核心
│   │   ├── ff.h
│   │   ├── ffconf.h                FatFs 配置（CODE_PAGE=437, NORTC=0）
│   │   ├── diskio.c                ★ FatFs 磁盘接口（桥接 sd_spi）
│   │   └── diskio.h
│   ├── os_cfg.h                    uC/OS-III 内核配置
│   ├── os_cfg_app.h                uC/OS-III 应用配置
│   ├── os_app_hooks.c              OS 钩子函数
│   ├── os_app_hooks.h
│   ├── cpu_cfg.h                   uC/CPU 配置
│   └── lib_cfg.h                   uC/LIB 配置
├── bsp/
│   ├── bsp.c                       BSP 初始化（PE0 LED + 时钟）
│   └── bsp.h
├── scatter/
│   ├── debug.sct                   ★ 调试 target 分散加载（3段：向量+APP_INFO+App）
│   ├── updata_app.sct              ★ 升级 target 分散加载（2段：APP_INFO+App）
│   └── debug.ini                   调试初始化脚本
├── RVMDK/
│   └── Fire_uCOS.uvprojx           Keil 工程（2 target: debug + updata_app）
├── Libraries/
│   ├── CMSIS/                      Cortex-M3 内核 + 系统时钟
│   ├── FWlib/                      ST 标准外设库（GPIO/SPI/USART/RTC/BKP...）
│   └── mbedtls/                    mbedtls V2.28 LTS（独立副本）
├── uc-cpu/                         uC/CPU 移植层
├── uc-lib/                         uC/LIB 基础库
└── ucos-iii/                       uC/OS-III 内核 + 移植层
```

### 4.2 任务结构

| 任务 | 优先级 | 栈大小 | 状态 | 职责 |
|------|--------|--------|------|------|
| AppTaskStart | 26 | 1088B | ✅ 已实现 | BSP/CPU/SysTick 初始化 |
| AppTaskBlink | 28 | 512B | ✅ 已实现 | PE0 LED 闪烁测试 |
| AppTaskSDStorage | 16 | 1536B | ✅ 已实现 | SD 卡数据采集+存储 |
| AppTaskOTA | 12 | 2048B | 🔲 预留 | OTA 固件接收 |
| AppTaskDataReport | 14 | 1536B | 🔲 预留 | 数据采集+4G 上传 |
| AppTaskMonitor | 18 | 1536B | 🔲 预留 | 实时监控+调控 |

### 4.3 两个 Target

| Target | 用途 | 分散加载 | VECT_TAB_OFFSET |
|--------|------|---------|-----------------|
| `debug` | 调试 | 3段：向量@0x08000000 + APP_INFO@0x0800F800 + App@0x08010000 | 0x0000 |
| `updata_app` | 升级固件 | 2段：APP_INFO@0x0800F800 + App@0x08010000 | 0x10000 |

---

## 5. 产线工具 (`tools/`)

Windows GUI 工具 + 命令行脚本，Python 实现。

### 5.1 文件清单

```
tools/
├── factory_tool.py                 ★ 产线 GUI 工具（tkinter，3 Tab）
├── encrypt_firmware.py             命令行固件加密
├── gen_master_keys.py              生成三把主密钥（AES + HMAC + DeviceKey）
├── gen_device_secret.py            派生设备认证 secret
├── hex_utils.py                    Intel HEX 解析/合并/版本读取库
└── md2docx.py                      Markdown 转 Word 文档
```

### 5.2 factory_tool.py 功能

| Tab | 功能 | 输入 | 输出 |
|-----|------|------|------|
| 加密 | HEX -> 加密 blob | APP_型号_版本.hex | UP_型号_V版本.bin |
| 合并 | BT + APP -> 出厂镜像 | BT*.hex + APP*.hex | FA_型号_V版本.bin/hex |
| 密钥 | 密钥管理（密码锁屏） | - | keys.json |

### 5.3 密钥体系

```
gen_master_keys.py -> keys.json
  ├─ aes_key (32B)           -> bootloader/src/crypto.c (编译进 BT)
  ├─ hmac_key (32B)          -> bootloader/src/crypto.c (编译进 BT)
  └─ master_device_key (32B) -> app/app_info.c (编译进 APP_INFO)

gen_device_secret.py
  └─ secret = HMAC-SHA256(master_device_key, dev_id)[:16]
       -> STM32 首次上电自主计算 (device_secret.c)
       -> 服务器预计算 + 认证时重算比对
```

---

## 6. 设计文档 (`docs/`)

```
docs/
├── ota-design.md                   ★ 主设计文档（v3.0）
│   ├── §1  项目概述（6大功能 + 引脚分配 + 设备命名）
│   ├── §2-4  OTA架构 + 数据库 + API
│   ├── §5-6  MQTT协议 + 前端页面
│   ├── §7  STM32端设计（Flash布局 + Bootloader + OTA流程 + 加密 + 防克隆）
│   ├── §8  4G模块端设计
│   ├── §9  通信协议
│   ├── §10-13  安全 + 部署 + 测试 + 实施计划
│   ├── §14-20  数据采集平台（架构 + TDengine + 数据上报）
│   └── §21-26  监控大盘 + 告警 + 企业级能力
├── ota-design.docx                 Word 版本
└── superpowers/
    ├── specs/                      规格文档
    └── plans/                      实施计划
```

---

## 7. 硬件引脚分配概要

### UART（5路，含重映射）

| UART | 引脚 | 设备名 | 用途 |
|------|------|--------|------|
| USART1 | PA9/PA10 | DBG_TX/RX | 调试 |
| USART2 | PD5/PD6 (remap) | RS485_2_TX/RX | 传感器 RS485 |
| USART3 | PD8/PD9 (full remap) | HMI_TX/RX | RS232 -> 串口屏 |
| UART4 | PC10/PC11 | 4G_TX/RX | 4G 模块 |
| UART5 | PC12/PD2 | RS485_1_TX/RX | 冷机 RS485 |

### SPI（2路）

| SPI | 引脚 | 设备名 | 用途 |
|-----|------|--------|------|
| SPI1 | PA4-7 | FLASH_* | W25Q64 片外 Flash |
| SPI2 | PB11+PB13-15 | SD_* | SD 卡 |

### GPIO 外设

| 类型 | 数量 | 设备名 | 引脚 |
|------|------|--------|------|
| DS18B20 | 8 | T1-T8 | PE8-11, PC4-5, PB0-1 |
| 电磁阀 | 10 | V1-V10 | PD3-4/7, PE0-1, PB5-9 |
| LED | 11 | LED_* | PA0-3, PD12-13, PE4-6/12-13 |
| 按键 | 3 | KEY1-3 | PE14, PE15, PB10 |
| 蜂鸣器 | 1 | BUZZER | PA15 |
| 4G控制 | 3 | 4G_PWR/RST/FACTORY | PC8/7/6 |
| RS485 DE/RE | 2 | RS485_1/2_DE | PC3/PC2 |
| 屏幕电源 | 1 | HMI_PWR | PB12 |

---

## 8. 数据流架构

```
                    ┌──────────────────────────────────────┐
                    │         STM32F103VE (uC/OS-III)       │
                    │                                      │
  T1-T8 ──GPIO──→  │  采集任务 ──→ SD卡存储 (本地备份)     │
  (DS18B20)        │      │                               │
                    │      ├──→ 4G模块 ──→ 云端 (MQTT上传)  │
  V1-V10 ←─GPIO──  │  调控任务 (电磁阀控制, 逻辑待定)       │
  (电磁阀)          │                                      │
                    │  HMI任务 ──→ 串口屏 (RS232 展示)      │
  KEY1-3 ──GPIO──→ │  交互任务 (按键/LED/蜂鸣器)           │
  LED_*  ←─GPIO──  │                                      │
  BUZZER ←─GPIO──  │  OTA任务 ←── 4G模块 ←── 云端 (固件)   │
                    │      │                               │
                    │  Bootloader (AES解密 + HMAC验签)      │
                    └──────────────────────────────────────┘
                              │ SPI1
                    ┌─────────┴────────┐
                    │ W25Q64 片外Flash  │  OTA固件中转 + 备份
                    └──────────────────┘
```

---

## 9. 安全体系

| 层面 | 机制 | 密钥位置 |
|------|------|---------|
| 固件加密 | AES-256-CTR | BT crypto.c @0x0800BF88 |
| 固件签名 | HMAC-SHA256 (verify-before-write) | BT crypto.c @0x0800BFA8 |
| 设备认证 | HMAC-SHA256 派生 secret | App APP_INFO @0x0800F802 |
| 防克隆 | UID 绑定加密ID | BT @0x0800C000/C880 |
| 芯片保护 | RDP Level 1 (SWD不可读) | - |
| 传输安全 | 4G + MQTT TLS | - |

---

## 10. 已实现与待实现

### ✅ 已实现

| 模块 | 文件 | 功能 |
|------|------|------|
| Bootloader | boot_main.c | 6步升级 + 回滚 + UID防克隆 |
| 固件加密 | crypto.c | AES-256-CTR + HMAC-SHA256 |
| 参数区 | param.c | magic+CRC32 校验 + 持久化 |
| APP_INFO | app_info.c | 版本号 + master_device_key |
| 设备认证 | device_secret.c | secret 自主派生 |
| SD卡存储 | sd_storage.c + sd_spi.c + FatFs | 4KB缓冲 + 日文件 + 90天清理 |
| RTC | rtc.c | LSE + VBAT + Unix转换 |
| 产线工具 | factory_tool.py | 3 Tab GUI + 密钥管理 |
| 密钥工具 | gen_master_keys.py + gen_device_secret.py | 三把密钥 + 设备secret |

### 🔲 待实现

| 模块 | 依赖 | 说明 |
|------|------|------|
| DS18B20 驱动 | T1-T8 引脚 | 8路独立1-Wire温度采集 |
| 电磁阀驱动 | V1-V10 引脚 | 10路GPIO控制 + 调控逻辑 |
| 串口屏驱动 | HMI_TX/RX (USART3) | RS232通信 + 界面协议 |
| 4G AT指令 | 4G_TX/RX (UART4) | MQTT/HTTP 透明桥接 |
| RS485驱动 | RS485_1/2 (UART5/USART2) | 双485 + DE/RE方向控制 |
| 按键/LED/蜂鸣器 | KEY1-3, LED_*, BUZZER | 消抖 + 状态指示 + 报警 |
| 数据采集任务 | DS18B20 + 调控逻辑 | AppTaskDataReport |
| 数据上传任务 | 4G AT指令 | AppTaskDataReport |
| 调试CLI | DBG_TX/RX (USART1) | 串口命令行配置 |
| 服务器后端 | FastAPI + Vue3 | 设备管理 + OTA任务 + 数据平台 |
| flash_ext.c CS 更新 | PA4 (原PB0) | Bootloader W25Q64 CS 引脚变更 |
| sd_spi.c CS 更新 | PB11 (原PB12) | SD卡 CS 引脚变更 |
