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

| 主控 MCU | STM32F103VE（裸机/RTOS，片内 flash 512KB，App 区 288KB，含 RTC + VBAT 后备） |

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



### 1.4 设备出厂预置（Provisioning）



**产线烧录**（一次性，出厂即上线）：



| 烧录目标 | 内容 | 说明 |

|---|---|---|

| STM32 — `factory.bin` | Bootloader + APP_INFO + App 完整镜像 | 合并工具 `factory_tool.py` 生成 |

| STM32 — 参数区（`factory.bin` 内） | `dev_id`（uint32） | 产线分配，参数区 OTA 不擦除 |

| ESP-07S flash | WiFi SSID/密码、MQTT broker 地址（DNS 域名）、`dev_id` | `secret` 不在产线烧录，由 STM32 首次上电后计算并下发 |



**首次上电自动配置**：



```

设备首次上电:

  Bootloader → UID 绑定 → 参数区初始化 → jump_to_app()

  App 启动:

    ① dev_id 已在参数区

    ② master_device_key 在 APP_INFO @0x0800F802（编译期写入）

    ③ device_secret_gen(dev_id, secret)   ← STM32 自主计算

    ④ 存 secret 到参数区（后续上电直接读取）

    ⑤ UART → 发送 secret 给 ESP-07S

    ⑥ ESP 保存 secret → MQTT CONNECT → 上线

```



**密钥分发（产线一次性生成）**：



| 密钥 | 位置 | 用途 |

|------|------|------|

| `aes_key` | `bootloader/src/crypto.c`（编译进 BT） | AES-256-CTR 固件加解密 |

| `hmac_key` | `bootloader/src/crypto.c`（编译进 BT） | HMAC-SHA256 固件签名 |

| `master_device_key` | `app_info.c`（编译进 APP_INFO, scatter 定位 0x0800F802） | 派生每台设备认证 secret |



> `secret = HMAC-SHA256(master_device_key, dev_id_le)[:16]`，`master_device_key` 只有一份权威来源（APP_INFO），不存多副本，不会出现不同步。<br>

> 服务器端用 `gen_device_secret.py` 预计算所有 `dev_id` 的 secret，认证时重算比对，不存每设备明文 secret。
#### 本地配置接口

WiFi SSID/密码、MQTT broker 地址、dev_id 等信息出厂时写入默认值（产线烧录），支持现场通过以下方式修改：

| 配置方式 | 接口 | 场景 |
|---------|------|------|
| 调试串口 CLI | STM32 UART (预留 GPIO) | 产线调试、现场维护 |
| 人机界面 (HMI) | 本地显示屏 + 按键 / 触摸 | 有屏设备现场配置 |

存储位置：参数区 (0x08058000)，OTA 不擦除，掉电保持。

配置流程：
  设备上电 -> 读取参数区配置
    - 配置有效 (CRC 通过) -> 使用现有配置
    - 配置无效 (首次上电/损坏) -> 使用编译期默认值 -> 初始化参数区

  进入配置模式:
    调试串口: 上电时检测特定引脚电平 或 收到配置指令
        -> CLI 交互: set wifi_ssid xxx / set mqtt_host xxx / save / reboot
    HMI: 系统菜单 -> 网络设置 / 设备设置 -> 修改 -> 保存 -> 重启

配置项列表：

| 配置项 | 默认值来源 | 可修改 | 说明 |
|--------|-----------|--------|------|
| wifi_ssid | 产线烧录 | 是 | WiFi AP 名称 |
| wifi_password | 产线烧录 | 是 | WiFi 密码 |
| mqtt_host | 产线烧录 | 是 | MQTT broker DNS 域名 |
| mqtt_port | 编译期 (8883) | 是 | MQTT TLS 端口 |
| dev_id | 产线烧录 | 否 (出厂后锁定) | 设备唯一标识 |

> dev_id 出厂后锁定不可修改——它与服务器 devices 表和设备认证 secret 绑定，修改会导致设备无法认证。

#### 本地配置接口

WiFi SSID/密码、MQTT broker 地址、 等信息出厂时写入**默认值**（产线烧录），支持现场通过以下方式修改：

| 配置方式 | 接口 | 场景 |
|---------|------|------|
| **调试串口 CLI** | STM32 UART (预留 GPIO) | 产线调试、现场维护 |
| **人机界面 (HMI)** | 本地显示屏 + 按键 / 触摸 | 有屏设备现场配置 |

**存储位置**：参数区（），OTA 不擦除，掉电保持。

**配置流程**：


**配置项列表**：

| 配置项 | 默认值来源 | 可修改 | 说明 |
|--------|-----------|--------|------|
|  | 产线烧录 | 是 | WiFi AP 名称 |
|  | 产线烧录 | 是 | WiFi 密码 |
|  | 产线烧录 | 是 | MQTT broker DNS 域名 |
|  | 编译期 () | 是 | MQTT TLS 端口 |
|  | 产线烧录 | **否** (出厂后锁定) | 设备唯一标识 |

>  出厂后锁定不可修改——它与服务器  表和设备认证  绑定，修改会导致设备无法认证。




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

Bootloader 读标志 -> 先校验新固件 HMAC（verify-before-write，失败则不动片内、保留当前 App）-> 备份片内 App 到片外备份区 -> 擦片内 -> 解密写片内 -> 跳转



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

| `secret_hash` | VARCHAR(64) | HMAC-SHA256(master_device_key, dev_id) 的 hex，EMQX auth 验证用 |

| `name` | VARCHAR(128) | 设备名 |

| `group_id` | INT | 分组 ID |

| `location` | VARCHAR(128) | 安装位置 |

| `mcu_version` | VARCHAR(32) | STM32 固件版本 |

| `esp_version` | VARCHAR(32) | ESP-07S 固件版本（预留） |

| `bootloader_version` | VARCHAR(32) | Bootloader 版本（只读） |

| `last_seen` | DATETIME | 最后在线时间（EMQX 规则引擎实时更新） |

| `status` | ENUM | online/offline/upgrading/archived |

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

| `file_size` | INT | **加密 blob** 字节数（= IV 16B + 密文 + HMAC 32B；本地加密工具输出值） |

| `iv` | BINARY(16) | AES-CTR 初始向量（本地加密时随机生成；管理员录入，仅作审计） |

| `hmac` | BINARY(32) | HMAC-SHA256（覆盖 IV + 密文；本地加密时计算；管理员录入，仅作审计） |

| `release_notes` | TEXT | 发布说明 |

| `status` | ENUM | draft/released/archived |

| `created_at` | DATETIME | |



> 说明（方案 A + 本地离线加密）：固件**在本地离线加密**（`tools/encrypt_firmware.py`），密文 blob（`IV+密文+HMAC`）由管理员上传 OSS 后在后台录入元数据；同一版本的所有 OTA 任务复用此 blob，不重复加密。设备端从 blob 固定偏移读取 IV/HMAC，不依赖 MySQL 这两列——此处两列仅供服务端审计。



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

POST   /api/v1/firmwares              # 上传加密 blob (multipart) + 元数据 (version/iv/hmac)

GET    /api/v1/firmwares              # 列表

GET    /api/v1/firmwares/{id}         # 详情（含 file_size/版本号/iv/hmac）

POST   /api/v1/firmwares/{id}/release # 发布（标记为 released，供 OTA 任务选择）

DELETE /api/v1/firmwares/{id}



# 设备管理

GET    /api/v1/devices                # 列表（按 version/group/status 过滤）

GET    /api/v1/devices/{id}           # 详情（含升级历史）

PUT    /api/v1/devices/{id}           # 更新元数据（name/location/group_id/sn）

DELETE /api/v1/devices/{id}           # 注销设备（标记 archived，不物理删除）



# OTA 任务

POST   /api/v1/ota/tasks              # 创建任务（含策略）

GET    /api/v1/ota/tasks              # 列表

GET    /api/v1/ota/tasks/{id}         # 详情（含每设备进度）

POST   /api/v1/ota/tasks/{id}/pause   # 暂停

POST   /api/v1/ota/tasks/{id}/cancel  # 取消

GET    /api/v1/ota/stats              # Dashboard 统计



# 告警规则管理

GET    /api/v1/alert-rules              # 列表

POST   /api/v1/alert-rules              # 创建

PUT    /api/v1/alert-rules/{id}         # 编辑

DELETE /api/v1/alert-rules/{id}         # 删除



# 设备分组管理

GET    /api/v1/device-groups              # 列表

POST   /api/v1/device-groups              # 创建

PUT    /api/v1/device-groups/{id}         # 编辑

DELETE /api/v1/device-groups/{id}         # 删除

```



### 设备端接口



```

POST   /api/v1/device/register        # 首次上线注册（一机一密）

POST   /api/v1/device/heartbeat       # 心跳上报（HTTP 兜底；主心跳走 MQTT，见下注）

GET    /api/v1/device/ota/check       # 主动检查更新（MQTT 丢失兜底，返回签名 URL）

POST   /api/v1/device/ota/report      # 上报升级结果

```



> **心跳主路径是 MQTT，不是 HTTP**：STM32 每 60s 经 UART CMD 0x0A 发心跳 → ESP-07S publish 到 `device/heartbeat/{dev_id}`（§5）→ EMQX 规则引擎更新 `devices.last_seen`（§15.3）。`POST /api/v1/device/heartbeat` 仅作 MQTT 长时间不可用时的 HTTP 兜底上报，两者更新的是同一个 `last_seen` 字段，不重复计时。



> **固件下载（直连 OSS）**：固件字节流**不经过 FastAPI**。后端生成 OSS 限时签名 URL（5min 过期，期内可多次 Range 请求）随 MQTT 下发（或在 `/ota/check` 返回），ESP-07S 直接从 OSS 拉流，OSS 原生支持 Range 断点续传。FastAPI 不经手固件内容，仅负责签发 URL 与接收结果上报，避免万级并发下载压垮单台 ECS。（注：此处"直连 OSS"是下载路径决策，与 §7.1 的"方案 A"blob 存储约定是两件独立的事。）



> **固件加密（本地离线）**：加密在本地离线完成（`tools/encrypt_firmware.py`），服务器**不持有主密钥**、**不持有明文固件**。管理员本地加密后将 blob 上传 OSS，然后在管理后台创建固件记录（录入 version / file_size / iv / hmac / oss_key）。服务器只负责存储和转发 blob。



### 鉴权



- 设备端：一机一密（`dev_id` + `secret` 烧录到 flash），MQTT username/password 用之

- 前端：JWT（`POST /api/v1/auth/login` 用 username/password 换取 token，后续请求带 `Authorization: Bearer <token>`）



### 设备认证与自动注册



设备首次上线无需显式调 `POST /api/v1/device/register`，而是通过 **EMQX 认证钩子自动注册**：



1. **产线预置**：`dev_id`（uint32）与 `secret` 在出厂时同时烧录到 STM32 flash 并录入服务端 `devices` 表（`dev_id` + `secret_hash`）。`secret` 可由服务端主设备密钥按 `HMAC-SHA256(master_device_key, dev_id)` 派生，这样服务端不存每设备明文 secret，验证时重算比对即可。

2. **ESP-07S 连接 MQTT**：username = `dev_id` 十进制串，password = `secret` 的 hex 字符串。

3. **EMQX auth 钩子**：收到 CONNECT 后，查 `devices` 表（或计算派生 secret）验证 username/password。通过则允许连接。

4. **自动注册**：若 `dev_id` 不在 `devices` 表中，EMQX 钩子自动 `INSERT` 一条初始记录（`dev_id` + `secret_hash` + 默认分组 + `status=offline` + `created_at=NOW()`），首次心跳到达后更新为 `online`。



> 这种方式无需设备主动调 HTTP 注册接口，出厂烧录即上线。`POST /api/v1/device/register`（§4）保留用作手动注册/预录入的便捷入口。



### 实时监控接口（WebSocket）



设备实时状态监控页面（§21）通过 WebSocket 与 FastAPI 建立会话，FastAPI 居中协调查询/应答。**监控数据不落库、只实时透传给浏览器**（用户明确要求：监控信息只看、不存）。



**WebSocket 端点**：`WS /api/v1/ws/monitor/{dev_id}`（JWT 鉴权，同一设备同时只允许一个监控会话）



**浏览器 -> 服务端**消息：

```

{"action":"start"}                                  # 启动监控（服务端开始每秒 5 条查询）

{"action":"stop"}                                   # 停止监控

{"action":"get_config","type":"net_config"}         # 手动读取某一种配置

```



**服务端 -> 浏览器**消息：

```

{"kind":"status","type":"run_state","ok":true,"data":{...},"ts":1721712000123}   # 实时状态应答

{"kind":"status","type":"run_state","ok":false,"reason":"timeout"}                # 该条查询超时（>900ms 无应答）

{"kind":"config","type":"net_config","ok":true,"data":{...}}                      # 配置读取应答

{"kind":"config","type":"net_config","ok":false,"reason":"timeout"}

{"kind":"session","running":true}                                                  # 会话状态变更通知

```



**服务端行为**：

- 收到 `start` -> 启动 1 秒间隔定时器，每 tick 向 `cmd/{dev_id}/query` 下发 **5 条查询**（5 种 `type` 各一条，每条带唯一 `qid`），同时订阅 `device/query/resp/{dev_id}`；按 `qid` 匹配应答，900ms 内无应答则判超时，推 `ok:false`。

- 收到 `get_config` -> 向 `cmd/{dev_id}/config_get` 下发 1 条（带 `qid`），订阅 `device/config/resp/{dev_id}` 匹配应答，同样 900ms 超时。

- 收到 `stop` 或 WebSocket 断开 -> 停定时器、退订、清理。

- 数据全程内存中转发，**不写 TDengine、不写 MySQL**。



**5 种实时状态查询类型**（CMD 0x20 的 `query_type`，应答 data < 512B JSON）：



| query_type | 含义 | 应答 data 内容示例 |

|---|---|---|

| `run_state` | 运行态 | `{"cpu_load":62,"mem_free":4096,"uptime":863400,"loop_rate":99}` |

| `comm_stat` | 通信统计 | `{"uart_rx":1234567,"uart_tx":987654,"uart_err":3,"rssi":-58,"mqtt":"online"}` |

| `sensor_snap` | 传感器快照 | `{"temp":25.3,"hum":68.2,"pressure":101.3,...}`（当前各测点值） |

| `ota_state` | OTA 状态 | `{"ota_state":"IDLE","cur_version":"1.2.0","receive_offset":0}` |

| `power_stat` | 电源状态 | `{"volt_in":220.1,"current_a":1.25,"mcu_temp":41,"wdg_reset_cnt":0}` |



**5 种配置读取类型**（CMD 0x22 的 `config_type`，应答 data < 512B JSON）：



| config_type | 含义 | 应答 data 内容示例 |

|---|---|---|

| `net_config` | 网络配置 | `{"wifi_ssid":"shop-a","mqtt_host":"mqtt.example.com","mqtt_port":8883}` |

| `report_config` | 上报配置 | `{"report_interval_ms":5000,"sample_period_ms":1000,"sensors":"temp,hum,volt_in"}` |

| `threshold_config` | 告警阈值 | `{"temp":{"op":">","val":80,"dur":30},"volt_in":{"op":"<","val":200,"dur":10}}` |

| `time_config` | 时间配置 | `{"rtc_time":"2026-07-24T10:00:00","ntp":"ntp.aliyun.com","sync_h":24}` |

| `sys_config` | 系统配置 | `{"dev_id":10001,"fw_version":"1.2.0","boot_version":"1.0.0","debug":0}` |



---



## 5. MQTT 主题设计



```

ota/cmd/{dev_id}                # 下发升级指令（单设备）

  payload: {"task_id":"...","target":"mcu","version":"1.2.0",

            "url":"https://oss.../fw.bin?token=...","size":123456}

  # IV/HMAC 在密文 blob 内（方案 A），不下发；设备端从 blob 固定偏移读取



ota/cmd/group/{group_id}           # 分组广播



device/heartbeat/{dev_id}       # 设备心跳（非 retained：心跳是瞬态数据，retained 会保留过时值；离线判断用 EMQX 连接状态或 devices.last_seen 字段）

device/status/{dev_id}          # 通用状态上报（在线/版本等）

device/ota/progress/{dev_id}    # OTA 进度上报（下载/升级过程中周期性）

  payload: {"task_id":"...","state":"downloading","download_offset":123456,"progress":56}

device/ota/result/{dev_id}      # 升级最终结果上报（success/failed）

device/config/ack/{dev_id}      # 配置下发结果上报（ESP 收到 STM32 CMD 0x13 后转发）

  payload: {"result":0}         # 0=ok/1=parse_err/2=unsupported，对应 §9 CMD 0x13



cmd/{dev_id}/query              # 实时状态查询（云→设备，监控页面每秒下发 5 条）

  payload: {"qid":"<uuid>","type":"run_state"}     # type ∈ 5 种实时状态（见 §4 实时监控接口）

device/query/resp/{dev_id}      # 实时状态应答（设备→云），data < 512B

  payload: {"qid":"<uuid>","type":"run_state","data":{...}}



cmd/{dev_id}/config_get         # 配置读取（云→设备，监控页面手动触发）

  payload: {"qid":"<uuid>","type":"net_config"}    # type ∈ 5 种配置（见 §4 实时监控接口）

device/config/resp/{dev_id}     # 配置读取应答（设备→云），data < 512B

  payload: {"qid":"<uuid>","type":"net_config","data":{...}}

```



> **OTA 进度回传**：下载/升级过程中，ESP-07S 周期性（如每收 N 个块或每秒，取较疏者）publish `device/ota/progress/{dev_id}`，携带 `task_id + state + download_offset + progress`。云端（FastAPI 订阅或 EMQX 规则引擎）据此更新 `ota_task_records` 的 `download_offset/progress/status`，驱动 §6 实时进度看板。`download_offset` 由 ESP 从 HTTP 下载进度取得（STM32 写入进度经 CMD 0x06 ACK 的 offset 回传）。最终成败经 `device/ota/result`（对应 CMD 0x09）上报。



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

| **系统设置** | 设备密钥导出、MQTT/OSS 连接配置。用户/角色/权限管理属企业级能力（§25 Phase 13），MVP 阶段简化为单管理员 JWT 登录 |



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

0x0800_0000  Bootloader   (48KB = 24页)  永不升级，出厂烧录



> Bootloader 区尾部 256B 为密钥区 (0x0800BF00~0x0800BFFF)：前 136B 随机填充 + AES_KEY(32B @0x0800BF88) + HMAC_KEY(32B @0x0800BFA8) + 后 56B 随机填充。Bootloader 编译时 __at() 写入，App 运行时指针读取。

0x0800_C000  加密ID标记区  (2KB = 1页)   首次上电标记 (0x1234, uid_flag.c 编译期放置)

0x0800_C800  加密ID区      (2KB = 1页)   UID绑定生成的设备指纹 (16B @页内偏移128B)

0x0800_D000  保留区1      (12KB = 6页)   预留

   0x0800_F800  APP_INFO     (128B)         fw_version + master_device_key + 预留

0x0801_0000  App          (288KB = 144页) OTA 目标

0x0805_8000  参数区        (96KB = 48页)  状态机 + 版本 + 升级标志

0x0807_0000  保留区2       (64KB = 32页)  预留



0x00_0000  新固件头     (4KB)    magic + size + version + receive_offset

0x00_1000  新固件区     (512KB)  完整密文 blob：[IV 16B][密文][HMAC 32B]

0x08_1000  备份固件头   (4KB)    backup_magic + size + version

0x08_2000  备份固件区   (512KB)  完整密文 blob：[IV 16B][密文][HMAC 32B]

0x10_2000  保留

```



#### 固件版本常量 (APP_INFO)



App 固件中定义 16-bit 版本号常量，编译期写入 `0x0800F800`（保留区 1 内），由分散加载文件的独立段定位。



**格式**：高 8 位 = 主版本，低 8 位 = 次版本。例 `0x0102` = v1.02。



**实现**：

```c

// app_info.c

const app_info_t APP_INFO

    __attribute__((section(".app_info")))

    = FW_VER_VALUE;       // 宏展开为 ((MAJOR << 8) | MINOR)

```



**分散加载**：

```

LR_APP_INFO 0x0800F800 0x00000004 {

  ER_APP_INFO 0x0800F800 0x00000004 {

    *.o (.app_info)          // 只捕获版本常量

  }

}

```



**用途**：

- 产线工具 (`factory_tool.py`) 从 HEX 自动读取版本号，无需人工填写

- Bootloader 可通过读 `0x0800F800` 获取编译期版本（fallback）

- 服务器可校验 flash 镜像中此位置确认固件版本



**与 OTA blob 的关系**：

- APP_INFO 随 App HEX 一起进入加密 blob (合并所有段, 间隙填 0xFF)
- OTA 升级后 APP_INFO 自动更新为新版本号


#### 分散加载：debug vs updata_app



App 工程有两个 target，分散加载文件不同：



**debug target** (`debug.sct`) — 调试用，三段布局：



| 段 | 地址 | 大小 | 内容 |

|---|---|---|---|

| LR_VECTOR | 0x08000000 | 1KB | 向量表 + __scatterload 启动代码 |

| LR_APP_INFO | 0x0800F800 | 4B | 固件版本常量 |

| LR_IROM1 | 0x08010000 | 288KB | 代码 + RO + XO |



```

硬件复位 → SP/PC 从 0x08000000 取 → 向量表已就位 → 正常启动

            ┌─ VECT_TAB_OFFSET = 0x0000

无需 Bootloader，可直接调试。调试完后需重新烧录 Bootloader 恢复 OTA。

```



**updata_app target** (`updata_app.sct`) — 升级固件用：



| 段 | 地址 | 大小 | 内容 |

|---|---|---|---|

| LR_APP_INFO | 0x0800F800 | 4B | 固件版本常量 |

| LR_IROM1 | 0x08010000 | 288KB | 代码 + RO + XO + 向量表 |



```

Bootloader → 设置 SCB->VTOR = 0x08010000 → 跳转 App

             ┌─ VECT_TAB_OFFSET = 0x10000

向量表在 App 区起始，无 Bootloader 时硬件复位会崩溃（预期行为）。

```



### 7.2 App OTA 模块（新增核心）



App 运行时挂一个 OTA 后台任务，主业务不阻塞：



```

UART DMA 接收 + 空闲中断 -> 解帧 -> OTA 状态机



状态机（存在片内参数区）:

  IDLE → DOWNLOADING → DOWNLOADED → UPGRADE_REQUESTED → (软复位) → UPGRADING → IDLE

  干净失败（CRC 错/写 flash 错/重传超限）→ IDLE + 上报错误

  下载中途断电（非干净失败）→ 状态保持 DOWNLOADING，receive_offset 已在片外固件头持久化

                              → 重启后 ESP 查 offset 用 Range 续传，不回 IDLE



  **回滚 (立即生效)**：

  App 收到 CMD 0x0B "立即回滚" → state = ROLLBACK_REQUESTED → NVIC_SystemReset()

  Bootloader 启动后检测到 ROLLBACK_REQUESTED → boot_rollback():

    ① 检查 W25Q64 备份区 magic

    ② HMAC 验签备份 blob

    ③ 擦除片内 App 区

    ④ AES-CTR 解密 + 写回片内

    ⑤ state = IDLE, result = ROLLBACK_OK → 跳转旧 App

  备份不存在/HMAC 失败 → result = ROLLBACK_FAIL → 跳转当前 App



  **DOWNLOADED 状态滞留处理**：

  - 场景: 固件下载完成但管理员未下发"立即升级"指令, 或"立即升级" MQTT 消息丢失

  - 场景: DOWNLOADED 期间有新版本固件下发 → App 允许覆盖, 重置 state→DOWNLOADING 重新接收

  - App 启动时检测到 state == DOWNLOADED → 经 CMD 0x09 上报服务器 `status=downloaded, awaiting upgrade command`

  - 服务器据此在管理后台显示"待升级"状态, 提醒管理员手动触发或排查



#### OTA 下载全流程 (App 侧)



**阶段 1 — 开始下载 (收到 CMD 0x03)**：



  1. 解析 task_id + version + size (blob 总字节数)

  2. **擦除 W25Q64 旧固件头**（确保新任务从 offset=0 开始，不会残留旧任务的 receive_offset）

  3. 写参数区：task_id、new_version、state = DOWNLOADING

  4. 写 W25Q64 fw_header: magic="FWHD", size, version, receive_offset=0

  5. 回 CMD 0x04 ACK: start_offset = 0



**阶段 2 — 接收固件块 (循环 CMD 0x05)**：



  1. 解析 offset + data → 写 W25Q64 固件区基地址 + offset

  2. receive_offset = offset + len → 更新 W25Q64 fw_header

  3. 回 CMD 0x06 ACK (含新 receive_offset, ESP 用于进度上报)



**阶段 2 续 — 掉电重启后续传**：



  1. Bootloader 见 state=DOWNLOADING → 不干预 → jump_to_app

  2. App 启动: state=DOWNLOADING → 读 W25Q64 receive_offset

  3. 拼 CMD 0x04 ACK (start_offset=receive_offset) → ESP 发起 Range 续传

  4. 回到阶段 2 循环



**阶段 3 — 下载完成 (收到 CMD 0x07)**：



  1. 校验 receive_offset == fw_header.size

  2. 通过: state = DOWNLOADED → 回 ACK → ESP 上报云"待升级"

  3. 失败: 上报错误, state = IDLE

  (HMAC-SHA256 密码学校验由 Bootloader 负责，App 不持有密钥)



**阶段 4 — 触发升级 / 回滚**：



```

收到"立即升级"(CMD 0x08):

  state = UPGRADE_REQUESTED → NVIC_SystemReset()



收到"立即回滚"(CMD 0x0B):

  state = ROLLBACK_REQUESTED → NVIC_SystemReset()

```



**App 启动时（每次）**：



  1. 读 upgrade_result: != 0 → CMD 0x09 上报云(task_id+result+version) → 清零

  2. 读 state:

     - UPGRADING + app_healthy=0 → App 健康 → 写 app_healthy=1, state=IDLE

     - DOWNLOADED → CMD 0x09 上报 "awaiting upgrade command"

     - DOWNLOADING → 回到阶段 2 续传输



**关键实现**：

- UART 用 DMA + 空闲中断收变长帧，不阻塞主循环

- W25Q64 写入按扇区擦除（4KB），维护"已擦除扇区位图"

- **收到 CMD 0x03 必须先擦除旧固件头**，否则不同任务的 receive_offset 会错乱

- `receive_offset` 每块更新前先写后校验，防掉电丢失



### 7.3 Bootloader（集中升级职责）



Bootloader 不涉及 UART 协议，集中负责备份片内、读片外新固件、写片内、校验、跳转、回滚全流程：



```

上电:

  1. 读参数区状态

  2. if state == UPGRADE_REQUESTED:

     a. 读片外新固件头: size（blob 总长，含 IV+密文+HMAC）；若读取失败（SPI 超时等），写 upgrade_result=4，state = IDLE，跳转当前 App

        * IV    = 新固件区[0 : 16]            # blob 首 16B

        * HMAC  = 新固件区[size-32 : size]    # blob 末 32B

        * 密文  = 新固件区[16 : size-32]

     b. 【先验签 verify-before-write】流式读 blob [IV + 密文]，增量算 HMAC-SHA256，比对 blob 末 32B：

        * 失败 -> 片内 flash 未动（当前 App 完好），写 upgrade_result=2，state = IDLE，跳转当前 App（无需回滚）

        * 通过 -> 继续

     c. 验签通过后，检查片外备份区 magic:

        - magic 不存在: 备份片内 App (加密备份)

          * 生成随机备份 IV

          * 流式: 读片内 App 1KB (明文) -> AES-256-CTR 加密 (主密钥+备份IV) -> 写片外备份区 1KB (密文)

          * 计算备份 HMAC, 拼接备份 blob [IV + 密文 + HMAC] 写入备份区

          * 写备份固件头 (backup_magic + size + version)

          * 写 magic

          * **备份过程中任何一步失败 -> 写 upgrade_result=3，state = IDLE，跳转当前 App（片内 App 未动，完整无损）

        - magic 存在: 跳过备份 (上次已备份,这是断电重入)

     d. 擦除片内 App 区

     e. 流式解密写入:

        * 读新固件区 1KB 密文 (从偏移 16 开始)

        * AES-256-CTR 解密 (主密钥 + 新固件区 IV) -> 1KB 明文

        * 写片内 flash 1KB 明文

        * 循环

        * 写入失败 -> 从备份区读密文 blob -> 解密 (备份 IV, 在备份区[0:16]) -> 写片内明文 -> 写 upgrade_result=5 -> state = IDLE -> 跳转

     f. 全部成功: 写 upgrade_result=1, state = UPGRADING, new_version 移入 cur_version, 清备份 magic, 启动 IWDG(30s), 跳转

     （验签已在 b 前置完成；c~e 任一步掉电，重启后从 b 重新验签→续做，全程幂等）

  3. elif state == UPGRADING (上次刚升级完,等 App 确认):

     - Bootloader 跳转前已启动 IWDG 独立看门狗 (30s)

     - 新 App 正常启动后写 "app_healthy" 标志并喂狗; Bootloader 不再运行

     - App 未在 30s 内写 app_healthy -> IWDG 超时复位 MCU

     - 复位后 Bootloader 见 state==UPGRADING:

       * app_healthy == 0: App 未确认（启动失败）-> 从备份区回滚 -> 写 upgrade_result=6 -> state = IDLE -> 跳转

       * app_healthy == 1: App 已确认但断电在清标志前 -> 判定健康 -> 清 app_healthy, state = IDLE -> 跳转

     - App 正常确认后清 app_healthy, state = IDLE (回到正常态)

  4. else: 直接跳转 App



跳转 App:

  SCB->VTOR = 0x0800C000;          // 重定位中断向量

  __set_MSP(*(uint32_t*)0x0800C000); // 设置栈指针

  ((void(*)())(*(uint32_t*)0x0800C004))(); // 跳转

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

[IV (16B, 每版本发布时随机生成一次)] + [AES-256-CTR 密文] + [HMAC-SHA256 (32B)]

```

HMAC 计算范围：`IV || Encrypted_Data`，确保 IV 不可篡改。

密文首字节对应 flash 地址 `FW_WRITE_ADDR = 0x0800F800` (Bootloader 硬编码)。



**本地离线加密流程**（发布时加密一次，blob 按版本复用）：



> **安全原则：服务器不持有主密钥，不持有明文固件。加密完全在本地离线完成。**



1. 管理员在本地运行 `tools/encrypt_firmware.py`：

   ```

   python encrypt_firmware.py app_v1.2.bin --keys keys.json -o app_v1.2_blob.bin

   ```

2. 脚本执行：

   - 生成随机 IV (16B) —— 每个固件版本仅生成一次

   - AES-256-CTR 加密固件（主密钥 + IV）

   - 计算 HMAC-SHA256(主密钥, IV || 密文)

   - 拼接加密 blob：`IV + 密文 + HMAC`

   - 输出 blob 文件 + 打印 IV / HMAC hex / file_size

3. 管理员将加密 blob 上传 OSS

4. 在管理后台创建固件记录，填入 version + file_size + iv + hmac + oss_key

5. 服务器存储和转发 blob，**不解密、不持有密钥**



> **复用约定**：同一固件版本只加密一次，其密文 blob 被所有针对该版本的 OTA 任务复用（任务不重新加密）。CTR 模式下，同版本 = 同明文，用固定 (主密钥, IV) 重复加密得到相同密文，不泄露额外信息，密码学上安全；因此 IV 只需保证"不同版本互不相同"（每次发布随机生成即可）。



**STM32 端加解密职责**：

| 阶段 | 操作 | 密钥持有者 |

|---|---|---|

| App 接收固件块 | UART 收密文 blob 字节 -> 原样写片外固件区 | App 无密钥，纯转发写入 |

| App 接收完成 | `receive_offset == size` 完整性校验（非密码学） | App 无密钥 |

| Bootloader 升级 | **先校验** blob HMAC（IV 首16B + 末32B），**通过后才**解密写片内明文（verify-before-write） | Bootloader 持有 AES + HMAC key |

| Bootloader 备份 | 读片内明文 -> AES 加密 -> 拼备份 blob 写备份区 | Bootloader 持有 AES + HMAC key |

| Bootloader 回滚 | 读备份区 blob：IV+HMAC 校验，密文区解密写片内明文 | Bootloader 持有 AES + HMAC key |



**性能估算（STM32F103VE @72MHz 软件 AES）**：

- AES-256-CTR 软件实现：~50-80 KB/s

- HMAC-SHA256 软件实现：~150 KB/s

- ~288KB（App 区上限）加密或解密：3-5s

- 单次升级增加耗时：6-10s（备份加密 3-5s + 升级解密 3-5s）

- 总升级时间：约 15-25s（含 UART 传输 + flash 读写），可接受



**安全性分析**：

- ✓ 片外 flash 是密文：攻击者读片外 flash 无法获取固件

- ✓ 传输全程加密：HTTPS（外层）+ 固件加密（内层），双重保护

- ✓ 片内 App 区是明文：但 App 不含密钥，App 被反编译无密钥泄露

- ✓ Bootloader 含主密钥：RDP Level 1 保护，SWD 不可读

- ✓ **服务器不持有主密钥**：加密在本地离线完成，服务器被入侵不会泄露密钥

- ✓ **服务器不持有明文固件**：OSS 只存加密 blob，明文仅在管理员本地

- ⚠ 剩余风险：主密钥所有设备共用，单设备被深度攻破（侧信道攻击）则全设备固件可被解密逆向。但 UID 绑定（§7.5）已阻止批量克隆——换了芯片 UID 不同，加密 ID 不匹配，App 拒绝运行。攻击者要同时具备物理访问 + 侧信道能力，门檻极高。如需更高安全（如固件 IP 保护），未来可升级为每设备一密钥（OTP 存储）



#### 产线工具 (tools/)



Windows GUI 产线工具 `tools/factory_tool.py`，三个 Tab：加密 | 合并 | 密钥。



**文件命名规范**：

- Bootloader HEX：`BT_<设备型号>_xxx.hex`（如 `BT_XL800.hex`）

- App HEX：`APP_<设备型号>_<版本>.hex`（如 `APP_XL800_1.21.hex`）

- 版本号从 HEX 内容 `0x0800F800` 读取，不依赖文件名



**1. 加密 Tab**：

- 选择 `APP*.hex` → 自动读 `0x0800F800` 获取版本号 → 自动检测型号

- 非 `APP*` 开头文件名拦截报错

- 多段 HEX 自动合并（间隙填 0xFF，从 0x0800F800 起）→ AES-256-CTR + HMAC-SHA256 → 输出 `UP_<型号>_V<版本>.bin`

- 中间明文文件：`UP_<型号>_V<版本>_no_encry.bin`（可校验加密前内容）

- 密钥未加载时提示



**2. 合并 Tab**：

- 一个浏览按钮，Ctrl+多选 2 个文件（`BT*` + `APP*`），且只能选 2 个

- 选文件时即时校验：缺少 BT/APP 前缀、设备型号不一致 → 弹窗提示重新选择

- BIN/HEX 格式可选，填充字节可选 0xFF/0x00

- BIN 输出到 App 区末尾（~350KB），HEX 跳过间隙

- 输出命名：`FA_<型号>_V<版本>.bin` (或 .hex)

- 日志显示 bootloader 段（BL 代码 + 加密ID标记 4B）+ app 段（固件版本 + App 代码）

- `0x0800C000` UID 标记由 Bootloader `uid_flag.c` 编译期生成，合并工具无需处理



**3. 密钥 Tab**：

- 进入 Tab 需密码解锁（225219），离开后自动上锁

- 显示/隐藏密钥内容需再次输入密码

- 密钥文件路径 + 所有文件选择路径自动记忆（`.factory_tool_config.json`），下次启动自动恢复



**配套脚本**：

| 脚本 | 用途 |

|---|---|

| `gen_master_keys.py` | 生成 AES + HMAC 主密钥对 |

| `encrypt_firmware.py` | 命令行加密 `app.bin` → OTA blob |

| `hex_utils.py` | Intel HEX 解析/合并/版本读取核心库 |



**主密钥管理**：



产线一次性生成三把密钥（`gen_master_keys.py`）：



| 密钥 | 长度 | 存储位置 | 用途 |

|------|------|---------|------|

| `aes_key` | 32B | `bootloader/src/crypto.c`, `keys.json` | AES-256-CTR 固件加密/解密 |

| `hmac_key` | 32B | `bootloader/src/crypto.c`, `keys.json` | HMAC-SHA256 固件签名/验签 |

| `master_device_key` | 32B | 服务器环境变量, `keys.json` | 派生每台设备 MQTT/HTTP 认证 secret |



**设备认证密钥派生**（`gen_device_secret.py`）：

```python

secret = HMAC-SHA256(master_device_key, dev_id_le)[:16].hex()

# dev_id 10001 → secret = "e96d17a7a47a9fff..."

```



- `secret` 在产线烧录到 STM32 参数区 + ESP flash

- `secret_hash` 写入服务器 `devices` 表 (或服务端重算比对, 不存明文)

- 设备 MQTT CONNECT: username=`dev_id`, password=`secret`

- 设备 HTTP 调用: body 带 `dev_id` + `secret`

- `keys.json` 禁止提交 git（`.gitignore` 已排除）



### 7.5 设备防克隆（UID 绑定）



**目标**：阻止全片 flash 内容被暴力复制到另一台设备上正常运行。即攻击者通过 SWD/JTAG 或物理拆片获取某台合法设备的完整固件内容 → 烧录到另一台设备 → 设备拒绝运行。



**原理**：利用 STM32F1 片内 **96-bit 唯一 ID**（`0x1FFFF7E8`）生成设备指纹，Bootloader 首次上电时计算并写入加密 ID。App 运行时重新计算比对，不匹配则反复复位。



**Flash 布局**（512KB 尾部，Bootloader 和 App 均不占用）：



| 偏移 | 绝对地址 | 所在 2KB 页 | 内容 | 说明 |

|---|---|---|---|---|

| 48KB+2KB = 0x0800C000):

     a. 读 UID (0x1FFFF7E8, 12B)

     b. 加密ID = HMAC-SHA256(主密钥, UID) 取前 16B

     c. 写加密ID 到 0x0800C800

     d. 擦除 � 所在页 (整页变 0xFF)

     e. 进入正常 Bootloader 流程

  3. else (标记 == 0xFFFFFFFF 或中间态):

     → 加密ID 已生成，跳至正常流程



App 每次启动:

  1. 读 UID → 重复计算加密ID = HMAC-SHA256(主密钥, UID)

  2. 读 0x0800C800 处存储的加密ID → 比对

  3. 不匹配 → NVIC_SystemReset() 软复位（反复重启，拒绝运行）

  4. 匹配 → 继续正常业务

```



**断电/异常处理**：



| 场景 | 结果 | 幂等性 |

|---|---|---|

| 加密 ID 已写入、标记**被擦除前**断电 | 下次上电标记仍为 0x1234 → 重新生成加密 ID（同 UID 同结果）| ✅ |

| 加密 ID 已写入、标记**已被擦除**后断电 | 下次上电标记=0xFFFF → 跳过生成 | ✅ |

| 标记页擦除中途断电 | 标记值不确定（可能既≠0x1234 也≠0xFFFF）→ Bootloader `else` 兜底，相信加密 ID 已写入 | ✅ |



**安全性**：



| 攻击 | 结论 |

|---|---|

| 复制全片 BIN 到另一台设备 | App 验证：加密 ID ≠ 新设备 UID 计算值 → 反复重启 |

| 反汇编 App 获取验证算法 | 算法可见，但主密钥在 Bootloader 区 (RDP L1)，无法计算正确加密 ID |

| 修改 App 跳过验证逻辑 | 攻击者需完整替换 App——但 App 是密文传输的，获取解密后明文 App 的前提是已经攻破 Bootloader |

| 替换 Bootloader + App 全部 | 攻击者可绕过——但需物理访问 + 精通 ARM 汇编重写 Bootloader 跳转逻辑，成本极高 |



**涉及的文件**：

| 文件 | 职责 |

|---|---|

| `bootloader/src/uid_flag.c` | 编译期放置 `0x00001234` @0x0800C000（`__attribute__((at))`） |

| `bootloader/src/boot_main.c` | `uid_bind_first_run()` — 检测 0x1234 → HMAC-SHA256(UID) → 写加密 ID → 擦标记页 |

| `envir-control/app/uid_verify.c` | App 启动时 `uid_verify()` — 重算 HMAC → 比对加密 ID → 不匹配则复位 |



---



## 8. ESP-07S 端设计（流式转发）



### 启动与网络初始化



ESP-07S 上电后按以下顺序初始化（任何一步失败则重试，重试间隔递增，最大 60s）：



```

1. 连接 WiFi（SSID/密码**产线烧录**到 ESP flash，与 dev_id+secret 同一批次预置）

2. 连接 MQTT broker（地址/端口**产线烧录**到 ESP flash 中的 DNS 域名，如 mqtt.example.com:8883）

3. 以 dev_id + secret 做 MQTT CONNECT 认证（详见 §4 设备认证与自动注册）

4. 订阅主题：ota/cmd/{dev_id}（QoS 1）、cmd/{dev_id}/config（QoS 1）

5. 发布 device/heartbeat/{dev_id}（payload: `{"state":"booting","version":"0.0.0"}`）宣告上线——此时 ESP 尚未从 STM32 收到心跳帧，state/version 用占位符；STM32 随后发来 CMD 0x0A 心跳后，ESP 转发到同一 topic 并载入真实值

6. 进入主循环：UART 收帧 → 按 CMD 分发处理

```



> WiFi 断线时 ESP 自动重连 MQTT，重连成功后重新订阅并发布心跳。WiFi/MQTT 重连期间，STM32 持续发来 CMD 0x0A 心跳/0x10 数据帧——ESP UART 收帧缓冲暂存（若不缓存则丢弃，见 §20.5 错误处理）并继续尝试重连。



### 流式转发（OTA 下载）



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

- 若签名 URL 已过期（断网 >5min），ESP 先经 `GET /api/v1/device/ota/check`（§4）重新换取签名 URL，再 Range 续传



**流控关键**：

- HTTP 下载速度（几百 KB/s）> UART 转发速度（~80KB/s @921600）

- ESP-07S 读完一块**主动暂停读** HTTP stream，等 UART ACK 后再读下一块

- TCP 接收缓冲区会自然 backpressure，不会丢数据



**进度上报**：下载过程中 ESP-07S 周期性 publish `device/ota/progress/{dev_id}`（task_id + 当前 offset + progress），供云端刷新 `ota_task_records` 与进度看板（§5）。



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

| `dev_id`（CMD 0x10） | uint32 大端，4 字节（STM32 出厂烧录的全链路唯一机器标识；权威来源为 STM32 帧头，ESP 持有产线烧录的认证副本用于 MQTT CONNECT，转发时从帧头读取拼 topic） |

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

| 0x12 | ESP→MCU | 配置下发 | `json_len[2] + config_json[...]` | 云端 `cmd/{dev_id}/config` 透传，或 ESP 自主 NTP 校时（§20.2），如 `{"report_interval_ms":10000}` / `{"rtc_sync":<unix_ts>}` |

| 0x13 | MCU→ESP | 配置 ACK | `result[1]` | 0=ok/1=parse_err/2=unsupported；ESP 收到后 publish `device/config/ack/{dev_id}` 上报云端（§5） |

| 0x20 | ESP→MCU | 实时状态查询 | `qid[16] + query_type[1]` | 实时监控（§21），每秒 5 条，5 种 query_type |

| 0x21 | MCU→ESP | 实时状态应答 | `qid[16] + query_type[1] + data[<512]` | 应答 CMD 0x20，data 为 JSON，< 512B |

| 0x22 | ESP→MCU | 配置读取 | `qid[16] + config_type[1]` | 手动读取，5 种 config_type |

| 0x23 | MCU→ESP | 配置应答 | `qid[16] + config_type[1] + data[<512]` | 应答 CMD 0x22，data 为 JSON，< 512B |



### UART 复用与并发



OTA 块传输（0x05/0x06）、数据上报（0x10/0x11）、心跳（0x0A）、配置（0x12/0x13）**共用同一条 UART**，需避免帧交织错乱：



- **主机制：OTA 优先，暂停周期上报**（推荐，简单可靠）：进入 OTA 下载（DOWNLOADING）后，STM32 主动暂停 5s 数据上报与 60s 心跳，OTA 结束（成功/失败回 IDLE）后恢复。OTA 仅 15-25s，错过几条遥测可接受，且 `upgrading` 状态下 5min 离线阈值不会误触发。

- **防御式回退：按 CMD 分发的非阻塞帧调度**：即使暂停机制失效（如 STM32 未及时停止上报），两端帧解析器按帧头 `CMD` 路由到对应处理器，**不硬等某一特定 CMD**；即使在 OTA 块传输中间收到 0x10 数据帧，也能正确转发后再继续等 ACK。帧自同步（AA 55 + LEN + CRC），天然可解复用，不会因意外交织帧而死锁。



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

| 固件加密性能 | STM32F103VE 软件 AES-256 ~50-80 KB/s，~288KB 加解密 3-5s，可接受 |

| 时序数据丢失 | 时序数据对少量丢包不敏感（下一条 5s 后就来）；ESP 不缓存，WiFi 断连丢几秒数据可接受 |

| TDengine 存储增长 | KEEP 自动清理 + 降采样；监控磁盘使用率，接近 80% 扩容或缩短保留期 |

| 告警风暴（万设备同时触发） | FastAPI 告警评估任务限制单次最多 100 条推送；前端告警中心分页展示，超阈值合并 |



### 安全



- HTTPS 下载（传输层）+ 固件 HMAC-SHA256 校验（内容层，Bootloader 执行）

- OSS 私有读 + **限时**签名 URL（5 分钟过期，期内允许**多次 Range 请求**以支持断点续传；非"单次请求即失效"）

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

2. 对每条规则，从 TDengine **批量查询**（非逐设备循环）：

   ```sql

   SELECT dev_id, count(*) AS over_cnt

   FROM telemetry

   WHERE ts > NOW - <duration> AND <metric> > <threshold>

   GROUP BY dev_id

   ```

   （`<metric>` 取自 `alert_rules.metric`，**⚠ 必须与 §17.1 列名白名单比对后才拼入 SQL**，禁止直接从 DB 取值拼接）

3. 对查询返回的每个 `(dev_id, over_cnt)`，判断 `over_cnt ≥ 应有采样数 × 容忍比例` → 触发 → 写入 alerts 表（MySQL）+ 写入 alert_snapshots（TDengine）+ 通过 §6 **WebSocket 推送告警通知**给前端。前端亦可轮询 `GET /api/v1/alerts?status=active` 兜底

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

-- KEEP 按需设置：推荐 180d（6 个月），可调 90d/365d

CREATE DATABASE iot_data KEEP 180d DURATION 10d BLOCKS 4;



CREATE STABLE telemetry (

  ts         TIMESTAMP,

  -- 环境量

  temp       FLOAT,          -- 温度 (°C)

  hum        FLOAT,          -- 湿度 (%)

  pressure   FLOAT,          -- 气压 (hPa)

  -- 电气量

  volt_in    FLOAT,          -- 输入电压 (V)

  volt_out   FLOAT,          -- 输出电压 (V)

  current_a  FLOAT,          -- 电流 (A)，列名避开 SQL 保留字 current

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

  dev_id     INT UNSIGNED     -- 设备 ID（uint32）；taosX 从 MQTT topic `data/{dev_id}/telemetry` 提取，不在 JSON body 内

);

-- 注：group_id 和 location 属于设备静态元数据，存 MySQL devices 表（§3）；

--      按组查询时 FastAPI 先查 MySQL 取该组 dev_id 列表，再在 TDengine 中 WHERE dev_id IN (...)。

--      不放入 TDengine TAG 以避免需要在 JSON 中携带静态数据、及分组变更时的双写同步问题。

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

| taosX → TDengine | JSON key 与列名一一对应；TAG `dev_id` 从 MQTT topic `data/{dev_id}/telemetry` 自动提取 | 列：零映射；TAG：topic 提取 |

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

| `current_a` | 电流 | A |

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



> **使用边界**：`/telemetry/latest` 面向**分页等小批量**场景（设备列表当前页的几十台），`dev_ids` 不宜一次传上万。全局大盘的总数/在线率/告警分布等走 §19.3 **聚合接口**（`/dashboard/*`），不逐设备取 latest；**按组看板**可用 `?group_id=3` 过滤（后端查 `devices` 表取该组 dev_id 列表，再在 TDengine 中 `WHERE dev_id IN (...)` 取 latest，内部自动分页）。



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



> ESP-07S **持有产线烧录的 dev_id + secret 副本**（供 MQTT CONNECT 认证和 topic 拼接使用），但不独立产生/管理身份：身份的**权威持有者是 STM32**（每帧 UART header 带的 dev_id 才是规范的），两者产线同批烧录，应一致。ESP 转发时从帧头读 dev_id 拼 MQTT topic，JSON 内容不解析、原样透传。



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



### 21.3 实时设备监控页面



单设备的实时运行状态监控页，通过 WebSocket（§4 实时监控接口）与设备秒级交互。**数据只实时展示、不落库**。



**页面布局**：



```

┌──────────────────────────────────────────────────────────────────┐

│  ← 返回   设备 10001（车间A-1号柜）   ● 在线   [▶ 启动监控]        │

│                                                          会话: 已连接│

├──────────────────────────────────────────────────────────────────┤

│  实时状态（启动监控后每秒刷新 5 项，超时项标灰+原因）              │

│  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐    │

│  │ 运行态      │ │ 通信统计    │ │ 传感器快照  │ │ OTA 状态   │    │

│  │ CPU 62%    │ │ UART 1.2M  │ │ temp 25.3  │ │ IDLE       │    │

│  │ mem 4.0KB  │ │ rssi -58   │ │ hum  68.2  │ │ v1.2.0     │    │

│  │ up  10d    │ │ mqtt online│ │ volt 220.1 │ │            │    │

│  │  ▁▂▃▄▅▆   │ │  ▁▂▃▄▅▆   │ │  ▁▂▃▄▅▆   │ │            │    │

│  └────────────┘ └────────────┘ └────────────┘ └────────────┘    │

│  ┌──────────────────────────────────────────────────────────┐    │

│  │ 电源状态   volt_in 220.1V  current 1.25A  MCU 41℃       │    │

│  │            看门狗复位 0 次   欠压 0 次    ▁▂▃▄▅▆▇       │    │

│  └──────────────────────────────────────────────────────────┘    │

├──────────────────────────────────────────────────────────────────┤

│  配置读取（手动触发，点击后该卡片显示加载态，应答后填充）          │

│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐│

│  │网络配置   │ │上报配置   │ │告警阈值   │ │时间配置   │ │系统配置   ││

│  │[读取]    │ │[读取]    │ │[读取]    │ │[读取]    │ │[读取]    ││

│  │ssid:..   │ │间隔:5s   │ │temp>80   │ │rtc:..    │ │dev:10001 ││

│  │mqtt:..   │ │采样:1s   │ │  30s     │ │ntp:..    │ │fw:1.2.0  ││

│  └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────┘│

└──────────────────────────────────────────────────────────────────┘

```



**交互细节**：

- 顶部"启动监控"按钮：点击后变绿色脉冲 `● 监控中` + `■ 停止`；WebSocket 发 `start`。每秒 5 个状态卡片同步刷新（值变化时有高亮闪动），单个查询 900ms 无应答则该卡片标灰显示"超时"。

- 每个状态卡片内嵌**迷你 sparkline**：保留监控期间最近 ~60 个采样点（纯前端内存，不落库），停止监控即清空。

- 5 个配置卡片各有"读取"按钮：点击后按钮转 loading，WebSocket 发 `get_config`，应答后卡片展开 JSON 内容（折叠面板，< 512B），超时显示"设备无应答"。

- 页面离开/关闭标签页 -> WebSocket 断开 -> 服务端自动 stop、退订、清理（§4）。



> **不落库约定**：监控页所有查询/应答数据仅存在于浏览器内存和 FastAPI 会话内存中，**不写 TDengine、不写 MySQL**，会话结束即丢弃。历史趋势请用 §19.1 `/telemetry/history`（那是已落库的周期上报数据）。



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



不需新增**独立** ECS 实例，但现有 ECS 需升配至 4C8G 并挂载 2TB 数据盘（见 §12），再安装 TDengine 开源版。



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

│       │   ├── monitor.py     (新增，WebSocket 实时监控接口)

│       │   └── ota.py         (已有)

│       ├── services/

│       │   └── td_connector.py(新增)

│       └── mqtt/              (已有)

├── frontend/                  (扩展)

│   └── src/

│       ├── views/

│       │   ├── Dashboard.vue      (新增)

│       │   ├── DeviceDetail.vue   (新增)

│       │   ├── AlertCenter.vue    (新增)

│       │   └── MonitorDevice.vue  (新增，实时设备监控页)

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

| EMQX → TDengine | MQTT 客户端模拟设备并发上报，逐步加压至满量程（10,000 设备 / 2,000 条每秒）；单进程上限不足时使用多进程或分布式压测 | paho-mqtt + Python / locust 等 |

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

| 设备标识 | dev_id = uint32，STM32 出厂烧录，全链路统一；ESP 产线同批烧录副本用于 MQTT 认证 | 4 字节省空间；ESP 持有认证副本但不独立管理身份 |

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

