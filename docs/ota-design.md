# STM32 + ESP-07S OTA 升级 & 数据采集系统 — 设计文档

> 版本: v2.0 | 更新: 2026-07-23 | 状态: 设计阶段完成

**本文档覆盖两大子系统：**
- **Part A（§1-13）：OTA 固件升级系统** — 远程固件下发、加密传输、Bootloader 安全升级
- **Part B（§14-26）：数据采集平台** — 10,000 节点遥测上报、时序存储、监控大盘

---

## 1. 项目概述

### 1.1 基本信息

| 项目 | 说明 |
|---|---|
| 主控 MCU | STM32F1（裸机/RTOS，片内 flash 256KB，App 区 220KB，含 RTC + VBAT 后备） |
| WiFi 模块 | ESP-07S (ESP8266)，**透明数据桥接**（UART ↔ MQTT/HTTP 双向转换，不做业务处理、不缓存数据） |
| 主控 ↔ WiFi | UART 连接（460800 或 921600 bps，DMA + 空闲中断） |
| 云端 | 阿里云 ECS + RDS MySQL + OSS + EMQX |
| 设备规模 | **10,000 台** |
| 后端 | Python FastAPI + SQLAlchemy 2.0 async |
| 前端 | Vue3 + Element Plus + Vite + TypeScript |
| 时序数据库 | TDengine（ECS 自建，开源版） |

### 1.2 两大子系统

| 子系统 | 说明 | 详见 |
|---|---|---|
| **OTA 固件升级** | 远程下发加密固件，ESP-07S 流式转发，STM32 Bootloader 安全升级，支持断点续传/回滚/灰度 | Part A（§2-13） |
| **数据采集平台** | 10,000 节点每 5 秒上报遥测数据（环境量+电气量+开关量），TDengine 时序存储，监控大盘+历史曲线+告警 | Part B（§14-26） |

### 1.3 安全要求

- 固件加密：AES-256-CTR + HMAC-SHA256，片外 flash 存密文，Bootloader 加解密
- 传输安全：HTTPS + MQTT TLS + JWT
- 芯片保护：STM32 RDP Level 1 读保护
- 设备认证：一机一密（MQTT/HTTP）

---

## 2. OTA 系统架构

> 数据平台架构见 Part B §15。两套系统共用 ECS / EMQX / MySQL / Vue3 基础设施。

```
┌──────────────┐   HTTPS      ┌─────────────────────────────┐
│  Vue3 前端   │ ───────────-> │  FastAPI 后端 (ECS)         │
│  管理后台    │              │  ├ REST API                  │
└──────────────┘              │  ├ 任务调度                  │
                              │  └ MQTT Publisher            │
                              └──┬──────────┬──────────┬─────┘
                                 │          │          │
                          ┌──────▼───┐ ┌────▼────┐ ┌───▼──────┐
                          │ RDS      │ │  OSS    │ │ MQTT     │
                          │ MySQL    │ │ 固件存储│ │ Broker   │
                          └──────────┘ └─────────┘ │(EMQX)    │
                                                    └────┬─────┘
                                                         │ MQTT
                                            ┌────────────┴──────────┐
                                            │  ESP-07S (透明桥接)    │
                                            │   ↓ HTTP 流 → UART 流  │
                                            │  STM32 主控            │
                                            │   ↓ SPI                │
                                            │  片外 SPI flash        │
                                            └───────────────────────┘
```

### 关键设计选择

1. **ESP-07S 流式转发**（不缓存完整固件）：HTTP 流 → UART 流，纯管道模式
2. **STM32 App 在线接收**：运行时流式写片外 SPI flash，业务中断时间最短
3. **片外 SPI flash 中转**：新固件区 + 备份区 + 固件头，支持回滚和断点续传
4. **Bootloader 集中升级职责**：备份 + 写入 + 回滚都在 Bootloader，App 只收固件 + 触发复位（职责单一，App 代码更简单）
5. **固件加密防泄露**：AES-256-CTR + HMAC-SHA256，片外 flash 存密文，Bootloader 加解密，所有设备共用主密钥

### 4 阶段 OTA 流程

```
[阶段1: App 在线接收固件]
服务器 ──HTTPS 流──> ESP-07S ──UART 流──> STM32 App ──SPI──> 片外 flash
                                                              ↓
                                                    收完 + 大小/块计数校验
                                                              ↓
                                                     状态: downloaded

[阶段2: 触发升级]
ESP-07S 发"立即升级" -> App 写标志 -> 软复位（不做备份，备份由 Bootloader 负责）

[阶段3: Bootloader 刷写]
Bootloader 读标志 -> 备份片内 App 到片外备份区 -> 擦片内 -> 从片外新固件区解密写片内 -> HMAC-SHA256 校验 -> 跳转

[阶段4: 启动确认]
新 App 写"app_healthy" -> 通过 ESP-07S 上报结果
未 healthy 超时 -> Bootloader 下次上电从备份区回滚
```

---

## 3. 数据库设计 (MySQL)

### `devices` 表（OTA + 数据平台共用）

> **设备标识**：`dev_id`（uint32）是全链路唯一的**机器标识**，由 STM32 出厂烧录，UART / MQTT topic / TDengine TAG / API 全部用它。`sn` 仅供人工识别（机身标签、UI 搜索），不参与任何协议。

| 字段 | 类型 | 说明 |
|---|---|---|
| `dev_id` | INT UNSIGNED PK | 设备 ID（uint32，STM32 出厂烧录，全局唯一） |
| `sn` | VARCHAR(64) | 出厂序列号/MAC（人工识别用，可空，不参与协议） |
| `name` | VARCHAR(128) | 设备名 |
| `group_id` | INT | 分组 ID |
| `location` | VARCHAR(128) | 安装位置 |
| `mcu_version` | VARCHAR(32) | STM32 固件版本 |
| `esp_version` | VARCHAR(32) | ESP-07S 固件版本（预留） |
| `bootloader_version` | VARCHAR(32) | Bootloader 版本（只读） |
| `last_seen` | DATETIME | 最后在线时间（EMQX 规则引擎实时更新） |
| `status` | ENUM | online/offline/upgrading |
| `created_at` | DATETIME | 注册时间 |

### `device_groups` 表

| 字段 | 说明 |
|---|---|
| `id`, `name`, `rule`(JSON) | 分组规则（版本/地域过滤） |

### `firmwares` 表

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | INT PK | |
| `target` | ENUM('mcu','esp') | 目标芯片 |
| `version` | VARCHAR(32) | 版本号 |
| `oss_key` | VARCHAR(256) | OSS 对象 key |
| `file_size` | INT | 字节数 |
| `iv` | BINARY(16) | AES-CTR 初始向量（每版本发布时随机生成一次；仅服务端记录/审计，不下发设备） |
| `hmac` | BINARY(32) | HMAC-SHA256（覆盖 IV + 密文；仅服务端记录/审计，不下发设备） |
| `release_notes` | TEXT | 发布说明 |
| `status` | ENUM | draft/released/archived |
| `created_at` | DATETIME | |

> 说明（方案 A + 每版本加密一次）：固件**发布时加密一次**，密文 blob（`IV+密文+HMAC`）存 OSS，`oss_key`/`iv`/`hmac` 按版本记入本表；同一版本的所有 OTA 任务复用此 blob，不重复加密。设备端从 blob 固定偏移读取 IV/HMAC，不依赖 MySQL 这两列——此处两列仅供服务端审计/重算校验。

### `ota_tasks` 表

| 字段 | 说明 |
|---|---|
| `id`, `firmware_id`, `name` | |
| `strategy` | ENUM('all','group','partial') |
| `target_group_id`, `target_devices`(JSON) | |
| `batch_size`, `batch_interval_sec` | 批次控制 |
| `status` | ENUM('pending','running','paused','completed','failed') |
| `created_at` | |

### `ota_task_records` 表（核心可观测表）

| 字段 | 类型 | 说明 |
|---|---|---|
| `id`, `task_id`, `dev_id` | | |
| `status` | ENUM | pending/notified/downloading/downloaded/upgrade_requested/upgrading/success/failed |
| `download_offset` | INT | 已下载字节数（断点续传可视化） |
| `upgrade_phase` | VARCHAR(32) | downloading/upgrading/rollback |
| `progress` | INT(0-100) | |
| `error_msg` | TEXT | |
| `started_at`, `finished_at` | DATETIME | |

---

## 4. 后端 API 设计

### 前端管理接口

```
# 固件管理
POST   /api/v1/firmwares              # 上传 bin (multipart)
GET    /api/v1/firmwares              # 列表
POST   /api/v1/firmwares/{id}/release # 发布（触发一次性加密：生成 IV、产出密文 blob 存 OSS，见 §7.4）
DELETE /api/v1/firmwares/{id}

# 设备管理
GET    /api/v1/devices                # 列表（按 version/group/status 过滤）
GET    /api/v1/devices/{id}           # 详情（含升级历史）
POST   /api/v1/devices/{id}/group     # 调整分组

# OTA 任务
POST   /api/v1/ota/tasks              # 创建任务（含策略）
GET    /api/v1/ota/tasks              # 列表
GET    /api/v1/ota/tasks/{id}         # 详情（含每设备进度）
POST   /api/v1/ota/tasks/{id}/pause   # 暂停
POST   /api/v1/ota/tasks/{id}/cancel  # 取消
GET    /api/v1/ota/stats              # Dashboard 统计
```

### 设备端接口

```
POST   /api/v1/device/register        # 首次上线注册（一机一密）
POST   /api/v1/device/heartbeat       # 心跳上报（HTTP 兜底；主心跳走 MQTT，见下注）
GET    /api/v1/device/ota/check       # 主动检查更新（MQTT 丢失兜底，返回签名 URL）
POST   /api/v1/device/ota/report      # 上报升级结果
```

> **心跳主路径是 MQTT，不是 HTTP**：STM32 每 60s 经 UART CMD 0x0A 发心跳 → ESP-07S publish 到 `device/heartbeat/{dev_id}`（§5）→ EMQX 规则引擎更新 `devices.last_seen`（§15.3）。`POST /api/v1/device/heartbeat` 仅作 MQTT 长时间不可用时的 HTTP 兜底上报，两者更新的是同一个 `last_seen` 字段，不重复计时。

> **固件下载（直连 OSS）**：固件字节流**不经过 FastAPI**。后端生成 OSS 一次性签名 URL（5min 过期）随 MQTT 下发（或在 `/ota/check` 返回），ESP-07S 直接从 OSS 拉流，OSS 原生支持 Range 断点续传。FastAPI 不经手固件内容，仅负责签发 URL 与接收结果上报，避免万级并发下载压垮单台 ECS。（注：此处"直连 OSS"是下载路径决策，与 §7.1 的"方案 A"blob 存储约定是两件独立的事。）

### 鉴权

- 设备端：一机一密（`dev_id` + `secret` 烧录到 flash），MQTT username/password 用之
- 前端：JWT

---

## 5. MQTT 主题设计

```
ota/cmd/{dev_id}                # 下发升级指令（单设备）
  payload: {"task_id":"...","target":"mcu","version":"1.2.0",
            "url":"https://oss.../fw.bin?token=...","size":123456}
  # IV/HMAC 在密文 blob 内（方案 A），不下发；设备端从 blob 固定偏移读取

ota/cmd/group/{group_id}           # 分组广播

device/heartbeat/{dev_id}       # 设备心跳（retained）
device/status/{dev_id}          # 状态上报
device/ota/result/{dev_id}      # 升级结果上报
```

**QoS 1** + 设备主动 `/ota/check` 兜底（每 5min），防 MQTT 丢消息。

---

## 6. 前端页面设计 (Vue3 + Element Plus)

### OTA 相关页面

| 页面 | 核心功能 |
|---|---|
| **登录页** | JWT 鉴权 |
| **Dashboard（统一）** | 总设备数/在线数/告警数（统计卡片）+ 在线率趋势 + 各版本占比 + 进行中 OTA 任务。数据平台监控大盘功能并入此页（详见 §21） |
| **固件管理** | 上传弹窗（拖拽 bin + 前端预计算 SHA256 + 版本号 + release notes）、列表、发布 |
| **设备管理** | 表格（搜索/筛选/批量分组）、详情抽屉（版本历史时间线） |
| **OTA 任务** | 创建向导（4 步：选固件 → 选目标 → 配批次 → 确认）、列表、实时进度看板 |
| **系统设置** | MQTT/OSS 配置、设备密钥导出 |

**关键组件**：
- 任务进度看板用 WebSocket（FastAPI 原生支持）实时推送 `ota_task_records` 变化
- 固件上传用分片 + SHA256 前端校验
- 下载进度显示字节级 offset（配合 `download_offset` 字段）

> 数据平台新增页面（监控大盘、设备详情、告警中心）详见 Part B §21。

---

## 7. STM32 端设计

### 7.1 Flash 布局

#### 片内 flash

```
0x0800_0000  Bootloader  (32KB)   永不升级，出厂烧录
0x0800_8000  App         (220KB)  OTA 目标
0x0803_F000  参数区      (4KB)    状态机 + 版本 + 升级标志
```

> 合计 32 + 220 + 4 = 256KB，恰好填满 STM32F1 片内 flash（`0x0800_0000`~`0x0804_0000`）。App 区上限即 220KB。

**参数区结构（4KB @ `0x0803_F000`，掉电保持）**：

```c
typedef struct {
  uint32_t magic;           // 0x5041524D ("PARM")，参数区有效标志
  uint8_t  state;           // OTA 状态机: IDLE/DOWNLOADING/DOWNLOADED/UPGRADE_REQUESTED/UPGRADING
  uint8_t  app_healthy;     // 启动确认标志（新 App 启动成功后置 1）
  uint8_t  upgrade_flag;    // 触发 Bootloader 升级标志
  uint8_t  reserved;
  char     cur_version[16]; // 当前运行的 App 版本
  char     new_version[16]; // 升级目标版本（升级完成后移入 cur_version）
  uint32_t crc32;           // 本结构体校验（覆盖 magic~new_version）
} ota_param_t;              // 定长，远小于 4KB，剩余空间预留
```

> - `receive_offset` **不在**参数区，在片外 flash 新固件头里（§7.1 片外布局）。
> - 参数区只在**状态迁移时**写入（每次 OTA 寥寥几次），不高频写，flash 寿命无忧。
> - 写入用"先写后校验"（写完回读比对 `crc32`），防掉电写半。

#### 片外 SPI flash（如 W25Q64 8MB）

> **存储约定（方案 A）**：新固件区 / 备份固件区均**原样存完整密文 blob** = `[IV(16B)][密文][HMAC(32B)]`。App 收到第 N 字节就写第 N 偏移，`receive_offset` 即已写 blob 字节数，与 HTTP 下载 offset、固件区写入 offset 三者相等，断点续传零换算。IV/HMAC 由 Bootloader 用固定偏移从固件区读取（IV 在 blob 首 16B，HMAC 在 blob 末 32B），故固件头不再单独存 IV/HMAC。

```
0x00_0000  新固件头     (4KB)    magic + size + version + receive_offset
0x00_1000  新固件区     (256KB)  完整密文 blob：[IV 16B][密文][HMAC 32B]
0x04_1000  备份固件头   (4KB)    backup_magic + size + version
0x04_2000  备份固件区   (256KB)  完整密文 blob：[IV 16B][密文][HMAC 32B]
0x08_2000  保留
```

### 7.2 App OTA 模块（新增核心）

App 运行时挂一个 OTA 后台任务，主业务不阻塞：

```
UART DMA 接收 + 空闲中断 -> 解帧 -> OTA 状态机

状态机（存在片内参数区）:
  IDLE -> DOWNLOADING -> DOWNLOADED -> UPGRADE_REQUESTED -> (软复位)
  干净失败（CRC 错/写 flash 错/重传超限）-> IDLE + 上报错误
  下载中途断电（非干净失败）-> 状态保持 DOWNLOADING，receive_offset 已在片外固件头持久化
                              -> 重启后 ESP 查 offset 用 Range 续传，不回 IDLE

收到固件块 (CMD 0x05):
  1. 解析 offset + data
  2. 写片外 flash：固件区基地址 + offset（offset 即 blob 字节偏移，与 HTTP 下载偏移一致）
  3. 更新固件头 receive_offset = offset + len
  4. 回 ACK

收到全部完成 (CMD 0x07):
  1. 校验 receive_offset == 固件头 size（blob 完整落盘）
  2. 通过: 状态 -> DOWNLOADED, 上报"待升级"
  3. 失败: 上报错误, 状态 -> IDLE
  （HMAC-SHA256 密码学校验由 Bootloader 在阶段3 负责，App 不持有密钥）

收到立即升级 (CMD 0x08):
  1. 状态 -> UPGRADE_REQUESTED
  2. NVIC_SystemReset() 软复位
  （备份由 Bootloader 负责，App 不参与 flash 备份逻辑）
```

**关键实现**：
- UART 用 DMA + 空闲中断收变长帧，不阻塞主循环
- 片外 flash 写入按扇区擦除（4KB），维护"已擦除扇区位图"避免重复擦
- `receive_offset` 每块更新前先写后校验，防掉电丢失

### 7.3 Bootloader（集中升级职责）

Bootloader 不涉及 UART 协议，集中负责备份片内、读片外新固件、写片内、校验、跳转、回滚全流程：

```
上电:
  1. 读参数区状态
  2. if state == UPGRADE_REQUESTED:
     a. 读片外新固件头: size（blob 总长，含 IV+密文+HMAC）
        * IV    = 新固件区[0 : 16]            # blob 首 16B
        * HMAC  = 新固件区[size-32 : size]    # blob 末 32B
        * 密文  = 新固件区[16 : size-32]
     b. 检查片外备份区 magic:
        - magic 不存在: 备份片内 App (加密备份)
          * 生成随机备份 IV
          * 流式: 读片内 App 1KB (明文) -> AES-256-CTR 加密 (主密钥+备份IV) -> 写片外备份区 1KB (密文)
          * 计算备份 HMAC, 拼接备份 blob [IV + 密文 + HMAC] 写入备份区
          * 写备份固件头 (backup_magic + size + version)
          * 写 magic
        - magic 存在: 跳过备份 (上次已备份,这是断电重入)
     c. 擦除片内 App 区
     d. 流式解密写入:
        * 读新固件区 1KB 密文 (从偏移 16 开始)
        * AES-256-CTR 解密 (主密钥 + 新固件区 IV) -> 1KB 明文
        * 写片内 flash 1KB 明文
        * 循环
     e. 整体 HMAC 校验 (重读新固件区 blob [IV + 密文], 计算并比末 32B HMAC)
     f. 通过: state = UPGRADING, 写新版本号, 清备份 magic, 启动 IWDG(30s), 跳转
     g. 失败: 从备份区读密文 blob -> 解密 (备份 IV, 在备份区[0:16]) -> 写片内明文 -> state = IDLE -> 跳转
  3. elif state == UPGRADING (上次刚升级完,等 App 确认):
     - Bootloader 跳转前已启动 IWDG 独立看门狗 (30s)
     - 新 App 正常启动后写 "app_healthy" 标志并喂狗; Bootloader 不再运行
     - App 未在 30s 内写 app_healthy -> IWDG 超时复位 MCU
     - 复位后 Bootloader 见 state==UPGRADING:
       * app_healthy == 0: App 未确认（启动失败）-> 从备份区回滚 -> state = IDLE -> 跳转
       * app_healthy == 1: App 已确认但断电在清标志前 -> 判定健康 -> 清 app_healthy, state = IDLE -> 跳转
     - App 正常确认后清 app_healthy, state = IDLE (回到正常态)
  4. else: 直接跳转 App

跳转 App:
  SCB->VTOR = 0x08008000;          // 重定位中断向量
  __set_MSP(*(uint32_t*)0x08008000); // 设置栈指针
  ((void(*)())(*(uint32_t*)0x08008004))(); // 跳转
```

> **IWDG 是永久系统看门狗，不是一次性启动确认**：STM32F1 的 IWDG 一旦启动**无法用软件关闭**，只能不断喂狗（写 `0xAAAA` 重载计数）。因此 App 必须在**整个运行期间**跑一个周期性喂狗任务（如每 10s 重载一次），而不是只在启动时喂一次。"30s 启动确认"只是它的第一个窗口：App 若 30s 内没开始喂狗就复位回滚；启动成功后，IWDG 继续作为运行时 hang 检测常駐生效（这反而是收益，不是负担）。若项目不想要永久看门狗，需改用 RTC/软件定时器做启动确认（可关闭），但会失去运行时保护。

### 7.4 固件加密设计

**算法选型**：
- 加密：AES-256-CTR（流式加密，无需填充，Bootloader 可边读边解密边写）
- 认证：HMAC-SHA256（覆盖 IV + 密文，防篡改 + 防传输错误）
- 库：mbedtls（ARM 官方推荐，STM32 Cube 集成良好）

**密钥管理**：
- 主密钥：AES-256 key (32B) + HMAC-SHA256 key (32B)，共 64B
- 存储：硬编码在 Bootloader 内部（const 数组，编译进 flash）
- 所有设备共用同一主密钥（简化部署，接受单点泄露风险）
- App 不持有密钥（防止 App 被反编译后泄露密钥）
- 启用 STM32 RDP Level 1 读保护：SWD 不可读 flash，防 Bootloader 被读出

**密文格式**：
```
[IV (16B, 每版本发布时随机生成一次)] + [Encrypted Firmware Data] + [HMAC-SHA256 (32B)]
```
HMAC 计算范围：`IV || Encrypted_Data`，确保 IV 不可篡改。

**服务器端加密流程**（发布时加密一次，blob 按版本复用）：
1. 管理员上传明文 bin（可选留存明文到 OSS 私有 bucket 供审计/重新加密）
2. **发布固件时**，后端读明文
3. 生成随机 IV (16B) —— 每个固件版本仅生成一次
4. AES-256-CTR 加密固件（主密钥 + IV）
5. 计算 HMAC-SHA256(主密钥, IV || 密文)
6. 拼接密文 bin：`IV + 密文 + HMAC`
7. 密文 bin 上传 OSS（独立 key），并把 `oss_key` / `iv` / `hmac` 写入 `firmwares` 表

> **复用约定**：同一固件版本只加密一次，其密文 blob 被所有针对该版本的 OTA 任务复用（任务不重新加密）。CTR 模式下，同版本 = 同明文，用固定 (主密钥, IV) 重复加密得到相同密文，不泄露额外信息，密码学上安全；因此 IV 只需保证"不同版本互不相同"（每次发布随机生成即可）。

**STM32 端加解密职责**：
| 阶段 | 操作 | 密钥持有者 |
|---|---|---|
| App 接收固件块 | UART 收密文 blob 字节 -> 原样写片外固件区 | App 无密钥，纯转发写入 |
| App 接收完成 | `receive_offset == size` 完整性校验（非密码学） | App 无密钥 |
| Bootloader 升级 | 读固件区 blob：IV(首16B)+HMAC(末32B) 校验，密文区解密写片内明文 | Bootloader 持有 AES + HMAC key |
| Bootloader 备份 | 读片内明文 -> AES 加密 -> 拼备份 blob 写备份区 | Bootloader 持有 AES + HMAC key |
| Bootloader 回滚 | 读备份区 blob：IV+HMAC 校验，密文区解密写片内明文 | Bootloader 持有 AES + HMAC key |

**性能估算（STM32F1 @72MHz 软件 AES）**：
- AES-256-CTR 软件实现：~50-80 KB/s
- HMAC-SHA256 软件实现：~150 KB/s
- ~220KB（App 区上限）加密或解密：3-5s
- 单次升级增加耗时：6-10s（备份加密 3-5s + 升级解密 3-5s）
- 总升级时间：约 15-25s（含 UART 传输 + flash 读写），可接受

**安全性分析**：
- ✓ 片外 flash 是密文：攻击者读片外 flash 无法获取固件
- ✓ 传输全程加密：HTTPS（外层）+ 固件加密（内层），双重保护
- ✓ 片内 App 区是明文：但 App 不含密钥，App 被反编译无密钥泄露
- ✓ Bootloader 含主密钥：RDP Level 1 保护，SWD 不可读
- ⚠ 剩余风险：主密钥所有设备共用，单设备被深度攻破（如侧信道攻击）则全设备可解密。如需更高安全，未来可升级为每设备一密钥（OTP 存储）

---

## 8. ESP-07S 端设计（流式转发）

ESP-07S 是纯管道，HTTP 流进来一块就 UART 转一块，不在本地缓存：

```cpp
HTTPClient http;
WiFiClient *stream = http.getStreamPtr();
int offset = 0;
while (offset < total_size) {
  // 1. 从 HTTP 流读 1KB 到 buffer
  size_t n = stream->readBytes(buf, 1024);

  // 2. 打包成 UART 帧 CMD 0x05 发送
  sendFrame(0x05, offset, n, buf);

  // 3. 等 STM32 ACK (带超时 + 重传)
  if (!waitAck(offset, 3000)) {
    if (++retry > 3) { reportFailed(); break; }
    sendFrame(0x05, offset, n, buf);  // 重传
  }

  offset += n;
}
// 4. 发 CMD 0x07 通知完成
// 5. 发 CMD 0x08 触发升级 (可由后端控制时机)
```

**断点续传**（三偏移相等，零换算）：
- 网络中断后 ESP-07S 重连，UART 发 CMD 0x01 查询 STM32 已收 offset
- STM32 返回固件头里的 `receive_offset`（= 固件区已写 blob 字节数）
- ESP-07S 用 `Range: bytes=receive_offset-` 重新发 HTTP 请求（即固件区写入偏移 = HTTP 下载偏移 = receive_offset）
- OSS 原生支持 Range

**流控关键**：
- HTTP 下载速度（几百 KB/s）> UART 转发速度（~80KB/s @921600）
- ESP-07S 读完一块**主动暂停读** HTTP stream，等 UART ACK 后再读下一块
- TCP 接收缓冲区会自然 backpressure，不会丢数据

---

## 9. 通信协议（ESP-07S ↔ STM32）

### 帧格式

```
[0xAA][0x55][LEN_HI][LEN_LO][CMD][DATA...][CRC16_HI][CRC16_LO]
```

- LEN = CMD + DATA 长度
- CRC16-Modbus，覆盖 LEN + CMD + DATA
- UART 速率：460800 或 921600 bps

### 字节序与编码约定

| 项 | 约定 |
|---|---|
| 多字节整数字段 | **大端（网络序，高字节在前）**，与 `LEN_HI/LEN_LO` 一致。STM32 内部小端，收发时在边界做一次转换 |
| `offset` / `receive_offset` / `size` | uint32，大端，4 字节 |
| `len`（块长度） | uint16，大端，2 字节 |
| `state` / `result` | uint8，1 字节 |
| `version` 字符串 | 长度前缀：`ver_len[1] + utf8[ver_len]`，最长 31 字节 |
| `task_id` | 16 字节定长（UUID 二进制，不足补 0） |
| `dev_id`（CMD 0x10） | uint32 大端，4 字节（STM32 出厂烧录的全链路唯一机器标识；ESP 从帧读取后用于拼 MQTT topic，自身不持有） |
| CRC16-Modbus | 多项式 0xA001（即 0x8005 反射），初值 0xFFFF，输入/输出均反射；`CRC16_HI` 为高字节 |

### 命令表

| CMD | 方向 | 含义 | DATA | 备注 |
|---|---|---|---|---|
| 0x01 | ESP→MCU | 查询状态 | - | App 模式响应 |
| 0x02 | MCU→ESP | 状态应答 | `state[1] + receive_offset[4] + version[1+N]` | 含断点续传 offset |
| 0x03 | ESP→MCU | 开始升级 | `task_id[16] + version[1+N] + size[4]` | App 进入接收模式（IV/HMAC 在 blob 内） |
| 0x04 | MCU→ESP | 开始 ACK | `start_offset[4]` | 0 全新 / N 续传 |
| 0x05 | ESP→MCU | 固件块 | `offset[4] + len[2] + data[len]` | 1KB/块 |
| 0x06 | MCU→ESP | 块 ACK | `offset[4] + result[1]` | 0=ok/1=crc/2=write_err |
| 0x07 | ESP→MCU | 传输完成 | - | 触发 STM32 整体校验 |
| 0x08 | ESP→MCU | 立即升级 | - | App 写标志 + 软复位（备份由 Bootloader 做） |
| 0x09 | MCU→ESP | 升级结果 | `task_id[16] + result[1] + new_version[1+N]` | 新 App 启动后发 |
| 0x0A | MCU→ESP | 心跳 | `state[1] + version[1+N]` | 60s 一次 |
| 0x10 | MCU→ESP | 数据上报 | `dev_id[4] + JSON[...]` | §20 数据采集 |
| 0x11 | ESP→MCU | 数据上报 ACK | `result[1]` | 0=ok/1=fail |
| 0x12 | ESP→MCU | 配置下发 | `json_len[2] + config_json[...]` | 云端 `cmd/{dev_id}/config` 透传，如 `{"report_interval_ms":10000}` |
| 0x13 | MCU→ESP | 配置 ACK | `result[1]` | 0=ok/1=parse_err/2=unsupported |

---

## 10. 关键风险与对策

| 风险 | 对策 |
|---|---|
| App 接收时片外 flash 写失败 | 块级 ACK + 重传 3 次；3 次失败整任务标记 failed |
| HTTP 中断 | ESP-07S 用 Range 续传，从 STM32 查 `receive_offset` |
| 升级中途断电 | 状态机 + 备份区：UPGRADE_REQUESTED 状态断电后 Bootloader 自动完成或回滚 |
| App 收固件时主业务阻塞 | UART DMA + 空闲中断异步收；片外 flash 写入用低优先级任务 |
| UART 流控溢出 | ESP-07S 读一块等 ACK 再读下一块，TCP 自然 backpressure |
| 新 App 启动失败 | Bootloader 30s 内未收到 `app_healthy` 则从备份区回滚 |
| 片外 flash 寿命 | 升级不频繁，远低于 10 万次擦写；按 4KB 扇区擦除 |
| Bootloader 自身砖机 | 不参与 OTA，只通过 SWD 烧录；预留 SWD 测试点 |
| UART 误码 | 每块 CRC16 + ACK + 重传 3 次；建议加硬件流控 RTS/CTS |
| ESP-07S 与 STM32 复位时序 | ESP-07S 通过 GPIO 控制 STM32 RESET，避免 UART 丢字节 |
| 主密钥泄露 | 所有设备固件可解密；启用 STM32 RDP Level 1 读保护，SWD 不可读 flash |
| 固件加密性能 | STM32F1 软件 AES-256 ~50-80 KB/s，~220KB 加解密 3-5s，可接受 |
| 时序数据丢失 | 时序数据对少量丢包不敏感（下一条 5s 后就来）；ESP 不缓存，WiFi 断连丢几秒数据可接受 |
| TDengine 存储增长 | KEEP 自动清理 + 降采样；监控磁盘使用率，接近 80% 扩容或缩短保留期 |
| 告警风暴（万设备同时触发） | FastAPI 告警评估任务限制单次最多 100 条推送；前端告警中心分页展示，超阈值合并 |

### 安全

- HTTPS 下载（传输层）+ 固件 HMAC-SHA256 校验（内容层，Bootloader 执行）
- OSS 私有读 + 一次性签名 URL（5 分钟过期）
- 一机一密，MQTT/HTTP 都校验
- 可选：固件 ECDSA 签名（防服务器被入侵后植入恶意固件）

### 灰度发布（大规模必备）

- 任务策略支持：全量 / 按分组 / 指定设备列表
- 批次控制：`batch_size` + `batch_interval_sec`，如每批 10 台间隔 60s
- 暂停/继续：发现问题可一键暂停
- 成功率阈值：批次失败率 > 30% 自动暂停告警

---

## 11. 工作量估算

### OTA 系统

| 模块 | 工作量 | 备注 |
|---|---|---|
| STM32 Bootloader | 3-4 天 | 集中升级职责 + AES/HMAC 加解密 |
| STM32 App OTA 模块 | 3-4 天 | UART 协议 + 片外 flash + 状态机 |
| ESP-07S 端 | 2-3 天 | 纯流式转发，不缓存 |
| 后端 FastAPI (OTA) | 4-5 天 | 固件/设备/任务/下载/上报 + MQTT publisher + 加密下发 |
| 前端 Vue3 (OTA) | 2-3 天 | 固件管理/设备管理/OTA 任务/登录 |
| OTA 联调 + 灰度 | 2-3 天 | |
| **OTA 小计** | **~2-3 周** | 单人 |

### 数据平台

| 模块 | 工作量 | 备注 |
|---|---|---|
| TDengine 部署 + EMQX taosX 桥接 | 0.5-1 天 | 安装配置 + 建库建表 |
| 后端 FastAPI (数据平台) | 3-4 天 | 时序查询 API + 告警评估 + TDengine connector |
| 前端 Vue3 (数据平台) | 3-4 天 | 监控大盘 + 设备详情 + 告警中心 + ECharts |
| STM32 data_report | 0.5-1 天 | 传感器采集 + JSON 打包 + UART 帧封装 |
| ESP-07S data_forwarder | 0.5 天 | UART 收帧 → MQTT publish |
| 联调 + 压力测试 | 1-2 天 | |
| **数据平台小计** | **~1.5-2 周** | 单人 |
| **全系统合计** | **~4-5 周** | 单人 |

---

## 12. 部署架构

| 组件 | 部署位置 |
|---|---|
| FastAPI + Uvicorn | 阿里云 ECS，systemd 守护，Nginx 反代 HTTPS |
| Vue3 前端 | 构建为静态文件，Nginx 托管 |
| MySQL | 阿里云 RDS（高可用版，自动备份） |
| OSS | 阿里云 OSS 私有 bucket |
| MQTT Broker | EMQX 开源版（ECS 自建） |
| TDengine | ECS 自建（开源版，taosX 桥接 EMQX） |
| 域名 + SSL | 阿里云域名 + 免费 DV 证书 |

ECS 配置建议：**4C8G** 起步（EMQX + TDengine + FastAPI + Nginx 同机），数据盘 2TB 起（TDengine 压缩后存储）。设备量增长后可 EMQX / TDengine 各拆独立 ECS。

---

## 13. 下一步可选起点

### OTA 系统

1. **STM32 Bootloader 代码骨架** — Flash 布局 + 读片外 + 写片内 + 跳转 + 回滚 + AES/HMAC
2. **STM32 App OTA 模块骨架** — UART DMA 收帧 + 片外 flash 读写 + 状态机
3. **ESP-07S 流式转发骨架** — MQTT 订阅 + HTTP stream → UART 帧分块发送 + 断点续传
4. **FastAPI 后端 MVP 骨架** — 核心数据模型 + API + MQTT publisher
5. **片外 flash 驱动骨架** — W25Qxx 扇区管理 + 断点续传 offset 管理

### 数据平台

6. **TDengine 部署 + 建库建表** — ECS 安装 TDengine + taosX 订阅 EMQX + 创建超级表
7. **FastAPI 时序查询 API** — TDengine connector + telemetry/events/alerts 路由
8. **前端监控大盘 MVP** — ECharts 实时值卡片 + 在线率趋势 + 设备列表
9. **STM32 data_report 模块** — 传感器采集 + JSON 打包 + UART CMD 0x10
10. **ESP-07S data_forwarder 模块** — UART 收帧 → MQTT publish + ACK

全系统实施顺序详见 §25。

---

# Part B：数据采集平台

---

## 14. 数据平台概述

在已有 OTA 基础设施上叠加数据采集上报能力。10,000 台 STM32 + ESP-07S 设备定时上报遥测数据（传感器读数、电气参数、开关量），云端存储、查询、展示。

### 14.1 核心指标

| 指标 | 值 |
|---|---|
| 节点数 | 10,000 台 STM32 + ESP-07S |
| 上报频率 | 每 5 秒 |
| 单条大小 | ~1 KB（环境量 + 电气量 + 开关量，约 16 字段） |
| 写入吞吐 | 2,000 条/s，~2 MB/s |
| 日增量 | ~172 GB（压缩后 ~10 GB） |
| 数据保留 | 6-12 个月 |
| 查询场景 | 按设备+时间段查历史曲线、聚合统计 |
| 展示 | 全局监控大盘 + 单设备详情 + 告警中心 |

### 14.2 与 OTA 系统关系

- 复用现有云基础设施（ECS / RDS MySQL / EMQX / OSS）
- 复用 STM32 + ESP-07S 硬件和 UART 帧协议（新增 `CMD_DATA_REPORT` 命令）
- 复用 FastAPI + Vue3 + Element Plus 技术栈
- OTA 能力保持不动，数据采集为增量功能

---

## 15. 数据平台架构

```
┌──────────────────────────────────────────────────────────────────┐
│                        阿里云                                      │
│                                                                    │
│  ┌─────────┐     ┌──────────┐     ┌──────────────┐               │
│  │  EMQX   │────▶│ TDengine │     │  MySQL (RDS)  │               │
│  │ MQTT    │     │ (ECS自建) │     │ 设备/用户/OTA │               │
│  │ Broker  │     │ 时序数据   │     │ 配置/告警规则 │               │
│  └────▲────┘     └─────┬────┘     └──────┬───────┘               │
│       │                │                 │                         │
│       │          ┌─────┴────────┬────────┘                        │
│       │          │              │                                  │
│       │     ┌────▼────┐   ┌────▼────┐                             │
│       │     │ FastAPI │   │ FastAPI │                             │
│       │     │ (时序API)│   │ (管理API)│                             │
│       │     └────┬────┘   └────┬────┘                             │
│       │          │              │                                  │
│       │     ┌────▼──────────────▼────┐                            │
│       │     │      Vue3 前端          │                            │
│       │     │  监控大盘 + 设备详情     │                            │
│       │     └────────────────────────┘                            │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘
       │
   MQTT (port 1883/8883)
       │
  ┌────┴────────────┐
  │  10,000 台设备   │
  │  STM32 + ESP-07S │
  └─────────────────┘
```

### 15.1 组件职责

| 组件 | 职责 | 变化 |
|---|---|---|
| ESP-07S | OTA 不变；新增数据上报：UART 收帧 → CRC 校验 → MQTT publish | 加 data_forwarder |
| STM32 | OTA 不变；定时读传感器 → 构造 JSON → UART 发送 | 加 data_report |
| EMQX | MQTT 路由、设备认证 | 新增 data topic 路由 |
| TDengine | 时序数据：遥测 + 事件 + 告警 + 自动降采样聚合；通过 taosX 原生 MQTT 订阅直接消费 EMQX 消息 | **新增组件** |
| MySQL | 元数据：设备台账、用户、分组、告警规则、OTA 任务 | 不存时序数据，保持轻量 |
| FastAPI | 时序查询 API + 管理 API + MQTT 控制下发 | 拆分 router，各连各 DB |
| Vue3 | 全局大盘 + 设备详情 + 告警中心 + OTA 管理 | 新增监控页面 |

### 15.2 告警评估机制

告警规则存储在 MySQL `alert_rules` 表中，由 **FastAPI 后台定时任务** 周期性评估（10-30s 可配）：

1. 从 MySQL 读取所有启用的告警规则
2. 对每条规则，从 TDengine 查询 `SELECT count(*) FROM telemetry WHERE dev_id = ? AND ts > NOW - <duration> AND temp > <threshold>`
3. 判断触发：over_cnt ≥ 应有采样数 × 容忍比例 → 写入 alerts 表 + 推送 MQTT 通知
4. 未触发且之前是告警态 → 自动恢复，更新 resolved_at

> **应有采样数** = `duration ÷ 上报间隔`（如 30s ÷ 5s = 6 点）。乘以**容忍比例**（默认 0.8，可配）以容忍偶发丢包/上报抖动，避免个别点缺失导致漏报；即 6 点窗口内有 ≥5 点超阈值即判为"持续超阈值"。

| 告警场景 | 实现 |
|---|---|
| 阈值告警 | `temp > 80` 且持续 30s → 触发 |
| 离线告警 | `last_seen` 超过 5 分钟未更新 → 触发（直接看 MySQL devices 表） |
| 恢复判断 | 值回落 + 持续正常 ≥ 1 个评估周期 → 自动 resolve |

### 15.3 设备在线判定

- 每次收到 MQTT 遥测（5s）或 OTA 心跳（60s），**EMQX 规则引擎**更新 `devices.last_seen = NOW()`
- `online` — `last_seen` 在 5 分钟内
- `offline` — `last_seen` 超过 5 分钟
- 纯 OTA 设备（无数据平台）靠 60s 心跳维持在线；启用数据平台后 5s 遥测是主要更新源
- taosX 负责海量数据写入，EMQX 规则引擎负责轻量 `last_seen` 更新，互不干扰

---

## 16. 数据库选型：为什么加 TDengine

| | MySQL | TDengine |
|---|---|---|
| 年存储成本 | ~84 万元（62 TB 云盘） | ~6 万元（~3 TB 云盘，压缩后） |
| 写入路径 | 事务 + 索引 + redo log | 无事务开销，直接追加列文件 |
| 查询性能 | 随数据增长退化 | 一设备一表，物理隔离，永远快 |
| 维护负担 | 需持续 DBA 调优 | 几乎零运维 |
| 适用场景 | 设备台账、用户、OTA（元数据） | 时序遥测、事件、告警（海量数据） |

**MySQL 不能去掉**：用户/角色/权限、设备台账 CRUD、OTA 任务状态机、告警规则配置——这些关系型操作仍是 MySQL 的主场。TDengine 只管时序数据。

---

## 17. 数据模型

### 17.1 TDengine — 遥测超级表

```sql
-- KEEP 按需设置：推荐 180 DAYS（6 个月），可调 90/365
CREATE DATABASE iot_data KEEP 180 DAYS 10 BLOCKS 4;

CREATE STABLE telemetry (
  ts         TIMESTAMP,
  -- 环境量
  temp       FLOAT,          -- 温度 (°C)
  hum        FLOAT,          -- 湿度 (%)
  pressure   FLOAT,          -- 气压 (hPa)
  -- 电气量
  volt_in    FLOAT,          -- 输入电压 (V)
  volt_out   FLOAT,          -- 输出电压 (V)
  current    FLOAT,          -- 电流 (A)
  power      FLOAT,          -- 功率 (W)
  energy     FLOAT,          -- 累计电量 (kWh)
  freq       FLOAT,          -- 电网频率 (Hz)
  -- 开关量
  di1        TINYINT,        -- 数字输入 1
  di2        TINYINT,        -- 数字输入 2
  di3        TINYINT,        -- 数字输入 3
  di4        TINYINT,        -- 数字输入 4
  -- 设备状态
  rssi       INT,            -- WiFi 信号强度 (dBm)
  uptime     BIGINT,         -- 设备运行秒数
  -- 扩展（冷门/临时字段，后续可 ALTER STABLE 提升为正式列）
  payload    NCHAR(1024)      -- 扩展字段 (JSON)
) TAGS (
  dev_id     INT UNSIGNED,    -- 设备 ID（uint32，出厂烧录）
  group_id   INT,             -- 分组
  location   NCHAR(64)        -- 位置
);
```

> 当前列表示例覆盖环境 + 电气 + 开关量典型场景，单条序列化约 800~1000 字节。TDengine 列式压缩让 20~30 列也不浪费存储。**这些列名就是 STM32 上报 JSON 的 key**（§18），taosX 按 key=列名直接入库，无需改名。新增列：`ALTER STABLE telemetry ADD COLUMN xxx FLOAT;`，万张子表秒级继承。

### 17.2 TDengine — 事件表

```sql
CREATE STABLE events (
  ts       TIMESTAMP,
  level    TINYINT,       -- 0=info 1=warn 2=error
  code     NCHAR(32),
  msg      NCHAR(256)
) TAGS (
  dev_id   INT UNSIGNED      -- 设备 ID（uint32）
);
```

### 17.3 TDengine — 告警快照表

告警触发时，FastAPI 把触发时刻的原始遥测写入此表（MySQL `alerts` 只存概要，原始快照存这里）：

```sql
CREATE STABLE alert_snapshots (
  ts         TIMESTAMP,        -- 触发时刻
  rule_id    INT,              -- 触发的告警规则 ID（对应 MySQL alert_rules.id）
  metric     NCHAR(32),        -- 触发测点名（如 temp）
  value      FLOAT,            -- 触发时该测点的值
  snapshot   NCHAR(1024)       -- 触发时刻全量测点 JSON 快照
) TAGS (
  dev_id     INT UNSIGNED      -- 设备 ID（uint32）
);
```

> MySQL `alerts.triggered_at` + `dev_id` 可定位到本表对应快照行。

### 17.4 扩展字段策略

**"宽表 + payload 兜底"**：常用字段建列保证查询性能；冷门字段走 payload JSON 灵活扩展；确认稳定后 `ALTER STABLE` 提升为正式列。

### 17.5 MySQL — 新增元数据表

在已有 OTA 表基础上新增：

```sql
-- 告警规则
alert_rules: id, name, metric, operator(>/</=), threshold,
             duration, level, enabled

-- 告警记录（MySQL 存概要，原始快照存 §17.3 alert_snapshots）
alerts: id, dev_id, rule_id, level, msg, triggered_at, resolved_at
```

`devices` 表沿用 §3 定义（已包含 `status` 和 `last_seen`，OTA 与数据平台共用），无需额外改动。

---

## 18. 数据格式约定

> **核心约定：JSON key = TDengine 列名，全链路统一、零映射。** STM32 直接用列名（`temp`/`hum`/...）做 key，ESP 透传，taosX 按 key=列名直接入库，FastAPI 原样返回前端。UART 是点对点独占链路，每 5s 一条，列名 key 比短 key 多的那点字节（~30%）无实际带宽影响，换来全链路无需任何 key 改名。

| 层 | 格式 | 说明 |
|---|---|---|
| STM32 → ESP-07S | `{"ts":1721712000,"temp":25.3,"hum":68.2,"pressure":101.3,"volt_in":220.1,...}` | JSON key = TDengine 列名 |
| ESP-07S → MQTT | 同上，透传 | 不解析、不转换 |
| taosX → TDengine | JSON key 与列名一一对应 | 零映射直入 |
| FastAPI → 前端 | `{"ts":"2026-07-23T10:00:00","temp":25.3,"hum":68.2,...}` | 沿用列名 key，前端自行配中文显示标签 |

### 18.1 测点字段字典（JSON key / TDengine 列）

各测点 key（即 §17.1 `telemetry` 列名）的含义与单位：

| key / 列名 | 含义 | 单位 |
|---|---|---|
| `ts` | 时间戳（STM32 RTC 提供，见 §20.2 校时） | Unix s |
| `temp` | 温度 | °C |
| `hum` | 湿度 | % |
| `pressure` | 气压 | hPa |
| `volt_in` | 输入电压 | V |
| `volt_out` | 输出电压 | V |
| `current` | 电流 | A |
| `power` | 功率 | W |
| `energy` | 累计电量 | kWh |
| `freq` | 电网频率 | Hz |
| `di1`~`di4` | 数字输入 | 0/1 |
| `rssi` | WiFi 信号 | dBm |
| `uptime` | 运行时间 | s |
| `payload` | 扩展字段（未建列测点的 JSON 字符串） | - |

> 新增测点：`ALTER STABLE telemetry ADD COLUMN xxx FLOAT` 后，STM32 固件在 JSON 里直接加同名 key 即可，taosX 自动入库，**无需任何映射配置**。

---

## 19. API 设计（数据平台部分）

### 19.1 时序查询 API

```
GET /api/v1/telemetry/latest?dev_ids=10001,10002
  → 每个设备最新一条数据（大盘实时值）

GET /api/v1/telemetry/history
  ?dev_id=10001&start=2026-07-22T00:00:00&end=2026-07-23T00:00:00&interval=1m
  → 降采样查询，interval 支持 10s/1m/5m/1h/1d

GET /api/v1/telemetry/stats
  ?dev_id=10001&start=2026-07-16&end=2026-07-23
  → 统计：avg/max/min/count
```

> **使用边界**：`/telemetry/latest` 面向**分页等小批量**场景（设备列表当前页的几十台），`dev_ids` 不宜一次传上万。全局大盘的总数/在线率/告警分布等走 §19.3 **聚合接口**（`/dashboard/*`），不逐设备取 latest；按组看板可用 `group_id` 过滤聚合。

### 19.2 事件/告警 API

```
GET /api/v1/events?dev_id=10001&level=2&start=...&end=...
GET /api/v1/alerts?status=active
GET /api/v1/alerts/history?dev_id=10001&page=1&size=20
```

### 19.3 大盘聚合 API

```
GET /api/v1/dashboard/summary
  → { total, online, offline, alerts_active }

GET /api/v1/dashboard/group-stats
  → 每个分组的在线率、告警数

GET /api/v1/dashboard/alert-trend?days=7
  → 最近 7 天每天告警趋势
```

### 19.4 MQTT Topic 设计（新增）

```
data/{dev_id}/telemetry    → 遥测 JSON，TDengine taosX 原生 MQTT 订阅直接写入
data/{dev_id}/event        → 事件/告警
cmd/{dev_id}/config        → 云端下发配置（如修改上报间隔）
```

> `{dev_id}` 为 uint32 的十进制串（如 `data/10001/telemetry`），与 §3 `devices.dev_id`、TDengine TAG 一致。

---

## 20. 数据采集 — STM32 / ESP-07S

### 20.1 总体原则

STM32 只管采集 + 打包，ESP-07S 只管透明转发。ESP 不解析 JSON 内容，不缓存数据。与 OTA 的"ESP 不缓存固件"哲学一致。

### 20.2 STM32 端

```
定时器中断 (5s) → 读传感器 → 构造 JSON (~1KB, key 用列名 temp/hum/...) → 封装 UART 帧 (CMD=0x10) → DMA 发送
```

改动：`uart_protocol.c` 加 `CMD_DATA_REPORT (0x10)`，新建 `data_report.c`（~100 行）。

**时间戳与 RTC 校时**：
- `ts` 取自 STM32 片内 RTC（LSE 32.768kHz 晶振 + VBAT 后备电池保持走时，掉电不丢时间）。
- RTC 需**初始校时 + 定期同步**，否则时间戳漂移、数据落错时间点。校时路径：ESP-07S 有 WiFi，联网后取 NTP 时间，经 CMD 0x12 配置下发 `{"rtc_sync": <unix_ts>}` 给 STM32 设置 RTC。
- 建议：开机联网后校一次，之后每天校一次；若 RTC 未校时（`ts` 接近 0），taosX 可配置用服务器接收时间兜底。

### 20.3 ESP-07S 端

```
UART 收帧 → 校验 CRC16 → 从帧头读 dev_id(4B) → 提取 JSON
         → mqttClient.publish(data/{dev_id}/telemetry, JSON) → 返回 ACK
```

> ESP-07S **不持有设备身份**：dev_id 每帧由 STM32 携带，ESP 只是临时从帧头读出用于拼 MQTT topic，转发完即弃，无需 provision 任何 ID。JSON 内容 ESP 不解析、原样透传。

改动：新建 `data_forwarder.cpp`（~50 行）。

### 20.4 UART 帧格式（复用现有协议）

```
[0xAA][0x55][LEN_H][LEN_L][CMD=0x10][dev_id 4B][JSON...][CRC16]
```

帧开销 11 字节（AA 55 + LEN×2 + CMD + dev_id×4 + CRC×2），JSON 本体 ~1KB，总帧长约 1035 字节。460800 bps 下传输约 22ms。

### 20.5 错误处理

| 端 | 场景 | 处理 |
|---|---|---|
| STM32 | 传感器读取失败 | 对应字段填 null，不跳过上报 |
| STM32 | UART 发送缓冲区满 | 丢本次数据，记录丢包计数 |
| STM32 | 连续 N 次 ACK 超时 | 标记通信故障，上报事件 |
| ESP-07S | WiFi 断连 | 不缓存，丢就丢了 |
| ESP-07S | MQTT publish 失败 | 重试 1 次，仍失败则丢弃 |
| ESP-07S | CRC16 校验失败 | 直接丢弃，不 ACK |

---

## 21. 前端页面设计

### 21.1 新增页面

```
📊 监控大盘     → 4 统计卡片 + 在线率趋势 + 告警分布饼图 + 分组状态表格
📈 设备详情     → 实时值卡片 + ECharts 历史曲线 (1h/6h/24h/7d/30d) + 事件时间线
🔔 告警中心     → 分级筛选标签 + 分页列表 + 展开详情
```

### 21.2 前端技术组合

| 需求 | 选型 |
|---|---|
| 框架 | Vue3 + TypeScript + Vite（沿用） |
| 组件库 | Element Plus（沿用） |
| 图表 | ECharts — 大数据量曲线，dataZoom 缩放 |
| 状态管理 | Pinia（沿用） |
| 实时更新 | MQTT over WebSocket 直连 EMQX |
| 历史曲线 | HTTP 请求 FastAPI/TDengine，按需加载 |

---

## 22. 部署与成本

### 22.1 服务器规划

```
现有 ECS (复用)
├── EMQX (MQTT broker, port 1883/8883/8084)
├── TDengine (时序数据库)            ← 新增
└── FastAPI (Nginx + uvicorn, port 8000)

RDS MySQL (现有，复用) ─── 设备/用户/OTA 元数据
OSS (现有，复用)        ─── OTA 固件包
```

不需要新增云资源，仅现有 ECS 上安装 TDengine 开源版。

### 22.2 成本对比

| 保留期 | TDengine 方案年成本 | 纯 MySQL 方案年成本 |
|---|---|---|
| 6 个月 | ~5.8 万元 | ~42 万元 |
| 12 个月 | ~6.8 万元 | ~84 万元 |

差距全在存储压缩上（TDengine 压缩比 10-20x，MySQL 不压缩）。MySQL 6 个月 = 31 TB 云盘，12 个月 = 62 TB 云盘。

### 22.3 项目目录结构

```
D:\claude\514\
├── stm32-bootloader/          (已有，不动)
├── stm32-app/                 (扩展)
│   └── src/
│       ├── ota_task.c         (已有)
│       ├── data_report.c      (新增)
│       └── uart_protocol.c    (扩展)
├── esp07s/                    (扩展)
│   └── src/
│       ├── data_forwarder.cpp (新增)
│       └── uart_transport.cpp (扩展)
├── backend/                   (扩展)
│   └── app/
│       ├── api/
│       │   ├── telemetry.py   (新增)
│       │   ├── devices.py     (新增)
│       │   └── ota.py         (已有)
│       ├── services/
│       │   └── td_connector.py(新增)
│       └── mqtt/              (已有)
├── frontend/                  (扩展)
│   └── src/
│       ├── views/
│       │   ├── Dashboard.vue      (新增)
│       │   ├── DeviceDetail.vue   (新增)
│       │   └── AlertCenter.vue    (新增)
│       ├── api/
│       │   └── telemetry.ts       (新增)
│       └── components/
│           ├── StatCard.vue       (新增)
│           └── TimeSeriesChart.vue(新增)
└── docs/
    └── ota-design.md              (本文件)
```

---

## 23. 安全

- MQTT: TLS (port 8883) 传输加密 + 一机一密（username/password）认证
- HTTP API: HTTPS + JWT token
- TDengine 部署在 ECS 内网，不暴露公网端口；仅 FastAPI 通过内网 IP 连接
- MySQL 沿用现有 RDS 安全组配置

---

## 24. 测试策略

| 层 | 方法 | 工具 |
|---|---|---|
| STM32 data_report | PC 串口工具模拟 ESP-07S，验证帧格式 + JSON + CRC | 串口助手 + Python |
| ESP-07S data_forwarder | PC 起 MQTT broker + 串口发帧，验证 publish | Mosquitto |
| 后端 telemetry API | 单元测试 + httpx 集成测试 | pytest |
| EMQX → TDengine | MQTT 客户端模拟 1000 设备并发上报 | paho-mqtt + 压力脚本 |
| 前端 | Vitest 组件测试 + Playwright E2E | Vitest / Playwright |
| 端到端 | 1-5 台真实设备验证完整链路 | 真实硬件 |

---

## 25. 实施顺序（全系统）

```
Phase 1: STM32 Bootloader                → OTA 核心
Phase 2: STM32 App OTA 模块              → OTA 链路
Phase 3: ESP-07S 流式转发                → OTA 链路
Phase 4: 后端 MVP (FastAPI + MySQL)       → OTA 云端
Phase 5: 前端 MVP (上传 + 触发升级)       → OTA 前端
Phase 6: TDengine 部署 + EMQX 桥接        → 数据能进来  ← 数据平台起步
Phase 7: FastAPI 时序查询 API             → 数据能出来
Phase 8: 前端监控大盘 MVP                 → 数据能看
Phase 9: STM32 + ESP-07S 数据上报适配     → 真实设备对接
Phase 10: 告警规则引擎 + 告警中心          → 智能化
Phase 11: 前端设备详情 + 历史曲线          → 完整体验
Phase 12: 联调 + E2E                      → 全链路验证
Phase 13: 企业级能力（分组/批次/灰度/导出） → 运营能力
```

---

## 26. 关键设计决策汇总

| 决策 | 结论 | 理由 |
|---|---|---|
| 设备标识 | dev_id = uint32，STM32 出厂烧录，全链路统一；ESP 无状态、从帧读取 | 4 字节省空间、无需映射表；ESP 纯转发不持有身份 |
| 遥测 JSON key | 直接用 TDengine 列名（temp/hum/...），不用短 key | UART 点对点非瓶颈，列名换来 taosX 零映射直入 |
| 固件下载路径 | ESP-07S 直连 OSS 签名 URL，不经 FastAPI | OSS 扛并发带宽 + 原生 Range，避免压垮单台 ECS |
| 固件加密粒度 | 每版本发布时加密一次，blob 按版本复用 | 与 firmwares 表结构一致；CTR 下同明文复用 IV 安全 |
| 时序存储 | TDengine，不用 MySQL 存时序数据 | 存储成本降 14 倍，查询不退化 |
| 数据分层 | 时序 → TDengine，元数据 → MySQL | 各用最合适的数据库 |
| ESP-07S 角色 | 纯管道，不缓存固件也不缓存数据 | 代码简单，内存低，断电一致 |
| 扩展字段 | 宽表 + payload 兜底 | 查询性能 + 灵活性平衡 |
| 上报协议 | 复用现有 UART 帧协议 + CMD_DATA_REPORT | 最小化协议层改动 |
| 前端图表 | ECharts | 大数据量曲线 + dataZoom 缩放 |
| 告警评估 | FastAPI 后台定时任务 | 无需引入独立规则引擎 |
| 在线判定 | EMQX 规则引擎更新 last_seen + 5min 超时 | 低延迟 + 防抖动 |
| 加密 | AES-256-CTR + HMAC-SHA256，RDP Level 1 | OTA 固件安全（详见 Part A） |
