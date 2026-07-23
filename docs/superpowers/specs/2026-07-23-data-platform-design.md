# 10000 节点数据采集平台 — 设计文档

> 版本: v1.0 | 日期: 2026-07-23 | 状态: 设计完成

---

## 1. 项目概述

在现有 STM32 + ESP-07S OTA 升级系统基础上，叠加数据采集上报能力。10,000 台嵌入式设备通过 MQTT 上传遥测数据，云端存储、查询、展示。

### 1.1 核心指标

| 指标 | 值 |
|---|---|
| 节点数 | 10,000 台 STM32 + ESP-07S |
| 上报频率 | 每 5 秒 |
| 单条大小 | ~1 KB |
| 写入吞吐 | 2,000 条/s，~2 MB/s |
| 日增量 | ~172 GB |
| 月增量 | ~5.2 TB |
| 年增量 | ~62 TB |
| 数据保留 | 6-12 个月 |
| 查询场景 | 按设备+时间段查历史曲线、聚合统计 |
| 展示 | 全局监控大盘 + 单设备详情 + 告警中心 |

### 1.2 与 OTA 项目关系

- 复用现有云基础设施（阿里云 ECS / RDS MySQL / EMQX / OSS）
- 复用 STM32 + ESP-07S 硬件和 UART 帧协议
- 复用 FastAPI + Vue3 + Element Plus 技术栈
- OTA 能力保持不动，数据采集为增量功能

---

## 2. 架构设计

### 2.1 总体架构

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

### 2.2 组件职责

| 组件 | 职责 | 变化 |
|---|---|---|
| ESP-07S | OTA 模式不变；新增数据上报：UART 收帧 → CRC 校验 → MQTT publish | 加 data_forwarder |
| STM32 | OTA 不变；定时读传感器 → 构造 JSON → UART 发送 | 加 data_report + CMD_DATA_REPORT |
| EMQX | MQTT 路由、设备认证 | 新增 data topic 路由 |
| TDengine | 时序数据：遥测 + 事件 + 告警 + 自动降采样聚合；通过 taosX 原生 MQTT 订阅直接消费 EMQX 消息 | **新增组件** |
| MySQL | 元数据：设备台账、用户、分组、告警规则、OTA 任务 | 精简，移除时序表 |
| FastAPI | 时序查询 API + 管理 API + MQTT 控制下发 | 拆分 router，各连各 DB |
| Vue3 | 全局大盘 + 设备详情 + 告警中心 + OTA 管理 | 新增监控页面 |

### 2.3 关键数据流

```
设备端 ──────────────────── 云端 ──────────────────── 前端
                                                
STM32 读传感器                                   
  │                                               
  ▼                                               
构造 JSON ──UART DMA──▶ ESP-07S ──MQTT──▶ EMQX ──▶ TDengine
                                                     │
                                                     ▼
                                          FastAPI 查询接口 ◀── Vue3 大盘
```

### 2.4 告警评估机制

告警规则存储在 MySQL `alert_rules` 表中，由 **FastAPI 后台定时任务** 周期性评估：

```
每隔 N 秒（建议 10-30s，可配置）
    │
    ▼
从 MySQL 读取所有启用的告警规则
    │
    ▼
对每条规则，从 TDengine 查询最近窗口的数据
  SELECT last(*) FROM telemetry WHERE dev_id = ?
    │
    ▼
判断是否触发：当前值 超过/低于 阈值 且 持续时长 ≥ duration
    │
    ├── 触发 → 写入 alerts 表 + 推送 MQTT 通知 + 写入 events 表
    │
    └── 未触发 且 之前是告警态 → 自动恢复，更新 resolved_at
```

| 告警场景 | 实现 |
|---|---|
| 阈值告警 | `temp > 80` 且持续 30s → 触发 |
| 离线告警 | `last_seen` 超过 5 分钟未更新 → 触发（不查 TDengine，直接看 MySQL devices 表） |
| 恢复判断 | 值回落 + 持续正常 ≥ 1 个评估周期 → 自动 resolve |

### 2.5 设备在线状态判定

设备在线状态通过 `devices.last_seen` 字段判定：

- 每次收到 MQTT 遥测数据，通过 **EMQX 规则引擎**更新 `devices.last_seen = NOW()`（轻量 SQL，不影响吞吐）
- `status = 'online'` — `last_seen` 在 **5 分钟**内
- `status = 'offline'` — `last_seen` 超过 **5 分钟**

> taosX 负责海量数据写入 TDengine，EMQX 规则引擎负责轻量的 `last_seen` 更新，各走各路互不干扰。阈值 5 分钟可配置，选 5 分钟而不是 5 秒是为了容忍 WiFi 瞬时断连，避免在线状态频繁抖动。

---

## 3. 数据库选型

### 3.1 为什么不能只用 MySQL

| | MySQL | TDengine |
|---|---|---|
| 年存储成本 | ~84 万元（62 TB 云盘） | ~6 万元（~3 TB 云盘，压缩后） |
| 写入路径 | 事务 + 索引 + redo log | 无事务开销，直接追加列文件 |
| 查询性能 | 随数据增长退化 | 一设备一表，物理隔离，永远快 |
| 维护负担 | 需持续 DBA 调优（分区/归档/索引） | 几乎零运维 |
| 适用场景 | 设备台账、用户、OTA（元数据） | 时序遥测、事件、告警（海量数据） |

### 3.2 TDengine 核心特点

1. **"一设备一张表"模型** — 每个设备独立物理文件，查询互不干扰，无需索引
2. **列式存储 + 时间轴压缩** — 同一列连续数据极端可压缩，10-20 倍压缩比
3. **写入路径短** — 无事务、无索引，直接追加写入
4. **SQL 兼容** — 标准 SQL 查询，`WHERE` / `GROUP BY` / `INTERVAL` 完全支持
5. **原生 MQTT 集成** — 可直接订阅 EMQX topic，无需中间适配层

---

## 4. 数据模型

### 4.1 TDengine — 遥测超级表

```sql
-- KEEP 按需设置：推荐 180 DAYS（6 个月），可调 90/365
CREATE DATABASE iot_data KEEP 180 DAYS 10 BLOCKS 4;

CREATE STABLE telemetry (
  -- 时间
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
  dev_id     NCHAR(32),       -- 设备编号
  group_id   INT,             -- 分组
  location   NCHAR(64)        -- 位置
);
```

> **设计说明**：当前列表示例覆盖环境 + 电气 + 开关量典型场景，单条序列化约 800~1000 字节。实际项目中按具体传感器配置增减列——TDengine 的列式压缩让 20~30 列也不浪费存储。新增一列只需 `ALTER STABLE telemetry ADD COLUMN xxx FLOAT;`，10,000 张子表秒级继承，零停机。

子表命名：`t_dev_0001`, `t_dev_0002` … `t_dev_10000`，自动创建，物理文件独立。

### 4.2 TDengine — 事件表

```sql
CREATE STABLE events (
  ts       TIMESTAMP,
  level    TINYINT,       -- 0=info 1=warn 2=error
  code     NCHAR(32),     -- 事件码
  msg      NCHAR(256)     -- 事件描述
) TAGS (
  dev_id   NCHAR(32)
);
```

### 4.3 扩展字段策略

采用"宽表 + payload 兜底"模式：

- **常用字段建列** — 保证查询性能，列式压缩高效
- **冷门字段走 payload JSON** — 灵活扩展，不需要改表结构
- **确认稳定后 ALTER STABLE 提升** — `ALTER STABLE telemetry ADD COLUMN pressure FLOAT;`，10,000 张子表秒级继承，零停机

### 4.4 MySQL — 元数据表（精简）

```sql
-- 设备台账
devices: id, dev_id, name, group_id, location, firmware_ver,
         status(online/offline), last_seen, created_at

-- 设备分组
device_groups: id, name, parent_id, description

-- 告警规则
alert_rules: id, name, metric, operator(>/</=), threshold,
             duration, level, enabled

-- 告警记录（MySQL 存概要，TDengine 存触发时的原始快照）
alerts: id, dev_id, rule_id, level, msg, triggered_at, resolved_at

-- 用户/角色/权限（沿用现有 OTA 设计）
-- OTA 相关表（复用现有，不动）
```

---

## 5. 前后端数据格式约定

| 层 | 格式 | 说明 |
|---|---|---|
| STM32 → ESP-07S | `{"ts":1721712000,"t":25.3,"h":68.2,"p":101.3,"vi":220.1,"vo":12.05,"c":1.25,"po":1250.5,"e":38472.3,"f":50.01,"d1":1,"d2":0,"d3":1,"d4":0,"r":-58,"up":863400}` | 简短 key 节省 UART 带宽 |
| ESP-07S → MQTT | 同上，透传 | 不做转换 |
| FastAPI → 前端 | `{"ts":"2026-07-23T10:00:00","temperature":25.3,"humidity":68.2,"pressure":101.3,...}` | 完整 key，前端友好 |
| 前端 → FastAPI | 标准 REST 参数 | `?dev_id=xxx&start=...&end=...` |

---

## 6. API 设计

### 6.1 时序查询 API

```
GET /api/v1/telemetry/latest
  ?dev_ids=dev_001,dev_002
  → 每个设备最新一条数据（大盘实时值）

GET /api/v1/telemetry/history
  ?dev_id=dev_001
  &start=2026-07-22T00:00:00
  &end=2026-07-23T00:00:00
  &interval=1m
  → 时间范围内降采样查询，interval 支持 10s/1m/5m/1h/1d

GET /api/v1/telemetry/stats
  ?dev_id=dev_001
  &start=2026-07-16
  &end=2026-07-23
  → 统计：avg/max/min/count，一次返回所有测点
```

### 6.2 事件/告警 API

```
GET /api/v1/events
  ?dev_id=dev_001&level=2&start=...&end=...
  → 设备事件列表

GET /api/v1/alerts?status=active
  → 当前未恢复的告警

GET /api/v1/alerts/history
  ?dev_id=dev_001&page=1&size=20
  → 告警历史
```

### 6.3 设备管理 API（查 MySQL）

```
GET    /api/v1/devices
  ?group_id=3&status=online&page=1&size=50
  → 设备列表，支持分组/状态筛选

GET    /api/v1/devices/{dev_id}
  → 设备详情 + 在线状态

POST   /api/v1/devices
  → 注册新设备

PUT    /api/v1/devices/{dev_id}
  → 修改分组/位置
```

### 6.4 大盘聚合 API

```
GET /api/v1/dashboard/summary
  → {
      total: 10000,
      online: 9853,
      offline: 147,
      alerts_active: 12
    }

GET /api/v1/dashboard/group-stats
  → 每个分组的在线率、告警数

GET /api/v1/dashboard/alert-trend?days=7
  → 最近 7 天每天告警数量趋势
```

### 6.5 MQTT Topic 设计

```
data/{dev_id}/telemetry    → 遥测 JSON，TDengine taosX 原生 MQTT 订阅直接写入
data/{dev_id}/event        → 事件/告警
cmd/{dev_id}/config        → 云端下发配置（如修改上报间隔）
```

---

## 7. 前端设计

### 7.1 页面结构

```
侧边栏                       内容区
─────────────────────────────────────
📊 监控大盘                   默认首页
📋 设备列表                   分页列表 + 搜索筛选
📈 设备详情 (点进去)           实时值卡片 + 历史曲线 + 事件时间线
🔔 告警中心                   筛选 + 分页列表 + 展开详情
⚙️  设备管理                   台账/分组/注册/批量操作
🔄 OTA 升级 (原有)             保持不变
👤 系统设置                   用户/角色
```

### 7.2 监控大盘（首页）

4 个统计卡片（总设备 / 在线 / 离线 / 告警）+ 在线率趋势折线图 + 告警分布饼图 + 分组在线状态表格。

### 7.3 设备详情页

- 实时值卡片（温度/湿度/电压/功率等）
- 历史曲线（ECharts，支持 dataZoom 缩放 + 时间范围切换 1h/6h/24h/7d/30d）
- 最近事件时间线
- 设备基础信息卡片（型号/固件/IP/位置）

### 7.4 告警中心

按级别筛选标签（严重/警告/信息），分页列表展示触发时间/设备/规则/状态，每行可展开详情。

### 7.5 前端技术组合

| 需求 | 选型 |
|---|---|
| 框架 | Vue3 + TypeScript + Vite（沿用现有） |
| 组件库 | Element Plus（沿用） |
| 图表 | ECharts — 大数据量曲线性能好 |
| 状态管理 | Pinia（沿用） |
| 实时更新 | MQTT over WebSocket 直连 EMQX（大盘在线状态） |
| 历史曲线 | HTTP 请求 FastAPI/TDengine，按需加载 |

---

## 8. STM32 / ESP-07S 数据上报

### 8.1 总体原则

STM32 只管采集 + 打包，ESP-07S 只管透明转发。ESP 不解析 JSON 内容，不缓存数据。

### 8.2 STM32 端流程

```
定时器中断 (5s)
    │
    ▼
读传感器 (ADC/I2C/SPI)
    │
    ▼
构造 JSON 字符串 {"ts":...,"t":...,"h":...,"p":...,"vi":...,...}  // ~800~1000 字节，简短 key
    │
    ▼
封装 UART 帧 (CMD=DATA_REPORT 0x10)
    │
    ▼
UART DMA 发送 → ESP-07S
```

改动：`uart_protocol.c` 加 `CMD_DATA_REPORT (0x10)`，新建 `data_report.c`（~100 行），定时器回调调用。

### 8.3 ESP-07S 端流程

```
UART 收到帧 (CMD=0x10)
    │
    ▼
校验 CRC16 → 提取 JSON 字符串
    │
    ▼
拼接 MQTT topic: data/{dev_id}/telemetry
    │
    ▼
mqttClient.publish(topic, json)
    │
    ▼
返回 ACK 给 STM32 (CMD=0x11)
```

改动：`uart_transport.cpp` 解析命令，新建 `data_forwarder.cpp`（~50 行）。

### 8.4 UART 帧格式

```
[0xAA][0x55][LEN_H][LEN_L][CMD=0x10][dev_id 4B][JSON...][CRC16]
                                                       ↑
                                               CRC 覆盖 LEN+CMD+dev_id+JSON
```

帧开销 12 字节，JSON 本体 ~1KB，总帧长约 1032 字节。460800 bps 下传输时间约 22ms。

### 8.5 上报频率可调

```c
#define REPORT_INTERVAL_MS  5000   // 默认 5 秒

// 通过 MQTT 下发配置修改：
// cmd/{dev_id}/config  {"report_interval_ms": 10000}
```

### 8.6 错误处理

| 端 | 场景 | 处理 |
|---|---|---|
| STM32 | 传感器读取失败 | 对应字段填 null，不跳过上报 |
| STM32 | UART 发送缓冲区满 | 丢本次数据，记录丢包计数 |
| STM32 | 连续 N 次 ACK 超时 | 标记通信故障，触发事件上报 |
| ESP-07S | WiFi 断连 | 不缓存，丢就丢了 |
| ESP-07S | MQTT publish 失败 | 重试 1 次，仍失败则丢弃 |
| ESP-07S | CRC16 校验失败 | 直接丢弃，不 ACK |

### 8.7 关键设计决策：ESP-07S 不缓存数据

与 OTA 项目中"ESP-07S 不缓存固件"原则一致。时序数据特点是"丢了就丢了，下一条马上来"，不像固件必须完整。不缓存方案代码简单、内存占用低、断电行为一致。

---

## 9. 部署与运维

### 9.1 服务器规划（阿里云）

```
现有 ECS (复用)
├── EMQX (MQTT broker, port 1883/8883/8084)
├── TDengine (时序数据库)            ← 新增
└── FastAPI (Nginx + uvicorn, port 8000)

RDS MySQL (现有，复用)
└── 设备/用户/OTA 元数据

OSS (现有，复用)
└── OTA 固件包
```

不需要新增云资源，仅现有 ECS 上安装 TDengine。

### 9.2 容量与成本

| 保留期 | 压缩后存储 | 建议磁盘 | 云盘月费 |
|---|---|---|---|
| 3 个月 | ~0.8 TB | 2 TB | ~2,000 元 |
| 6 个月 | ~1.5 TB | 2 TB | ~2,000 元 |
| 12 个月 | ~3 TB | 4 TB | ~4,000 元 |

年度总成本约 6 万元（ECS + 云盘），vs 纯 MySQL 方案的 84 万元。

### 9.3 项目目录结构

```
D:\claude\514\
├── stm32-bootloader/          (已有，不动)
├── stm32-app/                 (扩展)
│   └── src/
│       ├── ota_task.c         (已有)
│       ├── data_report.c      (新增)
│       └── uart_protocol.c    (扩展：加 CMD_DATA_REPORT)
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
    ├── ota-design.md              (已有)
    └── superpowers/
        └── specs/
            └── 2026-07-23-data-platform-design.md (本文件)
```

### 9.4 TDengine 运维要点

- **日常**：基本不用管，写入路径无碎片，无索引维护
- **备份**：`taosdump` 定时导出扔 OSS，每天一次增量
- **清理**：`CREATE DATABASE` 已设 `KEEP`（默认 180 天，可按需调整），自动删过期数据
- **监控**：自带 `taosKeeper` + Grafana 模板
- **升级**：GitHub release 下载，`rpm -U` 一行搞定

---

## 10. 安全

### 10.1 传输安全

- MQTT: TLS (port 8883)，设备证书认证
- HTTP API: HTTPS + JWT token
- 前端 WebSocket: WSS (port 8084)

### 10.2 设备认证

- MQTT: 一机一密（username/password 或 TLS 客户端证书）
- HTTP: 设备通过 MQTT 获取一次性 OSS 签名 URL（OTA 场景复用）

### 10.3 数据安全

- TDengine 部署在 ECS 内网，不暴露公网端口
- 仅 FastAPI 通过内网 IP 连接 TDengine
- MySQL 沿用现有 RDS 安全组配置

---

## 11. 测试策略

| 层 | 方法 | 工具 |
|---|---|---|
| STM32 data_report | PC 串口工具模拟 ESP-07S，验证帧格式 + JSON + CRC | 串口助手 + Python 脚本 |
| ESP-07S data_forwarder | PC 起 MQTT broker + 串口发帧，验证 publish | Mosquitto + 串口工具 |
| 后端 telemetry API | 单元测试 + httpx 集成测试，预灌数据查接口 | pytest |
| EMQX → TDengine | 用 MQTT 客户端模拟 1000 设备并发上报，验证写入 | paho-mqtt + 压力脚本 |
| 前端 | Vitest 组件测试 + Playwright E2E | Vitest / Playwright |
| 端到端 | 1-5 台真实设备验证完整链路 | 真实硬件 |

---

## 12. 实施顺序

```
Phase 1: TDengine 部署 + EMQX 桥接       → 数据能进来
Phase 2: FastAPI 时序查询 API            → 数据能出来
Phase 3: 前端监控大盘 MVP                 → 数据能看
Phase 4: STM32 + ESP-07S 数据上报适配     → 真实设备对接
Phase 5: 告警规则引擎 + 告警中心           → 智能化
Phase 6: 前端设备详情 + 历史曲线           → 完整体验
Phase 7: 企业级能力（灰度/导出/批量操作）    → 运营能力
```

---

## 13. 关键设计决策汇总

| 决策 | 结论 | 理由 |
|---|---|---|
| 时序存储 | TDengine，不用 MySQL | 存储成本降低 14 倍，查询性能不退化 |
| 数据分层 | 时序 → TDengine，元数据 → MySQL | 各用最合适的数据库 |
| 扩展字段 | 宽表 + payload 兜底 | 查询性能 + 灵活性的平衡 |
| ESP-07S 缓存 | 不缓存 | 时序数据丢包可接受，代码简单 |
| 上报协议 | 复用现有 UART 帧协议 + 新 CMD | 最小化协议层改动 |
| 前端图表 | ECharts | 大数据量曲线性能好，dataZoom 缩放 |
| MySQL 角色 | 保留，只存元数据 | 用户/设备/OTA 逻辑不变 |
