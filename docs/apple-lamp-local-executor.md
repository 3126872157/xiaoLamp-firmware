# 台灯本地执行器：分版本实施

2026-10-06，功能分支 `codex/lamp-local-executor`。此文的“实机版”当前为只读准备版本，尚不执行机械运动。

| 构建角色 | 独立板型身份 | 执行路径 | UART/机械运动 |
| --- | --- | --- | --- |
| `simulator` | `apple-lamp-ov3660` | 小智MCP→电脑HTTP桥→网页 | 不初始化舵机UART |
| `local-readonly` | `apple-lamp-ov3660-local-readonly` | 小智MCP状态→本地缓存反馈 | UART0只读，拒绝所有运动/扭矩写入 |

两个角色在同一功能分支维护共享MCP语义，构建角色显式选择，产物按角色分目录。
已有模拟器分支 `codex/posture-heading-and-approach` 保留；新增版本没有烧录。用户要求本次暂不做UART回读。

## 构建、归档和回退

先进入ESP-IDF6.1终端，在固件仓库执行：

```powershell
python scripts/build_lamp_variant.py --role simulator
python scripts/build_lamp_variant.py --role local-readonly
```

每次先走项目标准 `scripts/build.py`，再检查二进制角色标记、板型身份和Kconfig。
生成 `artifacts/lamp/<role>/<git-commit>-<app-sha>/manifest.json`，归档所有烧录镜像及SHA256。
`build/`会随角色切换，不能把它当作模拟器备份。产物和本地联网配置不提交Git。

`--archive-only`只归档当前构建且会检查角色；`--flash --port COM8`才烧录，端口必须显式指定。
原生USB实际端口需重新核实，不能使用蓝牙COM口。只读固件不包含可用于实机移动的能力。

## UART实现边界

BusLinker v3.0：GPIO43→J1第1脚EX_TX，GPIO44←第2脚EX_RX，共地；J8跳帽1–3、2–4。
丝印从外部控制器视角命名，不按常见“同名TX交叉”规则接错。逻辑接口3.3V，舵机独立供电。
USB Serial/JTAG为日志控制台，禁用次级UART控制台，UART0为115200、8N1。

只有独立低优先级任务读ID1–5，位置CMD28、温度26、电压27、运行模式30、力矩32。
解析器匹配ID、命令、长度和校验和；帧损坏/回显不会成为有效反馈。缓存带各字段时间戳。
全部位置反馈新鲜才标记状态`available`；这表示反馈可用，运动仍为`motion_enabled=false`。
MCP目录没有公布实机动作；未实现操作明确返回`READ_ONLY`。
不使用曾失败的CMD7/8预置和CMD11启动；后续播放器沿用CMD1直写。

## 数据与下一阶段

网页“导出ESP32本地配置”生成`lamp-device/1`包：URDF弧度、模型/标定版本、Home、模式限位、
13个有效本地动作预设、完整标定、关节链、工具及几何参考。没有STL，也没有velocity/effort限制。
模型仍只用`lamp-action/2`的语义选项，不修改硬件映射或机械限位。

先做纯C++轨迹/FK与Web对照，再接本地播放器，逐项开放已验证能力。不能以构建成功宣称五轴实机通过。
NVS持久化、真实卸力/使能确认、到位确认、休息后关闭对话和实际灯光驱动仍在后续阶段。

验证：Web127项通过、只读协议及角色2项通过，两个角色构建通过。
完整固件宿主测试112项中100项通过、12项受既有Windows临时文件/路径/Ogg环境问题影响。
没有本轮UART或机械运动实测。
