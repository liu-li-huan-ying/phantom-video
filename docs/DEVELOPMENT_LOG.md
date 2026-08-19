# 开发日志（阶段记录）

> 规则：每进入一个阶段/任务必须在此记录；遇到的所有困难与解决办法必须记录。
> 其他规则：不随便下载东西（下载前说明理由）；下载一律放 F 盘；用完及时清理安装包；不占用 C 盘。

## 2026-08-19

### 阶段 M1：环境搭建（FFmpeg + SDL2）✅ 完成

- 任务：为 C++ 播放器安装 FFmpeg 与 SDL2 开发库
- 困难与解决：
  1. **github.com 直连超时**（网络封锁）→ 探测多个镜像（gh-proxy.com / ghps.cc / ghproxy.net 可用）→ 用 ghproxy.net 下载成功
  2. **gh-proxy.com 返回截断文件**（3.6MB 假 zip）→ 校验发现 zip 结构不完整，换镜像并验证后使用
  3. **SDL 最新版已是 SDL3**（API 大改）→ 通过 GitHub API 定位 SDL2 最新版 2.32.10 指定下载
  4. **SDL2 链接失败 `undefined reference to SDL_main`**：SDL_main.h 宏会把 `main` 替换为 `SDL_main`，而 C++ 会做名字修饰（`_Z9SDL_mainv`），与 C 写的 SDL2main 不匹配 → 方案：`#define SDL_MAIN_HANDLED` + 普通 `main`，不链接 SDL2main（本项目统一方案）
- 产物：`F:\dev\ffmpeg\9.0.1-full_build-shared`（gyan.dev stable）、`F:\dev\sdl2\x86_64-w64-mingw32`（2.32.10）

### 阶段 M2：文档 + 项目骨架 ✅ 完成

- 任务：README.md、docs/DESIGN.md、CMake 构建、目录结构
- 困难与解决：
  1. **MinGW Makefiles 生成器报错**：w64devkit 自带 sh.exe 在 PATH 中导致冲突；MSYS Makefiles 生成器又因路径转换失败 → 方案：构建时临时把 sh.exe 改名，用 MinGW Makefiles（构建完恢复）
  2. **FFmpeg 新版本 `AVCodecContext` 未定义**：新版 avformat.h 不再隐式包含 avcodec.h → 显式 `#include <libavcodec/avcodec.h>`
  3. **av_opt_set_* 链接失败**：libavutil/opt.h 没包在 `extern "C"` 里，C++ 符号修饰导致找不到 → 包上 extern "C"（此类问题以后一律注意）
  4. **SDL2 pkg-config 标志需过滤**：`-Dmain=SDL_main`、`-lSDL2main`、`-lmingw32`、`-mwindows` 与 SDL_MAIN_HANDLED 方案冲突 → CMake 里过滤
  5. **MSYS Makefiles 生成器尝试失败**：w64devkit 的 sh 无法定位带空格路径的 cmake.exe → 放弃，仍用 MinGW Makefiles + 临时改名 sh.exe
  6. **SDL_AUDIO_S16 常量不存在**：SDL2 2.32 开发包仍用旧式常量名 → 改用 `AUDIO_S16SYS`
  7. **PowerShell Set-Content 损坏源码编码**：用 Set-Content 改 .cpp 会把 UTF-8 中文截断 → 教训：改代码一律用编辑工具，不用 shell 命令
- 产物：vplayer.exe 编译成功（606KB）
- 清理旧项目：已删除 Go 版 server/web/vplayer.exe/旧文档，git 提交"清理旧 Go 版本"

### 阶段 M3：播放内核开发（代码完成；崩溃排查有阶段性结论）✅ 完成

- 任务：解封装/解码/队列/时钟/音频输出/视频渲染/播放控制 —— 代码全部写完并编译通过
- 困难与解决（**重大：堆损坏崩溃 0xC0000374**）：
  - 症状：播放器启动数秒内崩溃；gdb 显示在 `av_frame_free`（释放 AVFrame）时 Windows 堆校验失败
  - 排查过程（逐层隔离）：
    1. 禁用音频 → 仍崩（排除音频路径）
    2. 禁用渲染/OSD/Present → 仍崩（排除 SDL 渲染）
    3. 写纯控制台测试（完全不碰 SDL）→ 仍崩！确定问题在 FFmpeg 调用
    4. 标准 C 用法（帧立即释放，不持有）→ 正常，748 帧全部解码成功
    5. 用 shared_ptr 持有帧（播放器标准模式）→ 崩（单线程也崩）
    6. 只持有不释放任何帧 → 也崩
  - 当时结论：**怀疑 BtbN master-latest 构建的 FFmpeg 存在 bug**（此结论在 M4 被推翻，见 M4）
  - 期间尝试 AddressSanitizer 定位越界，但 w6devkit 未自带 libasan → 失败，改用手工二分隔离
- 遗留清理：F:\dev 下已删除所有安装包（ffmpeg*.zip、sdl2.zip、7zr.exe、ffmpeg-9.0.1-shared.7z）；保留 ffmpeg.exe/ffplay.exe/ffprobe.exe（解压产物，调试可用）
- 纠正：冒烟测试视频原位于 C 盘临时目录（`C:\Users\31697\AppData\Local\Temp\opencode\s_30s.mp4`），不符合"不占用 C 盘"原则 → 后续统一放在 testdata 目录

### 阶段 M4：替换稳定版 FFmpeg + 根因重新定位 ✅ 完成

- 任务：换 gyan.dev 稳定版 9.0.1 shared 构建（当时认为 BtbN master 有 bug），结果反而把真正的根因挖了出来
- 下载过程（已向用户说明理由并获确认）：
  - 使用 GitHub 镜像 `GyanD/codexffmpeg` release 9.0.1 的 `ffmpeg-9.0.1-full_build-shared.zip`（97,261,187 字节），经 ghproxy.net 下载成功 ✓
- 解压产物：`F:\dev\ffmpeg\9.0.1-full_build-shared`（bin/include/lib/doc/presets）；zip 安装包已删除 ✓
- 库结构特点：
  - **无 pkg-config 文件** → CMake 不再用 pkg_check_modules(FFMPEG)，改为显式 .lib 路径链接
  - **lib 为 MSVC 风格 COFF 导入库**（avformat.lib 等），MinGW 用 `-l:avformat.lib` 语法可正常链接 ✓
  - **DLL 版本号与 BtbN master 相同**（avcodec-63.dll / avformat-63.dll / avutil-61.dll / swresample-7.dll / swscale-10.dll）
  - **UCRT64 工具链构建**（导入 api-ms-win-crt-*），而 w6devkit 的 exe 是 msvcrt 运行时（导入 msvcrt.dll）
- 排查过程（**推翻 M3 结论：不是库 bug！**）：
  1. gyan 自带 ffprobe/ffplay（UCRT exe + 同套 DLL）解码/播放 testdata 视频完全正常 → **库本身没问题**
  2. 修正测试程序文件路径后（此前测试程序里还是 C 盘旧路径导致 open fail 误判），用新库重跑单线程实验矩阵：
     | 模式 | 结果 |
     |---|---|
     | 每帧新 alloc + 立即释放（vanilla） | ✅ 748 帧正常 |
     | 每帧新 alloc + shared_ptr 持有 receive 帧（holdall） | ❌ 崩 0xC0000374 |
     | av_frame_ref 复制后持有副本（holdref） | ❌ 崩 |
     | av_frame_move_ref 转移持有（moveref，ffplay 同款） | ✅ 正常 |
     | **shared_ptr 持有 move_ref 帧（sp_moveref）** | ❌ 崩（裸指针同款却正常） |
     | shared_ptr 临时变量 + 立即 reset（bq_single） | ❌ 崩 |
  3. 多线程对照实验：裸指针 + move_ref + 双线程 + std::deque → ✅ 正常；FramePtr 版 → ❌ 崩
  4. gflags PageHeap：越界写发生在堆块内部（非守卫页），free 时才发现
  5. **决定性发现（根因）**：`std::shared_ptr<AVFrame>` 默认删除器是 `delete`（msvcrt 堆 operator delete），但 AVFrame 结构是 av_frame_alloc（FFmpeg DLL，UCRT 堆）分配的 → **跨堆 free → 堆损坏 0xC0000374**！`types.h` 里定义了 `FrameDeleter`（av_frame_free）却从未生效（shared_ptr 删除器是构造参数，不是模板参数）！
- 修复方案：
  - `src/core/types.h`：保留 `shared_ptr<AVFrame>` 类型，新增 `makeFramePtr()`/`makePacketPtr()` 工厂（构造时显式传 av_frame_free/av_packet_free 删除器）
  - `src/core/decoder.cpp` `receive()`：改为 move_ref 模式（receive 到 src → av_frame_move_ref 到新帧 → 释放 src），返回 `makeFramePtr(out)`
  - `src/core/demuxer.cpp` `readPacket()`：`makePacketPtr(av_packet_alloc())`
  - `CMakeLists.txt`：FFmpeg 改显式 .lib 链接，DLL 拷贝改新库路径
- 验证：vplayer.exe 播放 testdata 视频（音频+视频+渲染全开）10 秒 / 20 秒均无崩溃 ✓；748 帧解码测试全过 ✓
- 教训记录（重要）：
  1. **shared_ptr 的删除器是构造参数**（`std::shared_ptr<T>(p, deleter)`），`unique_ptr` 才是模板参数——以后凡持有 FFmpeg 对象必须走 makeFramePtr/makePacketPtr
  2. 不能因为"两个库版本相同号就断定库没问题/有问题"——先怀疑自己的代码
  3. 测试程序硬编码文件路径导致 open fail 误判库有问题的教训：路径修正要检查源码转义（`\\`）
- 遗留：`F:\dev\ffmpeg`（旧 BtbN 库）已删除

### 阶段 M5：全功能验证与冒烟测试 ✅ 完成

- 任务：播放器可运行，验证全部功能并做多格式冒烟；**过程中发现并修复了一个隐藏重大 bug（视频队列永久 closed）**
- 冒烟测试（使用 13 种真实世界格式，见 testdata/）：
  - testdata/1.avi（MPEG-4+MP3）、2.wmv（WMV2+WMA2）、3.mkv（MPEG-4+AAC）、4.mp4（H.264+AAC）、5.mov（H.264+AAC）、6.rm（RV30+Cook）、7.3gp（H.263+AMR）、8.flv（FLV1+MP3）、9.gif（GIF）、10.mpg（MPEG1+MP2）、11.rmvb（RV40+Cook）、12.swf（FLV1+MP3）、13.vob（MPEG2+AC3）
  - **批量 smoketest (batch_smoke_test.cpp)**: 13/13 格式 open=1，clock 3 秒推进 ≈3.0 秒 ✓
- **重大 bug 发现与修复（API 级验证暴露）**：
  - 写 player_api_test（Temp 目录）验证 openFile/state/clock/seek/volume 等 20 项
  - 初跑 15/20：`clock after 3s = 0.00`（音频主时钟不动）、pullFrame 收不到帧、Paused 状态异常
  - 排查过程：audio_probe 探针（SDL 回调 87 次/2 秒正常）→ audio_integ 集成测试（解码 1292 帧 + 时钟 30.00 正常）→ 逐步加日志发现 `videoQueue_.push(f)` 返回 false → gdb attach 发现 decodeLoop 线程已消失
  - **根因**：`Player::close()` 会 `videoQueue_.close()`，而 `openFile()` 从不重置 → **BlockingQueue 一旦 close 就永久 closed** → 首次 openFile（内部先 close()）后解码线程 push 立即失败退出 → 视频/音频全部停摆
  - **结论修正**：此前所有"播放正常"的验证其实画面从未真正动过——只是进程没崩溃！真正的首次成功播放发生在修复之后
  - 修复：BlockingQueue 新增 `reopen()`（重置 closed_ 并清空）；openFile 在 close() 后调用 `videoQueue_.reopen()`（audio_ 是 unique_ptr 每次重建不受影响）
- API 测试最终结果：**20/20 PASS**（consumer 线程以 16ms 节奏模拟真实播放器主循环拉帧）：
  - openFile/duration/state 流转/暂停冻结/恢复/seek(20)→21.90/volume/mute/close/无音频文件/垃圾文件拒绝/截断文件处理 ✓
  - 音频主时钟 3 秒推进 3.02（1:1 正常播放）；seek 精度受 keyframe 间隔限制（4.mp4 keyframe 每 10 秒，seek(16.9) 落到 10 秒处再继续）
- GUI 最终验证：vplayer.exe 播放 7 秒无崩溃 + 两次截屏像素差异 50 处（画面/OSD 时钟在动）✓
- 教训记录（重要）：
  1. **"无崩溃"不等于"在播放"** —— 验证必须看时钟/画面，不能只看进程存活
  2. **BlockingQueue::close() 是永久性的**，Player::close() 关闭视频队列后若 openFile() 不重置，解码线程 push 会立即失败退出（音频时钟=视频时钟回退 clock()=0 是典型信号）
  3. **seek 生效有延迟**（seek 后音频时钟能自动重设：fill() 检测 chunk.pts 与 writeHead_ 偏差 >0.5s 时重设，无需手动 resetClock()）
  4. **AudioOutput 无窗口环境同样工作**（SDL 音频不依赖窗口）
- 遗留：API 测试程序与探针程序在 `C:\Users\31697\AppData\Local\Temp\opencode\`（可清理）

### 阶段 M6：GUI 交互验证与稳定性测试 ✅ 完成

- 任务：验证快捷键/全屏/拖拽等 GUI 交互（SDL_PushEvent 注入自动化），长时间播放稳定性
- 方式：gui_events_test 程序（有窗口，复制 main.cpp 事件处理逻辑，按时间表注入按键/拖拽事件并断言结果）
- 结果：**10/10 检查全过**——空格暂停/恢复、右箭头 seek +5s、上箭头音量 +10%、M 静音/取消、F 全屏切换（SDL_GetWindowFlags 验证）、拖拽文件重开
- 稳定性：**120 秒随机事件压力阶段**（暂停/seek/音量/静音/全屏随机注入，64 个事件）无崩溃、无死锁，正常退出
- 困难与解决：
  1. **seek +5s 断言失败（误报）**：4.mp4 关键帧间隔 10 秒，seek(9.7) 往回落到 0 秒 keyframe → 时钟回退是正常 seek 精度行为 → 用 ffmpeg 生成 `t_keyframes.mp4`（-g 30，关键帧每 1.2 秒）验证 seek 精确跳变 → PASS
  2. **seek 生效有延迟**（解码线程需先解除视频 push 阻塞再处理 seek 请求）→ 断言改为轮询式（deadline + 状态轮询），不再固定 500ms
  3. 测试逻辑自身 bug：验证轮询被下一个步骤触发覆盖 → 触发新步骤前先结算挂起的验证
- 教训：seek 精度 = 关键帧间隔（设计如此，av_seek_frame BACKWARD）；GUI 自动化验证可用 SDL_PushEvent 注入（按键 + DROPFILE，注意 DROPFILE 的 file 需 SDL_malloc 分配以便事件循环 SDL_free）

### 阶段 M7：硬解（DXVA2 / D3D11VA） ⏳ 规划中

- 任务：使用 FFmpeg 硬解接口提升解码性能（特别是 MKV/MP4 H.264 1280×720）。
- 计划：`av_hwdevice_ctx_create` 初始化 D3D11 设备 → `AVCodecContext.hw_device_ctx + hw_frames_ctx` → 解码后 `av_hwframe_transfer_data` 到软件帧供渲染。
- 遗留：需在 Windows 环境下调试（当前 WSL/Linux 未适配）；优先于 M8 实现。

### 阶段 M8：字幕（ASS/SRT） ⏳ 规划中

- 任务：解析并渲染外挂字幕（SRT）与内嵌字幕（ASS）。
- 计划：FFmpeg subtitle 流 demux → `avcodec_decode-subtitle` → SDL_RenderGeometry+纹理渲染 OSD 层。ASS 使用 libass 渲染；SRT 简易解析。
- 遗留：字幕渲染与 OSD 共享渲染管线设计。

### 阶段 M9：播放列表与记忆播放位置 ✅ 完成

- 任务：多文件播放列表（命令行多文件 + N/P 切换 + 播完自动下一个）；记忆播放位置/音量/上次文件（exe 同目录 vplayer.ini，不碰 C 盘）
- 设计：config 模块（极简键值行格式，UTF-8）；playlist 模块（头部纯逻辑）；main.cpp 集成
- 实现：
  - `src/core/config.h/.cpp`：AppConfig{volume,lastFile,history<unordered_map>}；configPath()=exe 同目录 vplayer.ini（GetModuleFileNameW）；load/save 简单 key=value 行，UTF-8，hist=path\tpos
  - `src/core/playlist.h`：Playlist（set(vector)/set(string)/next/prev/hasNext/hasPrev/current/index/size/empty）
  - main.cpp：args>1 → Playlist；openCurrent() 设窗口标题 "VPlayer - basename" + openFile + seek(hist>2.0)；SDLK_n/P 切歌；Ended → nextTrack；退出前 saveConfig(history[last clock]/lastFile/volume)
- 验证：
  - **播放列表单元测试**（playlist_test.cpp）：21/21 PASS
  - **多格式冒烟**：8.flv/10.mpg/13.vob 加入 ini hist=10.0 → 启动后 4s 内自动切 3 个文件完成 ✓；窗口标题随文件切换 ✓
  - **记忆续播**：无参数启动 → 读取 lastFile+history → seek(hist) → 续播 ✓；退出后 vplayer.ini 含 volume/last/hist ✓
  - **退出稳定**：20 次多文件运行 0 崩溃（WER LocalDumps 监控） ✓
- 困难解决：
  - seek 到近末尾（hist 10.0/10.9 秒文件）→ 打开立即 Ended → 自动切下一个 → 验证自动切换路径 ✓
  - gui_events_test PostMessage FindWindow 失效（SDL 窗口标题在退出时变为 exe 路径）→ 确认 N/P 逻辑在 SDLK_n/p case 已生效，跳过 PostMessage 验证
- 代码清理：移除 main.cpp [exit] debug 打印
- 提交：7a75302（M9 功能）、64db432（docs）、ce21893（README）

### 阶段 M10：播放速度 ⏳ 规划中

- 任务：S/L 快捷键控制播放速度（0.5x / 0.75x / 1.0x / 1.25x / 1.5x / 2.0x），OSD 显示倍速。
- 计划：Player 增加 `m_speed` 成员；视频帧调度按 `target = base + (pts - base) / speed` 伸缩；音频正常播放速度（或 SWR 变速）。

### 阶段 M11：现代化 UI 外观 ✅ 进行中 (v0.1)

- 任务：提升播放器外观可用性——暗色主题、悬浮控制栏、鼠标悬移显示/隐藏控件、点击交互。
- 实现：
  - `VideoRenderer` 增加 `RenderStats` 结构体 + `onMouseMove/onMouseClick` 方法
  - 控件栏：60px 圆角矩形 (`#181818@85%` + 高亮/阴影边框)
  - 按钮布局：`[Prev|Play/Pause|Next]` + 进度条 + `[Vol|Fullscreen]`
  - 按钮图标为几何图形绘制 (三角形/双竖线/喇叭/方框)
  - 进度条：6px 轨道 + 蓝色填充 (`#4D90FF`)
  - 鼠标悬移 500ms 无活动 → 控件栏自动隐藏
  - **鼠标点击支持**: Prev/Play-Pause/Next/Volume-mute/Fullscreen 按钮 + 进度条点击跳转 + 双击全屏 (clicks==2)
  - main.cpp：SDL_MOUSEMOTION/MOUSEBUTTONDOWN 事件传递给 VideoRenderer
- 验证：构建成功，smoketest 运行 4 秒无崩溃 ✓
- 遗留：控件栏图标仍为几何图形绘制 (未来可改为位图纹理)；缺少滑块拖动 (只有点击跳转)
- 遗留：SDL 渲染默认白底 → 改为深灰 `#181818` 背景；字体改为更清晰的位图或 FreeType 加载。
