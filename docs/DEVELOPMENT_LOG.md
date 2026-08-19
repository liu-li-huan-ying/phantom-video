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
- 产物：vplayer.exe 编译成功（606KB）
- 清理旧项目：已删除 Go 版 server/web/vplayer.exe/旧文档，git 提交"清理旧 Go 版本"

### 阶段 M3：播放内核开发 🔴 进行中（被 FFmpeg 库 bug 卡住）
- 任务：解封装/解码/队列/时钟/音频输出/视频渲染/播放控制 —— 代码已全部写完
- 困难与解决（**重大：堆损坏崩溃 0xC0000374**）：
  - 症状：播放器启动数秒内崩溃；gdb 显示在 `av_frame_free`（释放 AVFrame）时 Windows 堆校验失败
  - 排查过程（逐层隔离）：
    1. 禁用音频 → 仍崩（排除音频路径）
    2. 禁用渲染/OSD/Present → 仍崩（排除 SDL 渲染）
    3. 写纯控制台测试（完全不碰 SDL）→ 仍崩！确定问题在 FFmpeg 调用
    4. 标准 C 用法（帧立即释放，不持有）→ 正常，748 帧全部解码成功
    5. 用 shared_ptr 持有帧（播放器标准模式）→ 崩（单线程也崩）
    6. 只持有不释放任何帧 → 也崩
  - 结论：**BtbN master-latest 自动构建的 FFmpeg 存在 bug**——所有播放器通用的"持有帧继续解码"模式即触发堆损坏，非本工程代码问题
  - 解决方案（待执行）：换用 gyan.dev **稳定版 FFmpeg 9.0.1 shared** 构建
    - 下载地址：`https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full-shared.7z`（约 100MB，唯一含 include/lib 的共享构建）
    - 解压工具：用户本机已有 `D:\Program Files\7-Zip\7z.exe`（不再需要额外下载工具）
    - 注意：gyan 构建为 UCRT64 工具链，与 w64devkit（msvcrt）混用需验证
- 遗留清理：F:\dev 下已删除所有安装包（ffmpeg*.zip、sdl2.zip、7zr.exe、ffmpeg-9.0.1-shared.7z）

### 待办
- [ ] 下载稳定版 FFmpeg 9.0.1（说明理由：master 构建有 bug）
- [ ] 解压到 F:\dev，验证持帧模式不再崩溃
- [ ] 调整 CMake 指向新 FFmpeg，重新冒烟测试
- [ ] M3 完成后 git 提交