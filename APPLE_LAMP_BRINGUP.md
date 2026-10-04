# Apple Lamp 无屏小智：第一轮构建与启动记录

日期：2026-10-04。此次完成独立固件工程、无屏板型、编译、烧录、配网与联网后启动修复；当前等待用户完成云端绑定。

## 版本与目录

- 工程：`esp-ver/xiaozhi-esp32`，独立 Git 仓库。
- 上游：`https://github.com/78/xiaozhi-esp32`。
- 上游 commit：`0d576d3d4c049c6f55eaf879725dc23e516511b4`。
- 上游项目版本：2.5.1。
- 定制分支：`codex/apple-lamp-ov3660`。
- 板级适配提交：`a8778fc`（`feat(board): add headless Apple Lamp OV3660 variant`）。
- 联网崩溃修复：`fb29d66`（`fix(board): initialize LVGL allocator before headless asset loading`）。
- SDK：ESP-IDF 6.1，实际位置 `D:/esp/v6.1/esp-idf`。
- IDF Python：`C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe`。
- VS Code 工作区：`esp-ver/apple-lamp-firmware.code-workspace`。
- 用户要求删除的 `esp-ver/hello_world` 已移除，未保存 Arduino 示例基线。

## 编译与烧录结果

| 项目 | 结果 | 证据／范围 |
|---|---|---|
| 官方微雪变体 `esp32-s3-cam-2` | 编译、合并成功 | 仅构建，没有烧录带屏变体 |
| 定制 `apple-lamp-ov3660` | 编译、合并成功 | 中文、无屏、USB 日志、AEC 关闭 |
| Flash 实物检测 | 16 MB | esptool `flash-id`，Manufacturer 20 / Device 4018 |
| 芯片与 PSRAM 检测 | ESP32-S3 rev 0.2，8 MB | esptool 与启动日志共同确认 |
| 定制固件烧录 | 成功 | COM8，写入校验成功 |
| 联网资源加载修复 | 编译、烧录及实机通过 | 在本板补充 LVGL 分配器初始化，沿用已保存 Wi-Fi，云端返回激活流程 |
| 板型／目录／构建选择检查 | 29 项通过 | BoardMenu / BoardSelection / BoardSource 19 项，CiSelection 10 项 |
| C/C++ 格式、补丁检查 | 通过 | clang-format 与 git diff --check |
| 全量主机测试 | 未全部通过 | 98 项运行，11 个错误；5 个 Windows 临时目录清理错误、6 个 Bash 工作流子项错误；不报告全量通过 |

完整主机测试日志：`build/host-tests-final.log`。
定制构建日志和烧录日志：`build/headless-build.log`、`build/flash.log`。
启动日志：`build/bringup-startup.log`。
修复过程：`build/headless-fix-build.log`、`build/headless-fix-flash.log`。
联网验证：`build/bringup-network-fixed.log`、`build/bringup-activation.log`。
日志和二进制位于忽略目录，不提交进 Git；源码与可重建的板型配置已提交。

## 硬件实测结果

1. **PSRAM 正常**：`Found 8MB PSRAM device`，80 MHz，8192K 加入堆。
2. **摄像头初始化正常**：检测到 OV3660 / PID 0x3660，PSRAM DMA 开启，两帧缓冲分配成功，`Camera initialized: format=0`。
   当前仅验证初始化；图片内容、方向、拍照上传与识别尚未验证。
3. **无屏板型正常**：`SKU=apple-lamp-ov3660`，`No-display board ready; USB console, AEC off, UART not enabled`。
4. **音频驱动正常**：ES8311、ES7210、TDM 通道初始化完成，`BoxAudioDevice initialized`，输出 codec 打开成功。
5. **扬声器播放通过**：用户确认已经外接扬声器，并听到“进入配网模式”。麦克风录音和语音对话尚未验证。
6. **Wi-Fi 配网入口正常**：设备启动热点 `Xiaozhi-6355`，DHCP 地址 `192.168.4.1`，Web server started。
7. **配网阶段短时运行正常**：完整启动后观察约 25 秒，未出现崩溃；此前约 35 秒读取中也保持运行。
   初版联网后发现资源加载崩溃，已经修复。修复后一次联网监听约 40 秒，另一次重启监听约 35 秒，未再出现崩溃；未做长时间稳定性测试。
8. **内存观察**：初始化后空闲内部 SRAM 约 222 KB；配网及提示音后约 177 KB，最低约 70 KB。
   这些数字属于配网阶段，不代表语音、视觉同时运行时的预算。
9. **联网通过**：用户在配网页填入热点信息后，设备拿到 IP；重烧修复版后仍能从 NVS 读取 Wi-Fi 自动重连。
   路由器看到的主机名为 `Xiaozhi-6354`，与配网热点 `Xiaozhi-6355` 不同，因为分别使用 STA 和 SoftAP MAC。
10. **云端可达**：修复后 `Ota::CheckVersion` 成功，返回当前版本与激活流程；设备保持 `activating` 等待绑定。
    尚未完成绑定和对话验证。激活阶段内部 SRAM 空闲约 173–179 KB、最低约 65 KB。

摄像头探测日志先出现 `ov3640: Mismatch PID=0x3660`，随后识别 OV3660 成功；前者是候选传感器探测信息，不是本次初始化失败。
当前 `zh-CN` 启动提示来自固件内置音频资源；用户觉得口音像台湾腔。云端对话音色尚未验证。

## 联网崩溃的定位与修复

初版联网后在 `Assets::LvglStrategy::Apply` 构造 `LvglCBinFont` 时发生 `LoadProhibited`。
用当前 ELF 和 addr2line 解码发现栈顶为 `lv_tlsf_malloc`：无屏板型没有初始化 LCD，
也没有运行 LCD 初始化中的 `lv_init()`，但上游资源流程仍调用 LVGL 字体分配器。

修复在本板构造函数中通过 `lv_is_initialized()` 检查后调用 `lv_init()`。
保留约 1 MB PSRAM 的上游 LVGL 分配池，不创建显示屏或显示任务。
没有修改共享 Assets 逻辑，后续裁剪图形资源时可再减少这部分开销。
修复后唤醒模型资源加载成功，忽略 NoDisplay 不使用的字体，正常完成版本查询并等待激活。

## Windows 构建问题与处理

官方首次构建遇到 `ninja: fatal: CreateProcess: The parameter is incorrect`，日志指出命令过长。
通过 CMake 的响应文件机制解决，未修改 SDK 或依赖源码。

```powershell
$env:PYTHONUTF8 = '1'
$env:CMAKE_NINJA_FORCE_RESPONSE_FILE = '1'
python scripts/build.py waveshare/apple-lamp-ov3660 --name apple-lamp-ov3660 --language zh-CN
idf.py -p COM8 flash
idf.py -p COM8 monitor
```

先打开正确的 ESP-IDF 终端再执行。COM8 为本次检测端口，重新连接后应核对。
提供的 VS Code 工作区已关闭通用 CMake 自动配置与 kit 扫描，并设置上述构建环境变量。
串口监视器退出快捷键为 `Ctrl + ]`。

原生 USB 串口自行读取启动日志时，要先释放 DTR/RTS 初始状态再复位；否则可能只读到运行日志，漏掉启动段。
正式调试优先使用 ESP-IDF 串口监视器。

## 构建产物

- `releases/official-waveshare-esp32-s3-cam-2.bin`：官方配置合并产物，仅用于构建对照。
  SHA256：`A8A7F4A1E7D0302122FB69557324A442AD957E00DF2FF3A405E3A2183E00DB65`。
- `releases/apple-lamp-ov3660.bin`：当前已烧录的修复版无屏配置合并产物。
  SHA256：`C38A37C85E07A5623A616058F3FC5FD621BEA12E29F865FD027396F43E2B2338`。
- `releases/apple-lamp-ov3660-initial.bin`：保留供故障对照的初版，会在联网加载字体时崩溃，不能作为当前运行版本。
  SHA256：`E48552D98B36ED823DA28F68DE76DB81A38BA0EC3B5F0AEFECE82822D5A2D0A3`。

合并文件与 `idf.py flash` 使用的分离镜像内容对应。不要把官方带屏对照产物当作台灯默认固件。

## 用户下一步：联网与绑定

1. 手机或电脑连接 `Xiaozhi-6355`。提示该热点没有互联网是正常现象。
2. 浏览器打开 `http://192.168.4.1`。
3. 输入家庭 2.4 GHz Wi-Fi 的 SSID 和密码，保存，让设备联网。
4. 手机恢复正常上网，打开小智官方控制台，登录后添加设备，输入台灯播报的绑定验证码。
   当前上游 USB 日志输出激活网址／状态，数字验证码由扬声器播报，并未直接打印在日志中。
5. 先验证一句话能被识别并得到语音回答，再验证“看看桌上有什么”的按需拍照问答。

已完成 Wi-Fi 入网及重启恢复。尚未完成：云端绑定、麦克风实录、唤醒与对话、拍照识别。
下一轮按这个顺序处理；UART 舵机、动作库、示教和灯光随后加入。
