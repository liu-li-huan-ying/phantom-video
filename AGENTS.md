# AGENTS.md

## 强制规则（必须遵守，优先级最高）

### 1. 技术栈（2026-08 架构切换后）
- 本项目已**彻底放弃 Python / PyQt**，改用：
  - 后端：Go 1.24.5（`go build` / `go test`）
  - 前端：TypeScript + 原生 CSS（编译用 `npx tsc`，编译产物由 Go `embed` 打包进二进制）
  - 播放：系统 Edge 浏览器（`--app=` 窗口模式）——**唯一播放内核**，依赖其自带 H.264/AAC 专有解码器
- 旧 Python 代码（main.py / app/ / .venv）已删除，**不得**重新引入 Python 运行时。
- **禁止**使用系统 Python（`E:\anaconda3\python.exe`）运行、调试项目代码。

### 2. 磁盘与依赖约束
- C 盘仅剩约 2GB：**严禁**往 C 盘安装任何东西（npm 全局包、Go 模块缓存等）。
- Go：只用标准库（net/http / embed），**零第三方模块**；若需模块缓存，重定向到 E:。
- npm：依赖仅限 `web/` 目录内 `node_modules`（typescript 一个包），项目在 F: 盘不影响 C:。
- ffmpeg.exe（仅作 probe 元数据用，只读调用，不可替代播放）：
  `C:\Users\31697\AppData\Roaming\Python\Python311\site-packages\imageio_ffmpeg\binaries\ffmpeg-win64-v4.2.2.exe`
- Edge 路径：`C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe`

### 3. 常用命令
- 构建后端：`go build -o vplayer.exe ./server`（项目根目录执行）
- 前端编译：`npx tsc -p web/tsconfig.json`
- 运行：`vplayer.exe`（自动启动本地 HTTP 服务并拉起 Edge）
- 冒烟测试：用 `C:\Users\31697\AppData\Local\Temp\opencode\s_30s.mp4`（H.264，30 秒）

### 4. 版本控制（2026-08-19 起强制）
- 项目已纳入 git 管理（`git init` 于 F:\vedioplayer）。
- 每次实现完成一个功能/修复后，**必须** `git add -A` + `git commit`（中文提交信息）。
- 曾发生整个工作区内容被外部因素清空的事件，git 是唯一的防丢失手段。
- 任何大改动前先 `git status` 确认工作区干净。