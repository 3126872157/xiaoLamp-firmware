# 音频上行与输入溢出（Omni edge-runtime）

对象：`apple-lamp-ov2640` / `apple-lamp-ov3660` 的 `omni-poc`、`edge-runtime` 角色，协议实现见
`main/protocols/omni_realtime_protocol.*`。这里记录"麦克风 PCM 上行"这条路径的固定容量、
溢出策略与观测口径，**不改变音频格式、采样率与任何队列容量**。

## 链路与固定容量

| 环节 | 容量 | 位置 |
| --- | --- | --- |
| 采集帧 | 60 ms = 1920 B（16 kHz 单声道 PCM16） | `OPUS_FRAME_DURATION_MS` |
| `audio_send_queue_` | 4 帧（240 ms），满则丢最旧 | `AudioService::PushTaskToEncodeQueue` |
| `audio_queue_`（本协议） | 8 帧（480 ms） | `OmniRealtimeProtocol` 构造 |
| `queue_`（入站 JSON/事件） | 24 条 | 同上 |

`SendAudio` 只在 `recording_` 为真时入队，而 `recording_` 在 `response.created` 与首个
`response.audio.delta` 时被置 false，所以**溢出只可能发生在用户说话期间**，不是播放期间。

## 溢出意味着什么

`audio_queue_` 要填满 8 槽，需要共享 worker（`omni_ws`）连续约 480 ms 没有回到取音频队列。
单帧写入远不足以做到；能造成这种情况的是"单次循环迭代内的长占用"，主要有两类：

1. **超大消息一次性写完**：`input_image_buffer.append` 把整张 JPEG 的 base64 放在**一条** JSON 里
   （≤65536 B 图片 → 约 87 KB 消息 → 约 43 个 2048 B 分片），一条消息必须连续发完。
2. **入站事件抢先**：循环原先先取 `queue_`，24 条事件可以排在一帧音频之前。

## 现行策略

- **丢最旧，而不是拒绝整轮**：`Enqueue` 在音频队列满时弹出并丢弃**最旧**一帧，再放入新帧，
  累加 `dropped_frames`。这与本仓库既有取舍一致（编码队列、发送队列都丢最旧），
  代价是语音中间可能出现 60 ms 缺口，收益是整句不再作废。
- **只有持续丢帧才升级**：连续丢帧达到 4 次（约 240 ms 连续丢失）才置 `audio_overrun_`，
  走既有的"丢弃不完整输入 + 等待 600 ms 静音后继续"恢复路径；连接始终保持。正常抖动不会
  触发整轮拒绝。
- **音频优先**：循环先取 `audio_queue_`，再取控制事件（`@manual`/`@vad`/`@finish`），最后取入站
  JSON，并且不再在音频待处理时等待 20 ms。
- **落后时合并**：一次迭代最多把 3 个**已在队列里**的帧合并成一条 append（不等待新帧、
  不新增缓冲），减少消息与分片数。
- **一帧一条 append**：60 ms 帧不再固定拆成两个 30 ms append，消息数减半；超过 3840 B 的输入
  （合并积压或异常大帧）仍按 3840 B 边界拆分，不丢采样。
- **长消息前先让路**：一条多分片消息开始前，先把**已排队**的麦克风帧发出去（有界，最多 4 帧，
  且只在"本就该发送音频"的状态下）。注意 WebSocket continuation 帧不能与其它消息交错，
  所以**不能在一条消息中间插播音频**，只能在其前后让路。
- **音频积压时推迟可选工作**：`audio_queue_` 非空时，本次迭代不跑表达/观察/工具完成等可选工作，
  下一轮再跑（各自仍有自己的截止时间判断）。

## 明确不做

- 不增加 `audio_queue_`/`audio_send_queue_` 槽位，不改采样率、帧长与编码格式。
- 不把上行发送从编排循环里拆出去（独立发送任务属于结构性改动，需要单独方案与实测）。
- 不改厂商组件 `78/esp-ml307` 的 `EspSsl::Send`：它在 `ESP_TLS_ERR_SSL_WANT_WRITE` 时忙等，
  发送缓冲满时会占住调用线程。这是"单次阻塞"的放大器，但属 `managed_components/`，
  本仓库规则不允许手改；如需处理应作为独立改动并同步上游。

## 观测口径

`GET /api/lamp/runtime` 的 `audio_uplink` 对象与 `/api/lamp/omni` 的 `audio_*` 字段给出
`queue_high_water`、`dropped_frames`、`overruns`、`recoveries`、`appends`、`coalesced_frames`、
`worst_write_ms`；`audio_input_overrun` 诊断正文也带上当时的这些数字。判读方式：

- `high_water` 长期远小于 8、`dropped_frames` 为 0 → 未接近饱和，无需进一步处理。
- `dropped_frames` 增长但 `overruns` 为 0 → 发生过短暂拥塞，语音靠丢帧保住了，属预期行为。
- `overruns`/`recoveries` 增长 → 仍有约 480 ms 级阻塞；结合 `worst_write_ms` 判断是长消息还是
  网络/TLS，再决定是否需要结构性改动。

## 验证边界

宿主测试覆盖：队列满丢最旧、持续丢帧才升级、恢复后计数与清空、落后合并的字节守恒、
`input_audio_buffer.append` 顺序与单帧完整性、长消息前先发已排队音频（分片不交错）。
**构建与宿主测试通过不等于声学与弱网通过**：真实丢帧率、弱网表现与语音可懂度必须在设备上
（配合上面的计数）复核。
