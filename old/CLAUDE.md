# CLAUDE.md

本项目是 **STM32 + ESP-07S OTA 升级 & 数据采集系统**。本文件是项目开发指引，所有方案推荐、代码实现必须遵循此处约束。

完整设计文档：`./docs/ota-design.md`（必读，Part A OTA 升级 + Part B 数据平台，含架构图、数据库、API、协议、流程详解）

---

## 1. 项目概述

为 **10,000 台** STM32 嵌入式设备提供远程固件升级 + 数据采集能力：

- **设备端**：STM32F1 主控 + ESP-07S WiFi 透传模块（透明桥接，不做业务处理）
- **云端**：阿里云 ECS + RDS MySQL + OSS + EMQX MQTT broker
- **管理后台**：Vue3 + Element Plus
- **后端服务**：Python FastAPI

---

## 2. 核心架构决策（不可推翻）

以下决策已经用户确认，**禁止重新讨论或建议替代方案**：

### 通信
- MQTT 推送升级通知 + HTTP 下载固件 bin
- 不用阿里云 IoT 平台托管

### ESP-07S 角色：纯管道
- HTTP 流 -> UART 流式转发，**不缓存完整固件到 ESP-07S SPI flash**
- 读完 1KB 块等 ACK 再读下一块（流控）

### STM32 App 在线接收
- App 运行时通过 UART DMA 流式接收，写**片外 SPI flash**
- **不进入 Bootloader 接收**，业务中断时间最短
- 收完整 + 收到升级命令才重启进 Bootloader

### 片外 SPI flash 中转
- 分区：新固件头 + 新固件区 + 备份固件头 + 备份固件区
- 备份区用 magic 标记防断电重入

### Bootloader 集中升级职责
- 备份 + 写入 + 回滚 全在 Bootloader
- App 不参与 flash 备份逻辑（只写标志 + 软复位）
- Bootloader 不处理 UART 协议

### 加密设计
- AES-256-CTR + HMAC-SHA256（mbedtls 软件实现）
- 所有设备共用主密钥（AES key 32B + HMAC key 32B，共 64B）
- 主密钥硬编码在 Bootloader 内部，App 不持有
- 启用 STM32 RDP Level 1 读保护
- 片外 flash 始终存密文，片内 App 区明文运行
- 密文格式：`[IV(16B)] + [密文] + [HMAC(32B)]`

---

## 3. 关键约束（违反即为错误）

Claude 在推荐方案或写代码时，**禁止**以下行为：

1. ❌ 不要建议 ESP-07S 缓存完整固件
2. ❌ 不要建议 Bootloader 直接处理 UART 协议
3. ❌ 不要建议每设备一密钥 / OTP 存储密钥
4. ❌ 不要建议省略片外 flash 备份区
5. ❌ 不要建议用 MD5 替代 HMAC（加密后必须用 HMAC-SHA256）
6. ❌ 不要建议用同一密钥做 AES 和 HMAC（密钥分离原则）
7. ❌ 不要建议把 STM32F1 换成 F4/F7/H7 等其他型号
8. ❌ 不要建议 Bootloader 参与 OTA 升级（Bootloader 只能通过 SWD 烧录）
9. ❌ 不要建议省略 magic 标记（断电重入保护必备）
10. ❌ 不要建议用 SQLite 替代 MySQL（设备规模 >100 台）
11. ❌ 不要建议用 CBC/GCM 等其他加密模式替代 CTR
12. ❌ 不要建议 RDP Level 2（不可降级，过于激进）

---

## 4. 技术栈

| 层 | 选型 |
|---|---|
| 主控 MCU | STM32F1（裸机/RTOS，固件 < 256KB） |
| WiFi 模块 | ESP-07S (ESP8266)，Arduino 框架 + PlatformIO |
| 主控 ↔ WiFi | UART 460800/921600 bps，DMA + 空闲中断 |
| 片外 flash | W25Qxx 系列（如 W25Q64 8MB），SPI 接口 |
| 加密库 | mbedtls（AES-256-CTR + HMAC-SHA256） |
| 后端 | Python 3.10+ / FastAPI / SQLAlchemy 2.0 async / Pydantic v2 |
| 数据库 | MySQL 8.x（阿里云 RDS） |
| MQTT | EMQX 开源版，paho-mqtt 客户端 |
| 对象存储 | 阿里云 OSS 私有 bucket |
| 前端 | Vue3 + Element Plus + Vite + TypeScript + Pinia |
| 部署 | 阿里云 ECS + Nginx + systemd |

---

## 5. Flash 布局

### STM32 片内 flash
```
0x0800_0000  Bootloader  (32KB)   永不升级
0x0800_8000  App         (224KB)  OTA 目标
0x0803_F000  参数区      (4KB)    状态机 + 版本 + 升级标志
```

### 片外 SPI flash (W25Q64)
```
0x00_0000  新固件头     (4KB)    magic + size + IV + HMAC + version + receive_offset
0x00_1000  新固件区     (256KB)  密文存储
0x04_1000  备份固件头   (4KB)    backup_magic + size + IV + HMAC + version
0x04_2000  备份固件区   (252KB)  密文存储
0x08_2000  保留
```

---

## 6. 通信协议（ESP-07S ↔ STM32）

帧格式：`[0xAA][0x55][LEN_HI][LEN_LO][CMD][DATA...][CRC16]`
- CRC16-Modbus，覆盖 LEN + CMD + DATA
- UART 速率：460800 或 921600 bps

完整命令表见 `./docs/ota-design.md` §9。

---

## 7. 代码规范

### STM32 端（C 语言）
- HAL 库 + CubeMX 生成底层
- 命名：snake_case（函数 `ota_rx_task`，变量 `firmware_size`）
- 模块化：每个功能模块独立 .c/.h
- 关键文件预期：
  - `stm32-bootloader/src/main.c` - Bootloader 主流程
  - `stm32-bootloader/src/flash_helper.c` - 片内 flash 操作
  - `stm32-bootloader/src/spiflash.c` - 片外 flash 驱动
  - `stm32-bootloader/src/crypto.c` - AES/HMAC 加解密
  - `stm32-app/src/main.c` - App 主循环
  - `stm32-app/src/ota_task.c` - OTA 后台任务
  - `stm32-app/src/uart_protocol.c` - UART 协议解析

### ESP-07S 端（C++ Arduino）
- Arduino 框架 + PlatformIO 构建
- 命名：camelCase（函数 `sendFrame`，变量 `totalSize`）
- 关键文件预期：
  - `esp07s/src/main.cpp` - 主入口
  - `esp07s/src/mqtt_client.cpp` - MQTT 订阅/发布
  - `esp07s/src/http_downloader.cpp` - HTTP 流式下载
  - `esp07s/src/uart_transport.cpp` - UART 帧收发

### 后端（Python）
- Python 3.10+，类型注解必备
- FastAPI + SQLAlchemy 2.0 (async) + Pydantic v2
- 命名：snake_case（函数 `create_ota_task`，变量 `device_id`）
- 关键模块：
  - `backend/app/main.py` - FastAPI 入口
  - `backend/app/models/` - SQLAlchemy 模型
  - `backend/app/schemas/` - Pydantic schema
  - `backend/app/api/` - 路由
  - `backend/app/services/` - 业务逻辑（含加密下发）
  - `backend/app/mqtt/` - MQTT publisher
  - `backend/app/crypto/` - AES/HMAC 加密工具

### 前端（Vue3 + TypeScript）
- Composition API + `<script setup>`
- 命名：组件 PascalCase，变量 camelCase
- 关键目录：
  - `frontend/src/views/` - 页面
  - `frontend/src/api/` - API 调用
  - `frontend/src/components/` - 组件
  - `frontend/src/stores/` - Pinia store

---

## 8. 开发流程

### 推荐实现顺序
1. **STM32 Bootloader** 先行（最硬核，独立可测，用 PC 串口工具模拟）
2. **STM32 App OTA 模块**（用 PC 模拟 ESP-07S）
3. **ESP-07S 流式转发**（用 PC 起 HTTP 服务模拟服务器）
4. **后端 MVP**（FastAPI 单文件跑通端到端）
5. **前端 MVP**（单页面，上传 + 触发升级）
6. **联调**（三端打通）
7. **补企业级能力**（分组/批次/灰度/Dashboard/回滚）

### 测试策略
- STM32 端：PC 串口工具模拟 ESP-07S，验证 UART 协议 + flash 读写 + 加解密
- ESP-07S 端：PC 起 HTTP 服务返回测试 bin，验证流式转发 + 断点续传
- 后端：pytest 单元测试 + httpx 集成测试
- 前端：Vitest 单元测试 + Playwright E2E
- 端到端：1 台真实设备验证完整链路

### 安全检查清单（每次提交前确认）
- [ ] STM32 RDP Level 1 已启用
- [ ] AES key 和 HMAC key 分离，不共用
- [ ] 主密钥不在 App 区，只在 Bootloader
- [ ] 片外 flash 写入的是密文（不是明文）
- [ ] HTTPS 下载（非 HTTP）
- [ ] OSS 私有 bucket + 一次性签名 URL
- [ ] 一机一密设备认证（MQTT/HTTP）
- [ ] 状态机升级标志在每次状态转移时先写后校验

---

## 9. 关键风险对策

| 风险 | 对策 |
|---|---|
| 升级中途断电 | 状态机 + magic 标记 + 备份区回滚 |
| 新 App 启动失败 | Bootloader 30s 内未收 app_healthy 则回滚 |
| UART 误码 | CRC16 + 块级 ACK + 重传 3 次 |
| HTTP 中断 | Range 续传，从 STM32 查 receive_offset |
| 主密钥泄露 | RDP Level 1 保护 + 接受单点风险 |
| 片外 flash 寿命 | 按 4KB 扇区擦除，远低于 10 万次限制 |
| Bootloader 自身砖机 | 不参与 OTA，只 SWD 烧录；预留 SWD 测试点 |
| ESP-07S 与 STM32 复位时序 | ESP-07S 通过 GPIO 控制 STM32 RESET |

---

## 10. 文档与上下文

- `./docs/ota-design.md` - 完整设计文档（架构图、数据库 schema、API、MQTT 主题、协议、流程、风险）
- Claude memory 系统已记录用户偏好，跨会话自动加载：
  - `feedback_ota_architecture.md` - 流式转发 + App 接收 + 片外 flash（架构偏好）
  - `feedback_ota_encryption.md` - AES-256-CTR + HMAC + 共用主密钥（加密偏好）
  - `project_ota.md` - 项目架构选型
  - `user_role.md` - 用户是嵌入式工程师

---

## 11. 开发命令（待补充）

项目尚未初始化，以下命令待代码骨架建立后补充：

```bash
# STM32 Bootloader
cd stm32-bootloader && make

# STM32 App
cd stm32-app && make

# ESP-07S
cd esp07s && pio run

# 后端
cd backend && uvicorn app.main:app --reload --host 0.0.0.0 --port 8000

# 前端
cd frontend && pnpm dev
```

---

## 12. 协作偏好

- 用户是嵌入式工程师，熟悉 C/STM32，对 Web 前后端非专家
- 解释 Web 概念时类比嵌入式场景
- 代码示例需注明属于哪个模块、文件路径
- 涉及密码学/安全设计时，给出"为什么"而非仅"怎么做"
- 重大设计调整前先确认，不要擅自重构已确认的架构

---

本文件随项目演进持续更新。新增模块时补充对应规范。
