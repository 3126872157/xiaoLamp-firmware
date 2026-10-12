# 台灯软件交接资料

本目录配合仓库根目录的 [交接补充说明](../交接补充说明.md) 使用。

## 文件

- [最小动作接入示例.mjs](./最小动作接入示例.mjs)：Node.js 18+ 本地代理示例。服务器端
  访问 ESP32，浏览器访问代理，不绕过设备鉴权和机械保护。
- [动作文件模板.json](./动作文件模板.json)：`lamp-relative-action/1` 模板。必须替换
  `binding` 和 `model_version`，不能直接当成任意设备的有效动作。

## 运行接入示例

在与设备同一局域网的电脑上：

```powershell
$env:LAMP_DEVICE_URL = "http://192.168.1.50:8080"
node 交接资料/最小动作接入示例.mjs
```

然后访问 `http://localhost:8787`。示例提供：

- `GET /api/state`
- `GET /api/actions`
- `POST /api/play`，请求体 `{"id":"动作ID","end_mode":"return_to_reference"}`
- `GET /api/jobs/<job_id>`
- `GET /api/tasks/<task_id>`

示例启动时从设备首页解析令牌；设备重启后第一次收到 403 会重新获取首页并只重试
当前请求一次。它没有把令牌返回浏览器。当前没有实体设备可供本次运行验证，不能把
示例响应当作实机结果。

线上 HTTPS 课程不能直接使用这个“电脑 localhost”部署方式；需要把同样的代理部署到
能访问设备局域网的课程后端或 VPN/本地代理网络中。
