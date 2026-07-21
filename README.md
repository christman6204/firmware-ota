# STM32 + ESP-07S 远程 OTA 固件升级系统

为 >100 台 STM32 嵌入式设备提供安全的远程固件升级（OTA）能力。

## 架构概览

```
┌──────────┐  HTTPS    ┌──────────────┐  MQTT     ┌─────────────────┐
│ Vue3 前端 │ ───────→ │ FastAPI 后端  │ ────────→ │ ESP-07S (管道)   │
│ 管理后台  │          │ (阿里云 ECS) │           │ ↓ HTTP 流→UART 流│
└──────────┘          └──┬───┬───┬──┘           │ STM32 MCU        │
                         │   │   │              │ ↓ SPI            │
                    ┌────▼─┐ ┌▼─┐ └──────┐       │ 片外 SPI flash   │
                    │ MySQL │ │OSS│  EMQX  │      └─────────────────┘
                    │ (RDS) │ └──┘  Broker│
                    └───────┘             │
                                          └──── MQTT 推送升级通知
```

## 核心特性

| 特性 | 方案 |
|---|---|
| 通信模式 | MQTT 推送升级指令 + HTTP 下载固件 |
| 固件传输 | ESP-07S 流式转发（不缓存），UART DMA 到 STM32 |
| 固件存储 | STM32 片外 SPI flash（新固件区 + 备份区） |
| 加密防泄露 | AES-256-CTR + HMAC-SHA256，片外存密文 |
| 升级执行 | Bootloader 集中负责（备份 + 解密写入 + 回滚） |
| 断电保护 | 状态机 + magic 标记自动恢复 |
| 灰度发布 | 批次控制 + 可暂停/继续 + 失败率阈值自动暂停 |

## 技术栈

| 层 | 选型 |
|---|---|
| 主控 MCU | STM32F1（裸机/RTOS，固件 < 256KB） |
| WiFi 模块 | ESP-07S (ESP8266) + Arduino + PlatformIO |
| 主控 ↔ WiFi | UART 460800/921600 bps，DMA + 空闲中断 |
| 片外 flash | W25Qxx 系列，SPI 接口 |
| 加密库 | mbedtls（AES-256-CTR + HMAC-SHA256） |
| 后端 | Python FastAPI + SQLAlchemy 2.0 async + Pydantic v2 |
| 数据库 | MySQL 8.x（阿里云 RDS） |
| MQTT | EMQX 开源版 |
| 对象存储 | 阿里云 OSS 私有 bucket |
| 前端 | Vue3 + Element Plus + Vite + TypeScript + Pinia |
| 部署 | 阿里云 ECS + Nginx + systemd |

## 项目结构

```
firmware-ota/
├── README.md              # 项目说明（本文件）
├── CLAUDE.md              # AI 开发指引 + 架构约束
├── docs/
│   └── ota-design.md      # 完整设计文档
├── utils/
│   ├── crc16.c / crc16.py # CRC16-Modbus（UART 协议）
│   └── crc32.c / crc32.py # CRC32
└── .gitignore
```

> **状态**：设计阶段完成，代码开发待启动。详见 `docs/ota-design.md` §13 下一步可选起点。

## 设计文档

完整设计请阅读 [`docs/ota-design.md`](docs/ota-design.md)，包含：

- 数据库 schema（devices / firmwares / ota_tasks / ota_task_records）
- 后端 API 设计（管理端 + 设备端）
- MQTT 主题设计
- STM32 Flash 布局 & Bootloader 流程
- ESP-07S ↔ STM32 通信协议（10 条命令）
- 固件加密方案（AES-256-CTR + HMAC-SHA256）
- 安全 & 风险对策
- 工作量估算 & 推荐实现顺序

## 开发计划

1. STM32 Bootloader → 2. App OTA 模块 → 3. ESP-07S 端 → 4. 后端 MVP → 5. 前端 MVP → 6. 联调 → 7. 企业级能力

## License

MIT
