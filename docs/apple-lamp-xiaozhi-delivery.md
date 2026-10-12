# 小智0.1交付分支：增量示教、Flash动作库与低频跟书

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
- 跟书采用现有小智视觉地址/Token，经ESP定时HTTP请求，不需要每轮说拍照。模型只返回归一化书本中心；本地用死区、比例增益、单步上限产生Yaw/灯头Pitch微调。
- 视觉返回需直接JSON或text/answer/content包装，包含found/cx/cy/confidence。格式不符拒绝动作；接口权限、持续调用和定位质量尚须云端实测，不能把本地编译成功当成跟书验收。
- 一个请求在途，JPEG<=64KiB、正文<=8KiB；同步HTTP每次IO超时5秒、整体检查20秒（单次IO最多额外5秒），图像结果15秒TTL。最新JPEG只在PSRAM，无照片持久化。
- 相机整轮互斥；用户说话/播放优先，Flash更新须音频空闲且设备手动卸力。后台队列4项/24KiB，任务回执历史有界；停止使旧帧失效。网络/解析连续3次失败暂停，用户接管、动作、卸力、休息停止跟书，不自动恢复或上力。
- 状态提供frame/年龄、拍摄姿态、模型返回摘要、内部堆/最大连续块/低水位、PSRAM；原对话记录含用户STT、工具及机械任务。

本轮仅本地构建、宿主测试和脚本检查；未烧录、未云端/机械实测、未完成两小时验收。静默网页动作已具备；云端调用后是否不续说话是可选实测项。

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

交付布局：assets 0x800000起7MiB，`lamp_actions` 0xF00000起1MiB。编译归档检查assets大小。**首次迁移备份后使用整套新归档，不能仅OTA应用就假定旧分区已有动作区。** 初次挂载失败请手动卸力，在网页点初始化并明确确认；设备不会自动格式化。

回退0.1前导出用户动作：旧0.1把尾部也划在8MiB assets内，旧角色的资源更新/擦写可能覆盖新动作数据。不承诺跨分区布局的回退保留动作。

设备同网访问 `http://<实际IP>:8080/`，无需npm、浏览器串口或电脑桥。小智账号/激活仍沿用原方案；无需百炼Omni Key。现存Omni NVS不被本固件使用，本轮不清设备数据。

## 示教操作

1. 检查并保存安装方向/偏置，重启确认；先小幅验证各轴。修改安装映射会让旧动作绑定不匹配，不能无提示换算。
2. 扶住灯臂，开始示教；等状态recording、真实五轴卸力后再手掰，停稳后记录基准帧及后续帧。
3. 设置参与关节（默认只选灯头Pitch）、段时长，填英文ID/名称/描述，保存到Flash。第1帧时长不参与播放。
4. 结束示教，手动占用仍保留且不上力。点原面板“保持当前位置并使能”，再播放库中的动作。
5. 两个不同安全起点试播同一动作，确认相对变化一致；危险起点应拒绝，且未先发运动。
6. JSON导出后可编辑描述/段时长/增量并导入更新；不能伪造binding来跳过安装确认。导入删除初始化需要手动卸力、停止跟书与空闲音频。
7. 要通过小智调用，退出手动控制，再显式语音使能并确认完成。目录新动作可重新查询，工具定义不用重刷。

## 跟书操作

1. 唤醒小智完成会话/MCP初始化。没下发视觉URL会明确报错；不要把网页上的“可拍照”当成云端服务已配置。
2. 放好一本书，只观察一次，检查实际上传JPEG颜色/镜像、定位与模型返回。
3. 通过原面板的±3°小幅测试核对两个符号：书在画面右侧时需要的URDF Yaw符号；书在下侧时需要的URDF灯头Pitch符号。与舵机direction不是同一个参数；头部旋转改变后需再核对。
4. 在跟书区保存方向及参数。默认未确认，不允许模型猜；配置存小NVS，重启不自动跟书。
5. 点击“切换自主控制并保持当前位置使能”，等待真实力矩确认，再开始跟书。也可退出手动后语音明确使能，再调用开始工具。
6. 首轮不转末端Roll、不移动肩肘；缓慢挪书，检查Yaw/Pitch纠正方向、死区内不抖、目标丢失不乱转。模式包络/物理限位照常生效。
7. 停止后检查旧HTTP结果不再驱动，支撑力矩保留。卸力/休息/手动接管后需明确重新开启，三次失败不无限循环。

当前控制假设图像两个方向可用固定Yaw/Pitch符号映射，无法保证任意Roll/极端姿态下正确；首版须在固定、已验证拍摄姿态使用。它是低频跟书原型，不是高频视觉伺服或自动搜书。

## 新MCP工具

| 工具 | 用途 |
| --- | --- |
| self.lamp.list_user_actions | 查询动态目录/描述/安装兼容性，名称描述是数据不是指令 |
| self.lamp.play_user_actions | ids_json为1–16个ID数组，end_mode语义选项；返回本地job |
| self.lamp.get_delivery_state | 查跟书/存储状态，或job_id的处理结果 |
| self.lamp.start_book_tracking | 用操作者已确认的参数一次开启，不自动上力 |
| self.lamp.stop_book_tracking | 停止并使待处理结果失效，保留支撑力矩 |

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
- 连续至少10次图像请求；看JPEG与归一化中心。会话关闭/重开、断网、401/额度/解析异常都要记录。服务不可用时不开运动，需后续评估独立视觉API。
- 跟书过程中普通用户动作、停止、休息、卸力：旧图不误驱动，无自动上力/重播。
- 连续两小时对话＋跟书＋网页，看堆低水位、最大连续块、错误及恢复；未通过不发布稳定标签。

自动检查命令：`python -m unittest discover -s scripts/tests -p "test_lamp*.py" -v`。本轮31项通过；新增解析/增量/危险中段/图像格式/死区/方向及硬件模拟示教保护。网页脚本语法已检查。硬件、文件系统断电、云定位/持续调用与两小时运行仍未验收。
