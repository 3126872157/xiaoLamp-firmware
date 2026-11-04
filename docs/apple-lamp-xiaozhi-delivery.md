# 小智0.1交付分支：增量示教与 Flash 动作库

基线：`apple-lamp-v0.1` / `3c3d52a6b9f2232f630ba47e3b969f69bfc9bd23`。分支：`codex/xiaozhi-v0.1-delivery`。

这是小智路线的独立交付候选，不包含Edge Runtime/Omni实现，不改0.1标签，不修改小智后端。新角色 `xiaozhi-delivery`，板型身份 `apple-lamp-ov3660-xiaozhi-delivery`。0.1的 `local-twin` 保留。

同样的交付角色也已迁移到 OV2640 定制板，板型身份 `apple-lamp-ov2640-xiaozhi-delivery`
（OV2640 专用 SCCB + 外接 I2S 功放 + 外接 PDM 麦，无 CH32V003/无 ES 编解码；交付逻辑与
ov3660 完全一致）。两者通过 `scripts/build_lamp_variant.py --board <apple-lamp-ov3660|apple-lamp-ov2640>`
区分，角色开关、分区表、工具集与验收流程相同。

## 当前实现与验证边界

- 原8080测试网页及偏置/方向校准保留，新增示教、动作库、跟书、JPEG预览与资源诊断。
- 示教先确认五轴卸力，暂停拍照；采集真实反馈，最多32帧。未保存的草稿仅本次开机有效。
- 文件保存相邻URDF弧度增量，播放以本次实际起点累加。未选关节增量为0，不先回录制首帧；全路径、返回路径、URDF/几何/安装tick及设备限位均检查，拒绝而不削幅。
- 独立LittleFS动作分区，24动作、每文件12KiB上限；ID/描述/增量/时长/安装绑定版本校验，临时文件+同步+原子替换。损坏文件标记不可用，不自动删除或格式化。
- 原语仍在固件；新增用户动作无需重刷固件。可编辑、导入导出和编排1–16个用户动作，整组<=60秒；完整预检后交给原UART执行器，一条曲线连续播放。
- 跟书代码采用现有小智视觉地址/Token，经 ESP 定时 HTTP 请求；但该功能目前不完善，
  不能作为交付能力或验收通过项。模型只返回归一化书本中心的设计，以及本地死区、
  比例增益、单步上限逻辑，均只能视为实验实现。
- 视觉返回需直接JSON或text/answer/content包装，包含found/cx/cy/confidence。格式不符拒绝动作；接口权限、持续调用和定位质量尚须云端实测，不能把本地编译成功当成跟书验收。
- 一个请求在途，JPEG<=64KiB、正文<=8KiB；同步HTTP每次IO超时5秒、整体检查20秒（单次IO最多额外5秒），图像结果15秒TTL。最新JPEG只在PSRAM，无照片持久化。
- 相机整轮互斥；用户说话/播放优先，Flash更新须音频空闲且设备手动卸力。后台队列4项/24KiB，任务回执历史有界；停止使旧帧失效。网络/解析连续3次失败暂停，用户接管、动作、卸力、休息停止跟书，不自动恢复或上力。
- 状态提供frame/年龄、拍摄姿态、模型返回摘要、内部堆/最大连续块/低水位、PSRAM；原对话记录含用户STT、工具及机械任务。

本轮仅本地构建、宿主测试和脚本检查；未烧录、未云端/机械实测、未完成两小时验收。
跟书功能不完善，不能交付；静默网页动作已具备，但云端调用后是否不续说话仍需实测。

## 构建与烧录（交给实测同学）

激活ESP-IDF6.1，确认自己的USB端口。先备份设备安装映射/标准姿态/目标配置以及用户动作；不要整片擦除。

```powershell
python scripts/build_lamp_variant.py --role xiaozhi-delivery
# OV2640 定制板改用 --board apple-lamp-ov2640；默认 board 为 apple-lamp-ov3660。
python scripts/build_lamp_variant.py --board apple-lamp-ov2640 --role xiaozhi-delivery
python -m serial.tools.list_ports -v
# 确认归档身份后再烧录；COM8仅示例。
python scripts/build_lamp_variant.py --role xiaozhi-delivery --archive-only --flash --port COM8
```

SDK脚本会生成角色专属归档和manifest。日常烧录包含应用/分区表/资源等，但没有空动作分区镜像；脚本拒绝镜像跨入0xF00000用户区。安装配置NVS地址和两OTA应用地址保持0.1布局。

交付布局：assets 0x800000起7MiB，`lamp_actions` 0xF00000起1MiB。编译归档检查assets大小。**首次迁移备份后使用整套新归档，不能仅OTA应用就假定旧分区已有动作区。** 初次挂载失败请手动卸力，在网页点初始化并明确确认；设备不会自动格式化。`ESP_FAIL` 只说明分区里还没有合法文件系统（全新分区就是这样），不是烧录失败；初始化被拒时按报错指出的那一项补齐，若指向某个舵机位置反馈过期，查 board README 的“舵机状态不更新的排查”，属于舵机总线问题。

初始化／导入／删除只擦写动作分区，不读取也不应用姿态，因此**不要求测量的姿态落在 URDF 限位内**：卸力后手臂常常停在限位外，校准前用默认零位也可能越界，若把它们也算作前置条件，首次初始化会永远无法完成。示教采集帧仍然要求姿态在限位内，因为它要记录关节值。

回退0.1前导出用户动作：旧0.1把尾部也划在8MiB assets内，旧角色的资源更新/擦写可能覆盖新动作数据。不承诺跨分区布局的回退保留动作。

设备同网访问 `http://<实际IP>:8080/`，无需npm、浏览器串口或电脑桥。小智账号/激活仍沿用原方案；无需百炼Omni Key。现存Omni NVS不被本固件使用，本轮不清设备数据。

## 示教操作

1. 检查并保存安装方向/偏置，重启确认；先小幅验证各轴。修改安装映射会让旧动作绑定不匹配，不能无提示换算。
2. 扶住灯臂，开始示教；等状态recording、真实五轴卸力后再手掰，停稳后记录基准帧及后续帧。
3. 设置参与关节（默认只选灯头Pitch）、段时长，填英文ID/名称/描述，保存到Flash。第1帧时长不参与播放。
4. 结束示教，手动占用仍保留且不上力。点原面板“保持当前位置并使能”，再播放库中的动作。
5. 两个不同安全起点试播同一动作，确认相对变化一致；危险起点应拒绝，且未先发运动。
6. JSON导出后可编辑描述/段时长/增量并导入更新；不能伪造binding来跳过安装确认。导入、删除、初始化共用同一组前置条件：空闲音频 + 手动控制 + 五轴卸力确认 + 五轴位置反馈新鲜 + 已停止跟书；缺哪一项网页和接口都会直接指出该项。
7. 要通过小智调用，退出手动控制，再显式语音使能并确认完成。目录新动作可重新查询，工具定义不用重刷。

## 跟书实验（不可交付）

1. 唤醒小智完成会话/MCP初始化。没下发视觉URL会明确报错；不要把网页上的“可拍照”当成云端服务已配置。
2. 放好一本书，只观察一次，检查实际上传JPEG颜色/镜像、定位与模型返回。
3. 通过原面板的±3°小幅测试核对两个符号：书在画面右侧时需要的URDF Yaw符号；书在下侧时需要的URDF灯头Pitch符号。与舵机direction不是同一个参数；头部旋转改变后需再核对。
4. 在跟书区保存方向及参数。默认未确认，不允许模型猜；配置存小NVS，重启不自动跟书。
5. 点击“切换自主控制并保持当前位置使能”，等待真实力矩确认，再开始跟书。也可退出手动后语音明确使能，再调用开始工具。
6. 首轮不转末端Roll、不移动肩肘；缓慢挪书，检查Yaw/Pitch纠正方向、死区内不抖、目标丢失不乱转。模式包络/物理限位照常生效。
7. 停止后检查旧HTTP结果不再驱动，支撑力矩保留。卸力/休息/手动接管后需明确重新开启，三次失败不无限循环。

当前控制假设图像两个方向可用固定Yaw/Pitch符号映射，无法保证任意Roll/极端姿态下正确；
它是未完成的低频跟书原型，不是交付功能、高频视觉伺服或自动搜书。不得在交付验收、
课程流程或用户承诺中使用。

## 新MCP工具

| 工具                          | 用途                                                  |
| ----------------------------- | ----------------------------------------------------- |
| self.lamp.list_user_actions   | 查询动态目录/描述/安装兼容性，名称描述是数据不是指令  |
| self.lamp.play_user_actions   | ids_json为1–16个ID数组，end_mode语义选项；返回本地job |
| self.lamp.get_delivery_state  | 查跟书/存储状态，或job_id的处理结果                   |
| self.lamp.start_book_tracking | 用操作者已确认的参数一次开启，不自动上力              |
| self.lamp.stop_book_tracking  | 停止并使待处理结果失效，保留支撑力矩                  |

job完成只说明本地处理完成；如果返回hardware task受理回执，仍用原get_state/任务API确认机械completed。不能把两层“completed”混为到位。静默语音回复不作交付硬条件。

## 网页API与日志

原同源与X-Lamp-Token校验保留。GET `/api/lamp/delivery`读缓存；GET `/api/lamp/delivery/jobs/<job_id>`读回执；GET `/api/lamp/delivery/jpeg`读最新预览；POST `/api/lamp/delivery`携带operation/request_id/args，异步受理。普通请求不执行相机/Flash/UART阻塞操作。request_id重用不同内容拒绝，有界历史到期不保证跨重启去重。

操作：teach_start、capture、remove_frame、set_times、save、teach_end、format、import、delete、export、play、play_group、observe、configure_tracking、enable_tracking_control、track_start、track_stop。存储/示教/参数仅网页操作者可写。停止不排在普通队列后，后台视觉结果按控制世代及机械epoch失效。

使用原“导出对话记录”和“导出本页日志”，另保存交付诊断JSON。用户语音、图片与场景描述私下交接，不入Git。

## 同学验收清单

- 校准保存/重启、旧语音和内置动作正常，网页停止/卸力仍有效。
- 至少两个不同起点播放同一增量动作；中间危险帧、设备限位、缺反馈拒绝或停止。
- 示教期间尝试语音运动/网页上力应拒绝；采集反馈断开不能记录“成功”。
- 保存/覆盖/删除/导入、满容量/损坏文件、重启和普通交付固件升级；旧文件不会因失败被覆盖。断电写入试验在已有备份的测试动作上进行。
- 新动作通过目录发现并由小智播放；记录job与hardware task，不信口头“好了”。
- 连续至少10次图像请求；看JPEG与归一化中心。会话关闭/重开、断网、401/额度/解析异常都要记录。服务不可用时不开运动。完成这些实验不等于跟书可交付，仍需另行完成产品级验证。
- 跟书过程中普通用户动作、停止、休息、卸力：旧图不误驱动，无自动上力/重播。
- 连续两小时对话＋跟书＋网页，看堆低水位、最大连续块、错误及恢复；未通过不发布稳定标签。

自动检查命令：`python -m unittest discover -s scripts/tests -p "test_lamp*.py" -v`。本轮31项通过；新增解析/增量/危险中段/图像格式/死区/方向及硬件模拟示教保护。网页脚本语法已检查。硬件、文件系统断电、云定位/持续调用与两小时运行仍未验收。

## 2026-10-12 修正：模式范围与明确动作自动使能

- 内置语义动作（例如直接要求站起/坐下）在未授权时，在同一个UART任务内先保持实际回读位置并使能力矩；五颗舵机确认成功后继续动作。上电仍不主动上力。模型收到明确动作要求应提交run_sequence，不以“请先使能”代替执行。
- dialogue context的自动表达、跟书、用户增量动作播放、网页手动操作与示教不获隐式使能。示教/手动所有权仍阻止语音抢占。
- 三种模式Yaw、末端Pitch、Roll使用完整URDF范围；肩肘的模式工作范围至少为Home附近±45°，与URDF取交集。宽化在加载时标准化，配置重复加载不会继续增长，配置输出反映有效模式范围。
- 粗略几何模型的桌面/底座附加余量降至1毫米；灯罩采样点与大臂线段的粗略距离阈值由约35毫米减为12毫米。继续检查URDF、实际设备tick限位、安装方向/偏置、桌面底座和场景障碍，不禁用安全检查。现有模型不是精确碰撞模型，真实接触与偏置仍需实机核对。
- 自动检查覆盖未上力的明确站起、后台表达不能自动上力、原位使能、站姿向下看/工作向上看、肩肘包络及重复配置加载；本地检查不能替代实机验收。

实机优先测试：从未使能状态直接说“站起来”，再坐下、站起、抬头/低头；看task完成状态与auto_enable诊断。示教中尝试站起应拒绝；仅自动表达或跟书不能重新上力。紧凑坐姿低速测试，不持续命令机械已接触的关节。超出URDF/tick或偏置错误仍应拒绝，保存完整错误区分模式、几何和硬限位。

远端同学的OV2640板型与PC桥接提交已保留合并。上述限位与使能修正同步到OV3660、OV2640两套板级执行器；硬件模拟回归同时编译运行两套源码（31项检查通过）。本次没有改变相机引脚或合入Omni/Edge业务。

## 2026-10-12 修正：跟书/拍照内存压力

用户反馈：普通对话与普通拍照可用，手动单次观察或低频跟书一两次后出现`Insufficient internal heap for vision; retry after audio is idle`，等待音频结束也未必恢复。

定位：该错误来自交付版CaptureAndExplain在采集/HTTP之前自行设置的32 KiB总空闲、8 KiB连续块门槛，并非服务器错误，也没有检查音频是否空闲。图片没有逐张积累；但音频服务和有界对话/任务日志存在常驻占用，小于malloc阈值的日志字符串优先分配内部RAM，静音不等于这些占用释放。日志对具体实测设备的贡献仍需新增堆记录验证，不能仅据报错断言所有问题都来自日志。

实现：
- 相机初始化时预留64 KiB PSRAM JPEG工作缓冲，普通MCP拍照和跟书共享同一把采集锁，每次覆盖；采集/编码/上传错误不丢失工作区，销毁相机时释放。初始化分配失败可在后续拍照时重试。网页仍只保留一张最新预览，不保存照片历史或Flash图片。
- 交付版HTTP收发缓冲显式大于ALWAYSINTERNAL阈值，响应一次预留8 KiB；在当前PSRAM malloc配置下优先使用PSRAM。TLS原有外部内存设置不变，连接每次关闭并清理，不为拍照维持第二条常驻TLS连接。
- 交付版常驻日志字符串容量大于内部malloc阈值，避免128条有界对话/任务记录长期保留大量内部小块。仍保留128条/64 KiB正文限制，容量开销另计；这是PSRAM分配偏好，不保证PSRAM耗尽时绝不回退内部内存。
- 内部保留最低8 KiB总空闲及1 KiB连续控制块；没有取消所有内存保护。错误提供真实free/largest数值，不再声称“音频闲下来就必定恢复”。门槛是资源预算，仍需设备验证并根据实际峰值修订。
- 暂时内存不足分类为deferred；跟书等待下一周期重试，不增加或清零真实网络/解析失败计数，不标记新观察成功。真实连接、认证、编码及解析错误仍遵守原失败规则。
- 串口`Vision heap internal=前->后 largest=前->后 PSRAM=前->后`覆盖成功与错误路径；网页日志保留vision_deferred状态。固定JPEG缓冲减少反复分配，不表示JPEG编码器、TLS和JSON的所有动态分配都已消除。

人工验收：先正常对话20轮，再手动观察至少10次，交替普通拍照和观察，开启跟书持续10分钟；保存堆日志，检查首轮缓存增长后是否稳定。期间说话/停止/卸力/断网测试，确保旧图不驱动、资源不足后能继续观察、真实错误未被伪装成功。两小时组合运行仍是交付验收要求。当前只本地构建与检查，不烧录，不使能；不能宣称实机问题已完全消失。

## 2026-11-04 变更：set_mode 独立工具与目录能力页

现象：用户说“站起来”被模型派给 A03（低头），因当前姿态越出 sit 包络而返回 `Current pose outside mode; restore Home first`。排查确认 set_mode 本身可用（回读在 URDF 内，current→stand 全程 Safe），真正的问题是模型看不到站起该发什么。

根因：
- delivery 角色没有 `lamp_set_mode` 工具，它只存在于 Edge Runtime（`CONFIG_APPLE_LAMP_EDGE_RUNTIME=n`）。
- `Catalog()` 把 7 个 action 硬编码成裸字符串，从不读 JSON 的 `actions`，所以 `set_mode` 的 `mode` 枚举、默认值和说明都没下发。
- 13 个原语的 `description` 也不在白名单里，模型对 A03 的全部认知只有“低头”二字。

改动（OV3660 与 OV2640 同步）：
- 新增 `self.lamp.set_mode`（mode=sit/stand/work，小写校验）、`self.lamp.restore_home`。二者复用 run_sequence 的同一语义 step 信封，未使能时由执行器在实测位置先使能。
- `list_primitives` 改为 14 页：`cursor=0` 返回能力摘要（每条 action 的 id、内联枚举、简短 use），`cursor=1..13` 返回单个原语。actions/controls 只在摘要页下发；controls 不再重复，因为 set_torque/rest/record_calibration 已有专用工具。
- catalog JSON 的 `actions` 补 `execute_primitive` 条目和各 action 的 `use`，`sync_lamp_catalog.py` 增加 `--board`。
- `run_sequence` 与 `list_primitives` 的工具描述改为指向 set_mode，并明确“姿态切换不是原语”。
- 读只读执行器分支改用安全替换，避免 actions/controls 不存在时泄漏。

页预算（实测）：摘要页 831 字节，最大的原语页 709 字节（原先 857），全部在 900 以内。

未验证：本机无主机 C++ 编译器，`test_lamp_protocol.py` 的 catalog 断言未实际运行；两板 delivery 固件未重新编译。真机需要复测“站起来”“切换工作姿态”，并确认日志里出现 `self.lamp.set_mode {mode:"stand"}`。

## 2026-11-04 变更：描述词中文化、能力表扩充、每轮必带表现

问题：工具描述全英文且冗长（18 条共 7846 字节，run_sequence 单条 1901 字节），原语 description 与选项 description 被 StripDescriptions 剥离后完全不可见，6 处旧文案与硬件实现不符，且“说话配动作”被 2.5 秒冷却与“对话表达不自使能”两条限制挡住。

改动（OV3660 与 OV2640 同步）：
- 18 条工具描述改中文并压缩到约 4200 字节；工具名、枚举、skill_id、cue、end_mode 保持 ASCII。高风险意图加中文触发锚点（站起/坐下/开始工作、去休息、别用力、靠近一点、开灯/关灯）。
- 修正 6 处事实错误：get_state 的 simulator/hardware arrival、release_torque 的 simulates torque only、record_calibration 的 future hardware executor、run_sequence 与 set_mode 的互斥指令、execute_action 的 A01..A16 范围、set_mode 未说明手动/示教会拒绝。
- 能力表（cursor=0）新增 postures／completion／enable／expression 四块策略；每页预算上限由 900 提到 1000（摘要页实测 990），原语页仍守 900（实测最差 715）。摘要页不再重复 model_version/mode_targets/limits/local_parameters/execution，它们仍在每个原语页。
- 6 个原语改名消歧：A02 兴奋摇摆、A03 低头低落（非站起）、A04 收拢休息（非坐下）、A11 转开视线、A14 后退避开、B01 摇头否定。skill_id 与 NVS 绑定不变。
- 删除 JSON 中 9 条从未被固件读取的 mcp_tool_descriptions。
- 每轮必带表现：Context() 允许 1–3 步（原为 1 步）、cue 命中任意一步、新增 neutral→A01；executor 去掉 2.5 秒冷却（仅按 turn_id 去重）；对话表达在实测位置可自动使能（示教与手动仍阻止）；表达遇模式包络越界时先回到当前模式 home（保留 yaw）再执行，并标 limited；OV2640 动作失败且带 context 时用灯头亮度脉冲兜底。

验证：双板 sync_lamp_catalog.py --check 通过（sha256 97232dec…），lamp_protocol/lamp_pose_plan 交叉编译语法检查通过，OV2640 delivery 构建成功并归档到 artifacts/lamp/xiaozhi-delivery/4ae9d8b0-2aa4c0164ab2/。

未验证：本机无主机 C++ 编译器，test_lamp_protocol.py 的新断言未实际执行；“每轮必带表现”只能靠指令约束，是否调用工具仍由宿主决定；真机需复测站起/坐下/工作、连续多轮是否都有表现、以及动作失败时的灯光兜底。

## 2026-11-04 修正：上行 UDP 失败不再刷屏；不做固件侧自动动作

现场反馈两个问题：助手说完话没有动作；运行中持续打印 `E EspUdp: Send failed: ret=-1, errno=12`。

**“说完没动作”不放在固件里解决**：是否调用 `self.lamp.*` 完全由宿主决定。曾尝试过在 `HardwareLoop()` 里监视 `kDeviceStateSpeaking`、在宿主没发工具调用时自动补一次 A01，但被判定为错误的层次——固件不应猜测对话意图，也不应在宿主不知情的情况下驱动关节。该实现已完整删除，改由宿主侧的提示词约束“说话必带动作”。固件只保留以下与之相关的**事实性**改动，它们不产生任何自主动作：

- `Context()` 允许每轮 1–3 步、cue 命中任意一步、新增 `neutral`→`A01`，并把 `expression` 策略写进能力表——这是给宿主的可用空间，不是自动行为。
- 对话表达可在实测位置自动使能、越界时先回当前模式 home 再执行（均只在该轮宿主确实发了表达时发生）。
- `DIALOGUE_SKIPPED` 不再是静默的：`turn_id` 重复时打印 `LampHW: Dialogue skipped: turn_id '...' already used`。若宿主整场复用同一个 `turn_id`，第二句起所有表达都会被丢掉，这条日志就是排查证据。

**`errno=12` 是什么**：`EspUdp::Send()` 里 `send()` 返回 ENOMEM，即 lwIP/WiFi TX 队列或 lwIP 堆拒绝了这个数据报，通常出现在链路拥塞或 AP 已经掉线的场景。原实现每 60 ms 一帧失败就 `ESP_LOGE` 一次，永不恢复。现在 `MqttProtocol::SendAudio()`：

- 连续失败计数：第 1 次与每 30 次各打一条 `ESP_LOGW`，不再刷屏；
- 累计 60 次（约 4 秒完全发不出去）时打一条 `ESP_LOGE` 并 `SetError()` 结束本次会话，走正常重连，而不是一直静默；
- 成功一次即清零并提示 `Audio uplink recovered after N failed sends`；`udp_` 为空时也清零。

验证：`ninja` 单独重编 `mqtt_protocol.cc.obj` 通过。真机待测：上行恢复后 `E EspUdp` 是否停止刷屏；`turn_id` 是否被宿主复用（看有无 `Dialogue skipped`）。

边界：OV3660 无调光驱动，动作失败时没有灯光兜底。

## 2026-11-04 修正：WiFi/lwIP 缓冲改从 PSRAM 分配（治 `errno=12` 根因）

`EspUdp: Send failed: ret=-1, errno=12` 的完整链条（逐层可查）：

| 层  | 位置                                                                         | 行为                                                                 |
| --- | ---------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| 1   | `managed_components/78__esp-ml307/src/esp/esp_udp.cc:85,91`                  | `send()` 返回 -1，`errno=12`                                         |
| 2   | `components/lwip/lwip/src/api/err.c:50`                                      | `ERR_MEM` 映射为 `ENOMEM`                                            |
| 3   | `components/esp_netif/lwip/netif/wlanif.c:114-115`                           | `case ESP_ERR_NO_MEM: ret = ERR_MEM` ← **WiFi 驱动分配 TX 缓冲失败** |
| 4   | 同上 `low_level_output` → `esp_netif_transmit_wrap` → `esp_wifi_internal_tx` | 每帧都要把数据复制进 WiFi TX 缓冲                                    |

即：上行音频帧在加密后**根本没离开网卡**，不是服务器不可达。带宽不是原因——上行约 16.7 帧/秒 × 60~140 字节 ≈ 2 KB/s，而现场三条错误间隔正好 60 ms，说明是一次约 200 ms 的发射停顿。

原配置为了省内部 RAM 从 ESP32-C3 抄过来并进一步压缩（`sdkconfig.defaults:25-32`、`sdkconfig.defaults.esp32s3:13-15`）：`ESP_WIFI_STATIC_RX_BUFFER_NUM=3`、`DYNAMIC_RX_BUFFER_NUM=6`、`RX_BA_WIN=3`、`ESP_WIFI_IRAM_OPT=n`、`RX_IRAM_OPT=n`，且 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 未开启——8 MB PSRAM 对 WiFi/lwIP 完全不可用，TX 缓冲只能从内部堆挤。

改动（`sdkconfig.defaults.esp32s3`，仅此一处）：

```
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

连带效果（Kconfig 强制，非手工设置）：`ESP_WIFI_DYNAMIC_TX_BUFFER` 依赖 `!(SPIRAM_TRY_ALLOCATE_WIFI_LWIP && !SPIRAM_IGNORE_NOTFOUND)`，因此 TX 缓冲自动切为静态型——`ESP_WIFI_STATIC_TX_BUFFER=y`、`TX_BUFFER_TYPE=0`、`STATIC_TX_BUFFER_NUM=16`，并启用 32 深的 `ESP_WIFI_CACHE_TX_BUFFER_NUM`。IDF 帮助文本明确写着「If PSRAM is enabled, Static should be selected to guarantee enough WiFi TX buffers」；若 16 个静态缓冲全忙，后续包进入 32 深缓存队列而不是直接返回 ENOMEM，这正是本次要的语义。

构建验证：OV2640 delivery 全量重建成功（2175 个目标），`xiaozhi.bin` 0x2b9170（2,855,280 B，分区余 31%），归档 `artifacts/lamp/xiaozhi-delivery/8bba44bc-d5c22466b230/`，`app_sha256=d5c22466b230…`。生成的 `sdkconfig` 已确认带上述四项。

真机待测（每 10 秒 `SystemInfo: free sram: N minimal sram: M`）：
1. `free sram` 是否明显高于改动前（WiFi/lwIP 缓冲已移出内部 RAM）；
2. 长时间对话是否不再出现 `E EspUdp: Send failed`；
3. 唤醒词、ASR/opus 收发、相机抽帧仍正常（PSRAM 缓冲对时序敏感，必须实测）。

未做：`ESP_WIFI_STATIC_RX_BUFFER_NUM=3` 与 `RX_BA_WIN=3` 仍然偏低（IDF 在 PSRAM 模式下的默认是 10/16 与 16），但它们是既有配置、非本次回归；若第 2 项仍有残留，下一步再抬这两个值。OV3660 板需单独构建验证，配置共用同一个 `sdkconfig.defaults.esp32s3`。

回滚方式：删除该行并重新构建；`sdkconfig` 会在下次 reconfigure 时回到动态 TX 缓冲。

## 2026-11-04 变更：对话动作随语义变化并在语音开始时执行

现象：无论说什么，台灯都在开口前机械地点一次头，情绪动作没有区分度。

根因（三层叠加，都在代码里可查）：
- 能力表 `expression` 策略写的是「每轮至少 1 个表现；有 cue 用 cue，无 cue 用 A01」，
  `run_sequence` 描述又写「允许 A01 兜底」。
- `Context()` 把 cue 映射成**唯一**一个 skill，并且 `neutral` 固定落到 A01。
- 执行器在工具调用时立即执行，动作总是发生在 TTS 之前。

改动（OV3660 与 OV2640 同步，`lamp_edge_runtime.cc` 仅 OV2640 有）：
- `Context()` 的 cue 改成「动作族」：`agreement`=A01/A16、`question`=B02/A03、
  `denial`=B01/A14、`excitement`=A02/A01、`regret`=A03/B02、`emphasis`=A16/A01、
  `neutral`=A01/B02、`attention`=A09/B02、`avoidance`=A11/A14、`surprise`=A14/A02。
  删除 `neutral→A01` 兜底；同一轮重复同一 `skill_id` 直接拒绝（提示用 `repetitions`）；
  `A04` 会回 sit home，按姿态变更处理并在对话里拒绝。
- `lamp_bridge.cc` 的 `run_sequence` 描述写入 cue 表、变化要求、`target_id` 要求和
  「本地等语音开始后执行」，并去掉「A01 兜底」。
- `lamp_pose_plan.cc`：对话动作统一按 `kDialogueIntensity = 0.6` 播放（幅度上限也参与
  安全搜索上限，受限时只会更小）；明确指令仍是 1.0。
- `lamp_hardware_executor.cc`：带 `context` 的 `execute`/`execute_group` 只登记为
  `accepted`（`dialogue_pending`），由 `HardwareLoop()` 在 `kDeviceStateSpeaking` 时提升
  为真实任务；8 秒没有语音则取消。多步动作之间插入 0.9 秒间隔；`last_expression` /
  `expression_repeat_count` 记录连续重复（仅日志，不拦截）；显式动作可抢占还没开始播放的
  对话动作；`BUSY` 只拦第二个对话动作。
- `scripts/tests/fixtures/lamp_hardware_platform.h` 增加可设置的设备状态，
  `lamp_hardware_cli.cc` 覆盖「等待语音 / 语音开始后执行 / 无语音取消 / 显式抢占」四种路径；
  `test_lamp_protocol.py` 覆盖新 cue 族、`neutral` 不再固定 A01、重复 skill 与 A04 拒绝、
  `A09/A11/A14` 的目标要求。
- 新增 [`lamp_dialogue_prompt.md`](./lamp_dialogue_prompt.md)，给出可直接粘贴到小智后台的
  角色提示词（每轮一个轻微动作、必须换动作、`turn_id` 唯一、不重试 BUSY）。

验证：`sync_lamp_catalog.py --check` 双板通过；OV2640 delivery `idf.py build` 成功
（`xiaozhi.bin` 0x2b9f70 / 2,858,864 B，无 warning），归档
`artifacts/lamp/xiaozhi-delivery/d0a8935e-8a8f82013593/`；
`lamp_protocol.cc`、`lamp_pose_plan.cc`、`lamp_hardware_executor.cc`、`lamp_bridge.cc`
（两板）交叉编译语法检查通过。

未验证：本机无主机 C++ 编译器，`test_lamp_protocol.py` / `lamp_hardware_cli.cc` 未实际运行；
语音起点与 TTS 的实际同步、0.6 幅度是否够「轻微」、0.9 秒间隔是否合适、以及新 cue 族是否
真的让模型换动作，都必须在真机复测。OV3660 未重新构建。

