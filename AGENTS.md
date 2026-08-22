# AGENTS.md

## 强制规则（必须遵守，优先级最高）

### 1. 技术栈（2026-08-19 起，C++ 架构）
- 本项目为**自研 C++ 视频播放器**：
  - 语言：C++17（g++ 15.2.0，w64devkit，位于 D:\w64devkit）
  - 构建：CMake + MinGW Makefiles（构建前需临时改名 `D:\w64devkit\bin\sh.exe`，构建完恢复）
  - 解码：FFmpeg 9.0.1（gyan.dev stable shared 构建，位于 `G:\dev\ffmpeg-9.0.1-full_build-shared`，libavformat/avcodec/avutil/swresample，MinGW 用 `-l:xxx.lib` 链接 COFF 导入库）
  - 变速不变调：Sonic（Bill Cox，`G:\vedioplayer\dev\sonic\sonic.c`，零分配单缓冲 TSM）
  - 渲染/音频/窗口：SDL2（SDL_MAIN_HANDLED 方案，不链接 SDL2main）
  - 依赖定位：pkg-config（`PKG_CONFIG_PATH="G:\vedioplayer\dev\sdl2\x86_64-w64-mingw32\lib\pkgconfig"`；FFmpeg 不走 pkg-config，CMake 直接链接 `G:\vedioplayer\dev\ffmpeg-9.0.1-full_build-shared\lib\*.lib`）
- 已彻底放弃 Python / Go / TypeScript 技术栈，不得重新引入。

### 2. 下载与磁盘约束（2026-08-19 起强制）
- **严禁**往 C 盘安装/下载任何东西（C 盘仅剩约 2GB）。
- 一切下载**先说明理由**，再执行；不随便下载工具或库。
- 下载文件一律放 **G 盘**（如 G:\dev\）。
- 解压/使用完毕后**及时删除安装包**（.zip/.7z/.exe 安装器等），只保留解压产物。
- 优先使用本机已有工具（如 7-Zip：`D:\Program Files\7-Zip\7z.exe`），不重复下载。
- 构建产物一律放 `G:\vedioplayer\build\`。

### 3. 阶段记录（2026-08-19 起强制）
- 每个阶段/任务开始前和完成后，**必须在 `docs/DEVELOPMENT_LOG.md` 记录**：
  - 阶段编号与任务内容
  - 遇到的所有困难与解决办法（哪怕最终没解决，也要记录状态）
- 此规则贯彻在**后续每一个阶段**。

### 4. 常用命令
- 配置：`cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release`（需先改名 sh.exe）
- 构建：`cmake --build build`
- 调试版：`cmake -S . -B build-debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug`
- 运行：`build\vplayer.exe <视频路径>`（DLL 已由 CMake 自动拷贝到 build 目录）
- 冒烟测试：`G:\vedioplayer\testdata\4.mp4`（H.264+AAC，29.6 秒）；13 种真实世界格式全部兼容（详见 testdata）

### 5. 版本控制（2026-08-19 起强制）
- 项目已纳入 git 管理（`git init` 于 G:\vedioplayer）。
- 每次实现完成一个功能/修复后，**必须** `git add -A` + `git commit`（中文提交信息）。
- 曾发生整个工作区内容被外部因素清空的事件，git 是唯一的防丢失手段。
- 任何大改动前先 `git status` 确认工作区干净。

### 6. 已知问题备忘
- ~~FFmpeg 库 bug~~（M4 已推翻）：M3 曾怀疑 BtbN master 构建有堆损坏 bug，实为工程代码跨堆 free（见下条）。现用 gyan 稳定版 9.0.1（`G:\dev\ffmpeg-9.0.1-full_build-shared`）。
- FFmpeg 头文件（如 libavutil/opt.h）必须包 `extern "C"`，否则 C++ 符号修饰导致链接失败。
- **重大教训（M4 已定位 M3 崩溃根因）**：`shared_ptr<AVFrame>` 默认删除器是 `delete`（msvcrt 堆），但 AVFrame 由 FFmpeg DLL（UCRT 堆）分配 → 跨堆 free 导致堆损坏 0xC0000374！必须用 `makeFramePtr()`/`makePacketPtr()`（内部传 av_frame_free/av_packet_free 删除器）构造，禁止 `shared_ptr<AVFrame>(ptr)` 裸构造。
- **教训（M5）**：`BlockingQueue::close()` 是永久性的，`Player::close()` 关闭视频队列后若 `openFile()` 不重置，解码线程 push 会立即失败退出（画面/音频静默停摆，进程却不崩溃——**"无崩溃"不等于"在播放"**，验证必须看时钟/画面）。修复：BlockingQueue 提供 `reopen()`，`openFile()` 在 `close()` 后调用。音频时钟=视频时钟回退（clock() 返回 0）是解码线程停摆的典型信号。
- 测试素材：`G:\vedioplayer\testdata\`（13 种真实世界格式：AVI/WMV/MKV/MP4/MOV/RM/3GP/FLV/GIF/MPG/RMVB/SWF/VOB）；API 自动化测试在 `C:\Users\31697\AppData\Local\Temp\opencode\player_api_test.cpp`（编译方式见 DEVELOPMENT_LOG M5）。
- **教训（M20）**：音频状态变更（变速/seek）**必须在 fill() 线程内原子处理**，不可从外部线程直接调用 `rebuildSonic()`/`clearQueue()`/`setClock()`。根因：Player::speed_ 在 rebuildSonic() 前更新 → 视频用新速度但音频还在旧速度；clearQueue+setClock 非原子 → fill() 在两步之间推进时钟；旧 chunk 在 current_ 中按新速率推进时钟→时钟超前。修复：`pendingSpeed_`/`pendingSeek_` 原子量 + fill() 开头 exchange 处理。

### 7. 日志规范（2026-08-23 起强制）

#### 7.1 日志模块
- 统一日志入口：`src/core/logger.h` + `src/core/logger.cpp`（单例 `Logger::instance()`）
- **禁止**在任何 `.cpp` 中自行定义 `static FILE* g_dbg` 或 `static void dbg()`，必须使用统一日志模块
- 初始化：在 `main()` 入口处调用 `Logger::instance().setFile("vplayer.log")` + `setLevel()`

#### 7.2 日志级别
| 级别 | 宏 | 用途 |
|------|-----|------|
| TRACE | `LOG_TRACE(mod, fmt, ...)` | 极细粒度追踪（fill() 每次回调等高频路径，仅调试时开启） |
| DEBUG | `LOG_DBG(mod, fmt, ...)` | 关键状态变化（seek/speed/reanchor/pullFrame 等） |
| INFO | `LOG_INFO(mod, fmt, ...)` | 正常运行里程碑（文件打开/关闭/播放开始等） |
| WARN | `LOG_WARN(mod, fmt, ...)` | 异常但可恢复（队列满/帧丢弃等） |
| ERROR | `LOG_ERROR(mod, fmt, ...)` | 严重错误（解码失败/设备丢失等） |

#### 7.3 日志格式（全 ASCII，Windows 终端兼容）
```
[timestamp] [LEVEL] [MODULE] message
```
- `timestamp`：`[秒.毫秒]`，基于 `steady_clock`，格式 `%lld.%03d`
- `LEVEL`：单字符 `T`/`D`/`I`/`W`/`E`
- `MODULE`：模块标签，固定 5-7 字符：`MAIN`/`FILL`/`SEEK`/`SPEED`/`PULL`/`DECODE`/`ALOOP`/`DEMUX`/`VIDEO`/`AUDIO`/`UI`/`SUB`
- `message`：纯 ASCII，不带 `\n`（Logger 自动换行）
- **禁止中文**出现在日志内容中（Windows 终端 GBK 编码导致乱码）

#### 7.4 模块标签一览
| 标签 | 所在模块 | 说明 |
|------|----------|------|
| `MAIN` | main.cpp | 程序生命周期 |
| `FILL` | audio_output.cpp | SDL 音频回调（fill 函数） |
| `SEEK` | player.cpp | seek 请求与执行 |
| `SPEED` | player.cpp | 变速操作 |
| `PULL` | player.cpp | 视频帧同步拉取 |
| `DECODE` | player.cpp | 解码循环 |
| `ALOOP` | player.cpp | 音频读取循环 |
| `DEMUX` | demuxer.cpp | 解复用器 seek 操作 |
| `VIDEO` | video_renderer.cpp | 视频渲染 |
| `AUDIO` | audio_output.cpp | 音频设备管理 |

#### 7.5 关键路径强制日志
以下事件**必须**记录 DEBUG 级别日志（每个功能/修复提交前检查）：
- [ ] seek 请求发起（requestSeek）
- [ ] seek 执行（doSeek）
- [ ] fill() 中 pendingSeek/pendingSpeed 消费
- [ ] reanchor 触发（writeHead_ 重置）
- [ ] setSpeed 锚定与请求
- [ ] pullFrame audioWait 进入/退出
- [ ] demuxer seek 操作（含 time_base 和 timestamp）
- [ ] audioLoop seek 处理

#### 7.6 日志文件管理
- 输出目录：`<exe_dir>/logs/`，由 `Logger::init("vplayer", 7)` 自动创建
- 文件命名：`vplayer_YYYY-MM-DD_HHMMSS.log`（按启动时间戳命名，每次启动一个新文件）
- 自动轮转：每次启动时删除超过 `keepDays` 天的旧日志（默认 7 天）
- Release 模式每次覆盖（单文件足够），Debug 模式可保留历史
- **禁止**将日志文件提交到 git（`*.log` 已在 `.gitignore`）

#### 7.7 新增模块日志规则
- 新增 `.cpp` 文件时，如涉及关键状态变化，必须引入 `"core/logger.h"` 并添加相应日志
- 日志级别选择原则：正常流程用 INFO，调试/诊断用 DEBUG，高频路径用 TRACE
- 日志内容必须包含关键变量快照（如 pts、clock、speed、target 等），便于事后分析