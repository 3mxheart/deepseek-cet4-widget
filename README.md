# DeepSeek 余额 + 四级倒计时 悬浮窗（原生 C 版）

一个常驻桌面的小窗：左边显示 **DeepSeek 账户余额**，右边显示 **英语四级（CET-4）考试倒计时**。
用 C 写、Win32 + GDI+ 实现，编译出来是 **~230 KB 的单文件 exe**，不依赖 Python / .NET 运行时，
带任务栏托盘图标，背景图片随便换，开机自启一键开关。

![效果示意](screenshots/demo.png)

> 本项目是第三方开源小工具，与 DeepSeek 官方无关，仅调用其公开的账户余额接口。

## 特性

* **余额**：调用 DeepSeek 官方 `/user/balance` 接口，默认 30 秒刷新一次，最快可调到 10 秒
* **倒计时**：显示最近一场还没考完的 CET-4；考完自动跳到下一场，全部考完显示「已结束」
* **逐像素透明**：GDI+ 分层窗口，圆角边缘是真抗锯齿（不是色键抠图）
* **背景图片可自定义**：PNG / JPG / BMP / GIF，等比裁剪不变形；浅色图可调压暗程度
* **托盘图标**：左键显示 / 隐藏，右键出菜单，鼠标悬停看余额与倒计时
* **配置即文件**：位置、不透明度、刷新频率、背景图、API Key 全写在 `config.json`
* **不消耗 token**：余额接口是账户查询，不是模型推理调用，既不扣 token 也不产生费用

## 快速开始

1. 拿到 exe（自己编译见下文），放到任意文件夹，双击运行
2. 右键小窗 → **设置 API Key…**，粘贴你的 Key（`sk-` 开头，在
   <https://platform.deepseek.com/api_keys> 生成），点「保存并测试」
3. 右键小窗 → 勾选 **开机自动启动**

`config.json` 和 `widget.log` 会自动生成在 exe 旁边，换台机器拷走这两个文件即可沿用设置。

> Windows 11 默认把新出现的托盘图标收进任务栏的「`^`」折叠区：点开 `^`，把图标拖到底部那排即可常驻。

## 界面操作

| 操作 | 效果 |
| --- | --- |
| 按住左键拖动 | 移动位置（自动记住） |
| 滚轮上下 | 调整不透明度（自动记住） |
| 双击窗体 | 立即刷新余额 |
| 右键窗体 | 功能菜单（设置 Key / 刷新频率 / 换背景图 / 锁定位置 / 开机自启 / 退出） |
| 托盘图标左键 | 显示 / 隐藏悬浮窗 |
| 托盘图标右键 | 完整功能菜单 |
| 悬停托盘图标 | 提示气泡显示当前余额与倒计时 |

## 配置文件

`config.json` 与 exe 放在同一层，改完重新启动生效（也可以右键 → 编辑配置文件再重载）。
初始状态可以直接从 [`config.example.json`](config.example.json) 复制。

| 键 | 说明 |
| --- | --- |
| `deepseek_api_key` | DeepSeek 的 API Key，形如 `sk-xxxxxxxx` |
| `api_base` | 接口地址，默认 `https://api.deepseek.com` |
| `use_system_proxy` | 是否走系统代理，默认 `true`；遇到 401 或连不上可以改成 `false` 直连 |
| `balance_refresh_seconds` | 余额刷新间隔（秒），默认 30，最小 10 |
| `show_delta` | 余额变化时，标题旁是否短暂显示 `刚 -0.42` |
| `delta_hold_seconds` | 变化量保留多久（秒），默认 600；设 0 表示一直显示 |
| `show_status_line` | 是否在底部显示「更新于 19:37:11 · 每 30 秒刷新」 |
| `show_balance_detail` | 是否显示「赠送 / 充值」那行小字 |
| `exams` | 考试列表，见下文 |
| `background_image` | 背景图绝对路径，留空 = 内置深夜渐变 |
| `window.width` / `window.height` | 窗体尺寸（像素），默认 360 × 112 |
| `window.x` / `window.y` | 位置，`null` = 默认右上角；拖动后自动写入 |
| `window.opacity` | 整体不透明度，0.35 ~ 1.0 |
| `window.rounded` | 圆角半径，`0` = 直角 |
| `window.topmost` | 是否总在最前面 |
| `window.transparent` | 保留字段，本版本始终为逐像素透明 |
| `appearance.scrim_alpha` | 背景压暗程度 0~255，越大字越清楚 |
| `appearance.text_shadow` | 保留字段，本版本始终给文字加淡投影 |
| `appearance.font_family` | 字体，默认 `Microsoft YaHei UI` |
| `appearance.value_size` / `label_size` / `detail_size` | 三档字号 |
| `appearance.label_color` / `value_color` / `detail_color` / `accent_color` | 文字与光晕配色 |
| `lock_position` | 锁定位置，防止误拖 |

### 考试日期

`exams` 是一个按时间排序的数组，倒计时永远指向**最近一场还没考完的**，考完自动跳下一场：

```json
"exams": [
  { "name": "CET-4", "datetime": "2026-12-12 09:00" },
  { "name": "CET-4", "datetime": "2027-06-12 09:00" }
]
```

* 时间写 `YYYY-MM-DD HH:MM`，也可以只写 `YYYY-MM-DD`
* 开考后 3 小时内显示「考试中」，之后切下一场
* 想显示别的考试（六级、考研、期末）同样往数组里加即可，`name` 会直接显示在标题上

## 从源码编译

不需要 Visual Studio：用一个便携版 MinGW-w64（GCC）就行。

1. 下载 [winlibs 版 MinGW-w64](https://winlibs.com/)（选 `x86_64` + `UCRT` 的 zip），
   解压到项目的 `tools\` 目录下，使其成为 `tools\mingw64\bin\gcc.exe`
   （或者干脆把 `gcc` / `g++` / `windres` 装进 PATH，脚本会自动找到）
2. 执行：

```powershell
powershell -ExecutionPolicy Bypass -File build_c.ps1
```

产物是项目根目录下的 `DeepSeek悬浮窗C版.exe`（脚本会自动关掉正在运行的旧实例再覆盖）。

### 命令行参数

| 参数 | 作用 |
| --- | --- |
| （无） | 正常启动 |
| `--check` | 自检：把配置、倒计时、自启动状态、余额实测结果写进 `widget.log` 后退出 |
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
│  ├─ config.c / config.h  配置读写、WinHTTP 查余额、倒计时、自启动开关
│  ├─ ui.cpp               入口 + 窗口 / 菜单 / 对话框 / 托盘 / 后台线程 / GDI+ 渲染
│  ├─ app.rc               图标资源
│  └─ app.ico              应用图标
└─ tools/                  便携编译工具链（不入库，可删）
```

## 常见问题

**余额显示 `--`，下面写着「未设置 API Key」**
右键 → 设置 API Key… 粘贴进去保存；或直接改 `config.json` 的 `deepseek_api_key`。

**提示 `API Key 格式不对：应以 sk- 开头`**
Key 没复制全，注意开头的 `sk-` 也要带上。

**提示 `DeepSeek 拒绝了这个 API Key（401）`**
Key 已失效或被删除，去官网重新生成一个。

**提示「网络不通」或「被本机代理拦下了」**
本机代理（Clash 之类）没开或规则不对，可以把 `use_system_proxy` 改成 `false` 直连再试。

**为什么显示的是金额，不是 token 数？**
DeepSeek 官方只开放了「账户余额（金额）」接口 `/user/balance`，没有 token 余量接口。
想估算 token 余量，可粗略按「1 元 ≈ 数百万 token（视模型而定）」换算。

**余额会实时更新吗？**
DeepSeek 没有余额推送接口，只能定时来查。默认 30 秒一次，可以调到 10 秒。
余额数字是接口给的两位小数元，只有在你**确实调用过 API** 之后才会变——它不是随时间跳的计时器。

**不小心启动了两个？**
不会。程序用会话内互斥量做了单实例保护，第二次启动会直接退出。

**怎么彻底卸载？**
右键托盘图标 → 退出，取消「开机自动启动」，然后删掉整个文件夹即可。
注册表里只留下 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的一条启动项。

## 许可

[MIT](LICENSE)
