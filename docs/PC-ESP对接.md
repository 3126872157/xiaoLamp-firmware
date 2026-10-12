# PC-ESP 对接协议（轮询模式）

本文定义 PC 端与台灯设备（`apple-lamp-ov2640-xiaozhi-delivery`，及 ov3660 同角色）之间的
局域网对接协议。核心原则：**ESP32 只做 HTTP 服务端，不做任何广播/组播**；所有数据获取由 PC 端
主动轮询，所有指令由 PC 端主动发起 HTTP 请求。这样不依赖 UDP 广播（广播在部分路由器/AP 隔离、
防火墙环境下会被拦截），走标准 HTTP 即可穿透。

> 状态：ESP 端已实现 `/api/mcp/tools`、`/api/mcp/call` 与 `save_frames` 操作（见 §5/§6/§8）。
> 本文为 ESP 与 PC 两端的对接约定，涉及现有端点的字段以当前固件为准。

## 1. 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│ PC 端（Node 服务 + 浏览器 UI，`npm run` 形式）                  │
│   ┌──────────────┐   轮询/请求     ┌──────────────────────┐    │
│   │  Node 服务     │ ─────────────► │  ESP32 (HTTP 服务端)  │    │
│   │ (代理+鉴权)   │ ◄───────────── │  端口 8080            │    │
│   └──────────────┘   响应(JSON)    └──────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

- **ESP32**：纯 HTTP 服务端（`lamp_twin_web.cc`，端口 8080），不主动发任何报文。
- **Node 服务**：PC 端本地常驻进程，缓存设备令牌、定时轮询设备状态、把浏览器请求转发给 ESP。
- **浏览器 UI**：只与 Node 服务交互（展示状态/技能、示教、触发技能）。

## 2. 设备发现（无广播）

不依赖广播/组播，PC 端通过以下任一方式获知设备地址：

| 方式 | 说明 |
| --- | --- |
| 手动配置 | PC 客户端配置文件填写设备 IP（最稳，首选） |
| DHCP 静态租约 | 路由器给 ESP 绑定固定 IP，PC 配置该 IP |
| 主动探测 | 对配置的 IP 段逐个探测 `GET /api/lamp/state`（仅探测已知小网段，非全网段扫描） |

设备 IP 由 ESP 在 Wi-Fi 连接后打印到日志：`Test panel: http://<设备IP>:8080/`。
PC 端不依赖 mDNS，也不依赖 ESP 主动上报。

## 3. 配对与鉴权（已有）

- ESP 启动时生成 32 字节十六进制随机令牌，注入首页 HTML 的 `__TOKEN__` 占位。
- 除首页 `GET /` 外，所有 `/api/*` 请求需携带请求头 `X-Lamp-Token: <token>`，并做同源校验
  （`Origin` 匹配 `Host`）。
- Node 服务流程：发现设备 IP → `GET http://ip:8080/` → 解析 HTML 中的令牌 → 缓存 → 转发请求时
  补齐 `X-Lamp-Token`。
- 浏览器只与 Node 服务建立会话，不直接接触设备令牌。

## 4. 数据轮询（PC 主动拉取）

Node 服务定时轮询以下 GET 端点，并把最新快照推给浏览器 UI（或浏览器经 Node 按需拉取）。

### 4.1 硬件状态（已有）

`GET /api/lamp/state`

返回关键字段（详见 `LampHardwareExecutor::State()`）：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `executor` | string | `local_uart_hardware` / `local_uart_read_only` / `simulator` |
| `busy` | bool | 是否有运动任务在途 |
| `manual_control` | bool | 是否处于手动占用 |
| `torque_enabled_confirmed` | bool | 五轴力矩已确认使能 |
| `unloaded_confirmed` | bool | 五轴已确认卸力 |
| `motion_enabled` | bool | 软件运动授权 |
| `pose_rad` | object | 五关节反馈 `{baseYaw,shoulder,elbow,wrist,headRoll}`（URDF 弧度，缺新鲜反馈的关节不返回） |
| `servos[5]` | array | 每颗舵机 `position_fresh`、`position_ticks`、`position_age_ms`、`torque` 等 |
| `available` | bool | 五轴反馈全部新鲜 |
| `active_task` / `last_task` | string/object | 当前/最近任务回执 |

轮询周期建议：UI 展示 500ms–1s；示教关键帧采样按需（见 §7）。

### 4.2 交付状态（已有接口；跟书字段仅供实验）

`GET /api/lamp/delivery`

返回：`storage_ready`、`actions[]`（动作目录，含 `id/name/description/valid/compatible`）、
`teaching_status`、`frames[]`（当前草稿关键帧）、`tracking_status`、`catalog_revision`、
存储用量等。

`tracking_status`、JPEG 和跟书相关字段来自尚未完成的实验实现，跟书不能作为当前
交付或课程能力。动作库和示教状态可以按本文其他章节使用。

### 4.3 其他查询端点（已有）

| 端点 | 说明 |
| --- | --- |
| `GET /api/lamp/config` | 模型版本、关节 xyz/rpy/axis、URDF 范围、安装映射 |
| `GET /api/lamp/history?after=<seq>` | 对话与工具调用历史，分页 |
| `GET /api/lamp/tasks/<id>` | 硬件任务回执（受理/运行/完成/失败） |
| `GET /api/lamp/delivery/jobs/<id>` | 交付异步 job 回执 |
| `GET /api/lamp/delivery/jpeg` | 最新跟书 JPEG 预览 |

## 5. MCP 技能列表与调用（已实现）

当前 MCP 技能由 `McpServer` 在启动时注册，但 HTTP 层尚未暴露「列出技能」与「调用技能」。
PC 对接需新增两个端点：

### 5.1 列出技能：`GET /api/mcp/tools`（已实现）

复用 `McpServer` 的 JSON-RPC `tools/list`，返回完整 schema：

```json
{
  "tools": [
    {
      "name": "self.lamp.release_torque",
      "description": "User says don't resist...",
      "inputSchema": { "type": "object", "properties": {}, "required": [] }
    }
  ],
  "nextCursor": "..."
}
```

当前固件已注册的台灯技能（15 个）：

- 核心 10：`list_primitives`、`run_sequence`、`get_state`、`stop`、`execute_action`、
  `end_lighting`、`rest`、`release_torque`、`enable_torque`、`record_calibration`
  - 交付 5（其中跟书两个工具仅为实验接口，不代表可交付能力）：`list_user_actions`、`play_user_actions`、`get_delivery_state`、
  `start_book_tracking`、`stop_book_tracking`

> 技能列表在启动时确定；`list_user_actions` 返回的动作目录是**动态数据**（示教新增后即时变化），
> 但技能名本身不随运行变化。PC 端可缓存技能 schema，仅在设备重启后重新拉取。

### 5.2 调用技能：`POST /api/mcp/call`（已实现）

复用 `McpServer` 的 JSON-RPC `tools/call`，请求体：

```json
{ "name": "self.lamp.play_user_actions",
  "arguments": { "ids_json": "[\"wave_hello\"]", "end_mode": "return_to_reference" } }
```

响应（工具结果，JSON-RPC 风格）：

```json
{ "content": [ { "type": "text", "text": "{...}" } ], "isError": false }
```

错误：`{"error":{"code":-32602,"message":"Unknown tool: ..."}}`。

## 6. 新增 MCP：示教并保存新动作（核心流程）

「新增 MCP」在本架构里 = **示教一个用户动作并写入 Flash 动作库**，随后该动作即可被
`list_user_actions` 列出、被 `play_user_actions` / 网页播放。动作库为 LittleFS `lamp_actions`
分区，上限 24 个动作。

### 6.1 流程（网页端记录关键帧，PC 轮询驱动）

1. `teach_start`：ESP 停止跟书、进入 `unloading`、下发 `set_torque{enabled:false}` 卸力；
   卸力确认后进入 `recording`。
2. 网页端轮询 `GET /api/lamp/state`，用户手掰灯臂，点「记录关键帧」记下当前 `pose_rad` 与
   `duration_sec`（0.2–10s）。关键帧由 **PC/网页端** 记录，ESP 不自动采样。
3. 重复至 2–32 帧（至少 2 帧）。
4. 填写 `id` / `name` / `description` / `end_mode` / `joints[5]`。
5. 一次性提交全部关键帧（见 §6.2），ESP 校验卸载/反馈新鲜/安装绑定一致，计算相邻 `delta_rad`
   后写入 Flash。
6. `teach_end`：结束示教（保留手动占用，不自动上力）。

### 6.2 提交关键帧：`save_frames` 操作（已实现）

在 `POST /api/lamp/delivery` 的 `allowed` 操作集中新增 `save_frames`：

```json
{
  "request_id": "delivery-3f2a1c0d-00007b1e",
  "operation": "save_frames",
  "args": {
    "id": "wave_hello",
    "name": "挥手",
    "description": "对用户挥手致意",
    "end_mode": "return_to_reference",
    "joints": [true, false, false, true, false],
    "frames": [
      { "pose_rad": { "baseYaw": 0, "shoulder": 1.4, "elbow": -1.55, "wrist": 0.52, "headRoll": 0 }, "duration_sec": 1.0 },
      { "pose_rad": { "baseYaw": 0.1, "shoulder": 1.4, "elbow": -1.55, "wrist": 0.62, "headRoll": 0 }, "duration_sec": 0.8 }
    ]
  }
}
```

异步响应：

```json
{ "ok": true, "job_id": "delivery-3f2a1c0d-00007b1e", "status": "accepted",
  "instruction": "Poll delivery job; accepted is not motion completion" }
```

轮询 `GET /api/lamp/delivery/jobs/<job_id>` 取回执；`completed` 只表示本地处理完成，动作是否真正
执行/到位仍需 `GET /api/lamp/tasks/<task_id>` 确认。

### 6.3 动作持久化格式（lamp-relative-action/1，已有）

保存后 ESP 把帧数组转换为相邻关节增量存储：

```json
{
  "format": "lamp-relative-action/1",
  "id": "wave_hello",
  "name": "挥手",
  "description": "对用户挥手致意",
  "model_version": "lamp-v3-...",
  "binding": "<64 位小写十六进制>",
  "angle_unit": "urdf_rad",
  "end_mode": "return_to_reference",
  "joints": [true, false, false, true, false],
  "segments": [
    { "delta_rad": { "baseYaw": 0.1, "shoulder": 0, "elbow": 0, "wrist": 0.1, "headRoll": 0 },
      "duration_sec": 0.8 }
  ]
}
```

校验规则（`lamp_delivery_model.cc::ParseAction`）：

- `id`：≤40 字符，`[A-Za-z0-9_-]`；`binding`：64 位十六进制，须与安装映射一致。
- `joints`：5 布尔，至少选 1 个；未选关节 `delta_rad` 必须为 0。
- `segments`：2–32 帧 → 1–31 段；每段 0.2–10s；前向总时长 ≤50s。
- `delta_rad` 是相对增量，非绝对姿态；`end_mode` 为 `return_to_reference` 或 `hold_at_end`。
- 写入 Flash 前需音频空闲 + 手动卸力 + 已停止跟书。

## 7. 发送 MCP 指令

- 单一技能调用走 `POST /api/mcp/call`（§5.2）。
- 台灯技能内部仍遵循「受理 ≠ 完成」：`run_sequence`/`play_user_actions` 返回回执后，需轮询
  `get_state`/`get_delivery_state` + 任务回执确认实际到位。
- 运动类技能需要先 `enable_torque`（显式使能）后才能执行；示教/手动占用期间不可运动。

## 8. 现有 HTTP 端点汇总（PC 轮询可用）

| 方法 | 路径 | 说明 | 状态 |
| --- | --- | --- | --- |
| GET | `/` | 首页 HTML（含令牌占位） | 已有 |
| GET | `/api/lamp/state` | 硬件状态 | 已有 |
| GET | `/api/lamp/config` | 模型/映射配置 | 已有 |
| GET | `/api/lamp/delivery` | 交付状态 | 已有 |
| GET | `/api/lamp/delivery/jobs/<id>` | 交付 job 回执 | 已有 |
| GET | `/api/lamp/delivery/jpeg` | 跟书 JPEG | 已有 |
| GET | `/api/lamp/history?after=<seq>` | 对话/工具历史 | 已有 |
| GET | `/api/lamp/tasks/<id>` | 硬件任务回执 | 已有 |
| GET | `/api/mcp/tools` | MCP 技能列表 | 已实现 |
| POST | `/api/lamp/delivery` | 交付操作（含 `save_frames`） | 已实现 |
| POST | `/api/mcp/call` | 调用 MCP 技能 | 已实现 |
| POST | `/api/lamp/command` | 执行器操作 | 已有 |
| POST | `/api/lamp/calibration/preview` | 候选映射校验 | 已有 |

## 9. 实现清单（ESP 端）

| # | 改动 | 位置 | 状态 |
| --- | --- | --- | --- |
| 1 | 新增 `GET /api/mcp/tools`（复用 `tools/list`） | `main/boards/.../lamp_twin_web.cc` | 已实现 |
| 2 | 新增 `POST /api/mcp/call`（复用 `tools/call`） | `main/boards/.../lamp_twin_web.cc` | 已实现 |
| 3 | 注册 `/api/mcp/*` 路由 | `main/boards/.../lamp_twin_web.cc` | 已实现 |
| 4 | 新增 `save_frames` 交付操作 | `main/boards/.../lamp_delivery.cc` | 已实现 |

> 说明：MCP 技能列表/调用通过 `McpServer::ParseMessage`（JSON-RPC `tools/list`/`tools/call`）
> 在 HTTP 层同步桥接实现，未改动 `McpServer` 的可见性。`lamp_delivery.cc` / `lamp_twin_web.cc`
> 已同步到 ov2640 与 ov3660 两份板目录。

## 10. 约束与安全

- 无广播/组播，全部走 HTTP；设备只监听不主动发送。
- 除首页外所有请求需 `X-Lamp-Token` + 同源校验；写操作同样受限。
- 交付请求体 ≤16 KiB；MCP 调用参数长度受 schema 上限约束。
- 动作校验、存储写入、跟书停止等沿用现有规则；PC 只是入口，不绕过设备侧校验。
- 该协议面向可信局域网调试/示教，不是公网远程控制服务。
