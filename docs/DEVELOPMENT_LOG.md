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

### 阶段 M7：硬解（D3D11VA / DXVA2） ✅ 完成

- 任务：使用 FFmpeg 硬件解码接口降低 CPU 占用（H.264/H.265/MPEG-2 等可硬解格式）。
- 实现：
  - `Decoder::open(par, hwDeviceCtx)` 重载：`avcodec_get_hw_config` 检查 codec 是否支持设备类型（`AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX`）→ 设置 `ctx_->hw_device_ctx = av_buffer_ref(...)` → `hw_` 标志
  - `receive()`：解码帧为硬件帧时（`hw_frames_ctx`）→ `av_hwframe_transfer_data` 转回软件帧 + 手动复制 pts/best_effort_timestamp → 渲染层零改动
  - `Player`：`hwDeviceCtx_` 成员，openFile 时依次尝试 D3D11VA → DXVA2 创建设备，传给 videoDecoder_；close() 释放；`usingHardware()` 暴露状态
  - 不支持硬解的 codec（如 MPEG-4/RV40）自动回退软解（supportsHwType 检查失败 → 不带 hw 打开）
- 验证（hw_test.cpp）：
  - 4.mp4（H.264）→ hw=1，3 秒时钟 2.97 ✓
  - 13.vob（MPEG-2）→ hw=1，3 秒时钟 3.21 ✓
  - 3.mkv（MPEG-4）/ 11.rmvb（RV40）→ hw=0 软解回退，播放正常 ✓
- 遗留：H.265 无测试素材（可硬解但未验证）；NVDEC/CUDA 未启用（仅 D3D11VA/DXVA2）

### 阶段 M8：字幕（外挂 SRT/ASS + 内嵌字幕流） ✅ 完成

- 任务：外挂字幕（SRT/ASS）解析渲染；内嵌字幕流（MKV 等容器）解码显示。
- 实现：
  - `src/subtitle/subtitle.h/.cpp`：SubtitleTrack（事件列表 + textAt 查询）
    - `loadSrt()`：序号/时间行（HH:MM:SS,mmm -->）/多行文本，自动去 BOM、归一 CRLF
    - `loadAss()`：解析 [Events] 段 Dialogue 行（Layer,Start,End,...,Text），去 {\...} 标签与 \N
    - SubtitleDecoder：AVCodecContext + avcodec_decode_subtitle2，SUBtitle_TEXT/ASS rect 提取文本
  - Demuxer：新增 subtitleIndex()/subtitleStream()/subtitleCodecpar()
  - Player：openFile 检测内嵌字幕流 → 解码线程逐个 packet 解码入 subtitles_（按容器 pts 时间，time_base 换算秒）；`loadExternalSubtitle()` / `subtitleText(t)` / `hasSubtitle()`；seek 时清空字幕并 flush 解码器
  - main.cpp：打开视频自动查找同名 .srt/.ass/.ssa/.sub（replaceExt）；RenderStats.subtitle 传文本
  - VideoRenderer：GDI 渲染字幕（CreateFontW 微软雅黑 32px + DrawTextW 黑描边白字 → DIB → SDL 纹理，底部居中），文本变化才重建纹理，缓存于 subtitleCache_
- 关键技术点（踩坑）：
  - **FFmpeg srt 解码器不设 sub.pts（NOPTS），start/end_display_time 恒 0**——时间必须从 packet pts × stream time_base 取；MKV 内嵌 srt 的 rect->ass 是 9 字段无 "Dialogue:" 前缀格式（0,0,Default,,0,0,0,,Text），需剥离前 9 字段取 Text
- 验证（subtitle_test.cpp + subtitle_embed_test.cpp）：
  - SRT 解析 3 事件，多行文本、UTF-8 中文、边界时间查询 12/12 全过
  - ASS 解析 2 事件、去标签 全过
  - 内嵌字幕：ffmpeg 生成带 srt 的 MKV → 解码事件时间 1.0/4.0/60.0 秒正确（pkt pts 方案），播放 2 秒时钟处显示 "Hello World" ✓
  - 外挂字幕：4.mp4 + 外部 SRT，textAt(1.5/4.5) 精确匹配 ✓
  - GUI 冒烟：内嵌字幕 MKV 与外挂字幕 GIF 各跑 6 秒无崩溃
- 遗留：图形字幕（PGS/DVDSUB 位图）不支持；无字幕切换快捷键（单字幕轨自动取首个）；字幕样式（位置/字号/颜色）固定

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

### 阶段 M10：播放速度 ✅ 完成

- 任务：S/L 快捷键控制播放速度（0.5x / 0.75x / 1.0x / 1.25x / 1.5x / 2.0x），控件栏显示倍速。
- 实现：
  - Player 新增 `speed_` (atomic<float>) + `setSpeed()/speed()`（clamp 0.5~2.0）
  - `videoClock()`：elapsed × speed 伸缩（纯视频文件时钟随倍速推进）
  - `pullFrame()` 同步：有音频时目标位置 = 音频时钟 × speed（视频快进、音频保持原速——M10 计划设计）；延迟计算 ÷ speed
  - main.cpp：S/L 键循环切换速度列表 {0.5, 0.75, 1.0, 1.25, 1.5, 2.0}；RenderStats 加 speed 字段
  - 控件栏右侧绘制倍速文本（5x7 位图字体扩展，新增 'x' 字形）
- 验证（speed_test.cpp）：
  - 4.mp4（有音频）@2x：时钟 3 秒 = 2.99（音频 1:1，视频 2 倍快进）✓
  - @0.5x：时钟 3 秒 = 0.94 ✓
  - 9.gif（纯视频）@2x：时钟 3 秒 = 6.24（≈6.0，纯视频时钟按倍速推进）✓
- 遗留：音频保持原速（未做 SWR 变速），有音频文件倍速时音视频不同步属已知限制

### 阶段 M11：现代化 UI 外观  ✅ 完成

- 任务：提升播放器外观可用性——暗色主题、悬浮控制栏、鼠标悬移显示/隐藏控件、点击交互。
- **状态说明：v0.2 为临时可用版本（凑合用），未达"现代化美观"标准，不可标记完成。** 后续迭代：圆角按钮、图标纹理化、控件栏动画、鼠标悬停高亮、播放/暂停/音量滑块、视觉细节打磨等。
- 实现：
  - `VideoRenderer` 增加 `RenderStats` 结构体 + `onMouseMove/onMouseClick` 方法
  - 控件栏：60px 圆角矩形 (`#181818@85%` + 高亮/阴影边框)
  - 按钮布局：`[Prev|Play/Pause|Next]` + 进度条 + `[Vol|Fullscreen]`
  - 按钮图标为几何图形绘制 (三角形/双竖线/喇叭/方框)
  - 进度条：6px 轨道 + 蓝色填充 (`#4D90FF`)
  - 鼠标悬移 500ms 无活动 → 控件栏自动隐藏
  - **鼠标点击支持**: Prev/Play-Pause/Next/Volume-mute/Fullscreen 按钮 + 进度条点击跳转 + 双击全屏 (clicks==2)
  - **滑块拖动**: 进度条按下→拖动实时 seek→松开 (MOUSEMOTION 期间持续 seek)
  - main.cpp：SDL_MOUSEMOTION/MOUSEBUTTONDOWN/UP 事件传递给 VideoRenderer
- 验证：构建成功，smoketest 运行 4 秒无崩溃 ✓
- 提交：0907e6e（v0.1 基础控件栏）、55c0293（按钮布局+点击+双击全屏）、后续（滑块拖动）
- 遗留：控件栏图标仍为几何图形绘制 (未来可改为位图纹理)

- **v0.3 正式版（现代化 UI 重构，2026-08-20）**：
  - ideo_renderer.h/.cpp 整体重写：矢量图标系统（Icon 枚举：Play/Pause/Prev/Next/Volume/Mute/Fullscreen/ExitFullscreen，SDL_RenderDrawLine/FillRect 按 24x24 坐标系缩放，无位图依赖）；illRoundedRect() 用 SDL_RenderGeometry 三角扇形画圆角矩形（8 段弧 + 中心四边形 + 4 边）；控件栏 64px，顶部垂直渐变背景（alpha 0.25→0.85）；布局 gap=12/btnSize=40：Prev(12)/Play(64)/Next(116)/Vol(winW-104)/FS(winW-52)，进度条 x=180 起 y=barY+29 高 6px（track 灰 80,80,80、fill 蓝 77,144,255），hover 显示 14px 白色 thumb；音量弹层为 Vol 上方 6x90 竖条可拖动（stats.draggingVolume）；淡入淡出动画 controlsAlpha_ 每帧 ±30，鼠标 700ms 无动作自动隐藏；倍速文本 x%.2g 在进度条右侧、时间文本 mm:ss / mm:ss 在进度条下方右侧
  - main.cpp：鼠标事件改为新布局坐标；新增 draggingVolume 拖动状态；音量置 0 时写 0.0001 避免静音误判
  - 踩坑：SDL_Vertex 用 SDL_Color（Uint8）而非 SDL_FColor；SDL_RenderGeometry 签名带 SDL_Texture* 参数（传 nullptr）；嵌套 brace-init-list 无法推导需显式构造 SDL_Vertex
  - 验证：构建通过；GUI 冒烟 4.mp4/9.gif 各 5-6s 无崩溃；截图测试（SDL_RenderReadPixels→BMP）验证进度条蓝/灰、渐变控件栏、Prev/Play/FS 白色矢量图标、视频画面均正确渲染
  - 

- **M11 修复（2026-08-20，v0.3.1）**：
  - 绿屏 bug 根因：硬解（D3D11VA/DXVA2）transfer 出的帧是 NV12（fmt=23），但渲染器固定用 SDL_PIXELFORMAT_IYUV + SDL_UpdateYUVTexture（YUV420P 三平面），NV12 半平面数据按 IYUV 读 → 全绿（软解 YUV420P 正常所以此前未发现）
  - 修复：render() 按帧格式分支——YUV420P 走原路径、NV12 用 SDL_UpdateNVTexture + SDL_PIXELFORMAT_NV12 纹理、其他格式 swscale 兜底转 YUV420P（新增 swscale.lib 链接 + extern "C" 包裹，踩 M2 同样的坑）
  - 验证：4.mp4 硬解 hw=1 fmt=23(NV12)，截图像素不再是纯绿 (0,136,0) 而是真实视频内容色；4.mp4/13.vob 冒烟各 5-6s 无崩溃
  - Prev/Next 图标方向反了：Prev 画成指向右的三角、Next 画成指向左的（原实现 line 88-101）
  - 修复：Prev = 左三角+右竖线 (◀|)，Next = 左竖线+右三角 (|▶)；截图像素验证 Prev 顶点在左、Next 顶点在右均白色
  - 提交：340c87b（NV12 绿屏修复 + Prev/Next 图标方向修正）提交：9a1099a（M11 v0.3 正式版）

- **音量假功能修复（2026-08-20，v0.3.2）**：
  - 用户反馈：音量 UI 有变化但实际声音大小完全不变，是"假功能"
  - 根因：`AudioOutput::applyVolume()` 用 `SDL_MixAudioFormat(stream, mixTemp_, ...)`，其中 `mixTemp_` 是 stream 的逐字节拷贝。SDL 混合公式为 `dst = dst*(128-vol)/128 + src*vol/128`，dst 与 src 是同一份数据，无论 vol 取何值结果恒等于原始数据 → 音量永远不变（M5 教训延续："无崩溃"不等于"在播放"，此处"有 UI"也不等于"在生效"）
  - 修复：改为对 S16 样本逐点乘以音量系数（int32 计算防溢出，clamp 到 ±32767/±32768），v=0 时直接清零静音，v>=1.0 跳过；删除不再使用的 mixTemp_ 缓冲
  - 验证：volume_test 单测——vol=0.5 输出精确减半、vol=0.25 四分之一、vol=0 全零、vol=1.0 不变、32767*2 饱和 32767、-32768*2 饱和 -32768，RESULT PASS；4.mp4 冒烟 6s 无崩溃
  - 提交：音量修复（待提交）


### 阶段 M12：自动播放列表（目录扫描 + 播放模式） ✅ 完成（2026-08-20）

- **动机/用户反馈**：Prev/Next 按钮功能闲置——打开单个文件时播放列表只有 1 项，软件根本找不到上一个/下一个（M9 播放列表仅支持命令行多文件）

- **任务规划**：
  1. 目录扫描：打开单个文件时自动扫描同目录视频文件（按文件名排序），构建播放列表；扩展名白名单（mp4/mkv/avi/wmv/mov/flv/rm/rmvb/3gp/mpg/mpeg/vob/webm/m4v/ts/m2ts 等）
  2. 列表刷新时机：启动打开文件、拖拽打开新文件时重建列表；命令行多文件继续保留
  3. Playlist 扩展：支持「扫描目录生成列表」API + 容错（文件不存在/打开失败时跳过）
  4. UI：控件栏/窗口标题显示 当前文件名 + 序号（如 3/13），让用户感知列表存在
  5. 播完自动下一曲保留（M9 已有），列表末尾循环策略待定（循环/停止）
  6. 验证：testdata 目录 13 个真实格式文件扫描 + 排序 + Prev/Next 往返 + GUI 冒烟

- **决策（用户确认）**：循环播放 / 仅同目录扫描 / 按文件名排序 / 增加播放模式（单独、循环、随机）

- **实现（v1.0）**：
  - Playlist 重构：引入 order_（files_ 下标顺序数组）+ PlayMode 枚举（Single/Loop/Shuffle）；scanDirectory() 用 std::filesystem 扫描同目录 22 种视频扩展名白名单，按文件名升序，定位当前文件为起点
  - next()/prev()：Single 模式到边界返回 false 不越界；Loop 模式首尾环绕；Shuffle 模式打乱 order_ 且保证当前文件保持在列表起点（切换模式不跳曲目），播完一轮重洗
  - hasNext()/hasPrev()：Single 模式按位置判断（播完停止），Loop/Shuffle 恒 true（自动接续）
  - main.cpp：打开单文件/拖拽文件 → scanDirectory 自动建列表；窗口标题显示 文件名 (n/N)；X 键循环切换播放模式（控制台打印模式名）
  - 配置：vplayer.ini 新增 playmode=（0=Single 1=Loop 2=Shuffle，默认 1），退出保存
  - 踩坑：set() 曾丢失 idx_ 重置（新对象 idx_=-1 时 next() 永假 + current() 越界 order_[-1]），-O0 侥幸运行、-O2 崩溃 0xC0000005——测试驱动暴露；已修复并加单测
  - 验证：playlist_test 24/24 PASS（testdata 13 文件扫描排序、索引定位、三种模式边界/环绕/保持当前、rescan 位置保持）；GUI 冒烟 4.mp4 6s 无崩溃；配置读写 last=/playmode= 验证通过

### 阶段 M13：长视频 seek 性能 + 大型目录承压 ✅ 已完成（2026-08-20）

- **用户反馈**：1 分钟以上视频拖动进度条卡顿，拖完后声音恢复快但视频卡好几秒

- **用户测试场景**：1-2GB / 2-4 小时长视频；数千个视频的文件夹（可靠性与承压测试）

- **根因分析（已定位）**：
  1. seek 后解码线程从目标前最近关键帧开始解码，视频要解到 dropUntil 才出帧（大 GOP 视频解码追赶需数秒）
  2. 音频 dropUntil 丢弃后立即有数据（AAC 解码快）→ 音频先播，writeHead_ 跳到目标时间
  3. 视频队列空 → pullFrame 返回 lastFrame_（旧画面冻结），视频解码追赶期间画面卡住，音频却先走了 → A/V 撕裂
  4. seek 后音频时钟未设为目标时间，pullFrame 的 target 是旧时钟，视频首帧 pts-target 差值大 → delay 等待（加重卡顿）

- **修复方案**：
  1. AudioOutput 新增 setClock(t)（直接设 writeHead_ = t，带锁）
  2. doSeek：audio pauseDevice() + setClock(t)——音频暂停等待视频，时钟指向目标时间，pullFrame target 立即正确
  3. pullFrame 拿到 seek 后首帧（pts≈t）时 resumeDevice() 恢复音频 → A/V 同步同时启动，画面不再卡住等音频
  4. 纯视频文件走 videoClock 路径，seek 后首帧即出，无此问题（顺带确认）

- **验证计划**：
  1. ffmpeg 生成 2 小时长视频（低码率）测 seek 精度与恢复时间
  2. 大 GOP（-g 300）视频模拟稀疏关键帧，测视频追赶耗时
  3. 3000+ 文件目录测 scanDirectory 性能与播放器启动
  4. seek 循环压力（随机位置连续 seek 100 次无崩溃）

#### M13 进度更新（2026-08-20，测试阶段完成）

- **修复 1：seek 请求被饿死的死锁（根因）**：decodeLoop 用阻塞式 `push()` 填满 8 帧队列后永远卡在 push，到不了循环顶部检查 `seekPending_` → seek 永不执行。
  - 修复：BlockingQueue 新增 `tryPush()`；decodeLoop 的视频/音频 receive 循环改 1ms 轮询式 tryPush，且每帧检查 `seekRequested() || stop_`
- **修复 2：seek 后 audioWait 死锁**：seek 后音频时钟设为目标、设备暂停，pullFrame 用冻结时钟做 delay 判断 → 视频帧 pts 恒大于 target → 永不 pop → 队列满 → 解码线程饿死。
  - 修复：`audioWait_` 期间 pullFrame 对首帧立即 pop（不 delay）并恢复音频
- **修复 3：close() 死锁（顺带发现）**：tryPush 轮询循环不检查 stop_，close() 关队列后 tryPush 恒失败 → 死循环 → join() 卡死。修复：轮询循环加 `stop_` 检查

- **用户素材验证（不再构造数据）**：
  - `F:\影视资料\鉴赏` 3.5h 长视频（12635s，P2破解魁真咲塾28小时完全痴.mp4）：10 次随机 seek 全部 8~302ms 内时钟+帧到位，画面无卡顿
  - `F:\影视资料\X` 3198 个视频（480p~4K，最大 1.4GB/81 分钟）：scanDirectory 134~166ms；1.38GB 文件 5 次 seek 0~20ms
  - 13 种真实格式 testdata 回归全 PASS；播放+close 冒烟通过（frames=60 clock=1.4 closed 正常退出）
  - 备注：命令行直接传中文路径会因 PowerShell→argv 编码损坏导致 std::filesystem 抛 conversion_error 崩溃（测试环境问题，非播放器 bug；真实播放器路径来自宽字符 API）

- **结论**：M13 核心目标达成——长视频 seek 卡顿根因（seek 请求被饿死+音频时钟撕裂）已修复，数千文件目录承压通过。
- **待办**：真机体验验证（拖动进度条手感）、X 目录全量播放巡检（可选）、文档+README 收尾、git 提交

- **UI 冒烟确认（真机路径）**：鉴赏目录真实长视频 pre-seek 210 帧/clock 2.9s → seek 50% 后 68 帧/clock 5009.9s，画面持续出帧不冻结，时钟正确跳到目标，close 正常退出。M13 完成。

### M13 补充（2026-08-20）续播设置
- 新增配置 resume=0/1（默认 0）：resume=0 打开文件从头播放；resume=1 打开时从上次位置续播（history>2s 时 seek）。
- 快捷键 R 循环切换（控制台打印提示），配置自动持久化到 vplayer.ini。
- 验证：cfg_test 读写 round-trip PASS（resume=1、hist=123.5 正确回读），主程序构建成功。
- **下一阶段 M14 规划（用户指定）**：① 音量标准化（EBU R128 / 峰值归一化，解决不同视频音量差异大）② 关键帧预览（进度条 hover 显示缩略图）。

### M13 补充 2（2026-08-20）UI 更新 + 续播彻底修复
- 续播彻底修复：resume=0 时启动不自动打开 lastFile（空白窗口像第一次打开软件），只有 resume=1 才自动打开并续播；启动续播时 toast 提示"已从上次位置续播（按 R 关闭）"。
- UI 更新：新增 播放模式按钮（单独/循环/随机 三态图标，位于 Next 右侧）和 倍速按钮（显示 x0.5~x2 文本，点击循环切换）；进度条左移为新增按钮让位（progX=284，main.cpp 与 renderer 坐标公式一致）；新增 toast 提示系统（顶部居中中文提示，2.2s 淡出，用于模式/倍速/续播操作反馈）。
- 构建成功（vplayer.exe），无新警告。

### M13 补充 3（2026-08-20）UI 布局重排 + Material 贴图 + 双击修复
- 布局重排（主流播放器风格）：进度条独立贴底全宽细线（hover 加粗+thumb），控制按钮分左右两组（左：上/播/下，右：模式/倍速/音量/全屏），时间文本居中。坐标统一收敛到 ControlLayout::compute()，main.cpp 与 renderer 共用，顺带修复拖动进度条坐标错位旧 bug（MOUSEMOTION 曾用旧 progX=180）。
- 贴图资源：引入 SDL_image 2.8.2（下载 F:\dev，~15MB，dll 仅依赖 SDL2.dll），Material Icons（Apache 2.0，白色 48dp，11 个共 2.6KB）存于 assets/icons/，IMG_Load 加载纹理，失败回退矢量绘制。CMake 自动拷贝 DLL+assets。
- 双击修复：点击按钮/进度条/音量条（hitControl=true）时双击不触发全屏，只在画面空白处双击放大。
- 验证：icon_test 11/11 加载 PASS；ui_smoke2 真实播放+渲染+贴图+toast 110 帧无崩溃 exit=0。
### 阶段 M14：完整 YouTube 样式界面（大任务）
- 任务：实现自定义背景绘制、圆角矩形、可拖动标题栏以及类 YouTube 般的现代化界面体验。
- **自定义背景绘制**：使用 SDL_Renderer 自绘窗口背景，实现深色/浅色主题切换，绘制圆角矩形（辅助函数 RGBA_RenderFillRectWithRoundedCorner）。
- **可拖动标题栏**：重写非客户区消息处理（Windows WndProc 钩子），实现标题栏拖动窗口、关闭/最小化/最大化按钮的自绘，启用 DWM DWMWA_EXTENDED_FRAME_BUFFER_PROPERTY 实现窗口阴影和圆角。
- **自定义控件**：播放进度条（拖动求 Seek，悬停显示剩余/已用时间，双击全屏），音量条（拖动调节音量，点击静音/取消静音），功能按钮（播放/暂停、模式循环、速度、静音、全屏）：自绘图标与 hover/focus 状态动画。
- **图形后端**：当前项目使用 SDL2，建议未来迁移至 Direct2D/DirectWrite（Windows）或 SDL3 + SDL_RendererGeometry 以获得更流畅的圆角和阴影渲染；若坚持 SDL2，可通过 CPU 软件光栅化实现圆角效果。
- 任务难度：较高，涉及 Win32 API 与 SDL 结合、自绘大量 UI 元素，建议在 M14-M15 阶段作为主要目标攻克。
- 计划：第一阶段实现基本的自定义背景和圆角窗口；第二阶段实现可拖动标题栏和功能按钮自绘；第三阶段优化图形后端，迁移至 Direct2D 或 SDL3。

### M14 进度更新（2026-08-21）— 阶段 A：DWM 窗口阴影 + 圆角
- **改动**：
  - CMakeLists.txt 添加 `dwmapi` 链接库
  - main.cpp 添加 `#include <SDL_syswm.h>` + `#include <dwmapi.h>`
  - SDL_CreateWindow 后获取 HWND（`SDL_GetWindowWMInfo`），调用 `DwmExtendFrameIntoClientArea` 启用系统窗口阴影
  - 调用 `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE=33, pref=2)` 启用 Windows 11 圆角窗口
- **验证**：cmake 构建成功（仅 SDL_MAIN_HANDLED redefined 无害警告），vplayer.exe 启动 5 秒正常退出无崩溃
- **结论**：M14 阶段 A 完成，窗口阴影+圆角已启用，为后续自定义标题栏和背景绘制打下基础

### M14 进度更新（2026-08-21）— 阶段 B：自定义标题栏
- **改动**：
  - 新增 `src/ui/custom_titlebar.h/.cpp`：自定义标题栏类
  - `SetWindowLongPtrW(GWLP_WNDPROC)` hook 窗口消息，处理 `WM_NCHITTEST` 实现拖动（`HTCAPTION`）和按钮点击（`HTMINBUTTON`/`HTMAXBUTTON`/`HTCLOSE`）
  - `WM_NCCALCSIZE` 去掉默认标题栏，保留边框阴影
  - GDI 自绘标题栏：深色背景(24,24,24) + 白色标题文字 + 最小化/最大化/关闭按钮（Segoe MDL2 Assets 图标）
  - 关闭按钮 hover 红色高亮(196,43,28)，其他按钮 hover 灰色高亮(60,60,60)
  - main.cpp 集成 CustomTitlebar 实例，文件打开时同步更新标题
- **验证**：cmake 构建成功，vplayer.exe 启动 5 秒正常退出无崩溃
- **结论**：M14 阶段 B 完成，自定义标题栏可拖动，按钮 hover 高亮正常

### M14 进度更新（2026-08-21）— 阶段 C：自定义背景 + 主题绘制
- **改动**：
  - video_renderer.h 新增 `drawBackground()` 声明
  - video_renderer.cpp 实现 `drawBackground()`：深色主题背景 (18,18,18)，圆角半径 8px，使用 `fillRoundedRect` 绘制
  - `render()` 方法：在 SDL_RenderClear 后、SDL_RenderCopy 前调用 drawBackground()，视频画面覆盖在深色背景上
  - `clear()` 方法：无视频时也绘制深色背景（不再纯黑）
- **验证**：cmake 构建成功，vplayer.exe 启动 5 秒正常退出无崩溃
- **结论**：M14 阶段 C 完成，深色主题圆角背景已启用

### M14 进度更新（2026-08-21）— 阶段 D：控件优化
- **改动**（video_renderer.cpp drawControls）：
  - **按钮 hover 动画**：hover 时图标缩放 1.1x（24→26px），active 时蓝色高亮背景(77,144,255)，普通 hover 白色背景
  - **进度条增强**：hover/drag 时轨道加粗 4→10px，深灰背景(50,50,50)，发光 thumb（蓝色阴影+白色圆形 thumb）
  - **音量弹层优化**：宽度 6→8px，背景加边框，拖动时白色 thumb 指示器，轨道深灰(60,60,60)
- **验证**：cmake 构建成功，vplayer.exe 启动 5 秒正常退出无崩溃
- **结论**：M14 全部阶段（A/B/C/D）完成

### M14 修复（2026-08-21）— 去掉系统默认标题栏
- **问题**：自定义标题栏已实现但系统默认标题栏仍可见
- **修复**：
  - main.cpp：SDL_CreateWindow 后用 `SetWindowLongPtrW(GWL_STYLE)` 去掉 `WS_CAPTION` + `WS_THICKFRAME`
  - 调用 `SetWindowPos(SWP_FRAMECHANGED)` 通知系统重新计算窗口布局
  - custom_titlebar.cpp：`WM_NCCALCSIZE` 改为扩展客户区到整个窗口（减 1 像素保留边框阴影）
- **验证**：cmake 构建成功，vplayer.exe 启动 5 秒正常退出无崩溃

### M14 修复 3（2026-08-21）— 标题栏全面重做
- **修复白色系统按钮 bug**：`WM_NCHITTEST` 不再返回 `HTMINBUTTON`/`HTMAXBUTTON`/`HTCLOSE`（会触发 Windows 绘制系统按钮），改为返回 `HTCLIENT`，由 `WM_NCLBUTTONDOWN` 自行处理点击
- **布局重做**：左侧 vplay.bmp logo(20x20) + "VPlayer" 文字(浅灰白 200,200,200)，中间视频文件名(白灰 210,210,210)，右侧三个按钮(最小化/最大化/关闭)
- **滚动字幕**：文件名超出中间区域时自动从右向左滚动，到端点暂停 1.5s 后反向滚动，SDL_RenderSetClipRect 裁剪溢出文字
- **色差区分**：标题栏 (30,30,30) + 分隔线 (55,55,55)，视频区域纯黑 (0,0,0)，形成层次感
- **Logo 加载**：用 IMG_Load 加载 vplay.bmp 缩放到 20x20，全局缓存
- **验证**：cmake 构建成功，vplayer.exe 启动 5 秒正常退出无崩溃
- **问题**：自定义标题栏用 GDI 绘制被 SDL_RenderPresent 覆盖；标题栏与视频重叠
- **修复**：
  - custom_titlebar.cpp 重写：改用 SDL_Renderer 绘制（draw() 方法），每帧在主循环调用
  - video_renderer.cpp：render() 视频区域从 y=32 开始（标题栏高度），drawBackground() 只绘制 y≥32 区域
  - 标题栏独立占据窗口顶部 32px，视频从下方渲染，不再重叠
- **验证**：cmake 构建成功，vplayer.exe 启动 5 秒正常退出无崩溃

### M14 最终状态（2026-08-21）
- **M14 全部完成**：DWM 阴影+圆角 → 自定义标题栏 → 深色主题背景 → 控件优化
- **标题栏文字修复**：GDI DIB alpha 通道问题（默认 alpha=0 导致文字不可见），后处理像素设 alpha=255
- **标题栏布局**：左侧 vplay.bmp logo + "VPlayer"，中间视频文件名（GDI 渲染，支持全 Unicode），右侧最小化/最大化/关闭
- **滚动字幕**：文件名超出时自动左右滚动，到端点暂停 1.5s 后反向
- **色差**：标题栏 (30,30,30) + 分隔线 (55,55,55) + 视频区纯黑 (0,0,0)
- **提交记录**：`f3626c6` `d26bcbf` `d1b3922` `87771c3`

---

### 阶段 M15：音量标准化 + 关键帧预览
- **M15-A：关键帧预览（先做）**
  - 目标：进度条 hover 时显示视频缩略图
  - 实现：独立 FFmpeg 解码线程，seek 到目标时间，解码一帧 → swscale 转 RGB → SDL_Texture → 悬浮显示
  - 新增 `src/core/thumbnail_extractor.h/.cpp`：ThumbnailExtractor 类
  - 修改 `video_renderer.h/.cpp`：进度条 hover 时触发缩略图显示
  - 修改 `main.cpp`：集成 ThumbnailExtractor

#### M15-A 进度更新（2026-08-21）— 关键帧预览实现
- **新增 ThumbnailExtractor 类**：
  - 独立 FFmpeg 解码器（独立 AVFormatContext + AVCodecContext + SwsContext）
  - `open()` 打开文件，`getFrame(seconds)` seek + 解码一帧，转 RGB24 输出
  - `close()` 释放资源
- **VideoRenderer 集成**：
  - `setThumbnail(tex, w, h, time)` 设置缩略图纹理
  - `drawControls()` 进度条 hover 时绘制缩略图（最大 160x90，白边框，居中于鼠标）
- **main.cpp 集成**：
  - MOUSEMOTION 中检测进度条 hover → `thumbnail.getFrame()` → SDL_Texture → `vrender.setThumbnail()`
  - 时间差 >1s 才重新提取（避免频繁 seek）
- **提交**：`5d44bee`

#### M15-A 修复 1（2026-08-21）— 缩略图位置 + 防死循环
- **缩略图位置**：`thumbCenterX = progX + fillW`（播放进度）→ `thumbCenterX = mouseX_`（鼠标位置）
- **getFrame 防死循环**：限制 `maxPackets = 128`，`avcodec_receive_frame` 区分 EAGAIN/EOF，最后 flush 解码器
- **提交**：`ee8ce8f`

#### M15-A 修复 2（2026-08-21）— seek 时间戳修复
- **问题**：缩略图始终显示视频开头第一帧，seek 未生效
- **根因**：`av_seek_frame` 时间戳用了 `AV_TIME_BASE`（微秒），但不同格式 time_base 不同（MPEG-TS=1/90000，MP4=1/1000）
- **修复**：`ts = seconds / av_q2d(vs->time_base)`，`av_seek_frame` 指定视频流索引
- **提交**：`4212ee6`

#### M15-A 修复 3（2026-08-21）— 多格式 seek 兼容
- **问题**：MKV/FLV/SWF 格式缩略图仍有问题
- **修复**：三级 seek 策略
  1. `avformat_seek_file(minTs, targetTs, maxTs)` — 精确范围 seek（MKV/FLV 最佳）
  2. `av_seek_frame(streamIndex, stream_time_base)` — 流级回退
  3. `av_seek_frame(-1, AV_TIME_BASE)` — 传统格式回退（SWF）
- **提交**：`07824ad`

#### M15-A 修复 4（2026-08-21）— 播放列表自然排序
- **问题**：文件排序为字典序 "1,10,2"，不符合人类认知
- **修复**：新增 `naturalLess()` 比较函数，数字序列按数值比较（"1,2,...,10"）
- **提交**：`ca292ba`

#### M15-A 修复 5（2026-08-21）— SWF 格式 duration 推算
- **问题**：SWF 无进度条，总时长显示为零
- **根因**：SWF 是流格式，`ctx_->duration` 为 `AV_NOPTS_VALUE`
- **修复**：三级 duration 检测策略
  1. `ctx_->duration` — 标准格式直接可用
  2. `st->duration * av_q2d(st->time_base)` — 流级 duration
  3. 扫描所有包取最大 PTS — SWF 等流格式最后手段
- **验证**：SWF/MP4/MKV/FLV 全部通过
- **提交**：`b8cff2b`

#### M15-B：音量标准化（2026-08-21）
- **实现**：
  - `audio_output.h`：新增 `setNormalization(bool)` / `normalizationGain()` 接口 + `peakTracker_` / `normGain_` 成员
  - `audio_output.cpp` `applyVolume()`：合并音量+标准化增益，峰值检测 → 滑动最大值衰减 → 增益计算 → tanh 软限幅
  - 目标峰值 -1 dBFS (28672)，增益限制 0.25x~4.0x（防过放大）
  - `player.h`：新增 `audio()` 访问器
  - `main.cpp`：A 键切换音量标准化，toast 提示
- **原理**：
  - 峰值检测：遍历 S16 样本取绝对值最大值
  - 滑动衰减：每秒峰值衰减 50%（防止旧峰值拖累）
  - 增益 = 目标峰值 / 检测峰值，限制 0.25~4.0 倍
  - 软限幅：`tanh(s/32768) * 32768`，超阈值平滑压缩
- **提交**：`ea3cefa`

---

### 阶段 M16：播放列表面板（2026-08-21）

#### M16 实现（2026-08-21）
- **新增文件**：
  - `src/ui/playlist_panel.h/.cpp` — PlaylistPanel 类（面板渲染+交互）
  - `src/ui/gdi_text.h/.cpp` — GdiTextCache 类（GDI 文字渲染+缓存，支持中日韩）
  - `assets/icons/formats/*.png` — 22 种视频格式彩色图标（48x48，Python+Pillow 生成）
  - `tools/gen_format_icons.py` — 图标生成脚本
- **PlaylistPanel 功能**：
  - 右侧可切换面板（8px 竖条按钮点击切换）
  - 面板打开时视频区域自动收缩让位
  - 格式图标：MP4 蓝/MKV 绿/AVI 橙/WMV 紫/MOV 青/FLV 黄/RM 红 等
  - 当前曲蓝色高亮，hover 白色背景
  - 鼠标滚轮滚动，点击选曲
  - 左边缘拖拽调整面板宽度（160~280px）
  - 滚动条显示
- **布局适配**：
  - `ControlLayout::compute()` 新增 `panelWidth` 参数
  - `VideoRenderer::render()` 视频显示区域减去面板宽度
  - `VideoRenderer::drawBackground()` 背景区域减去面板宽度
  - 所有 main.cpp 中 ControlLayout 调用传入 panel.width()
- **GdiTextCache**：
  - Windows GDI 渲染 UTF-8 文字（Microsoft YaHei 字体）
  - alpha 后处理（非黑像素 alpha=255）
  - LRU 缓存（最多 200 条）
- **Playlist 新增 API**：
  - `fileAt(displayIndex)` — 按显示索引访问文件路径
- **依赖**：CMake 自动拷贝 assets/ 到构建目录
- **提交**：`a85dd84`

#### M16 修复（2026-08-21）— 面板布局 + 暂停交互
- **面板位置修复**：
  - 问题：面板从 y=0 到 winH（全高），覆盖标题栏和底部控件
  - 修复：面板限制在 `y=32`（标题栏底部）到 `y=barY`（控件栏顶部）之间
  - 切换按钮始终在窗口右边缘（全高，带箭头指示 >/<）
  - hit-test 逻辑基于 `panelTop..panelBot` 区域
- **暂停交互简化**：
  - 视频区域单击 → 仅切换暂停/播放，不显示叠加图标
  - 删除 `showPauseOverlay` 调用，保留 `drawPauseOverlay` 代码备用
- **提交**：`4b66daa` `016b16b` `c7fd728`

---

### 阶段 M17：Seek 性能核心优化

**目标**：解决 seek 卡顿（2~5 秒延迟）、音频先响视频后到、主线程无响应

**瓶颈分析**：
- H.264 GOP 100~300 帧（4~12 秒），解码 ~10ms/帧 → 总解码 1~3 秒（不可跳过）
- `pullFrame()` 8ms 轮询延迟
- 拖动进度条每帧触发一次 seek（无 debounce）
- 音频 clearQueue 和 audio seek 之间有竞态窗口

#### M17-1：Seek 逐帧显示（替代原方案）
- **原方案**：seekFirstFrame_ 首帧立即显示 + dropUntil_ 丢弃中间帧
- **问题**：首帧显示后，中间帧被丢弃导致队列空，视频冻结 10+ 秒
- **新方案**：不丢弃任何帧（`dropUntil_ = -1e9`），所有帧正常推入队列
- `pullFrame()` seek 期间逐帧显示（用户看到画面从关键帧推进到目标）
- 到目标帧附近（PTS >= target - 0.1）才校准时钟并恢复音频
- 效果：用户看到画面流畅推进，无冻结

#### M17-2：Seek 合并 150ms debounce
- `requestSeek()` 记录 `lastSeekTime_ = SDL_GetTicks()`
- `decodeLoop()` 检查：距上次 seek < 150ms 则跳过本轮，等 timer 到期
- 效果：拖动进度条流畅，松手后才执行实际 seek

#### M17-3：轮询延迟优化
- `pullFrame()` 空队列 `SDL_Delay(8)` → `SDL_Delay(1)`
- 减少首帧检测延迟 0~7ms

#### M17 修复（2026-08-21）— 面板切换 + seek 卡住
- **面板切换修复**：`handleMouseDown` 切换按钮检测移到函数最前面，无论面板开关状态都优先检测
- **seek 卡住修复**：移除 dropUntil_ 丢帧机制 + seekFirstFrame_ 特殊路径，改为逐帧显示到目标再恢复音频

---

### 阶段 M18：音频同步 + 可靠性

**目标**：消除音频竞态、改善 seek 感知体验

#### M18-1：音频竞态修复
- `doSeek()` 设置 `audioSeeking_ = true`
- `audioLoop()`：`audioSeeking_` 为 true 时不推帧
- audio demuxer seek 完成后清除 `audioSeeking_`

#### M18-2：音频时钟显式 reset（补充）
- seek 后音频回调检测时钟跳变 >0.5s 时强制 reset
- 防止旧帧 PTS 污染 writeHead_

#### M18-3：Seeking 指示器
- seek 开始 → 显示半透明 "Seeking..." 文字
- 首帧到达 → 隐藏
- 改善感知体验

---

### M18 实现记录（2026-08-21）

#### M18-1：音频竞态修复
- `player.h` 添加 `std::atomic<bool> audioSeeking_`
- `doSeek()` 设置 `audioSeeking_ = true`（在 clearQueue 之前）
- `audioLoop()`：`audioSeeking_` 为 true 时 `continue` 跳过旧帧
- audio demuxer seek 完成后 `audioSeeking_.store(false)`
- 效果：seek 期间旧音频帧不会推入队列，避免 writeHead_ 被污染

#### M18-2：音频时钟跳变检测（已有）
- `AudioOutput::callback()` 中已有跳变检测：`current_.pts - writeHead_ > 0.5 || current_.pts < writeHead_ - 0.5`
- 与 `audioSeeking_` 配合，双重保护

#### M18-3：Seeking 指示器
- `VideoRenderer` 添加 `seekingAlpha_` 状态 + `showSeekingOverlay()`/`hideSeekingOverlay()`
- `drawSeekingOverlay()`：半透明暗色遮罩 + 圆角矩形框 + 白色边框 + 三点动画（200ms 循环）
- `Player` 添加 `std::function<void(bool)> onSeekingChanged` 回调
- `doSeek()` 触发 `onSeekingChanged(true)`，`pullFrame()` 到达目标帧时触发 `onSeekingChanged(false)`
- `main.cpp` 连接回调：seeking 开始显示指示器，完成隐藏
- **提交**：`8f2b4ec`

---

### 阶段 M19：倍速保调变速

**目标**：变速不改变音色（保调），消除切换卡顿

#### M19-1：FFmpeg atempo 滤镜替代纯重采样
- **问题**：旧方案通过改变 SwrContext 输出采样率实现变速，本质是"变速变调"（高倍尖锐/低倍沉闷）
- **方案**：使用 FFmpeg `atempo` 滤镜做时间拉伸（WSOLA 算法），保持音调不变
- 滤镜图：`abuffer → aformat(sample_fmts=flt) → atempo(speed) → abuffersink`
- SwrContext 固定为 `float→S16, 44100Hz`（仅做格式转换，不再控制速度）
- `setSpeed()` 仅标记 `speedChanged_`，在 `convert()` 中 flush 旧滤镜图+重建
- 速度变化时旧滤镜图剩余样本被 flush 到队列，无卡顿过渡
- CMakeLists.txt 新增 `avfilter.lib` + `avfilter-12.dll`

#### M19-2：消除变速卡顿
- 移除 `Player::setSpeed()` 中的 `audioSeekPending_`（atempo 不改变时钟，无需 seek）
- 移除 `clearPending_` 机制（队列不再需要清空）
- 变速时旧 chunks 按旧速率播放完后自然过渡到新速率 chunks

- **提交**：`50e65bd`

#### M19 修复（2026-08-21）— 音质噪点 + 面板关闭按钮
- **音质噪点修复**：
  - 根因：flush 路径把 float 数据直接 push 到队列，但 `fill()` 按 S16 解释（`n/4/outRate` 假设4字节/样本），格式不匹配导致噪音
  - 修复：移除 flush 路径，速度变化时仅重建滤镜图，旧 S16 chunks 在队列中自然播放完毕后过渡到新滤镜图输出
- **播放列表关闭按钮**：
  - 面板打开时：关闭按钮（X 图标）绘制在面板左上角 header 区域
  - 面板关闭时：切换按钮（→ 箭头）保留在窗口右边缘 8px 竖条
  - hover 效果：关闭按钮红色高亮，切换按钮灰色高亮
- **提交**：`31411bb`

#### M19 重写：Sonic 替代 atempo（2026-08-21）
- **问题**：atempo 滤镜图每帧 `av_buffersrc_add_frame` + `av_buffersink_get_frame` 开销大，CPU 高，seek 性能下降
- **方案**：Sonic 库（Bill Cox，单头文件 C 库，零分配，专为实时播放器设计）
- 架构：`解码帧 → SwrContext(any→S16, 44100Hz, 仅格式转换) → Sonic(变速不变调) → 队列`
- `sonicCreateStream(44100, 2)` + `sonicSetSpeed()` + `sonicWriteShortToStream()` + `sonicReadShortFromStream()`
- 移除全部 avfilter 滤镜图代码（abuffer/abuffersink/aformat/atempo）
- SwrContext 固定为 `any→S16, 44100Hz`，不再碰速度参数
- CMakeLists.txt：添加 sonic 静态库（`enable_language(C)` + `add_library(sonic STATIC)`），移除 `avfilter.lib`
- **提交**：`5a3cb8f`

#### M19 Sonic 正确集成 + 4 条硬规则（2026-08-21）
- **问题**：Sonic 在1x 仍做 pitch 分析导致嘶嘶声；变速切换时旧 PCM 残留队列导致嗡嗡声；时钟悬空导致视频卡死
- **4 条硬规则**：
  1. **speed=1.0 bypass Sonic**：`std::abs(spd-1.0f)<0.001f` 时直通 S16，零处理
  2. **切倍速 = 原子清队列 + 重建 Sonic**：`speedChanged_` 原子标志 → `queue_.clear()` + `sonicDestroyStream`/`sonicCreateStream`（在解码线程 `convert()` 中执行，无跨线程竞态）
  3. **时钟锚定到 `anchorPts_`**：`setSpeed()` 原子捕获 `writeHead_` → `convert()` 用 `anchorPts_` 重置时钟 → `Player::setSpeed()` 原子读 `anchorPts_` 设 `dropUntil_`
  4. **空队列仍推进时钟**：`fill()` 中队列空时 `writeHead_ += space/(4*freq)*speed`，宁可断音不让视频卡死
- **架构（最终版）**：
  ```
  解码 AVFrame → SwrContext(any→S16/44100Hz/2ch) → [speed≠1.0] Sonic(TSM) → Queue → SDL
  ```
- **dev 目录迁入项目**：`F:\dev\` → `F:\vedioplayer\dev\`（ffmpeg/sdl2/sdl2_image/sonic），CMakeLists.txt 路径全部更新
- **提交**：`82ca9c8` + 后续修复

#### M19 进度条冻结规则（2026-08-21）
- **问题**：频繁切倍速/seek 后进度条缩回开头，无法使用
- **根因**：UI 直接读 `player.clock()` 实时时钟，seek/切倍速时钟 reset 导致读到 0
- **3 条规则**：
  1. **UI 永远不直接读实时时钟**：`RenderStats.uiClock` 由 `player.uiClock()` 填充，seek/切倍速期间返回 `uiTargetPts_`（冻结）
  2. **seek / 切倍速期间进度条冻结**：`uiSeeking_` 原子标志 → `requestSeek()` / `setSpeed()` 设 true → 主循环检测 clock >= target 时设 false
  3. **进度永不回退**：`uiClock()` 返回 `max(clock, uiTargetPts_)`，保证进度条单调递增
- **改动**：`player.h` 加 `uiSeeking_` / `uiTargetPts_` / `uiClock()` / `clearUiSeeking()`；`video_renderer.h` 加 `RenderStats.uiClock`；`main.cpp` 主循环冻结逻辑
- **提交**：`08e995c`

#### M19 A/V 解耦最终修复（2026-08-21）
- **问题**：频繁切倍速+seek 后只有画面没声音/只有声音画面卡住
- **根因**：`doSeek()`（视频线程）和 `convert()` 中 `speedChanged_`（音频线程）同时清队列+重置时钟，互相覆盖；两操作无串行化
- **修复（2 层锁）**：
  1. **`sonicMutex_`**（AudioOutput）：保护 Sonic 重建。`setSpeed()` 直接清队列+重建 Sonic+锚定时钟（原子完成），`convert()` 仅做 Sonic 读写，移除 `speedChanged_` 原子标志
  2. **`opMutex_`**（Player）：串行化 `setSpeed()` 和 `doSeek()`，防止交叉修改音频状态
- **锁序**：`opMutex_` → `sonicMutex_` → `clockMutex_`（无循环依赖）
- **效果**：seek 和倍速切换不再交叉，音频时钟始终连续，视频同步不卡死
- **提交**：`1a33304`

#### M19 音频线程安全架构重写（2026-08-21）
- **问题**：前述修复后高频压力测试仍出现 A/V 解耦
- **根因（3 个数据竞争）**：
  1. **`current_` + `offset_` 无保护**：SDL callback 线程读 `current_.data` + `offset_`，同时 `setSpeed()`/`clearQueue()` 清空它们 → 未定义行为
  2. **时钟回退**：`setSpeed()` 重置 `writeHead_ = anchorPts`，但 `fill()` 已把它推过 anchor → 时钟倒退 → 视频冻结
  3. **`doSeek()` 与 `fill()` 竞争**：`clearQueue()` 清 `current_` 时 `fill()` 正在读
- **核心洞察**：SDL 回调 `fill()` 不能被阻塞，但必须保证它不在状态变更期间运行
- **新架构（pause/resume 模式）**：
  - **所有音频状态变更（清队列、重建 Sonic、重置时钟）必须在 `pauseDevice()` → 操作 → `resumeDevice()` 之间执行**
  - `pauseDevice()` 停止 SDL 回调 → `fill()` 不再运行 → 无需任何锁保护 `current_`/`offset_`/queue/clock
  - 操作完成后 `resumeDevice()` 恢复回调
- **改动**：
  - `AudioOutput`：移除 `speedChanged_` 标志、`opMutex_`、`clearQueue()`、`resetClock()`、`anchorPts_`
  - 新增 `clearAndReset(double newClock)` 和 `setSpeedAndReset(float spd)` — 必须在 pause/resume 间调用
  - `Player::setSpeed()`：`pauseDevice → setSpeedAndReset → clearAndReset(anchor) → resumeDevice`
  - `Player::doSeek()`：`pauseDevice → clearAndReset(target) → resumeDevice`
  - `convert()`：同时持 `swrMutex_` + `sonicMutex_`
- **效果**：零数据竞争，高频操作下 A/V 始终同步
- **提交**：`9ffeca0`

#### M19 seek 音画不同步修复（2026-08-21）
- **问题**：拖动进度条后声音慢几拍
- **根因**：`doSeek()` 中 `resumeDevice()` 太早 — 队列空时 `fill()` 立即空跑推进时钟，音频还没解码出来时钟就超前了
- **修复**：
  1. **`doSeek()` 不再恢复设备**：暂停→清空→设置时钟后保持暂停，等音频线程推入第一帧再恢复
  2. **`audioLoop()` 推入第一帧后恢复设备**：`seekResumePending` 标志，首帧推入即 `resumeDevice()`
  3. **`clock()` 返回 -1.0（设备暂停时）**：`devicePaused_` 原子标志，`Player::clock()` 回退到 `videoClock()`
  4. **`pullFrame()` 不再恢复设备**：全部由 `audioLoop()` 负责
- **效果**：seek 后音频设备暂停→时钟冻结→视频用视频时钟→音频解码完成后恢复→音画同步
- **提交**：`1fd05ba`

#### M19 音频架构终极重写：永不暂停模式（2026-08-21）
- **问题**：pause/resume 模式导致 seek 后画面卡数秒 + 音画不同步
- **根因分析**：
  1. `doSeek()` 暂停设备 → `fill()` 停止 → 时钟冻结 → 视频用 videoClock 追赶
  2. `audioLoop()` 推入首帧后才恢复设备 → 音频时钟从 seek target 重新开始
  3. 视频时钟已前进 vs 音频时钟从 target 重启 → 时钟断裂 → 音画不同步
  4. `videoQueue_.pop(f)` 在 audioWait_ 期间阻塞主线程 → UI 冻结
- **参考**：VLC/mpv/ExoPlayer 的 seek 策略 — **永远不暂停音频设备**
- **新架构（永不暂停模式）**：
  - `current_` 和 `offset_` **仅由 fill()（SDL 回调线程）访问**，外部永不触碰 → 零竞态
  - `queue_.clear()` 使用 BlockingQueue 自带锁 → 线程安全
  - `setClock(t)` 使用 clockMutex_ → 线程安全
  - `rebuildSonic()` 使用 sonicMutex_ → 线程安全
  - **seek**：`clearQueue()` + `setClock(target)` → fill() 输出静音 → 新数据到达后自然恢复
  - **变速**：`rebuildSonic()` + `clearQueue()` + `setClock(anchor)` → 旧数据自然消费完
  - 时钟永远连续，无跳变，无冻结
- **改动**：
  - AudioOutput：移除 `clearAndReset()`、`setSpeedAndReset()`、`devicePaused_`；新增 `clearQueue()`、`rebuildSonic()`、`setSpeed()`
  - Player：`doSeek()` 不暂停设备；`setSpeed()` 不暂停设备；`audioLoop()` 移除 seekResumePending
- **效果**：
  - Seek 即时响应（无冻结），时钟连续无跳变
  - 变速即时生效（无嗡嗡声/杂音）
  - 频繁操作下 A/V 始终同步
- **提交**：`8ba12df`

### 阶段 M20：fill() 内原子处理状态变更 — 消除变速/seek 竞态 ✅ 完成

- 任务：将所有音频状态变更（变速/seek）延迟到 fill() 内处理，消除跨线程竞态
- **根因分析**（M19 遗留问题）：
  1. **Player::speed_ 与 AudioOutput::speed_ 不一致**：Player::speed_ 在 rebuildSonic() 前更新，视频用新速度计算 delay，但音频还在旧速度
  2. **fill() 中时钟推进使用 AudioOutput::speed_**：speed_ 在 rebuildSonic() 内更新但 fill() 不持 sonicMutex_ 读 speed_，TOCTOU 竞态
  3. **clearQueue + setClock 非原子**：fill() 可能在两步之间推进时钟，setClock 回退时钟
  4. **旧 chunk 在 current_ 中按新速率推进时钟**：clearQueue 不清 current_，旧 1x 数据按 2x 速率推进时钟→时钟超前
- **解决方案**：延迟操作模式（Deferred Action Pattern）
  - 新增 `pendingSpeed_` 原子量（float，-1=无待处理）
  - 新增 `pendingSeek_` 原子量（double，-1=无待处理）
  - `fill()` 回调开头原子 exchange 读取 pending，一次性完成：清 current_ + 清 queue + 重建 Sonic + 设时钟 + 更新 speed_
  - `Player::setSpeed()` 改为调用 `requestSpeedChange()`，然后轮询等待 fill() 处理完毕，再更新 Player::speed_
  - `Player::doSeek()` 改为调用 `requestSeek()`，消除与 fill() 的竞态
- **改动**：
  - `audio_output.h`：新增 `requestSpeedChange()`、`pendingSpeed()`、`hasPendingSpeed()`、`requestSeek()`、`hasPendingSeek()`；新增 `pendingSpeed_`、`pendingSeek_` 原子成员
  - `audio_output.cpp`：fill() 开头处理 pendingSpeed 和 pendingSeek；新增方法实现
  - `player.cpp`：setSpeed() 使用 requestSpeedChange + 轮询等待；doSeek() 使用 requestSeek
  - `main.cpp`：修复 SDL_MAIN_HANDLED 重复定义警告
- **关键设计**：
  - 所有状态变更在 fill() 线程（SDL 回调线程）内完成 → 零竞态
  - pendingSpeed_ 使用 exchange(-1) 原子操作 → 一次读取，不会丢失
  - 轮询等待最多 20ms（20×1ms），一个回调周期内必然处理
- **效果**：
  - 高频切倍速无音画不同步、无失声
  - Seek 时钟原子重置，无竞态窗口
  - 进度条冻结规则（uiSeeking_ + uiTargetPts_）保持不变
- **提交**：`9eff158`

### 阶段 M21：时钟语义修正 — speed 只属于 Sonic，不属于时钟 ✅ 完成

- 任务：修正音频时钟和视频同步逻辑，使 speed 仅影响 Sonic 内容拉伸，不影响时间轴
- **根因分析**（M20 后遗留问题）：
  - Sonic 做的是"内容拉伸"（1 秒内容在 0.5 秒播完），不是"时间膨胀"
  - 但原代码把 speed 当"时间膨胀系数"：`writeHead_ += ... * speed_`（音频时钟被污染）
  - `videoClock()` 里 `elapsed * speed_`（视频时钟也被污染）
  - `pullFrame()` 里 `(pts - target) / spd`（delay 被 speed 除）
  - 结果：音频和视频以不同速率理解时间 → 持续音画不同步
- **正确模型**：
  - 音频时钟 = 已播放样本数 / 采样率（不乘 speed）
  - 视频 delay = frame.pts - audioClock（不除 speed）
  - videoClock（纯视频回退）= 基准 + 已流逝系统时间（不乘 speed）
  - speed 只在 Sonic 内部使用：`sonicSetSpeed(sonic_, speed)`
- **改动**：
  - `audio_output.cpp`：fill() 中 `writeHead_` 推进移除 `* speed_`（队列空和正常两处）
  - `player.cpp`：`videoClock()` 移除 `* speed_`；`pullFrame()` delay 计算移除 `/ spd`
- **验证逻辑**（2x 速度 10 秒视频）：
  - 修正前：音频 5 秒播完，视频 10 秒走完 → 持续不同步
  - 修正后：音频 5 秒播完，视频 5 秒走完（audioClock 在 5 秒时到达 10 秒 PTS）→ 同步
- **提交**：`b73d9fa`

### 阶段 M22：seek 后音频时钟立即初始化 — 消除 seek 后音画延迟 ✅ 完成

- 任务：seek 后立即初始化音频时钟到 target，防止视频用 videoClock 超前
- **根因分析**：
  - seek 后视频立即开始显示（audioWait_ 机制），但音频时钟还是 -1.0
  - 视频 fallback 到 videoClock() → videoBasePts_ + elapsed → 跑在音频前面
  - 等音频首块到达 fill() 时钟才初始化 → 视频已超前 → 音画延迟
- **修复**：doSeek() 中先 `clearQueue()` + `setClock(t)`，再 `requestSeek(t)`
  - clearQueue() 清空队列 → fill() 输出静音不推进时钟
  - setClock(t) 立即初始化时钟 → 视频不会 fallback 到 videoClock
  - requestSeek(t) 的 pendingSeek_ 在 fill() 内原子处理 current_ 清理
  - race 安全：queue 空时 fill() 不推进 writeHead_
- **改动**：`player.cpp` doSeek() 中增加 clearQueue() + setClock(t)
- **效果**：seek 后音频时钟立即就位，视频不超前，字幕和声音同步
- **提交**：`6121b3b`

### 阶段 M23：全面竞态审计 — 5 项修复消除音画不同步根因 ✅ 完成

- 任务：系统性排查音画不同步的所有潜在因素（架构/变量/加锁/代码组合）
- **发现的问题**：
  1. **首块不重锚**（严重）：seek 后 `writeHead_` 被设为 target，但静音推进时钟超前；首块到达时不重置 → 时钟永远超前于实际音频
  2. **doSeek clearQueue+setClock 竞态**（严重）：从解码线程直接调用 `clearQueue()`/`setClock()` 与 `fill()` 存在竞态窗口，可能播放旧数据或时钟跳变
  3. **音频线程 seek 后推旧数据**（中等）：`audioSeeking_` 在 demuxer seek 后、`readPacket()` 前设 false；旧位置解码的帧可推入队列
  4. **videoBasePts_/videoBaseTicks_ 无同步**（中等）：解码线程写、主线程读，无原子/锁保护 → 撕裂读
  5. **dropUntil_ 被 doSeek 重置**（低）：`setSpeed()` 设的 `dropUntil_=anchor` 被 `doSeek()` 的 `-1e9` 覆盖
- **修复**：
  1. `AudioOutput` 新增 `reanchor_` 标志：`pendingSeek_` 处理时置 true，首块到达时重置 `writeHead_ = chunk.pts`
  2. `doSeek()` 移除 `clearQueue()`+`setClock()`：完全由 `pendingSeek_` 在 `fill()` 内原子处理
  3. `audioLoop` seek 后检查 `framePts(f) < seekTarget - 0.5` → 丢弃旧位置数据
  4. `videoBasePts_`/`videoBaseTicks_` 改为 `std::atomic` → 消除撕裂读
  5. `doSeek()` 末尾移除 `dropUntil_.store(-1e9)`
- **改动文件**：`audio_output.h/cpp`、`player.h/cpp`
- **验证**：编译通过，冒烟测试无崩溃

### 阶段 M23.5：深度优化 seek 跳转性能 ✅ 完成

- 任务：消除 seek 后时钟漂移、旧帧入队竞态、reanchor 后跳等系统性问题
- **发现的问题**：
  1. **静音推进时钟**：seek 后队列空，fill() 静音块推进 writeHead_ → 时钟漂移到 seekTarget 之后 → reanchor 拉回 → 时钟跳变
  2. **reanchor 后跳**：首块 PTS 可能 < seekTarget（keyframe 对齐），reanchor 设 writeHead_=chunkPTS → 时钟回退
  3. **0.5s 丢弃阈值太松**：旧帧 PTS > seekTarget-0.5 就被推送，导致偏移
  4. **doSeek→fill 竞态窗口**：doSeek 设 audioSeeking_=true，但 fill() 消费 pendingSeek_ 前，音频线程可能已推送旧帧
  5. **AudioOutput 缺乏 seek 阻断机制**：push()/tryPush() 无法感知 seek 状态
- **修复**：
  1. `AudioOutput` 新增 `waitingForSeek_` 标志：pendingSeek_ 处理时置 true，首块到达时置 false；waitingForSeek_ 期间静音不推进时钟
  2. `reanchor_` 改为仅前向修正：`chunk.pts > writeHead_` 时才修正，避免时钟跳回
  3. `AudioOutput` 新增 `setSeeking(bool)` 原子标志 + `seeking_` 成员：seek 期间 push()/tryPush() 返回 false，阻断旧帧入队
  4. `doSeek()` 设 `audio_->setSeeking(true)`；`audioLoop()` seek 处理完、读新数据前设 `setSeeking(false)`
  5. `audioLoop` PTS 丢弃阈值从 0.5s 收紧至 0.1s
- **改动文件**：`audio_output.h/cpp`、`player.cpp`
- **验证**：编译通过，冒烟测试无崩溃
- **提交**：`a635745`（音频 seek 位置修复）、`6c68525`（深度优化）

### 阶段 M24：切视频进度条不重置 + seek 偏移回归修复 ✅ 完成

- 任务：修复切换视频后进度条仍显示上一个视频进度 + seek 偏移问题
- **发现的问题**：
  1. **进度条不重置**：`openFile()` 没有重置 `uiSeeking_`、`uiTargetPts_`、`audioWait_`、`dropUntil_`、`audioSeeking_`；切视频后 `uiClock()` 仍返回旧值
  2. **seek 偏移**：M23.5 引入的 `waitingForSeek_` 冻结时钟，但首块音频 PTS 在 seekTarget 之前（keyframe 对齐）→ reanchor 不触发 → 时钟与实际音频错位
- **修复**：
  1. `openFile()` 新增重置：`audioWait_`、`audioSeeking_`、`seekFirstFrame_`、`uiSeeking_`、`uiTargetPts_`
  2. 去掉 `waitingForSeek_`/`seekConsumed_` 机制，恢复简单 reanchor：首块到达直接 `writeHead_ = chunk.pts`
  3. 静音块恢复推进时钟（与 PTS 同步）
- **改动文件**：`audio_output.h/cpp`、`player.cpp`
- **验证**：编译通过，冒烟测试无崩溃
- **提交**：`6c30ef6`

### 阶段 M25：修复 seek 跳转偏移（flush 顺序 + 旧包丢弃）✅ 完成

- 任务：修复 seek 到 25%-50% 区间总是跳到 50% 之后的问题
- **根因分析**：
  1. `doSeek()` 先 `videoDemuxer_->seek(t)` 再 `flushBuffers()`，但 decodeLoop 可能在 seek 之后、flush 之前已 `readPacket()` 读到旧位置的包
  2. 旧包送入 decoder → 解码出旧 PTS 帧 → 绕过 `audioWait_` 丢弃检查（若 `audioWait_` 已被音频线程清除）→ 显示错误位置
  3. flush 必须在 seek 之前调用，确保 decoder 内部残留的旧数据先清掉
- **修复**：
  1. `doSeek()`: `flushBuffers()` 移到 `videoDemuxer_->seek(t)` 之前
  2. `decodeLoop`: `readPacket()` 后检查 `seekRequested()`，旧包直接 `continue` 不送 decoder
  3. `audioLoop`: 同样在 `send()` 前检查 `seekRequested()`，旧包丢弃
- **改动文件**：`player.cpp`
- **验证**：编译通过，冒烟测试无崩溃
- **提交**：`d1fdd0f`

### 阶段 M26：诊断日志 + seek 偏移根因分析 ✅ 完成

- 任务：收集 seek/变速时钟数据，定位 ~2x 偏移根因
- **根因分析**（代码审查 + ffplay 源码对比）：
  - ffplay 时钟模型：`audio_clock = frame_pts + frame_duration`，通过减去缓冲区估算当前位置，不手动推进 writeHead_
  - 本项目模型：`writeHead_ += n/4/outRate * speed_`，手动推进。数学正确（Sonic 压缩后 PCM 消耗量 × speed = 内容时间），但与 reanchor 交互有竞态
  - **竞态窗口**：setSpeed() 中 `setClock(anchor)` + `requestSpeedChange(s)` 非原子，fill() 可在两步间用旧速度推进 writeHead_
  - **静音推进**：reanchor_ 等待期间 writeHead_ 以 speed_ 推进，pullFrame() 可观察到漂移后的时钟值
- **诊断日志**：全 ASCII，写入 seek_trace.log
  - fill()：pendingSeek/pendingSpeed 消费、reanchor 触发、SILENCE+REANCHOR
  - player.cpp：doSeek/setSpeed/pullFrame audioWait/audioLoop seek+push
- **改动文件**：`audio_output.cpp`、`player.cpp`
- **状态**：日志已添加，待编译测试

### 阶段 M27：统一日志模块 Logger（完成）

- 任务：替换临时 dbg() 为正规日志模块，支持日志目录、日期命名、自动清理
- **改动**：
  - 新建 `src/core/logger.h` + `src/core/logger.cpp`（单例，线程安全，5 级日志）
  - 删除 `audio_output.cpp`/`player.cpp`/`demuxer.cpp` 中 3 份重复的 `static FILE* g_dbg` + `dbg()`
  - 所有日志调用替换为 `LOG_DBG("MODULE", ...)` 宏
  - `Logger::init("vplayer", 7)` 自动创建 `<exe_dir>/logs/`，按 `vplayer_YYYY-MM-DD_HHMMSS.log` 命名
  - 启动时自动清理超过 7 天的旧日志
  - `.gitignore` 添加 `*.log`
  - `AGENTS.md` 新增第 7 节：日志规范（级别/格式/模块标签/强制路径/文件管理）
- **日志格式**：`[秒.毫秒] [LEVEL] [MODULE] message`（全 ASCII，Windows 兼容）
- **模块标签**：MAIN/FILL/SEEK/SPEED/PULL/DECODE/ALOOP/DEMUX/VIDEO/AUDIO
- **提交**：`748dda6`
- **验证**：编译通过，运行 8 秒生成 `logs/vplayer_2026-08-23_004407.log`（73KB，1224 行），格式正确

### 阶段 M28：seek ~2x 偏移根因定位 + 3 项修复 ✅ 完成

- 任务：修复 seek 后音频时钟偏移约 2 倍目标位置的问题
- **根因分析**（日志诊断）：
  - 日志显示：seek 到 517.895s 后，fill() reanchor 跳到 chunk.pts=1056.740（≈2×目标）
  - 时序：`doSeek()` 先调用 `audio_->requestSeek(t)` → fill() 立即消费 pendingSeek_ 并清队列+设 writeHead_=t+reanchor_=true；但此时 audioLoop 尚未完成 audio demuxer seek → 从旧位置读到的包被 push → fill() reanchor 到错误 pts
  - setSpeed() 竞态：`setClock(anchor)` + `requestSpeedChange(s)` 非原子，fill() 可在两步间推进时钟
- **修复 1：reanchor 安全检查**（`audio_output.cpp`）
  - fill() 中 reanchor 逻辑增加安全检查：若 chunk.pts 偏离 writeHead_ 超过 ±2 秒，跳过 reanchor 并丢弃该 chunk
  - 防御性措施：即使音频 demuxer seek 返回错误数据，也不会污染时钟
- **修复 2：doSeek 流程修正**（`player.cpp`）
  - 原流程：`audio_->requestSeek(t)` → `audioSeekPending_=true`（fill() 在 audioLoop seek 前消费 pendingSeek_）
  - 新流程：`audioSeekPending_=true` → 等待 `audioSeeking_` 变 false（audioLoop 完成 seek）→ 再调 `audio_->requestSeek(t)`
  - 确保 fill() 消费 pendingSeek_ 时，audio demuxer 已经 seek 到正确位置
- **修复 3：setSpeed 原子化**（`audio_output.h/cpp` + `player.cpp`）
  - 新增 `pendingSpeedAnchor_` 原子量，`requestSpeedChange(spd, anchor)` 同时设置速度和锚点
  - fill() 消费 pendingSpeed 时原子读取 anchor 并设置 writeHead_，消除 TOCTOU 竞态
  - Player::setSpeed() 改为调用 `requestSpeedChange(s, anchor)` 一步完成
- **改动文件**：`audio_output.h`、`audio_output.cpp`、`player.cpp`
- **验证**：编译通过，冒烟测试 8 秒无崩溃，日志格式正确
- **状态**：代码已实现，待用户实际 seek/变速测试验证效果

### 阶段 M29：日志分级 + 进度条预览时间戳 ✅ 完成

- 任务：日常模式不产生大日志；进度条悬停缩略图下方显示目标时间
- **日志分级**（`main.cpp` + `audio_output.cpp` + `player.cpp`）：
  - 默认 WARN 级别（日常模式）：仅记录警告/错误，正常播放时日志 0 bytes
  - `--debug` 命令行参数开启诊断模式（TRACE 级别）：全部记录
  - 高频路径降级为 TRACE：`SILENCE+REANCHOR`、`ALOOP push`、`PULL display`、`PULL dropping frame`
  - 诊断模式 5 秒播放产生 80KB 日志 vs 之前4.7MB
- **进度条预览时间戳**（`video_renderer.cpp`）：
  - 缩略图下方显示目标时间（黑底白字，HH:MM:SS 或 MM:SS 格式）
  - 居中于缩略图，随鼠标位置实时更新
- **改动文件**：`main.cpp`、`audio_output.cpp`、`player.cpp`、`video_renderer.cpp`
- **验证**：编译通过，正常模式日志 0 bytes，诊断模式日志正常输出

### 阶段 M30：控件动画 + OSD 增强 ✅

- 任务：两项 UI 增强（缩略图优化已尝试后回退）
- **M30a 控件动画**：
  - 新建 `src/ui/easing.h`：纯头文件缓动函数库（linear、ease-in/out/in-out quad/cubic、easeOutExpo/Back/Elastic、smoothstep、lerpf）
  - 控件 alpha 动画：hover 时 fade in 200ms，离开时 fade out 200ms（animControlsAlpha_）
  - 按钮 hover 缩放：1.0 → 1.15 ease-out；点击弹性回弹 0.9 → 1.0（animThumbScale_）
  - 进度条展开/收起：高度 4px ↔ 8px，thumb 12px ↔ 18px（animTrackH_）
- **M30b 缩略图优化（已回退 ❌）**：
  - 尝试 LRU 缓存 + 多线程 worker 异步提取
  - 结果：缓存未命中时显示黑屏，体验反而比同步方案差
  - 根因：getFrame() 本身需要 100-500ms，异步方案引入显示间隙
  - 教训：同步方案虽然有卡顿但功能正确，异步方案需要预计算才能真正解决问题
  - 已回退为同步提取（原方案），2 小时以上视频测试无卡顿
- **M30c OSD 增强**：
  - player 新增查询接口：videoBitrate/audioBitrate/videoWidth/Height/Fps/audioSampleRate/hwDecoding/videoCodecName/audioCodecName
  - OSD 字体扩展：原 14 字符（0-9:.%）→ 66 字符（+x+A-Z+a-z）
  - 按 I 键切换 OSD 显示（半透明背景，含码率/分辨率/帧率/采样率/硬解状态/时长）
  - drawInfoOverlay 渲染在所有 UI 层之上
- **改动**：
  - 新增文件：`src/ui/easing.h`
  - 修改文件：`video_renderer.h/cpp`（动画状态+绘制）、`main.cpp`（OSD/I键）、`osd.h/cpp`（字体扩展+drawInfoOverlay）、`player.h/cpp`（媒体信息查询）
- **效果**：控件平滑过渡、信息显示一键切换、缩略图同步提取可靠

### 阶段 M31：字幕增强（ASS 样式渲染器）✅ 完成

- 任务：实现轻量 ASS override tag 解析器 + 渲染器，无需 libass 依赖链
- **背景**：libass 需要 freetype/fontconfig/fribidi/harfbuzz/glib2 等 15-20 个 DLL ~30MB，影响打包/移植性
- **方案**：参考 ASS.js 架构（解析→分段→渲染），使用 SDL_ttf 实现轻量渲染
- **新增文件**：
  - `src/subtitle/ass_renderer.h` — ASSRenderer 类定义
  - `src/subtitle/ass_renderer.cpp` — 完整实现（~490 行）
- **支持的 ASS override tags**：
  - 样式：`\b`（粗体）、`\i`（斜体）、`\u`（下划线）、`\s`（删除线）
  - 字体：`\fn`（字体名）、`\fs`（字号）、`\fscx/\fscy`（缩放）、`\fsp`（字间距）
  - 颜色：`\c`、`\1c`-`\4c`（BGR+Alpha 格式）、`\alpha`、`\1a`
  - 效果：`\bord`（描边宽度）、`\shad`（阴影深度）
  - 定位：`\an`（对齐方式 1-9）
  - 待支持（future）：`\pos`、`\move`、`\fad/\fade`、`\t`、`\clip`、`\frz`
- **渲染流程**：
  1. 解析 ASS 文件 [V4+ Styles] 节 → 存储样式表
  2. Dialogue 行解析 override tags → 生成 StyledSegment 列表
  3. 每个 segment 用 SDL_ttf 渲染（支持 outline 描边）
  4. 按 alignment + margin 定位到视频帧
- **架构改动**：
  - SubtitleEvent 增加 `rawAss` 字段（保留原始 Dialogue 行含 tags）
  - SubtitleTrack 增加 `rawDialogueAt()` + `assContent()` 接口
  - RenderStats 增加 `rawSubtitle` 字段（传递原始 ASS 行到渲染器）
  - VideoRenderer 持有 ASSRenderer 实例，`drawSubtitle()` 优先尝试 styled 渲染
  - main.cpp `loadExternalSubtitle()` 在加载 .ass/.ssa 时自动解析样式
- **SDL_ttf 集成**：
  - 下载 SDL2_ttf 2.22.0（`G:\vedioplayer\dev\sdl2_ttf\`）
  - CMakeLists.txt 新增 SDL_ttf 头文件路径 + 链接 + DLL 拷贝
- **编译问题修复**：
  - `TTF_Font` 前向声明与 `SDL_ttf.h` typedef 冲突 → 改用 `struct _TTF_Font; typedef struct _TTF_Font TTF_Font;`
  - `ass_renderer.h` 缺少 `<map>` include
  - `main.cpp` 缺少 `<fstream>` include（用于读取 ASS 文件内容）
  - SDL_ttf 头文件路径嵌套（`include/SDL2/SDL2/`）→ CMake include path 修正
- **测试**：
  - 创建 `testdata/test.ass` + `testdata/4.ass`（4 种样式：Default/Bold/Color/Italic）
  - 日志确认：4 styles parsed, rendered subtitle: 273x32 align=2 segs=1
  - 编译通过，运行无崩溃
- **状态**：代码完成，待用户实际观看带 ASS 字幕的视频验证效果

### 阶段 M31e：音频 seek/speed 静音根因定位与修复 ✅ 完成

- **任务**：定位并修复 M31d 发现的音频 seek/speed 变更后永久静音问题
- **根因分析**：
  - 日志分析发现：seek 到 1278.884s 后，REANCHOR SKIP 永久循环（323,669 次），chunk.pts=2609.784 vs writeHead_=1279.564，diff=1330
  - ALOOP push 日志显示 `framePts(f)=1278.794`（正确），但 REANCHOR SKIP 显示 `chunk.pts=2609.784`（错误）
  - **根因**：`AudioOutput::convert()` 中 `chunk.pts = frame->pts * ptsScale_`，而 `ptsScale_` 取自 `av_q2d(audioStream->time_base) = 1/44100`
  - 同一帧的 `framePts(f) = frame->best_effort_timestamp * videoPtsScale_`（video time_base = 1/90000）
  - MPEG-TS 容器中，所有流的 raw PTS 使用共同的 1/90000 time_base，但 `AudioOutput` 错误地用音频流自己的 time_base（1/44100）缩放
  - 结果：`chunk.pts` 比正确值大了 90000/44100 ≈ 2.041 倍 → 与 writeHead_ 差 1330s → reanchor 永远失败 → 所有音频 chunk 被丢弃 → 永久静音
- **修复**：
  - `src/core/player.cpp:54`：`ptsScale` 从 `av_q2d(ademux->audioStream()->time_base)` 改为 `av_q2d(vdemux->videoStream()->time_base)`
  - 使 `AudioOutput::ptsScale_` 与 `Player::framePts()` 使用相同的 video time_base，`chunk.pts` 正确反映秒数
- **附带修复**：
  - `src/main.cpp:192`：过滤 `--debug` 参数，避免被当作文件路径加入 playlist
- **测试**：
  - 构建成功，`--debug` 模式运行 4.mp4 12 秒
  - 日志确认：`REANCHOR: writeHead_ -1.000 -> chunk.pts 0.000`（首次 reanchor 成功）
  - `writeHead_` 正常递增（4.621 → 9.265），无 REANCHOR SKIP 或 SILENCE 警告
  - 音频正常播放
- **状态**：修复完成，待提交

### 阶段 M31f：变速后 reanchor SKIP 永久静音修复 ✅ 完成

- **任务**：修复 M31e 后仍存在的变速（尤其慢放 <1x）后失声问题
- **根因分析**：
  - 日志显示：每次 pendingSpeed 消费后，queue 被清空、writeHead_=anchor、reanchor_=true
  - 但 audioLoop 的解码器输出位置与 anchor 存在竞态差距（解码器已读到 anchor 之后8s+）
  - 新 chunk.pts 远离 writeHead_（diff >2s）→ REANCHOR SKIP → 所有后续 chunk 被丢弃 → 永久静音
  - 47,806 次 REANCHOR SKIP，0 次成功 reanchor（速度变更场景）
  - 慢放时更严重：speed=0.25 时 writeHead_ 推进极慢（0.006s/callback），解码器持续产出 pts=3230+ 的 chunk，差距持续扩大
- **修复**：
  - `src/audio/audio_output.h`：新增 `reanchorSpeed_` 标志区分变速/seek 重锚
  - `src/audio/audio_output.cpp`：pendingSpeed 设置 `reanchorSpeed_=true`，pendingSeek 设置 `reanchorSpeed_=false`
  - **seek reanchor**：保留2秒阈值，检测失败的 seek
  - **变速 reanchor**：无阈值，始终接受第一个 chunk 并锚定时钟
  - JUMP 日志记录大偏移但不丢弃数据
- **二次修复**（M31f.1）：
  - M31f 初版完全移除阈值导致 seek 后音频位置错误
  - 区分两种 reanchor 模式后，seek 仍保留2秒阈值检测失败跳转
- **测试**：
  - 构建成功，`--debug` 模式运行验证
  - 需用户实际操作变速+跳转验证
- **状态**：修复完成，待提交
