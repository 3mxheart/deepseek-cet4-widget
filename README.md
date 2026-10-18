# DeepSeek 余额 + 四级倒计时 悬浮窗（原生 C 版）

新版变更：仅需 EXE，设置由程序管理，使用 Windows DPAPI 加密存储在当前用户注册表 `HKCU\Software\DeepSeekWidget`，不再生成配置 JSON 或日志。旧版同目录 JSON 首次启动时自动导入，确认成功后可删除旧 JSON/日志；下文 JSON 示例仅用于说明旧版字段。自定义背景、图标仍需保留原图片文件。

背景裁剪：按住 Shift 拖动悬浮窗可上下、左右调整背景取景，松手保存；图片按覆盖窗口比例缩放，只有超出窗口的方向可以移动。普通拖动仍移动窗口。

余额查询：已取消 Key 必须以 `sk-` 开头的错误限制；预置服务商也支持填写完整 URL 覆盖默认余额接口。中转站需要自己的余额查询接口，聊天 `/v1` 地址不能直接当余额接口。

一个常驻桌面的小窗：显示 **你正在花钱的那家 AI 服务商余额** + **英语四级（CET-4）考试倒计时**。
可以同时监控多家服务商，但卡片上**只显示最近扣过钱的那家**——平时不占地方，谁掉钱了谁出现。

原生 C + Win32 + GDI+，编译出来是 **~230 KB 的单文件 exe**，不依赖 Python / .NET 运行时，
带任务栏托盘图标，背景图和托盘图标都能换。

![效果示意](screenshots/demo.png)

> 本项目是第三方开源小工具，与 DeepSeek 等任何服务商官方无关，仅调用各家公开的账户余额接口。

## 特性

* **多服务商**：内置 DeepSeek / 月之暗面 Kimi / 硅基流动 / OpenRouter，
  其他任何返回 JSON 的接口都能用 `http` 类型自己接
* **只显示刚扣钱的**：谁余额掉了谁出现，带 `-0.07` 这样的变化量；两家同时扣钱就排两行，
  窗口自动加高（也可以关掉这个策略，改成全部显示）
* **逐像素透明**：GDI+ 分层窗口，圆角边缘真抗锯齿（不是色键抠图）
* **背景图 / 托盘图标都可自定义**：背景支持 PNG/JPG/BMP/GIF，图标支持 ICO/PNG/BMP
* **托盘图标**：左键显示 / 隐藏，右键菜单，鼠标悬停看余额与倒计时
* **不消耗 token**：余额接口是账户查询，不是模型推理调用，既不扣 token 也不产生费用

## 快速开始

1. 运行 `DeepSeek悬浮窗C版.exe`（自己编译见下文），放到任意文件夹
2. 右键小窗 → **设置 API Key…**：下拉框选服务商，粘 Key，点「保存并测试」
3. 右键小窗 → 勾选 **开机自动启动**

`config.json` 和 `widget.log` 会自动生成在 exe 旁边，换台机器拷走即可沿用设置。
首次运行只需 EXE：仅当 `config.json` 不存在时才生成默认配置。已有文件无法读取时不会重建。
如果在运行期间手动编辑了 JSON，程序会停止写回配置，保留你的修改；重启后加载新增的中转站。
外部修改后，当次运行的拖动位置、透明度等设置不会写入文件，请重启后再调整。

> Windows 11 默认把新出现的托盘图标收进任务栏的「`^`」折叠区：点开 `^`，把图标拖到底部那排即可常驻。

## 谁扣钱就显示谁

默认 `show_only_charged: true`：每轮查询后，只有**余额变少**的服务商会出现在卡片上，
按掉钱时间倒序排列，最多 4 行；`charged_hold_minutes`（默认 60 分钟）内一直保留，
超过就自动隐藏。一家都没扣过钱时，退回显示第一家，窗口不会空着。

右键菜单里的 **只显示最近扣费的服务商** 可以随时关掉这个策略，改成把所有服务商都列出来。

```
┌──────────────────────────────────┐
│ DeepSeek   -0.07        $19.43   │   ← 刚扣过钱的
│ Kimi       -0.18        ¥47.82   │
├──────────────────────────────────┤
│ CET-4 倒计时               55 天  │
│ 12-12 09:00 开考                 │
│   更新于 17:13:24 · 每 10 秒刷新  │
└──────────────────────────────────┘
```

## 增加其他服务商

右键悬浮窗或托盘 → **管理 API / 中转站…**，可添加、编辑、删除和启用服务商（最多 6 家）。
选择内置接口类型，或选择自定义中转站并填写完整 HTTPS 余额接口、API Key、余额取值路径（如 `data.balance`）、货币前缀与额外请求头。保存后立即刷新，无需重启；取消不会修改配置。

`config.json` 里的 `providers` 是一个数组，加一项就多监控一家：

```json
"providers": [
  { "name": "DeepSeek", "type": "deepseek", "api_key": "sk-xxxx", "enabled": true },
  { "name": "Kimi",     "type": "moonshot", "api_key": "sk-xxxx", "enabled": true },
  { "name": "硅基流动",  "type": "siliconflow", "api_key": "sk-xxxx", "enabled": true },
  { "name": "OpenRouter","type": "openrouter", "api_key": "sk-or-xxxx", "enabled": true },
  { "name": "我的中转站", "type": "http", "api_key": "",
    "url": "https://example.com/api/balance",
    "headers": "X-Api-Key: abc123|X-Foo: bar",
    "json_path": "data.balance", "prefix": "¥", "enabled": true }
]
```

内置 `type` 会自动用对应接口和解析规则：

| type | 接口 | 说明 |
| --- | --- | --- |
| `deepseek` | `api_base` + `/user/balance` | 优先取 CNY 账户，自动识别 ¥/$ |
| `moonshot` | `/v1/users/me/balance` | 月之暗面 Kimi，取可用余额 |
| `siliconflow` | `/v1/user/info` | 硅基流动，取 totalBalance |
| `openrouter` | `/api/v1/credits` | 余额 = credits − usage |
| `http` | 你自己填的 `url` | 通用类型，见下面的字段说明 |

`http` 类型的字段：

| 字段 | 说明 |
| --- | --- |
| `url` | 必填，GET 请求地址 |
| `json_path` | 必填，取值路径，点号分隔，如 `data.balance` |
| `headers` | 可选，额外请求头，多项用 `\|` 分隔；填了 `api_key` 会自动带 `Authorization: Bearer ...` |
| `prefix` | 金额前缀，默认 `¥` |
| `api_key` | 可选（`http` 类型可以留空，如果接口只要自定义头） |

> 有些服务商**没有公开的余额接口**（例如 OpenAI、Anthropic、Gemini 只有用量/账单接口，
> 智谱、阿里云百炼、火山方舟要走各家云平台的签名接口），这类接不进来。
> 判断标准很简单：能不能用一条 GET 请求 + 一个 Key 拿到 JSON 里的余额数字。

## 界面操作

| 操作 | 效果 |
| --- | --- |
| 按住左键拖动 | 移动位置（自动记住） |
| 滚轮上下 | 调整不透明度（自动记住） |
| 双击窗体 | 立即刷新余额 |
| 右键窗体 | 功能菜单 |
| 托盘图标左键 | 显示 / 隐藏悬浮窗 |
| 托盘图标右键 | 完整功能菜单 |
| 悬停托盘图标 | 提示气泡显示当前余额与倒计时 |

右键菜单：立即刷新 / 只显示最近扣费的服务商 / 设置 API Key / 刷新频率 /
更换背景图片 / 恢复默认背景 / **更换托盘图标 / 恢复默认托盘图标** / 打开文件夹 /
编辑配置 / 不透明度 / 锁定位置 / 开机自启 / 退出。

## 配置文件

`config.json` 与 exe 放在同一层，改完重新启动生效；也可以从
[`config.example.json`](config.example.json) 复制一份改。

| 键 | 说明 |
| --- | --- |
| `providers` | 服务商列表，见上一节 |
| `show_only_charged` | 只显示最近扣费的服务商，默认 `true` |
| `charged_hold_minutes` | 扣费后保留多少分钟，默认 60；`0` = 一直显示 |
| `api_base` | `deepseek` 类型的接口地址，默认 `https://api.deepseek.com` |
| `use_system_proxy` | 是否走系统代理，默认 `true`；遇到 401 或连不上可以改 `false` |
| `balance_refresh_seconds` | 刷新间隔（秒），默认 30，最小 10 |
| `show_delta` | 变化时是否显示 `-0.07` 这样的数字 |
| `delta_hold_seconds` | 变化量保留多久（秒），默认 600；`0` = 一直显示 |
| `show_status_line` | 是否显示底部「更新于 … · 每 N 秒刷新」 |
| `show_balance_detail` | 是否显示「赠送 / 充值 / 已用」那行小字 |
| `deepseek_api_key` | 旧字段，仅在 `providers` 不存在时兼容读取一次 |
| `exams` | 考试列表，见下文 |
| `background_image` | 背景图绝对路径，留空 = 内置深夜渐变 |
| `tray_icon` | 托盘图标路径（`.ico` / `.png` / `.bmp`），留空 = 内置蓝底 ¥ 图标 |
| `window.width` / `window.height` | 窗体宽度 / 最小高度（像素） |
| `window.x` / `window.y` | 位置，`null` = 默认右上角；拖动后自动写入 |
| `window.opacity` | 整体不透明度，0.35 ~ 1.0 |
| `window.rounded` | 圆角半径，`0` = 直角 |
| `window.topmost` | 是否总在最前面 |
| `appearance.scrim_alpha` | 背景压暗程度 0~255，越大字越清楚（用浅色背景图时调大） |
| `appearance.font_family` | 字体，默认 `Microsoft YaHei UI` |
| `appearance.value_size` / `label_size` / `detail_size` | 三档字号 |
| `appearance.*_color` | 标签 / 数值 / 说明 / 光晕配色 |
| `lock_position` | 锁定位置，防止误拖 |

### 考试日期

```json
"exams": [
  { "name": "CET-4", "datetime": "2026-12-12 09:00" },
  { "name": "CET-4", "datetime": "2027-06-12 09:00" }
]
```

* 时间写 `YYYY-MM-DD HH:MM`，也可以只写 `YYYY-MM-DD`
* 倒计时指向**最近一场还没考完的**，开考后 3 小时内显示「考试中」，之后自动切下一场
* 写成 `CET-6`、`考研`、`期末` 都行，`name` 会直接显示在标题上

## 从源码编译

不需要 Visual Studio，一个便携版 MinGW-w64（GCC）就够：

1. 下载 [winlibs 版 MinGW-w64](https://winlibs.com/)（选 `x86_64` + `UCRT` 的 zip），
   解压到项目的 `tools\` 目录，使其成为 `tools\mingw64\bin\gcc.exe`
   （或者直接把 `gcc` / `g++` / `windres` 装进 PATH，脚本会自动找到）
2. 执行：

```powershell
powershell -ExecutionPolicy Bypass -File build_c.ps1
```

产物是项目根目录下的 `DeepSeek悬浮窗C版.exe`，脚本会自动关掉正在运行的旧实例再覆盖。

> 项目路径带空格也没关系：脚本会自动切到 8.3 短路径，并把工具链镜像到
> `%LOCALAPPDATA%\mingw64-widget`（GCC 的链接器不认自身安装路径里的空格）。

### 命令行参数

| 参数 | 作用 |
| --- | --- |
| （无） | 正常启动 |
| `--check` | 自检：把配置、各服务商余额实测结果、自启动状态写进 `widget.log` 后退出 |
| `--keydlg` | 启动并直接弹出「设置 API Key」对话框 |

## 目录结构

```
deepseek-widget/
├─ DeepSeek悬浮窗C版.exe   编译产物（不入库）
├─ build_c.ps1             一键编译脚本
├─ config.example.json     配置模板
├─ config.json             你自己的配置（含 API Key，已被 .gitignore 排除）
├─ widget.log              运行日志（同样不入库）
├─ screenshots/            截图
├─ csrc/
│  ├─ config.c / config.h  配置读写、日志、倒计时、自启动
│  ├─ providers.c          各服务商的接口与解析（含通用 http 类型）
│  ├─ ui.cpp               入口 + 窗口 / 菜单 / 对话框 / 托盘 / GDI+ 渲染
│  └─ app.rc / app.ico     应用图标
└─ tools/                  便携编译工具链（不入库，可删）
```

## 常见问题

**就一家显示，其他家不见了？**
这是设计如此：只显示最近扣过钱的服务商。右键关掉「只显示最近扣费的服务商」就能全部列出。

**余额显示 `--`**
看下面的小字：`还没有填 API Key` / `接口返回 401（Key 无效或没有权限）` /
`连不上服务器` / `返回里没找到字段 xxx`，按提示处理即可。

**代理相关报错**
把 `use_system_proxy` 改成 `false` 直连再试。

**为什么显示的是金额，不是 token 数？**
各家开放的都是「余额（金额）」接口，没有 token 余量接口。想估算 token 余量可按各模型单价换算。

**余额会实时更新吗？**
没有推送接口，只能定时查；默认 30 秒，最小 10 秒。余额只有在你**确实调用过 API** 之后才会变。

**不小心启动了两个？**
不会，有单实例保护，第二次启动会直接退出。

**怎么彻底卸载？**
右键托盘图标 → 退出，取消「开机自动启动」，删掉整个文件夹即可
（注册表里只留 `HKCU\...\Run` 下一条启动项）。

## 许可

[MIT](LICENSE)
