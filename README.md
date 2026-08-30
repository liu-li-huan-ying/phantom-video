# 幻影视频 (Phantom Video)

自研 Windows 视频播放器 —— **libmpv 硬件加速解码 + Win32 ULW 透明 UI 覆盖层**，C++17 / Win32。

## 特性

### 播放核心

* **硬件加速解码**：mpv `gpu-next` + D3D11，`hwdec=auto-copy-safe`（默认禁用零拷贝），四级降级链自动兜底
* **四级 hwdec 降级链**：auto-copy-safe → auto-safe → d3d11va → no，同一文件最多重试 2 次
* **文件夹播放队列**：打开/拖入任意视频自动扫描同目录（25 种扩展名、自然排序、上限 2000）
* **进度记忆与续播**：每 3 秒落盘观看位置；`resume=1` 时重开自动跳转
* **三种播放模式**：Single / Loop / Shuffle
* **网络流支持**：Ctrl+U 输入 URL，支持 HTTP/RTMP 等，自动缓冲（80MiB 预读 + 30s 缓存）
* **章节导航**：G 键下一章节，章节菜单快速跳转

### 字幕 & 音轨

* **字幕控制**：cc 图标开关 + C/X/Z 快捷键（显示/延迟 ±0.5s）+ B 键字幕位置循环
* **外部字幕加载**：Shift+S 加载外部字幕文件，菜单「加载外部字幕...」
* **音轨选择**：V 键切换音轨，音轨菜单选择
* **字幕轨道选择**：字幕菜单选择

### UI 交互

* **PIP 置顶迷你窗**：一键悬浮小窗，状态天然同步
* **OSD 信息面板**：I 键查看 codec/分辨率/帧率/码率/音轨/hwdec 路径
* **列表缩略图**：后台逐项提取 + 磁盘缓存（7 天过期），二次打开零解码
* **拖拽排序**：列表面板内直接拖动调整顺序
* **多文件拖拽**：拖入多个文件自动加入播放列表
* **播放列表管理**：Insert 添加、Delete 移除、L 键切换显示
* **快捷键覆盖层**：按 `?` 显示所有快捷键

### 音频

* **6 段音频均衡器**：60Hz/170Hz/310Hz/600Hz/3kHz/12kHz，5 种预设（平坦/低音/高音/人声/摇滚）
* **多声道输出**：立体声 / 5.1 环绕 / 7.1 环绕 / 直通（Passthrough 到功放）
* **高清音频直通**：DTS / Dolby Digital / TrueHD / DTS-HD MA 源码输出
* **夜间模式**：acompressor 动态压缩，避免广告突然炸耳
* **WASAPI 独占输出**：减少延迟和重采样（默认关闭，设置面板可开启）

### 画面

* **完整 DPI 支持**：Per-Monitor V2，125%+ 缩放下像素精确、文字锐利
* **ULW 逐像素透明**：UpdateLayeredWindow + ARGB 纹理，半透明 UI 叠加层
* **画面比例切换**：R 键循环 auto → 16:9 → 4:3 → 1:1
* **运动插值**：display-resample + oversample 去 judder
* **高质量缩放**：ewa\_lanczossharp（GPU 开销较高）
* **去色带**：D 键四级切换（关/轻/中/强）

### 其他

* **i18n 双语**：中文 / English 一键切换
* **AB 循环**：A 键设置 A→B→清除三态循环
* **截图**：PrintScreen 键截图到 screenshots/
* **播放历史管理**：Ctrl+Shift+Delete 清除全部历史
* **音量点击拖拽**：hover 高亮，点击调节，不再路过误改

## 构建

### 环境要求

* [w64devkit](https://github.com/skeeto/w64devkit)（g++ MinGW）
* CMake ≥ 3.16
* 依赖已随仓库置于 `dev/`：SDL2 / FFmpeg 9.0.1 shared / libmpv / Sonic

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
build\phantom_video.exe <视频路径>
```

## 使用

| 操作     | 说明               |
| ------ | ---------------- |
| 拖入文件   | 播放并扫描同目录入队       |
| 拖入多个文件 | 全部加入播放列表         |
| 双击视频区  | 全屏切换             |
| 单击视频区  | 暂停/继续            |
| 滚轮     | 音量 ±5%（Toast 反馈） |

### 快捷键

| 键              | 功能           | 键           | 功能      |
| -------------- | ------------ | ----------- | ------- |
| Space          | 播放/暂停        | F           | 全屏      |
| ←/→            | seek -/+5s   | M           | 静音      |
| N/P            | seek -/+10s  | H/J         | 上一曲/下一曲 |
| ↑/↓            | 音量 +/-5%     | L           | 播放列表开关  |
| `[` / `]`      | 倍速 -/+0.25x  | R           | 画面比例循环  |
| A              | AB 循环        | D           | 去色带等级   |
| C              | 字幕开关         | E           | 均衡器开关   |
| X/Z            | 字幕延迟 -/+0.5s | B           | 字幕位置循环  |
| V              | 音轨切换         | G           | 下一章节    |
| S              | 加载外部字幕       | I           | OSD 信息  |
| ?              | 快捷键覆盖层       | PrintScreen | 截图      |
| Ctrl+O         | 打开文件         | Ctrl+U      | 打开 URL  |
| Insert         | 添加到列表        | Delete      | 从列表移除   |
| Ctrl+Shift+Del | 清除播放历史       | Esc         | 关闭弹层    |

### 配置（phantom.ini）

```ini
volume=0.8          # 音量
speed=1             # 倍速
resume=0            # 1=启动续播上次进度
playmode=1          # 0=Single 1=Loop 2=Shuffle
subautoload=1       # 同名字幕自动加载 (fuzzy)
thumbcache=1        # 缩略图磁盘缓存
hwdecode=1          # 硬件解码
zerocopy=0          # D3D11VA 零拷贝 (默认关, 有驱动风险)
volnorm=0           # 音量归一化 (已停用, 保留配置兼容)
night=0             # 夜间模式 (acompressor)
exclusive=0         # WASAPI 独占输出 (默认关)
audioout=0          # 音频输出: 0=立体声 1=5.1 2=7.1 3=直通 (需重启生效)
interp=0            # 运动插值 (display-resample)
hiq=0               # 高质量缩放 (ewa_lanczossharp)
pos=x,y,w,h         # 窗口位置记忆
hist=<path>\t<秒>    # 每文件观看位置
lang=0              # 0=中文 1=English
```

设置面板内改动实时生效并立即持久化。

## 架构

```
幻影视频进程
├─ Win32 parent（输入处理 parentProc）
│   └─ mpv 子窗口 STATIC (--wid → mpv 内部 D3D11 device + swapchain)
│       └─ mpvRelayProc: 输入中继转发给 parent
└─ ULW 分层覆盖窗（owned 顶层, WS_EX_LAYERED + per-pixel alpha）
    ├─ SDL2 软件渲染器: UI 元素（进度条/按钮/列表/面板）
    ├─ GdiTextCache 文字 / svgicon 光栅化图标
    └─ ThumbWorker(FFmpeg) ──mutex──> 渲染线程惰性上传纹理
```

要点：

* overlay 是 owned 顶层窗口（非子窗口）——Windows 不支持 WS\_EX\_LAYERED 子窗口
* 所有鼠标消息经 mpvRelayProc 转发给 parent 统一处理
* 像素度量经 `U(v)` 按 `g_uiBase × DPI` 缩放；文字 pt 由 `Tpt(pt)` 换算

详细设计文档见 [docs/DESIGN.md](docs/DESIGN.md)。

## 许可证

GPL-3.0。依赖许可明细：

| 组件           | 许可                            | 链接方式        |
| ------------ | ----------------------------- | ----------- |
| libmpv       | LGPL-2.1+                     | 动态链接        |
| FFmpeg 9.0.1 | GPL-3.0 (gyan.dev full build) | 动态链接        |
| SDL2         | Zlib                          | 动态链接        |
| Sonic        | LGPL                          | 静态链接 (源码内嵌) |

FFmpeg 的 GPL 组件（x264/x265）要求本项目以 GPL-3.0 发行。
如需 LGPL-only 分发，须替换为 LGPL 编译的 FFmpeg build。

## 计划功能

| 功能 | 状态 | 说明 |
|------|------|------|
| 画面调节（亮度/对比度/饱和度/锐化/降噪） | 未实现 | mpv `--vf=eq` 支持 |
| 去隔行实时调整 | 未实现 | mpv `--vf=nnedi` 支持 |
| SVP 倍帧/插帧 | 未实现 | 需 VapourSynth 集成，复杂度高 |
| AI 超分辨率 | 未实现 | 需 GPU 推理引擎，复杂度极高 |
| 色彩空间精确映射 | 未实现 | mpv tone-mapping 控制 |
