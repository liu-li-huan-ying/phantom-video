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
  - VapourSynth 滤镜（插帧/超分）：mpv v0.41.0 已内置 `-Dvapoursynth=enabled`，运行时动态加载 `VSScript.dll`（位于 `G:\vedioplayer\dev\vapoursynth\runtime\`）。依赖 MVTools v29（`mvtools.dll`）+ Real-CUGAN ncnn Vulkan（`librcnv.dll` + models/），.vpy 脚本位于 `build/vapoursynth/scripts/`
- 已彻底放弃 Python / Go / TypeScript 技术栈，不得重新引入。

### 2. 下载与磁盘约束（2026-08-19 起强制）
- **严禁**往 C 盘安装/下载任何东西（C 盘仅剩约 2GB）。
- 一切下载**先说明理由**，再执行；不随便下载工具或库。
- 下载文件一律放 **`G:\vedioplayer\dev\`**（绝对禁止放 G:\dev\ 或其他路径）。
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
- 运行：`build\phantom_video.exe <视频路径>`（DLL 已由 CMake 自动拷贝到 build 目录）
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
- 测试素材：`G:\vedioplayer\testdata\`（13 种真实世界格式：AVI/WMV/MKV/MP4/MOV/RM/3GP/FLV/GIF/MPG/RMVB/SWF/VOB）；API 自动化测试在临时目录 `player_api_test.cpp`（编译方式见 DEVELOPMENT_LOG M5）。
- **教训（M20）**：音频状态变更（变速/seek）**必须在 fill() 线程内原子处理**，不可从外部线程直接调用 `rebuildSonic()`/`clearQueue()`/`setClock()`。根因：Player::speed_ 在 rebuildSonic() 前更新 → 视频用新速度但音频还在旧速度；clearQueue+setClock 非原子 → fill() 在两步之间推进时钟；旧 chunk 在 current_ 中按新速率推进时钟→时钟超前。修复：`pendingSpeed_`/`pendingSeek_` 原子量 + fill() 开头 exchange 处理。
- **教训（M35）**：`SDL_SetWindowSize` 后立即调 `SDL_GetWindowSize` 返回旧值（SDL 内部通过 WM_SIZE 消息更新，未 poll 前不同步）。解决：直接用 `g_ui.winW/winH`。
- **教训（M35）**：`WM_SIZE` 只设 `g_dirty` 不够——主循环 idle 时 `MsgWaitForMultipleObjectsEx` sleep 最长 200ms，导致 resize 卡顿。解决：`WM_SIZE` 内直接调 `renderOverlay()`。
- **教训（M35）**：C++ fall-through 导致 seekbar 点击穿透到视频区 → `pendingPause` → 误触暂停。任何 UI 区域点击处理完毕必须 `return 0;`。
- **教训（M35）**：overlay 窗口 `HWND_TOPMOST` 但父窗口不是 → 切到其他应用时 overlay 浮在最上面。解决：`WM_ACTIVATEAPP` 失焦时隐藏 overlay。
- **教训（M36）**：非 DPI-aware 进程（PowerShell 测试脚本）的 `GetWindowRect`/`CopyFromScreen` 走虚拟化坐标（0.8x），layered window 局部区域错位丢失——UI 明明在渲染却"截图看不到"。**验证 UI 前必须 `SetProcessDPIAware()`**。
- **教训（M36）**：w64devkit 的 shobjidl.h 只前置声明 IFileDialog（无 vtable），COM 文件夹对话框编译失败。用 `SHBrowseForFolderW + BIF_NEWDIALOGSTYLE` 替代。
- **教训（M36）**：GDI 文字二值 alpha（非黑即透明）在抗锯齿边缘产生硬描边。正解是 luma-alpha：恒用白色渲染字形、亮度作 alpha、输出请求色；必须白色渲染，否则暗色文字双重变淡。
- **教训（M36）**：固定像素间距必须 ≥ 字号实际行高（≈ pt*dpi*1.4），否则缩放后大字号与相邻元素重叠。
- **教训（2026-09-01）**：VapourSynth R79 + VSScript 4.1 + Python 3.13 在 mpv v0.41.0 中集成的关键规则：
  - **不要预加载 VapourSynth.dll**。mpv 内部通过 VSScript.dll 加载，预加载会导致两个 DLL 副本加载→C++ 异常跨 DLL 边界（UB → 0xC0000374 崩溃）。让 VSScript 从 runtime 目录自然加载。
  - **`VSSCRIPT_PATH` 必须指向完整路径**：`G:\vedioplayer\build\vapoursynth\runtime\VSScript.dll`，不是目录。
  - **mpv 的 `--vf` 选项 `set_option_string` 在 pre-init 阶段不支持绝对路径**（`M_OPT_FILE` 校验失败返回 -7）。解决方案：`SetCurrentDirectoryA(exeDir().c_str())` 设 CWD 为 exe 目录，然后用相对路径 `vapoursynth=vapoursynth/scripts/minimal_test.vpy`。
  - **`python313._pth` 必须有 `import site`**，否则 VSScript 初始化静默失败。
  - **`%APPDATA%\vapoursynth\vapoursynth.toml`** 必须映射 VSScript.dll → runtime python.exe+python3.dll。
  - **VSScript API 版本**：`VS_MAKE_VERSION(4,1) = 0x00040001`（不是 `0x04010001`）。
  - **`.vpy` 脚本必须调用 `set_output()`**：`video_out = video_in` 只是 Python 变量赋值，`getOutputNode(0)` 查找的是通过 `clip.set_output()` 注册的节点。缺少 `set_output()` 导致输出节点为 NULL。

### 7. 日志规范（2026-08-23 起强制）

#### 7.1 日志模块
- 统一日志入口：`src/core/logger.h` + `src/core/logger.cpp`（单例 `Logger::instance()`）
- **禁止**在任何 `.cpp` 中自行定义 `static FILE* g_dbg` 或 `static void dbg()`，必须使用统一日志模块
- 初始化：在 `main()` 入口处调用 `Logger::instance().setFile("phantom.log")` + `setLevel()`

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
- 输出目录：`<exe_dir>/logs/`，由 `Logger::init("phantom", 7)` 自动创建
- 文件命名：`phantom_YYYY-MM-DD_HHMMSS.log`（按启动时间戳命名，每次启动一个新文件）
- 自动轮转：每次启动时删除超过 `keepDays` 天的旧日志（默认 7 天）
- Release 模式每次覆盖（单文件足够），Debug 模式可保留历史
- **禁止**将日志文件提交到 git（`*.log` 已在 `.gitignore`）

#### 7.7 新增模块日志规则
- 新增 `.cpp` 文件时，如涉及关键状态变化，必须引入 `"core/logger.h"` 并添加相应日志
- 日志级别选择原则：正常流程用 INFO，调试/诊断用 DEBUG，高频路径用 TRACE
- 日志内容必须包含关键变量快照（如 pts、clock、speed、target 等），便于事后分析

### 8. 已完成功能清单（2026-08-28）
- **M28-M36a**: 基础播放器、欢迎页、红色主题、播放列表、拖拽排序、DPI、i18n、AB 循环、画中画、设置面板、6 段 EQ、缩略图缓存
- **M37**: hwdec 四级回退链（auto-copy-safe → auto-safe → d3d11va → no）、GPL-3.0
- **M38**: seekbar 圆点 thumb（hover 发光）、ctrlBar 整体滑出
- **M39**: layoutRow1 barTopY 统一、seekbar 拖拽、音量 click-drag
- **P0-1**: 音轨/字幕选择菜单（CC 按钮 + 音轨按钮）
- **P0-2**: 外部字幕文件加载（Shift+S、菜单「加载外部字幕...」）
- **P0-3**: 网络流 URL 输入对话框（Ctrl+U）
- **P1-4**: 章节导航菜单（章节按钮 + 章节列表弹出）
- **P1-5**: 播放列表增删（Insert 添加、Delete 移除当前项）
- **P1-6**: EQ 预设按钮（Flat/Bass/Treble/Vocal/Rock）
- **P1-7**: 多文件拖拽（DragQueryFileW loop → 播放列表）

### 9. 教训备忘（P0-P1 阶段）
- **教训（P0-1）**：`TrackInfo` 结构体必须在 `subTracks()` 声明之前定义，否则 `std::vector<TrackInfo>` 找不到类型。
- **教训（P0-3）**：Win32 自定义对话框不能用 `GetMessageW` 做模态循环（父窗口已在 pump），要用 `PeekMessageW` + `WaitMessage` 模式。
- **教训（P1-5）**：`case 'A'` 已被 AB 循环占用，新增快捷键时必须检查已有 case 避免重复。`g_mpv->stop()` 不存在，应改用 `mpv_command` 或跳转下一个。
- **教训（P1-5）**：WM_KEYDOWN 的 switch/if 嵌套层级容易错位，每个 `}` 必须加注释标记所属块（`// switch`、`// if (g_mpv)`）。
- **教训（P1-7）**：`DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0)` 返回文件数量，循环 `DragQueryFileW(hDrop, i, ...)` 获取每个文件路径。
- **教训（P2-4c）**：提取 parentProc 需确保其调用的 30+ 个函数全部在 app_state.h 中声明（不再 static），且所有依赖的常量/类型已在 include 链中可见（如 `ui::CTRLBAR_HIDE_MS` 在 theme.h）。
- **教训（P4-1）**：loadFile() 后立即 unpause 会导致音频在视频就绪前播放→启动卡顿。正确做法：loadFile() 设 pause=1，FILE_LOADED 事件后通过标志恢复。
- **教训（P4-2）**：`MPV_END_FILE_REASON_ERROR` 同时触发 hwdec fallback 和 onPlaybackEnded → EOF 级联。修复：hwdec retry 时跳过 onPlaybackEnded。

### 10. 工程原则（2026-09-01 起强制）
1. **不保留向后兼容**。过时的直接删，别加兼容层、别写 migration、别留 fallback。
2. **选能满足当前需求的最简单实现**。不要预防性抽象，不要多此一举的配置层。
3. **系统分层长**。先跑通一个最小的端到端版本，再往上加东西。绝不为了未完成的复杂度拆掉能跑的东西。
4. **组件保持模块化，关注点分离**。
5. **优先用成熟的、有人维护的库**。没有明确理由别自己重写。
6. **先翻项目里已有的依赖能做什么**，再考虑加新包或自己写。别上来就假设库里没有。
7. **架构决策往长了做**。不接受"先这样以后再换"的临时方案。
8. **先看成熟产品怎么解决同一个问题**，用已验证的模式，别从零发明。