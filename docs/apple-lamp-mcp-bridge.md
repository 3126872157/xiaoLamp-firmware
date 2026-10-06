# Apple Lamp：设备端 MCP → 网页执行器

2026-10-06。基于小智 2.5.1 / ESP-IDF 6.1，仅接入 `apple-lamp-ov3660` 板型。
本轮固件已于2026-10-06通过 COM8 烧录。实机确认热点配网保留、MCP 工具注册、
HTTP 200 鉴权状态读取及云端 MQTT 上线。官方 Agent 实际语音调用、长期声学稳定性与
任务内存预算仍待用户实测。网页完成依据始终是 simulation。

实机曾暴露旧 EspTcp 被动关闭时的回调生命周期崩溃；桥 HTTP 已改为 ESP-IDF 原生同步
客户端，在独立桥任务中执行，并等到 STA 有有效 IP 才发请求。未修改 vendor、MQTT 或音频实现。
新增 `esp_http_client` 的组件依赖，保持1.5秒超时、64KiB响应上限、禁止自动重定向及原有去重语义。
首次添加可选的本地配置头时，ccache 可能保留旧 `__has_include` 结果；构建前设置
`$env:CCACHE_DISABLE='1'`，再配置并编译，烧录前须检查镜像确实包含本次地址与令牌，勿打印令牌。

最终 application 镜像 SHA256：
`116379F1EDEB1092F9F60AF12A258CCD0D181FBF74E24C0EBC4177C883B2DC0D`。
这份位于 `build/xiaozhi.bin`，使用分离镜像烧录；下文历史 releases 合并产物不是本次最终配置版。
日志见忽略的 `build/hotspot-mcp-flash-final.log` 与 `build/hotspot-mcp-boot-final.log`。
生产 C++ 协议10项测试通过，板型编译与格式检查通过。完整宿主套件108项出现12项
Windows临时目录占用／Bash路径与编码问题，未把该套件报告为全通过。

## 已实现的接口

MCP（官方服务与 ESP 的 JSON-RPC）和 `lamp-action/2`（设备与网页业务数据）是两层。
复用现有 HTTP 中继，不引入第二个 WebSocket 桥、不替换官方语音 OTA 地址。
用户提供的 [官方 MCP 接入点说明](https://my.feishu.cn/wiki/HiPEwZ37XiitnwktX13cEM5KnSb)
介绍的是云端本地服务接入点；本实现使用固件已有的设备端 MCP，无需填写该接入点 token。

| 工具 | 参数 | 行为 |
| --- | --- | --- |
| `self.lamp.list_primitives` | cursor（0–12，默认0）、skill_id（可选） | 每页一个原语，next_cursor=-1结束；可直接查 B02 等选项，available 表示执行器可达 |
| `self.lamp.run_sequence` | sequence_json | 验证完整语义组后入队，返回 bridge_request_id；16步／60秒，整组预检在网页执行 |
| `self.lamp.get_state` | bridge_request_id 或 task_id，均可省略 | 无参数读带 available 的缓存状态；请求ID读凭据；任务ID异步提交新的 get_task 查询 |
| `self.lamp.stop` | task_id（可选） | 高优先级 cancel，全局停止清理尚未发送的运动；不卸力 |
| `self.lamp.execute_action` | action_json | 单个 execute 业务外壳；支持需标定的 A10 holding，不塞进有限组 |
| `self.lamp.end_lighting` | 无 | 提交 end_lighting |
| `self.lamp.configure_bridge` | url、token | 仅用户／操作员工具，NVS保存配置；不注册为模型自行选用的配置工具 |

核心工具依赖 `LampActionExecutor` 的 Submit / Inspect / State / NewRequestId / Ready，
HTTP 实现在 `LampBridgeExecutor`。后续可以实现本地执行器并替换实例，不改模型动作 ID、
语义参数或常规工具注册。桥地址配置是网页适配器独立能力，不放进本地运动接口。

目录来自 Web 生成的 `research/lamp_action_catalog.v2.json`，导入了13个开放原语。
不接受机械 params、axis、delta_rad、XYZ、未开放选项、禁用原语、旧协议或非法对话 context。
模式权限、标定、几何、总时长、预检、默认参数冻结、对话冷却与执行由网页应用负责。
同 request_id 不重复 POST；同ID不同命令拒绝。模拟暂停不被报告为到位。

## 请求和状态：不要混淆两次受理

调用 `run_sequence`：

```json
{"sequence_json":"{\"protocol_version\":\"lamp-action/2\",\"request_id\":\"user-nod-1\",\"steps\":[{\"action_id\":\"execute_primitive\",\"skill_id\":\"A01\"}]}"}
```

此工具立即返回 `bridge_request_id=user-nod-1`、`delivery=queued`，还没有网页 task_id。
`queued` 只是 ESP 网络队列受理；accepted/dispatched/answered 是桥传输状态，均不等于运动完成。

1. `get_state(bridge_request_id="user-nod-1")` 读取此凭据。
2. 当 delivery=answered，检查 response.ok。失败保留网页错误码。
3. 成功时从 response.result.task_id 取实际网页任务ID。
4. `get_state(task_id="实际任务ID")` 创建一条新的查询，立即返回新的 bridge_request_id。
5. 查询该凭据，查看 response.result.status、completion_basis；completed/limited 才是模拟结束。

普通 get_state 不发 HTTP，而读取后台每约2秒刷新、超过6秒失效的状态。
HTTP只在优先级1的工作任务进行，每次网络操作超时1.5秒，不在 MCP 回调中等待网页。
响应读取限制64KiB；普通队列4项、待处理命令32KiB；控制操作保留额外队列空间。
当前保留本次 ESP 启动最多64个执行请求的去重凭据；达到容量后明确拒绝新运动，停止与查询
仍可使用。这是本版端侧内存预算，不等同于网页4096或桥2048的上限，长时间预算尚未实测。

网络结果不明返回 unknown，不自动重复运动 POST。查询同一 unknown 凭据时仅尝试 GET
原桥的请求结果。桥重启或地址改变后不重播旧任务；取消受理不等于已经停止。
代理无任何真实舵机、扭矩或机械反馈操作。

## 自然对话表达

sequence_json 可携带 context：

```json
{
  "protocol_version":"lamp-action/2","request_id":"assistant-42-expression",
  "steps":[{"action_id":"execute_primitive","skill_id":"B02"}],
  "context":{"source":"dialogue","turn_id":"assistant-42","cue":"question"}
}
```

按 `lamp_dialogue_mcp_instructions.md` 配置官方角色：赞同A01、询问B02、否定B01、兴奋A02、
遗憾A03、强调A16；每轮最多一个，通常省略options，自动表达返回参考体态。
BUSY、DIALOGUE_COOLDOWN、DIALOGUE_ALREADY_EXPRESSED 是跳过，不能换ID循环重试。
明确用户动作请求不附加 dialogue context。

本版透传并校验提供的 context，网页执行轮次去重和2.5秒冷却；没有官方后端真实 assistant
turn_id 钩子，也没有 TTS 分段同步。官方模型是否愿意调用、稳定轮次标识与准确音画同步
尚未验证；先接受收到动作即执行，不声称已经与语音逐句同步。拍照仍沿用现有按需工具，
本轮没有连续视觉或新增 face_glance 原语。

## 你的网络与后续实测步骤

目前电脑校园网、ESP手机热点，不能假定二者可互访；USB仅供电／烧录，不会自动建立IP桥。
后续让电脑和 ESP 接同一手机热点，读取电脑该网卡IPv4；若热点隔离设备，改用共同路由器。
不要把 127.0.0.1 写给 ESP。

在 `web-sim` 终端配置相同令牌并启动中继：

```powershell
$env:SIM_BRIDGE_HOST = '0.0.0.0'
$env:SIM_BRIDGE_TOKEN = '自行设置的随机令牌'
npm run bridge
```

另开终端 `npm run build`、`npm run preview`。网页连接地址与令牌见既有桥接说明；防火墙如有
拦截，按需要放行局域网端口8787。代码不修改系统防火墙或校园网设置。

在固件的 IDF 终端，先设置同一个令牌环境变量，再写本地构建配置：

```powershell
$env:SIM_BRIDGE_TOKEN = '同一个随机令牌'
python scripts/configure_lamp_bridge.py --url http://电脑热点网卡IP:8787
idf.py build
# 网络条件具备后再执行，并重新确认 COM 端口：
idf.py -p COM8 flash
idf.py -p COM8 monitor
```

生成的 lamp_bridge_local.h 已被 Git 忽略，脚本不会打印令牌。二进制含连接配置，应妥善保存。
NVS 中通过操作员 configure_bridge 保存过的值优先于构建默认；后续修改该配置时应使用
操作员工具更新。默认无地址时只注册工具，不发送桥请求，原语音和配网配置不变。

刷入后无需注册云端 MCP 接入点；设备重新建立会话并上报工具。先验证工具目录与
“请站起来，然后点头”，再测重复ID、忙、停止、模式／标定拒绝，最后做10–20轮自然对话。
不发送舵机指令；UART、本地存储与机械反馈留给后续本地执行器。

## 本轮验证与复现

- ESP-IDF 6.1 本板编译链接成功，双OTA分区未改；没有烧录。
- 本次合并固件：`releases/apple-lamp-ov3660-mcp.bin`，9,603,229字节；SHA256
  `E0CBB00B0C611363415A53562E69CDB851C3210EF2AA437A2161FE6279B08F40`。
  此构建未配置桥地址，正式联调需按上文写入本地配置并重新编译；旧基线固件保留。
- 生产 C++ 协议测试10项通过，包含非法机械输入、上下文、范围、旧版本、持久照明与重复字段。
- 与真实 Web 目录13项 Schema一致；本次网页闭环通过四步组、重复ID、冲突、整组拒绝、
  同轮表达去重、停止、离线。测试没有使用真实舵机。
- 中继测试5项通过，网页构建通过。
- 全量固件主机测试108项运行，仍有11个既有Windows临时目录清理／Bash工作流错误，
  不报告全量通过。日志在 build/lamp-mcp-host-tests.log。

在固件目录复现：

```powershell
python scripts/sync_lamp_catalog.py --source ../../research/lamp_action_catalog.v2.json --check
python -m unittest discover -s scripts/tests -p test_lamp_protocol.py -v
# web-sim 的 preview 已在 localhost:4173 启动后，脚本自行建立临时本机中继：
node scripts/verify_lamp_bridge.mjs
```

更新目录时，先由 Web 的 export-action-contract.ts 导出，再用 sync_lamp_catalog.py --source
导入固件并重编，不手工维护第二套参数。两个脚本用途与源版本在固件中可追溯。


## 2026-10-06：休息、卸力与标定（未烧录）

在既有固件分支基础上派生 codex/mcp-rest-calibration。增加普通模型可调用工具
self.lamp.rest、release_torque、enable_torque、record_calibration(target_id=user/book)，
分别映射 rest、set_torque(false/true)、record_calibration。仍通过 LampActionExecutor 抽象，
当前只模拟力矩，不修改 UART、不在 ESP 保存真实舵机标定。

休息先通过网页验证路径、切换坐姿，完成后卸力。桥任务只对自己提交的 rest_request_id
监听新鲜 rest_task=completed 且 torque_enabled=false，再至少等2秒；主任务中仅在
listening 且未检测到用户语音时 ToggleChatState，等 speaking 自然结束，不从 idle 误唤醒。
新动作、使能、卸力、取消、桥配置变化或服务器会话变化会撤销旧休息的关闭聊天请求。
正在说话时不会强行截断；长期一直说话或网页失联时不会假报成功或强行关闭。
此行为与真实麦克风、云端 TTS 的联动尚需设备重新上线测试。

卸力取消当前运动，取消未派发的旧运动／使能；使能保持当前姿态，不自动站起。
MCP 标定要求卸力且 idle，记录完整回读姿态，浏览器 localStorage 保存失败报错回滚。
真机移植时由硬件执行器确认全舵机反馈、卸力／上力状态，并存 ESP 本地配置，不用目标值
代替反馈，也不让模型传角度或坐标。

工具描述增加更热情的自主表达、点一次／点两下均一次 A01 调用（默认两次）、
明确3–5次才使用 repetitions；仍一轮最多一次、冷却、busy跳过、卸力禁动。
云端角色设定可采用根项目 research/lamp_dialogue_mcp_instructions.md，未自动修改云端设定。

验证：11项生产协议测试通过，Web 123项与HTTP中继6项通过；C++→HTTP→真实浏览器集成
覆盖手掰UI、持久化标定、卸力拦截、使能不跳、坐下才卸力；板型编译通过，app 0x26ffd0，
OTA分区38%空余。格式检查通过。ESP32按用户要求下线，本次未烧录，不能声称新工具实机通过。
详细控制语义见根项目 docs/mcp_rest_teaching_calibration.md。
