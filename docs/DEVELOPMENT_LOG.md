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
- 产物：`F:\dev\ffmpeg`（BtbN master 63.6）、`F:\dev\sdl2\x86_64-w64-mingw32`（2.32.10）
- 验证：测试程序输出 `SDL 2.32.10, FFmpeg 63.6.100` ✓

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
  - 期间尝试 AddressSanitizer 定位越界，但 w64devkit 未自带 libasan → 失败，改用手工二分隔离
- 遗留清理：F:\dev 下已删除所有安装包（ffmpeg*.zip、sdl2.zip、7zr.exe、ffmpeg-9.0.1-shared.7z）；保留 ffmpeg.exe/ffplay.exe/ffprobe.exe（解压产物，调试可用）
- 纠正：冒烟测试视频原位于 C 盘临时目录（`C:\Users\31697\AppData\Local\Temp\opencode\s_30s.mp4`），不符合"不占用 C 盘"原则 → 已移至 `F:\vedioplayer\testdata\s_30s.mp4` 并更新 AGENTS.md 引用

### 阶段 M4：替换稳定版 FFmpeg + 根因重新定位 ✅ 完成
- 任务：换 gyan.dev 稳定版 9.0.1 shared 构建（当时认为 BtbN master 有 bug），结果反而把真正的根因挖了出来
- 下载过程（已向用户说明理由并获确认）：
  - gyan.dev 直连下载 4MB 就断（服务器限速/不稳定）→ 删除残缺文件
  - 重试（带 UA + --retry）15 分钟超时 → 放弃直连
  - 改用 GitHub 镜像：`GyanD/codexffmpeg` release 9.0.1 的 `ffmpeg-9.0.1-full_build-shared.zip`（97,261,187 字节），经 ghproxy.net 下载成功 ✓
- 解压产物：`F:\dev\ffmpeg-9.0.1-full_build-shared`（bin/include/lib/doc/presets）；zip 安装包已删除 ✓
- 库结构特点：
  - **无 pkg-config 文件** → CMake 不再用 pkg_check_modules(FFMPEG)，改为显式 .lib 路径链接
  - **lib 为 MSVC 风格 COFF 导入库**（avformat.lib 等），MinGW 用 `-l:avformat.lib` 语法可正常链接 ✓
  - **DLL 版本号与 BtbN master 相同**（avcodec-63.dll / avformat-63.dll / avutil-61.dll / swresample-7.dll / swscale-10.dll）
  - **UCRT64 工具链构建**（导入 api-ms-win-crt-*），而 w64devkit 的 exe 是 msvcrt 运行时（导入 msvcrt.dll）
- 排查过程（**推翻 M3 结论：不是库 bug！**）：
  1. gyan 自带 ffprobe/ffplay（UCRT exe + 同套 DLL）解码/播放 s_30s.mp4 完全正常 → **库本身没问题**
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
  - 还原 M3 调试残留：main.cpp 渲染/OSD/Present、player.cpp debugNoAudio
  - `CMakeLists.txt`：FFmpeg 改显式 .lib 链接，DLL 拷贝改新库路径
- 验证：vplayer.exe 播放 s_30s.mp4（音频+视频+渲染全开）10 秒 / 20 秒均无崩溃 ✓；748 帧解码测试全过 ✓
- 教训记录（重要）：
  1. **shared_ptr 的删除器是构造参数**（`std::shared_ptr<T>(p, deleter)`），`unique_ptr` 才是模板参数——以后凡持有 FFmpeg 对象必须走 makeFramePtr/makePacketPtr
  2. 不能因为"两个库版本相同号就断定库没问题/有问题"——先怀疑自己的代码
  3. 测试程序硬编码文件路径导致 open fail 误判库有问题的教训：路径修正要检查源码转义（`\\`）
- 遗留：`F:\dev\ffmpeg`（BtbN 旧库）暂保留作对照；后续确认无问题后删除

### 阶段 M5（下一步规划）
- 全功能验证：播放/暂停/seek/音量/全屏/拖拽
- 更多格式冒烟：本地其它视频文件
- 性能与稳定性：长时间播放、异常文件（损坏/无音频/仅音频）
- 考虑：M3 时发现的"只持有不释放也崩"是同一个根因（shared_ptr 跨堆 free），无额外问题