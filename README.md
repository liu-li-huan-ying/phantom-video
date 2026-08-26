# 幻影视频 (Phantom Video)

自研 Windows 视频播放器 —— **libmpv 解码渲染（D3D11VA 零拷贝硬解）+ SDL2 UI 叠加层**，C++17 / Win32。

## 特性

- **零拷贝硬件解码**：mpv `gpu-next` + `d3d11` 上下文，`hwdec=auto-safe`
- **文件夹播放队列**：打开/拖入任意视频自动扫描同目录（14 种扩展名、按名排序、上限 2000）
- **进度记忆与续播**：每 3 秒落盘观看位置；`resume=1` 时重开自动跳转
- **三种播放模式**：Single / Loop / Shuffle
- **PIP 置顶迷你窗**：一键悬浮小窗，状态天然同步
- **字幕控制**：cc 图标开关 + C/X/Z 快捷键（显示/延迟 ±0.5s）
- **OSD 信息面板**：I 键查看 codec/分辨率/帧率/码率/音轨
- **列表缩略图**：后台逐项提取 + 磁盘缓存（7 天过期），二次打开零解码
- **拖拽排序**：列表面板内直接拖动调整顺序
- **完整 DPI 支持**：Per-Monitor V2，125%+ 缩放下像素精确、文字锐利
- **ULW 逐像素透明**：UpdateLayeredWindow + ARGB 纹理，半透明 UI 叠加层
- **i18n 双语**：中文 / English 一键切换
- **6 段音频均衡器 + 降噪**：运动插值、夜间模式、独占音频等高级选项

## 构建

### 环境要求

- [w64devkit](https://github.com/skeeto/w64devkit)（g++ MinGW）
- CMake ≥ 3.16
- 依赖已随仓库置于 `dev/`：SDL2 / SDL2_image / SDL2_ttf / FFmpeg 9.0.1 shared / libmpv / Sonic

### 编译

```powershell
# w64devkit 的 sh.exe 会干扰 MinGW Makefiles 生成器，构建前临时改名
# （将 <W64DEVKIT> 替换为你的 w64devkit 安装路径）
Rename-Item <W64DEVKIT>\bin\sh.exe sh.exe.bak
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
Rename-Item <W64DEVKIT>\bin\sh.exe.bak sh.exe
```

运行时 DLL 由 CMake 自动拷贝至 `build/`：

```powershell
build\phantom_video.exe <视频路径> [--debug]
```

## 使用

| 操作 | 说明 |
|------|------|
| 拖入文件 | 播放并扫描同目录入队 |
| 双击视频区 | 全屏切换 |
| 单击视频区 | 暂停/继续 |
| 滚轮 | 音量 ±5%（Toast 反馈） |

### 快捷键

| 键 | 功能 | 键 | 功能 |
|----|------|----|------|
| Space | 播放/暂停 | M | 静音 |
| ←/→ | seek ∓/±5s | N/P | seek ±10s |
| ↑/↓ | 音量 | F | 全屏 |
| `[` / `]` | 倍速 -/+0.25x | Ctrl+O | 打开文件 |
| C | 字幕开关 | I | OSD 信息 |
| X/Z | 字幕延迟 -/+0.5s | Esc | 关闭弹层 |

### 配置（phantom.ini）

```ini
volume=0.8        # 音量
speed=1           # 倍速
resume=0          # 1=启动续播上次进度
playmode=1        # 0=Single 1=Loop 2=Shuffle
subautoload=1     # 同名字幕自动加载 (fuzzy)
thumbcache=1      # 缩略图磁盘缓存
hwdecode=1        # 硬件解码
volnorm=0         # loudnorm 音量标准化
pos=x,y,w,h       # 窗口位置记忆
hist=<path>\t<秒>  # 每文件观看位置
lang=0            # 0=中文 1=English
```

设置面板内改动实时生效并立即持久化。

## 架构

```
幻影视频进程
├─ Win32 parent（输入处理 WndProc）
│   └─ mpv 子窗口 STATIC (--wid → D3D11VA)
│       └─ mpvRelayProc: 输入中继转发给 parent
└─ SDL2 overlay（owned 顶层窗, WS_EX_LAYERED+ULW per-pixel alpha+TOOLWINDOW）
    ├─ 逐像素 alpha 透明穿透显示视频
    ├─ GdiTextCache 文字 / svgicon 光栅化图标
    └─ ThumbWorker(FFmpeg) ──mutex──> 渲染线程惰性上传纹理
```

要点：
- overlay 是 owned 顶层窗口（非子窗口）——Windows 不支持 WS_EX_LAYERED 子窗口
- 所有鼠标消息经 mpvRelayProc 转发给 parent 统一处理
- 像素度量经 `S()` 按 DPI 缩放；文字 pt 由 GDI 内部换算，二者不可叠加

详细设计文档见 [docs/DESIGN.md](docs/DESIGN.md)。

## 许可证

MIT License
