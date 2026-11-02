# Apple Lamp v1.0 · OV2640：外接功放 + PDM 麦克风定制板

当前交付为main的v1.0（9206493 V1.0），使用xiaozhi-delivery角色；Omni/Edge角色仅作独立实验。

与 `apple-lamp-ov3660` 并列的台灯开发板。SoC 仍为 ESP32-S3R8，16 MB Quad Flash，
8 MB Octal PSRAM；相机换用 **OV2640**（200 万像素）；音频不再使用板载
ES8311/ES7210 编解码芯片，改为**外接 I2S 数字功放**与**外接 PDM 麦克风**。
台灯功能（舵机 UART / MCP / 本地执行器 / Omni / 边缘运行时）与 ov3660 完全对齐，
构建角色与开关语义一致。

## 接线表

### 相机 OV2640（DVP 8-bit，独立 SCCB 引脚）

| OV2640 信号 | ESP32-S3 GPIO | Arduino Y 命名 |
|---|---|---|
| SIOD (SCCB SDA) | 4 | — |
| SIOC (SCCB SCL) | 5 | — |
| VSYNC | 6 | — |
| HREF | 7 | — |
| D2 | 8 | Y4 |
| D1 | 9 | Y3 |
| D3 | 10 | Y5 |
| D0 | 11 | Y2 |
| D4 | 12 | Y6 |
| PCLK | 13 | — |
| XCLK | 15 | — |
| D7 | 16 | Y9 |
| D6 | 17 | Y8 |
| D5 | 18 | Y7 |
| RESET | NC（不接） | — |
| PWDN | NC（不接） | — |

XCLK 频率 20 MHz。

### 外接功放（I2S 数字功放，MAX98357A 一类）

| 功放信号 | ESP32-S3 GPIO |
|---|---|
| BCLK | 38 |
| LRCLK (LRC/WS) | 39 |
| DIN (DATA) | 40 |
| SD / 使能 | 接 3.3V 高电平（常开，无软件静音） |

### 外接麦克风（PDM 数字麦，INMP441 / SPK0641HT4H 一类）

| 麦克风信号 | ESP32-S3 GPIO |
|---|---|
| CLK | 41 |
| DATA | 42 |

> 音频引脚已从 GPIO9~13 移到 GPIO38~42，因为相机 DVP 总线占用 GPIO4~18
> （D0=11、D1=9、D2=8、D3=10、D4=12、PCLK=13 与旧音频引脚冲突）。

### 控制与其他

| 功能 | GPIO |
|---|---|
| BOOT 按键 | 0 |
| 灯头 PWM 调光输出 | 1 |
| 舵机 UART TX | 43 |
| 舵机 UART RX | 44 |
| 共地 | 所有外设 GND 连到 ESP32-S3 GND |

灯头调光输出为 **单路 PWM、10 位占空比、20 kHz、高电平点亮**，接 LED 驱动/MOSFET 的
调光或使能脚；`LAMP_LIGHT_GPIO` 定义在 [config.h](./config.h)。GPIO1 是本板唯一空闲的
输出脚：相机 DVP/SCCB 占 GPIO4–18，音频占 GPIO38–42，舵机 UART 占 GPIO43/44，
USB Serial/JTAG 占 GPIO19/20，Octal PSRAM 占 GPIO33–37，且 GPIO1 不是 ESP32-S3
的 strapping 脚。灯头只有**单通道亮度**，没有色温或 RGB 通道。

## 实现说明

- 音频走 `NoAudioCodecSimplexPdm`：功放由 I2S 端口 1 发送（BCLK/LRCLK/DATA），
  PDM 麦由 I2S 端口 0 接收（CLK/DATA）。参考 `dfrobot/df-s3-ai-cam`。
- 采样率：输入 16000，输出 24000。
- 相机由 `Esp32Camera` 按 PID 自动识别 OV2640（`OV2640_SUPPORT` 默认开启），
  SCCB 走独立 GPIO4(SDA)/GPIO5(SCL)，不复用 I2C 总线。
- 无 CH32V003 IO 扩展器：RESET/PWDN 置 NC，删除扩展器初始化与 I2C 总线。
- AEC 关闭（PDM 单麦 + 功放无回采参考，与 ov3660 一致）。
- 舵机 UART 固定 GPIO43(TX)/44(RX)，沿用台灯领域代码，无需改动。
- 灯头调光由 [lamp_light.cc](./lamp_light.cc) 驱动。相机 XCLK 已占用 LEDC
  `TIMER_0`/`CHANNEL_0`，调光因此固定用 `TIMER_1`/`CHANNEL_1`；ESP32-S3 的 LEDC
  各定时器必须共用同一时钟源，所以两边都用 `LEDC_AUTO_CLK`，不指定具体时钟。
  初始化失败时 `light_supported` 保持 false，`A10` 会被明确拒绝而不是假装成功。

## 联网路径：小智后台 vs 直连 Qwen

本板有两条互斥的联网路径，由构建角色（`CONFIG_APPLE_LAMP_OMNI_REALTIME`）决定：

| 构建角色 | 协议 | 联网目标 | 是否需小智绑定/激活 |
|---|---|---|---|
| `apple-lamp-ov2640`（simulator，默认） | MQTT / WebSocket | 小智官方后台 | 需要 |
| `local-readonly` / `local-hardware` / `local-twin` | MQTT / WebSocket | 小智官方后台 | 需要 |
| `xiaozhi-delivery` | MQTT / WebSocket | 小智官方后台 | 需要 |
| `omni-poc` / `edge-runtime` | OmniRealtimeProtocol | 直连阿里云百炼 MaaS（Qwen） | 不需要 |

- **小智后台路径**（默认）：开箱即用，走官方绑定、激活、OTA 与设备管理；设备首次联网需
  连接 `Xiaozhi-*` 热点完成配网绑定。
- **直连 Qwen 路径**（`omni-poc` / `edge-runtime`）：跳过小智后台，直接连百炼
  `qwen3.8-omni-flash-realtime`（ASR 为 `qwen3-asr-flash-realtime`）。需要你自己的
  阿里云百炼 `workspace` + `api_key`，在设备 8080 网页面板配置（存独立 NVS 命名空间，
  不打印 key）。接入点形如
  `wss://<workspace>.<region>.maas.aliyuncs.com/api-ws/v1/realtime`。

代码在 [application.cc](../../../../main/application.cc) 中按 `CONFIG_APPLE_LAMP_OMNI_REALTIME`
选择协议；直连实现见 [omni_realtime_protocol.cc](https://github.com/3126872157/xiaoLamp-firmware/blob/codex/edge-runtime-research/main/protocols/omni_realtime_protocol.cc)。

要直连 Qwen，刷 omni 角色而非默认 simulator：

```powershell
python scripts/build_lamp_variant.py --board apple-lamp-ov2640 --role omni-poc
```

烧录后连 Wi-Fi，浏览器打开设备日志打印的 `http://<设备IP>:8080/` 面板，填入百炼
`workspace` + `api_key`（region 默认 `cn-beijing`）即可。

## MCP 技能列表（云端 Qwen 可调用）

云端模型可调用的技能由构建角色决定。[omni_contract.h](https://github.com/3126872157/xiaoLamp-firmware/blob/codex/edge-runtime-research/main/protocols/omni_contract.h)
的 `omni::Session()` 与 [lamp_edge_runtime.cc](./lamp_edge_runtime.cc) 的 `Tools()` 决定最终
`session.update` 里的 `tools` 数组；协议层只接受已声明的工具名。

### omni-poc 角色（2 个，只读）

| 技能 | 参数 | 作用 |
|---|---|---|
| `lamp_get_state` | 无 | 读本地台灯缓存状态；不运动、不使能 |
| `lamp_end_conversation` | 无 | 明确告别/停止聊天时结束会话；不坐下、不卸力 |

### edge-runtime 角色（10 个，含动作）

| 技能 | 参数 | 作用 |
|---|---|---|
| `lamp_get_state` | 无 | 查询机械臂缓存；`available` 才表示五轴反馈新鲜 |
| `lamp_end_conversation` | 无 | 广义告别/停止聊天；不坐下卸力 |
| `lamp_rest` | 无 | 完整「去休息」：使能→坐下→卸力→成功后告别 |
| `lamp_set_mode` | `mode`: sit/stand/work | 明确站起/坐下/工作，保持 yaw |
| `lamp_stop` | 无 | 停止运动但保持支撑力矩 |
| `lamp_get_task` | `task_id` | 查任务实际完成结果 |
| `lamp_set_torque` | `enabled`: bool | 明确使能/卸力 |
| `lamp_record_calibration` | `target_id`: user/book | 手摆对准后记录标定 |
| `lamp_express` | `skill_id` / `action` / `speak` / `repetitions` / `end_mode` | 说话伴随表达：A01 点头、B02 歪头、B01 摇头、A02 兴奋、A03 低头、A16 强调 |
| `lamp_run_sequence` | `steps`（1–16 步）、`enable_if_needed` | 执行动作或有限动作组 |

表达技能 `lamp_express` 的 `action` 别名：`nod`/`sway`/`shake_head`/`tilt`/`emphasize` 等，
`repetitions` 仅对 A01 点头有效（默认 2，明确 3–5 次才设置）。

### 设备端本地 MCP 服务器（`self.lamp.*`，不归云端 Qwen 调用）

由 [lamp_bridge.cc](./lamp_bridge.cc) 的 `RegisterLampTools()` 注册，共 11 个 +
1 个用户专用工具：`list_primitives`、`run_sequence`、`get_state`、`stop`、
`execute_action`、`end_lighting`、`set_light`、`rest`、`release_torque`、`enable_torque`、
`record_calibration`、`configure_bridge`（用户专用）。

这套走设备端 MCP 服务器路径（`local-*` / `local-twin` 角色与电脑桥），在 omni 协议里
`SendMcpMessage` 是空实现，因此云端 Qwen 看不到 `self.lamp.*`。

要模型具备动作能力，刷 edge-runtime 角色：

```powershell
python scripts/build_lamp_variant.py --board apple-lamp-ov2640 --role edge-runtime
```

## 灯光语义（灯头调光）

`self.lamp.get_state` 的 `light_supported` 由 PWM 初始化结果决定，`light_on`/`light_level`
反映当前输出；只有 `light_supported=true` 时 `list_primitives` 才公布 `A10`。

| 语义 | 行为 |
|---|---|
| `set_light(brightness)` | 直接设定灯头亮度 `brightness`（0.0=关，1.0=最亮）；纯输出，任何状态下都可调用，不动姿态、不卸力 |
| `A10` 聚光指示（`hold_mode=timed`，默认） | 对准目标后按本地预设 `brightness` 淡入，保持 `holdSec` 后熄灭 |
| `A10` 聚光指示（`hold_mode=holding`） | 对准目标后灯光保持常亮，直到 `end_lighting` 或 `rest` |
| `A16` 强调（`lightGain > 1`） | 与身体脉冲同节拍闪亮，峰值亮度把 `lightGain` 的 1.0–1.5 映射到 0–100% |
| `end_lighting` | 立即关闭 PWM 输出（等价 `set_light(0)`），不动姿态、不卸力；无驱动时明确返回 `UNAVAILABLE` |
| `rest` | 先关灯，再回坐姿并卸力 |

- `set_light` 是简化后的直接调光入口，`brightness` 取 0.0–1.0，映射到 10 位 PWM 占空比；
  其余复合动作（A10/A16）仍按本地预设 `brightness` 0.1–1 / `lightGain` 1–1.5 驱动灯光。
- 点亮是纯输出：`set_light` / `end_lighting` 不受运动使能、手动占用、示教占用或 BUSY
  限制；但 `A10` 自身含对准动作，仍走正常运动授权。
- 本任务点亮的灯在运动失败或被抢占时会被关掉；`holding` 完成后的常亮由操作者用
  `end_lighting`（或 `set_light(0)`）结束。
- 只有亮度，没有色温/RGB 通道；上电默认关闭。

## 摄像头与视觉接入（AI 如何「看」）

**重要：当前没有任何 `lamp_*` 工具让 AI 主动说「拍照看看」。** 摄像头图像不是通过 MCP
工具触发，而是由 [OmniRealtimeProtocol](https://github.com/3126872157/xiaoLamp-firmware/blob/codex/edge-runtime-research/main/protocols/omni_realtime_protocol.cc)
通过面板路径接入云端模型（`OmniSetVision` 注册的图像源 [lamp_vision.h](./lamp_vision.h)
用 [Esp32Camera::CaptureJpeg](https://github.com/3126872157/xiaoLamp-firmware/blob/codex/edge-runtime-research/main/boards/common/esp32_camera.cc) 取帧；相机以 **PIXFORMAT_JPEG** 直出
（与硬件验证过的 `camera_test` 程序一致，OV2640 JPEG 模式稳定）。
**本板已删除周期自动拍摄**（相机只在「拍照预览」/手动附图时按需拍照，不再后台周期采样）。

| 面板功能 | 操作 | AI 看到图像的方式 |
|---|---|---|
| **拍照预览** | 8080 面板「相机与单图语音」→ 拍照预览 | 仅本地预览，不发给模型 |
| **手动附图** | `开启手动附图`(vision_on) → `开始录音 → 结束并回复` | 图片随语音 `input_image_buffer.append` 加入对话，AI 自然看图回答 |

实操让 AI 看图回答：

1. 打开 `http://<设备IP>:8080/` → 「相机与单图语音」→ 点「拍照预览」确认画面与方向；
2. 点「开启手动附图」；
3. 用面板「开始录音 → 结束并回复」说「看看桌上现在有什么」，AI 会在结束录音时带上图片回答。

⚠️ 手动附图要求：图片须在 30 秒内（过期需重拍）；切换附图开关前先断开会话；VAD 前先关闭附图。

若需要「AI 在对话中主动调用摄像头拍照并回答」的专用工具（如 `lamp_capture_image`），
这是当前固件未实现的扩展点，需在 omni 协议层新增工具声明与图像插入逻辑。

### 「拍照预览失败：Camera unavailable」排查

`Camera unavailable` 来自 [apple-lamp-ov2640.cc](./apple-lamp-ov2640.cc) 里
`esp_camera_sensor_get() == nullptr`，即 `esp_camera_init()` 失败——SCCB I2C 总线
probe 不到 OV2640（地址 `0x30`），不是面板或 AI 的问题。

**根因：相机 RESET/PWDN 未驱动。** 微雪 ESP32-S3-CAM 的 OV2640 复位是通过板载
CH32V003 IO 扩展器（PIN_0/PIN_1）控制的，而本板没有扩展器，[config.h](./config.h)
又把 `CAMERA_PIN_RESET`、`CAMERA_PIN_PWDN` 设为 `GPIO_NUM_NC`，导致相机模块从未被
复位/上电，SCCB 探测不到。

修复（二选一，取决于你的相机模块接线）：

1. **RESET/PWDN 接到直接 GPIO**（推荐）：在 [config.h](./config.h) 里给
   `CAMERA_PIN_RESET` / `CAMERA_PIN_PWDN` 配置实际 GPIO，`esp_camera` 会自动执行
   复位时序（见 `esp_camera.c` 的 `pin_reset`/`pin_pwdn` 分支）。
   注意避开已占用引脚与 strapping 脚（GPIO0/3/45/46），可用 GPIO1/2/14/19/20/21 等。
2. **相机模块硬接线**：RESET 常接 3.3V、PWDN 常接地（常上电），则保持 NC 即可，
   但需确认相机供电、XCLK(GPIO15) 与 SCCB(GPIO4/5) 实际连通，并共地。

判定与验证：启动日志若出现 `Detected camera not supported` 或 `sccb init err`，即 probe
失败；识别成功则打印 `Camera PID=0x26 0x40`（OV2640 PID 0x2640）与 `Detected OV2640 camera`。
烧录后看串口日志即可确认是「未复位」还是「SCCB 接线/供电」问题。

## Windows 构建

在项目根目录的 ESP-IDF 终端执行：

```powershell
$env:PYTHONUTF8 = '1'
$env:CMAKE_NINJA_FORCE_RESPONSE_FILE = '1'
python scripts/build.py waveshare/apple-lamp-ov2640 --name apple-lamp-ov2640 --language zh-CN
```

其他构建角色（local-readonly / local-hardware / local-twin / xiaozhi-delivery / omni-poc / edge-runtime）
使用对应的 `--name apple-lamp-ov2640-<role>`；也可用角色构建脚本（自动归档/校验/烧录）：

```powershell
python scripts/build_lamp_variant.py --board apple-lamp-ov2640 --role local-hardware
python scripts/build_lamp_variant.py --board apple-lamp-ov2640 --role local-twin --archive-only --flash --port <COM>
python scripts/build_lamp_variant.py --board apple-lamp-ov2640 --role xiaozhi-delivery
```

`xiaozhi-delivery` 角色启用增量示教与 Flash 动作库；跟书仍是不完善的实验功能，
不能作为交付能力。该角色使用独立分区表
`partitions/v2/16m_lamp_delivery.csv`（assets 7MiB + `lamp_actions` 1MiB）；迁移与验收见
[交付指引](../../../../docs/apple-lamp-xiaozhi-delivery.md)。

MCP 桥地址配置与语义目录同步同样支持 `--board`：

```powershell
$env:SIM_BRIDGE_TOKEN = '同一个随机令牌'
python scripts/configure_lamp_bridge.py --board apple-lamp-ov2640 --url http://电脑热点网卡IP:8787
python scripts/sync_lamp_catalog.py --board apple-lamp-ov2640 --check
```

## 分步硬件验证

1. `python -m esptool --chip esp32s3 --port <COM> flash-id` 只读确认硬件。
2. 编译后检查 `sdkconfig` 中板型 `BOARD_TYPE_APPLE_LAMP_OV2640`、16 MB Flash、
   Octal PSRAM、USB 控制台与 AEC 关闭设置。
3. `idf.py -p <COM> flash` 烧录，再 `idf.py -p <COM> monitor` 查看启动日志。
4. 确认 PSRAM、相机（PID 0x2640）、PDM 麦与功放初始化正常，且不发生循环崩溃。
5. 连接日志提示的 `Xiaozhi-*` 配网热点，绑定后逐项验证播放、录音/语音对话、拍照问答。
   本板需外接扬声器；串口日志不能替代声学实测。

## 舵机状态不更新的排查

内置页面（`http://<设备IP>:8080/`）每秒轮询 `GET /api/lamp/state`，舵机反馈区每个舵机一行：

```
ID n · tick <位置> · <温度>°C · <电压>V · 扭矩 <0/1> · 模式 <m> · 位置新鲜|位置过期 · <错误原因>
```

`available` 为真要求五个舵机的位置反馈都新鲜（约 1.5s 内）。

排查顺序：

1. **看页面每行末尾**。若显示 `位置过期 · CMD 28 read timeout/transport error`，
   说明 UART 发出了请求但**没有收到舵机总线应答**，属于总线侧问题，不是网页问题。
2. **核对固件引脚**：舵机 UART 固定在 UART0 的 `GPIO43(TX)/GPIO44(RX)`、115200 8N1，
   硬编码在 `lamp_local_executor.cc`；两根线不能与相机/音频引脚复用。
3. **核对接线**：BusLinker v3.0 → GPIO43→J1 第1脚 EX_TX，GPIO44←第2脚 EX_RX，**共地**；
   J8 跳帽 1–3、2–4；舵机独立供电，逻辑接口 3.3V。
4. **确认控制台设置**：本角色使用原生 USB Serial/JTAG 控制台并禁用次级 UART 控制台，
   UART0 才能空出来给舵机总线（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`、
   `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`）。

固件侧已核对：本板的 `lamp_*` 逻辑与已验证的 ov3660 **逐字节相同**，且 `uart_enabled`、
`setup_error`、`read_failures`、`last_error` 都会随 `/api/lamp/state` 返回，可直接判读。
本板不使用微雪 CH32V003 IO 扩展器，舵机总线由外部 BusLinker 独立供电。

## 注意事项

- GPIO45/46 是 ESP32-S3 strapping 引脚（VDD_SPI 电压选择）。用作相机 D0/D3 是微雪
  已验证的设计，自焊板需保证上电时这两个脚不被外部拉低/拉高导致误判。
- 功放 SD 脚默认常开；如需软件静音，需在板 `.cc` 额外增加使能 GPIO 控制。
- 本板为定制板，实机仍需验证相机识别、PDM 采音、功放出声与舵机 UART。
