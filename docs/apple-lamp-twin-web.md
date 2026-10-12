# ESP32 内置 twin 测试面板

分支 `codex/esp32-twin-calibration`。独立固件角色 `local-twin`，板型/OTA 身份 `apple-lamp-ov3660-local-twin`。
普通实机 `local-hardware`、只读版和模拟器版不启动这个服务。

## 访问与测试

在已加载 ESP-IDF 6.1 的终端、固件仓库根目录运行：

```powershell
python scripts/build_lamp_variant.py --role local-twin
# 确认实际原生 USB 端口和对应角色后烧录；不要把历史 COM8 当成固定端口。
python scripts/build_lamp_variant.py --role local-twin --archive-only --flash --port COM8
```

本轮不自动烧录，不自动使能。烧录后设备连接原 Wi-Fi，日志打印 `Test panel: http://<设备IP>:8080/`。
电脑或手机连接同一个局域网，打开这个地址。无需 npm、电脑桥、Web Serial 或小智模型在线。
仍使用 GPIO43→EX_TX、GPIO44←EX_RX、共地、J8 外部控制 1–3/2–4、115200 8N1；分别正确供电。
如果设备页面不可访问，先确认设备 IP 和热点是否隔离客户端。

1. 页面打开仅查询状态。2D 侧视图显示 J2/J3/J4；蓝色为目标，绿色为反馈。Yaw 和末端旋转以数值单独显示，五轴都有滑条。
2. 点击“进入手动测试”：阻止新的 MCP 动作，不改变物理扭矩。停止/卸力仍能抢占。
3. 扶住灯臂并卸力，检查五颗舵机反馈和扭矩状态。
4. 逐轴校准方向：点①记录起始 tick，沿**界面角度增大方向**手掰少量，点②判方向。J2/J3 的显示正方向与 URDF 相反，算法会处理这次符号转换。没有自动运动。
5. 手掰到已知参考角度，填写参考角度，再“从当前反馈记录偏置”。可填 0° 或容易定位的其他角度；不是把当前任意姿态强行当作零位。
6. 检查候选映射，再保存。保存必须读回五轴已卸力，检查新鲜位置与设备限位，并确认 NVS 写入成功；会清除旧人脸/书本标定，关闭软件运动授权。
7. 重启检查映射恢复。随后“保持当前位置并使能”，采用实际反馈作为目标，先做单轴 ±3° 小幅测试。
8. 滑条与 ±3° 按钮只预览，点击“执行目标姿态”才运动。使用当前位置到目标的平滑轨迹、沿途检查与终点回读确认。
9. 退出手动测试不会自动卸力，但关闭软件运动授权；MCP 恢复访问，需显式使能才能执行新运动。

该图是侧视连杆示意，省略 Yaw 和末端旋转，不表示网格碰撞结果。侧视使用现有三改 URDF 的连杆参数。
真实运动仍检查五轴 URDF、偏置后的舵机 0–1000、设备内部位置限位和既有几何检查。
手动测试不受三种标准姿态的模式范围约束，以便安装校准；MCP 原语继续使用原模式规则。
位置反馈超时、越界、未到终点会停止任务并关闭运动授权。停止 CMD12 无 ACK，回执为 `command_sent`；卸力、使能和动作完成需实际反馈。

第一版没有连续滑条下发。关闭网页不自动卸力，以免灯臂坠落；只要没有正在执行的任务，手动占用保持到明确退出或设备重启。
重新打开设备页可停止、卸力或退出。同一设备的网页会话共享这次启动的手动控制，不提供多用户并行操纵。

## 设备 API：为后续 URDF Mesh 客户端保留

版本 `lamp-twin/1`。API 与 2D 展示分离，角度空间统一为 `urdf_rad`。

| 请求 | 返回/作用 |
| --- | --- |
| `GET /api/lamp/config` | 模型版本、工具位移、五个关节 xyz/rpy/axis、URDF 范围、实际映射/可达范围/显示符号 |
| `GET /api/lamp/state` | 真实反馈 `pose_rad`、最近成功下发的五轴采样 `target_pose_rad`、原始 tick、温度电压、扭矩、反馈新鲜度、占用和任务 |
| `GET /api/lamp/tasks/<request_id 或 task_id>` | 受理、运行、完成、失败、取消；受理不等于动作完成 |
| `POST /api/lamp/calibration/preview` | `{ "mapping": [{ "direction": 1, "zero_ticks": 500 }, ...五项] }`，验证并返回候选可达范围；不运动、不保存 |
| `POST /api/lamp/command` | 有界操作对象，提交执行器工作任务；HTTP 回调不读写 UART |
| `GET /api/lamp/tools` | 运行期工具表状态与生效工具（见 [板级 MCP 联调说明](./apple-lamp-mcp-bridge.md)） |

`GET /api/lamp/runtime` 在 `audio_uplink` 对象里给出上行健康度，只读、不改变任何策略：

| 字段 | 含义 |
| --- | --- |
| `queue_capacity` | 固定容量 8 帧（60 ms/帧，合计 480 ms），不随负载变化 |
| `queue_high_water` | 本次启动以来音频输入队列达到过的最大深度 |
| `dropped_frames` | 队列满时丢弃的最旧帧总数（保住了当轮语音） |
| `overruns` / `recoveries` | 持续丢帧升级为"拒绝本轮 + 600 ms 静音恢复"的次数 |
| `appends` | 实际发出的 `input_audio_buffer.append` 条数 |
| `coalesced_frames` | 落后时合并发送的帧数（省下的消息数） |
| `worst_write_ms` | 单次发送（含分片）最坏耗时，用于判断 worker 是否被长消息阻塞 |

同一组计数也出现在 `/api/lamp/omni` 面板状态里（`audio_*` 前缀），以及 `audio_input_overrun` 诊断正文中。
策略与取舍见 [音频上行与输入溢出](./apple-lamp-audio-uplink.md)。

`pose_rad`/`target_pose_rad` 按 `baseYaw, shoulder, elbow, wrist, headRoll` 命名，缺少新鲜反馈的关节不返回数值。
不要将界面显示角度再当成 URDF 角度。`mapping[].display_sign` 仅用于界面，`direction` 是真实舵机相对 URDF 的方向。
模型变化必须匹配 `model_version`；未来 Mesh 客户端自行加载相应 URDF/STL 文件，把原始关节角直接应用到模型。
无需改变控制接口，也不要求 ESP32 存 Mesh。当前不提供 Mesh 下载路由。

操作对象：

```json
{"request_id":"web-unique-1","operation":"move","args":{"pose_rad":{"baseYaw":0,"shoulder":1.4,"elbow":-1.55,"wrist":0.55,"headRoll":0},"duration_sec":1}}
```

| operation | args |
| --- | --- |
| `acquire` / `release` | `{}`，改变手动控制占用，要求没有运动任务 |
| `set_torque` | `{ "enabled": true/false }`，使能需手动占用，卸力可抢占 |
| `cancel` | `{}`，停止并抢占 |
| `move` | 五关节 `pose_rad` 对象、`duration_sec` 为 0.2–10s |
| `install` | `mapping` 五项、`clear_targets: true`；已卸力才能执行 |

所有 API 使用 `X-Lamp-Token`，页面内置本次启动随机令牌；POST 使用 `application/json`，请求最多 2048 字节。
服务不开放跨来源控制或任意 UART 写入。未来电脑 3D 客户端优先通过受信任的同源代理接这些接口，并转发设备页会话令牌；不直接放开通配 CORS。
这是局域网调试页，不是公网远程控制服务。

## 存储与验证

安装映射为 `lamp_native/install_v1`、格式 `lamp-installation/1`，小于 2KiB；实际目标示教仍为 `device_v1`。
加载顺序：Flash 动作库 → 安装映射 → 对应映射绑定的目标标定。安装偏置可以为小数 tick，不强行裁剪到 0–1000；允许范围 -1000–2000，并要求存在实际可达交集。
两份记录更新发生断电/部分写失败时，绑定不一致会禁用运动，不将旧目标解释成新映射。不会擦除 Wi-Fi 命名空间。
旧固件不认识安装覆盖项，降级回旧实机固件前应核对映射和目标版本，不把旧固件当作自动迁移工具。

专项宿主测试覆盖真正执行器：手动占用拒绝 MCP、候选验证、偏置/方向保存、卸力要求、NVS 失败、重启恢复、手动轨迹以及旧功能。
浏览器检查实际内置页面的无启动写入、滑条只预览、运动、卸力、方向判定/偏置记录/保存、桌面与手机布局。
这些模拟和构建不代替实机方向、HTTP 可达性、音频共存及机械运动验证。

## 本地对话记录（2026-10-07，等待烧录验证）

测试面板新增“对话与工具调用历史”，捕获用户 STT、助手 TTS 句子、台灯 MCP 调用参数/结果（含 RPC id）以及动作任务受理/结束。
这不是云端完整聊天存档：没有音频、没有模型隐藏推理；仅记录设备实际收到的文字与执行结果。
Application 和 MCP 只调用 Board 的可选诊断接口，默认板无记录；LOCAL-TWIN 板记录在独立加锁的 RAM 环形缓冲，不能驱动 UART 或改变状态。

`GET /api/lamp/history?after=<sequence>` 使用相同会话令牌，每页最多 8 条，返回 `events/cursor/latest/gap`；继续以 cursor 读取。
事件字段为 `sequence/uptime_ms/kind/name/data/truncated`，时间是开机以来毫秒。
最多 128 条、64KiB 编码事件，单条原始内容最多 4096 字节；UTF-8 边界截断，明确返回 truncated。
较早事件被覆盖时 gap=true，页面显示记录缺口，不能当作完整历史。
浏览器每秒拉取一页，最多保留 500 条，可导出 `lamp-conversation.txt`。
长时间大量 get_state 时应保持面板打开并及时导出；记录不持久化，设备重启清空，网页刷新清空浏览器缓存。

到位判断现在容许 10 tick（2.4°），连续满足至少 200ms，收尾最多等 5s；超时报告逐轴 tick 残差。
有新鲜反馈且扭矩仍在线的单纯到位超时不撤销软件授权；通信/模式/扭矩故障仍停机并撤销授权。
模式只在实际到位确认后更新。纯 yaw 微调使用 URDF 限位和几何/硬件检查，不要求其他关节符合当前模式形态。
此策略同步到 Web 模拟器；俯仰和表达动作仍保留各自模式约束。
