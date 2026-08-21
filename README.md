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
├── AGENTS.md               # 开发约束与已知问题备忘
├── docs\
│   ├── DESIGN.md           # 详细设计文档
│   └── DEVELOPMENT_LOG.md  # 阶段开发日志（困难与解决）
├── src\
│   ├── main.cpp            # 入口 + SDL 事件循环 + 快捷键
│   ├── core\               # 播放内核（与 UI 无关）
│   │   ├── blocking_queue.h    # 线程安全有界队列（支持 close/reopen/clear）
│   │   ├── types.h             # FramePtr/PacketPtr 工厂（makeFramePtr 等）
│   │   ├── demuxer.h/.cpp      # 解封装
│   │   ├── decoder.h/.cpp      # 音视频解码器封装
│   │   └── player.h/.cpp       # 播放器总控（解码线程 + 时钟 + seek）
│   ├── audio\audio_output.h/.cpp   # SDL 音频输出 + 重采样 + 音频时钟
│   ├── video\video_renderer.h/.cpp # SDL 视频渲染（YUV 纹理）
│   └── ui\osd.h/.cpp            # 进度条/时间/音量 OSD 绘制
├── testdata\               # 冒烟测试素材（多格式/损坏文件）
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

## 功能（M5/M6 已通过 API 自动化测试 + GUI 事件自动化验证 + 压力测试）

- [x] 播放本地视频（H.264/H.265/MPEG4/VP9 等任意 FFmpeg 支持的格式）
- [x] 音视频同步（音频为主时钟，纯视频用系统时钟）
- [x] 播放 / 暂停 / 停止
- [x] 进度拖动（精确到关键帧），音量调节 / 静音
- [x] 拖拽文件到窗口直接打开
- [x] 全屏切换
- [x] OSD：进度条、时间显示、音量提示
- [x] 异常文件容错（截断/损坏文件打开失败不崩溃）
- [x] 播放列表（命令行多文件 / N / P / 播完自动下一曲）+ 记忆播放位置（vplayer.ini）
- [x] 自动播放列表（打开文件自动扫描同目录视频，单独/循环/随机三种播放模式）
- [x] 播放速度（S/L 0.5x~2.0x，控件栏显示倍速）
- [x] 硬件解码（D3D11VA/DXVA2，H.264/MPEG-2 硬解，不支持自动回退软解）
- [x] 字幕（外挂 SRT/ASS 自动加载 + 内嵌字幕流解码，底部 GDI 渲染）
- [x] 关键帧预览（进度条 hover 显示视频缩略图，独立 FFmpeg 解码）
- [x] 播放列表自然排序（"1,2,...,10" 而非 "1,10,2"）
- [x] SWF/MKV/FLV 等格式适配（duration 推算、seek 兼容）
- [x] 音量标准化（峰值检测 + 软限幅，A 键切换）
- [x] 播放列表面板（右侧可切换面板，格式彩色图标，点击选曲，宽度可调）
- [x] 视频区域单击暂停/继续

### 快捷键

| 按键 | 功能 |
|---|---|
| 空格 | 播放 / 暂停 |
| ← / → | 后退 / 前进 5 秒 |
| Ctrl+← / Ctrl+→ | 后退 / 前进 30 秒 |
| ↑ / ↓ | 音量 +10% / -10% |
| M | 静音 |
| F | 全屏 |
| N / P | 下一曲 / 上一曲 |
| X | 播放模式：单独 → 循环 → 随机 |
| S / L | 减速 / 加速（0.5x~2.0x 循环） |
| Esc / Q | 退出 |
| 拖拽文件 | 打开并播放 |

## 开发计划（Roadmap）

- [x] M1 环境搭建（FFmpeg 9.0.1 + SDL2 2.32.10 安装验证）
- [x] M2 骨架：构建系统 + 文档
- [x] M3 播放内核：解封装 → 解码 → 渲染 + 音频（期间定位跨堆 free 崩溃）
- [x] M4 替换稳定版 FFmpeg 9.0.1 + 根因修复（shared_ptr 跨堆 free 0xC0000374）
- [x] M5 全功能验证：API 自动化测试 20/20、多格式冒烟、修复 BlockingQueue 永久 closed bug
- [x] M6 GUI 交互验证（全屏/拖拽/快捷键）+ 120 秒随机事件压力测试
- [x] M7 硬解（DXVA2 / D3D11VA）
- [x] M8 字幕（外挂 SRT/ASS + 内嵌字幕流）
- [x] M9 播放列表与记忆播放位置
- [x] M10 倍速播放（S/L 0.5x~2.0x）
- [x] M11 现代化 UI 外观 ✅ v0.3 正式版（矢量图标 / 圆角渐变控件栏 / 进度条 hover / 音量弹层拖动 / 淡入淡出自动隐藏）
- [x] M12 自动播放列表 ✅（打开文件自动扫描同目录视频建列表，X 键切换单独/循环/随机播放模式，标题栏显示序号）
- [x] M13 长视频 seek 卡顿修复 ✅（解码线程 tryPush 轮询防 seek 饿死、seek 后音频时钟同步目标 + 画面首帧立即恢复、close 死锁修复；3.5 小时/1.4GB 视频 seek 8~300ms 恢复，3198 文件目录 140ms 扫描）
- [x] M14 YouTube 样式界面 ✅（DWM 窗口阴影+圆角、自定义标题栏 vplay.bmp logo + 滚动字幕、深色主题背景、控件 hover 动画/发光进度条）
- [x] M15 关键帧预览 ✅ + 音量标准化 ✅（进度条 hover 缩略图、多格式 seek 兼容、自然排序、峰值检测+软限幅音量标准化，A 键切换）
- [x] M16 播放列表面板 ✅（右侧可切换面板，22种格式彩色图标，GDI文字渲染，点击选曲，宽度可调）
- [ ] M17 Seek 性能优化（首帧立即显示、150ms debounce、轮询优化）
- [ ] M18 音频同步 + 可靠性（音频竞态修复、时钟 reset、Seeking 指示器）