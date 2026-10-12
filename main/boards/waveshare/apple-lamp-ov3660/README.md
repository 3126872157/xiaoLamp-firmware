## Edge Runtime 首批（2026-10-08）

独立构建 `python scripts/build_lamp_variant.py --role edge-runtime`，身份为 `apple-lamp-ov3660-edge-runtime`。基于同一Omni PCM与UART链增加有界Runtime语义工具：8项工具、异步确认、批次控制优先、完整Call ID及缓存、取消本轮拥有的任务。上电不使能；云动作仍需明确上力、释放网页手动控制和通过原路径安全检查。

GET `/api/lamp/runtime` 读取缓存与能力。普通动作返回accepted；力矩、标定、休息等待真实反馈。一般告别只结束会话，台灯休息成功后才坐下卸力并告别。图片、陪伴、表达预备/实际播放同步、五分钟滚动与完整遥测未实现，保留原omni-poc角色。实施/进度见父项目docs/05。先软件演练，实机小范围由用户明确使能验证。

# Apple Lamp OV3660：第一轮无屏适配

2026-10-08 新增实验构建 `apple-lamp-ov3660-omni-poc`（`--role omni-poc`）。
ESP直连百炼3.8 Realtime，PCM语音／取消／状态与结束对话工具；凭证在8080网页配置。
初版RAM保存，后按用户要求增加独立 `lamp_omni` NVS持久化；不改舵机校准。
用户已确认回复截断问题结束，能够流畅对话。新增lamp_end_conversation：告别播完且播放队列排空后退出VAD会话，in_use=false；不调用舵机动作。此工具语义联调待测。AEC关闭，半双工自然语音打断按用户要求延至后期。
不需要官方小智绑定；不改板级引脚或原0.1标签。
完整开通、构建、测试步骤见父项目 `docs/04_omni_realtime_poc.md`，
固件协议位于 `main/protocols/omni_realtime_protocol.*`。原文以下保留硬件基线。

本地执行器功能分支已增加独立只读构建角色，模拟器角色仍保留。
当前阶段与构建/产物区别见[本地执行器说明](../../../../docs/apple-lamp-local-executor.md)；只读版尚未烧录，不代表实机动作已支持。

2026-10-06 已增加设备端语义动作 MCP 与 HTTP 网页执行器适配，见
[板级 MCP 联调说明](../../../../docs/apple-lamp-mcp-bridge.md)。下面保留第一轮硬件基线。
新固件已于2026-10-06烧录，实机热点联网、网页桥鉴权状态读取和云端 MQTT 上线通过；
UART、机械运动与本地动作存储仍未接入。语音调用完整闭环留给用户测试。

硬件：微雪 ESP32-S3-CAM-OV3660，ESP32-S3R8，16 MB Quad Flash，8 MB Octal PSRAM。

上游：`https://github.com/78/xiaozhi-esp32`，基准 commit
`0d576d3d4c049c6f55eaf879725dc23e516511b4`（项目版本 2.5.1）。
本地定制分支：`codex/apple-lamp-ov3660`。SDK：ESP-IDF 6.1。

## 运行时工具表（改接口不用重编）

`edge-runtime` 的工具清单编译期固定，但可以存一张受校验的表来改写接口，不必重编固件：

- 位置：NVS 命名空间 `lamp_tools`、键 `table_v1`，格式 `lamp-tools/1`；
  面板 `GET/POST /api/lamp/tools`（`{"op":"validate"|"save"|"clear","table":{…}}`）读写。
- 可改：任何已公开工具的描述/schema、人格 `instructions`、**新增工具**（≤8 个）。
- 新增工具只能复用编译期 operation（`execute_group`/`cancel`/`get_task`/`set_torque`/
  `record_calibration`/`rest`/`get_state`），`args` 模板要过 `lamp::Command()` 语义校验；
  执行时模板优先，模型不能改写被模板固定的参数。
- 不可改：operation 集合、内置工具的 operation、引脚/限位/轨迹/标定语义；
  内置工具只可覆盖不能删除（`lamp_stop`、`lamp_set_torque` 永远在列表中）。
- 上限：整表 ≤3000 B、instructions ≤1500 B、描述 ≤320 B、schema ≤1200 B。
- 字段与示例见 [板级 MCP 联调说明](../../../../docs/apple-lamp-mcp-bridge.md) 的「运行时工具表」。

## 音频上行与溢出

麦克风上行的队列容量固定（8 帧 × 60 ms），溢出时改为**丢最旧帧保住整轮**，只有连续丢帧才升级为
"拒绝本轮 + 600 ms 静音恢复"；音频在循环里优先于入站事件，落后时合并已排队帧，长消息发送前先
让路。计数见 `/api/lamp/runtime` 的 `audio_uplink`，策略与取舍与不做项见
[docs/apple-lamp-audio-uplink.md](../../../../docs/apple-lamp-audio-uplink.md)。

## 本轮范围

- 独立板型和 OTA 身份 `apple-lamp-ov3660`。
- 沿用官方微雪 I2C、CH32V003、ES7210/ES8311 和摄像头引脚。
- 使用基类的 `NoDisplay`，不初始化 SPI LCD、不注册背光。
- 保留 LVGL 内存初始化：上游联网后的资源加载仍会解析字体，无屏时也需要分配器；不创建屏幕或显示任务。
- 保留上游扩展 IO 复位／使能时序。
- 摄像头沿用 RGB565 / QVGA / 双帧缓冲 / PSRAM。
- 音频沿用上游 ADC 通道布局，但设备与云端 AEC 均关闭。
- 日志走原生 USB Serial/JTAG；GPIO43/44 未初始化为舵机 UART。
- BOOT 单击切换对话；启动时单击或运行时长按进入 Wi-Fi 配网。
- 不加入动作控制、存储、示教、持续视觉或灯光。

板型继承项目默认的 16 MB 双 OTA 分区布局，PSRAM 继承 S3 的 Octal / 80 MHz 配置。
此阶段不修改动作存储分区。

## Windows 构建
在项目根目录的 ESP-IDF 终端执行：

```powershell
$env:PYTHONUTF8 = '1'
$env:CMAKE_NINJA_FORCE_RESPONSE_FILE = '1'
python scripts/build.py waveshare/apple-lamp-ov3660 --name apple-lamp-ov3660 --language zh-CN
```

使用 `esp-ver/apple-lamp-firmware.code-workspace` 打开 VS Code，避免通用 CMake 扩展
自动扫描／配置整个台灯项目。构建和监视使用 ESP-IDF 扩展或上述命令。
响应文件设置用于规避 Windows 创建进程时的长命令限制，不更改硬件配置。

独立上游仓库位于 `esp-ver/xiaozhi-esp32`；在此目录使用 Git 查看定制提交。

## 分步硬件验证

1. 用 `python -m esptool --chip esp32s3 --port COM8 flash-id` 只读确认硬件。
2. 无屏固件编译成功后，检查 `sdkconfig` 中的板型、16 MB Flash、Octal PSRAM、USB 控制台和 AEC 关闭设置。
3. `idf.py -p COM8 flash` 烧录构建产物，再 `idf.py -p COM8 monitor` 查看启动日志。
   COM8 是本轮检测端口，下次连接后应重新确认。
4. 先确认 PSRAM、CH32V003、摄像头和音频初始化正常，且不发生循环崩溃。
5. 连接日志提示的 `Xiaozhi-*` 配网热点，用浏览器进入日志给出的地址；不要把 Wi-Fi 密码写进仓库。
6. 联网和绑定后，逐项验证播放、录音／语音对话、按需拍照问答。
   本板需要外接扬声器才能验证听到的声音；串口日志不能替代声学实测。

设备绑定码、配网提示和对话文本由 `NoDisplay` 写入日志。串口监视器按 `Ctrl + ]` 退出。

## 状态记录

本轮构建、硬件检测和启动结果见项目根目录 `APPLE_LAMP_BRINGUP.md`。
编译通过、初始化通过、真实语音／图片功能通过分别记录。
