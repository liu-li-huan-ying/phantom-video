# VPlayer - 自研 C++ 视频播放器

基于 **C++17 + FFmpeg + SDL2** 的自研桌面视频播放器，从零实现解封装、解码、音视频同步与播放控制。

## 技术栈

| 组件 | 技术 | 说明 |
|---|---|---|
| 语言 | C++17 | g++ 15.2.0 (w64devkit) |
| 构建 | CMake 3.16+ | pkg-config 定位 SDL2，FFmpeg 显式链接 .lib |
| 解封装/解码 | FFmpeg 9.0.1 (gyan.dev stable) | libavformat / libavcodec / libavutil / libswresample |
| 窗口/渲染/音频 | SDL2 2.32.10 | SDL_Renderer + SDL_OpenAudioDevice |
| 平台 | Windows x64 | MinGW-w64 |

## 目录结构

```
F:\vedioplayer\
├── CMakeLists.txt          # 构建脚本
├── docs\DESIGN.md          # 详细设计文档
├── src\
│   ├── main.cpp            # 入口 + SDL 事件循环 + 快捷键
│   ├── core\               # 播放内核（与 UI 无关）
│   │   ├── blocking_queue.h    # 线程安全有界队列
│   │   ├── clock.h/.cpp        # 主时钟（音频时钟/视频时钟）
│   │   ├── demuxer.h/.cpp      # 解封装
│   │   ├── decoder.h/.cpp      # 音视频解码器封装
│   │   └── player.h/.cpp       # 播放器总控（线程 + 同步 + 拖动）
│   ├── audio\audio_output.h/.cpp   # SDL 音频输出 + 采样率转换
│   ├── video\video_renderer.h/.cpp # SDL 视频渲染（YUV 纹理）
│   └── ui\osd.h/.cpp            # 进度条/时间/音量 OSD 绘制
├── third_party\            # 运行时 DLL（发布时拷贝）
│   └── bin\
└── build\                  # 构建产物（gitignore）
```

## 构建

```powershell
# 环境变量（依赖位于 F:\dev；FFmpeg 不走 pkg-config，由 CMake 直接链接）
$env:PKG_CONFIG_PATH = "F:\dev\sdl2\x86_64-w64-mingw32\lib\pkgconfig"

cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：`build\vplayer.exe`

## 运行

```powershell
# DLL 已由 CMake 自动拷贝到 build 目录，直接运行即可
.\build\vplayer.exe "视频文件.mp4"
```

## 功能

- [x] 播放本地视频（H.264/H.265/MPEG4 等任意 FFmpeg 支持的格式）
- [x] 音视频同步（音频为主时钟，纯视频用系统时钟）
- [x] 播放 / 暂停 / 停止
- [x] 进度拖动（可超过缓冲），音量调节 / 静音
- [x] 拖拽文件到窗口直接打开
- [x] 全屏切换
- [x] OSD：进度条、时间显示、音量提示

### 快捷键

| 按键 | 功能 |
|---|---|
| 空格 | 播放 / 暂停 |
| ← / → | 后退 / 前进 5 秒 |
| Ctrl+← / Ctrl+→ | 后退 / 前进 30 秒 |
| ↑ / ↓ | 音量 +10% / -10% |
| M | 静音 |
| F | 全屏 |
| Esc / Q | 退出 |
| 拖拽文件 | 打开并播放 |

## 开发计划（Roadmap）

- [x] M1 环境搭建（FFmpeg + SDL2 安装验证）
- [x] M2 骨架：构建系统 + 文档
- [x] M3 播放内核：解封装 → 解码 → 渲染 + 音频（含崩溃根因修复：跨堆 free）
- [x] M4 替换稳定版 FFmpeg 9.0.1（BtbN master 构建弃用）
- [ ] M5 硬解（DXVA2 / D3D11VA）
- [ ] M6 字幕（ASS/SRT）
- [ ] M7 播放列表与记忆播放位置
- [ ] M8 倍速播放