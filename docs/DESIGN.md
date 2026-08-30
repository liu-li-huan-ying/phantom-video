# 幻影视频 设计文档

## 1. 总体架构

```
                 ┌─────────────────────────────────────────┐
                 │          main (UI 线程 + 消息循环)        │
                 │  Win32 PeekMessageW + MsgWaitForMultiple │
                 │  SDL 事件 / 快捷键 / OSD / 渲染            │
                 └──────────────┬──────────────────────────┘
                                │ 控制指令
                                ▼
                 ┌─────────────────────────────────────────┐
                 │            MpvBackend (libmpv)           │
                 │  mpv_set_property / mpv_command          │
                 │  mpv_observe_property → 事件回调          │
                 └──┬────────────┬────────────┬───────────┘
                    │            │            │
        ┌───────────▼──┐  ┌──────▼──────┐  ┌─▼──────────────┐
        │  mpv demux   │  │  mpv decode │  │  mpv D3D11     │
        │  解封装/流表  │  │  音/视解码  │  │  渲染到子窗口   │
        └───────────┬──┘  └──────┬──────┘  └───────┬────────┘
                    │            │                  │
              ┌─────▼─────┐  ┌───▼────┐   ┌────────▼────────┐
              │ WASAPI ao  │  │ Sonic  │  │  mpv 子窗口      │
              │ 音频输出   │  │ 变速   │  │  (STATIC HWND)   │
              │ 音频时钟   │  └────────┘  │  WM_PAINT → 黑   │
              └───────────┘              └─────────────────┘
```

**关键设计决策：使用 libmpv 而非自研解码管线**

- libmpv 封装了 FFmpeg 解封装/解码 + D3D11 渲染 + WASAPI 音频输出
- 播放器只需通过 `mpv_set_property` / `mpv_command` 控制 mpv
- 通过 `mpv_observe_property` 监听状态变化（音量/速度/hwdec/EOF 等）

**线程模型（3 线程）：**

| 线程 | 职责 |
|---|---|
| mpv 解码/渲染线程 | mpv 内部管理：解封装 → 解码 → D3D11 渲染到子窗口 |
| mpv 事件回调线程 | 处理 `mpv_wait_event`，更新状态，触发 `onFileLoaded`/`onPlaybackEnded` |
| Win32 主线程 | 消息循环、SDL overlay 渲染、快捷键处理、UI 状态管理 |

## 2. 窗口层次

```
┌─────────────────────────────────────────────┐
│  Win32 Parent Window (WndProc)              │
│  ┌──────────────────────────────────────┐   │
│  │  mpv 子窗口 STATIC (g_mpvHwnd)       │   │
│  │  --wid → mpv D3D11 渲染              │   │
│  │  mpvRelayProc 拦截输入转发给 parent   │   │
│  └──────────────────────────────────────┘   │
│  ┌──────────────────────────────────────┐   │
│  │  SDL2 Overlay Window (g_overlayHwnd) │   │
│  │  WS_EX_LAYERED | WS_EX_TRANSPARENT  │   │
│  │  ULW per-pixel alpha                 │   │
│  │  UI 渲染：进度条/按钮/列表/面板       │   │
│  └──────────────────────────────────────┘   │
└─────────────────────────────────────────────┘
```

- Parent：Win32 窗口，拥有 mpv 子窗口和 SDL overlay
- mpv 子窗口：STATIC 类，`--wid` 传给 mpv，D3D11 直接渲染到此窗口
- Overlay：SDL2 创建的 owned 顶层窗口，`WS_EX_LAYERED` + ULW 逐像素 alpha
- Overlay 是 `WS_EX_TRANSPARENT`：鼠标事件穿透到 mpv 子窗口/parent

## 3. 渲染管线

### 3.1 视频渲染（mpv 内部）

mpv 使用 `vo=gpu-next` + `gpu-context=d3d11`：
1. 解码线程：`av_read_frame` → `avcodec_send_packet` → `avcodec_receive_frame`
2. D3D11 渲染：mpv 内部将 YUV 帧上传到 GPU，着色器处理后 present 到 swap chain
3. 硬件解码降级链：auto-copy-safe → auto-safe → d3d11va → no

### 3.2 UI 渲染（SDL2 overlay）

```
renderOverlay()
    ├─ ovTexEnsure(w, h)          创建/重建 ARGB 纹理
    ├─ SDL_SetRenderTarget(ovTex) 切换到离屏渲染
    ├─ SDL_RenderClear(transparent) 全透明清屏
    ├─ 绘制 UI 元素               进度条/按钮/列表/面板
    ├─ overlayPresent()
    │   ├─ SDL_RenderReadPixels    读回 ARGB 像素
    │   ├─ 圆角遮罩处理            g_roundMasks
    │   ├─ 预乘 alpha              ULW 要求 premultiplied BGRA
    │   └─ UpdateLayeredWindow     ULW_ALPHA 逐像素透明
    └─ g_dirty 标记               仅脏区重绘
```

## 4. 输入处理

### 4.1 消息流

```
鼠标/键盘事件
    ↓
mpv 子窗口 (mpvRelayProc)
    ├─ WM_ERASEBKGND → FillRect(BLACK) 阻止灰色背景
    ├─ WM_PAINT → FillRect(BLACK) 阻止 STATIC 默认灰色填充
    ├─ 鼠标/键盘消息 → SendMessageW(parent) 转发
    └─ 其他消息 → CallWindowProc(oldProc) 交给 mpv
    ↓
Parent 窗口 (parentProc)
    ├─ 视频区点击 → mpv pause/unpause
    ├─ 控制栏区域 → overlay 处理
    └─ 键盘快捷键 → 对应操作
```

### 4.2 Overlay 穿透

Overlay 是 `WS_EX_TRANSPARENT`：
- 鼠标事件穿透到下层窗口（mpv 子窗口 / parent）
- Overlay 的 UI 元素通过 hit-test 在 parentProc 中处理
- 视频区：overlay → mpv 子窗口 → parent
- 控制栏：overlay → parent 直接处理

## 5. 状态管理

### 5.1 全局状态

| 变量 | 类型 | 说明 |
|------|------|------|
| `g_cfg` | `AppConfig` | 持久化配置（音量/速度/窗口位置/历史等） |
| `g_ui` | `UiState` | UI 状态（菜单开关/拖拽/动画等） |
| `g_mpv` | `MpvBackend*` | mpv 后端实例 |
| `g_dirty` | `atomic<bool>` | overlay 重绘标记 |

### 5.2 配置持久化

`phantom.ini` 由 `config.cpp` 读写，包含：
- 播放状态：volume, speed, resume, playmode
- 功能开关：hwdecode, volnorm, night, exclusive, audioout
- 窗口位置：pos=x,y,w,h
- 播放历史：hist=<path>\t<秒>（最多 500 条）

## 6. 文件结构

```
src/
├── main.cpp              主循环、配置、mpv 初始化
├── app/
│   ├── app_state.h       全局状态、缩放函数、i18n
├── core/
│   ├── mpv_backend.h/cpp mpv 后端封装
│   ├── config.h/cpp      配置读写
│   ├── logger.h/cpp      统一日志
│   └── thumbnail_extractor.h/cpp  缩略图提取
└── ui/
    ├── wndproc.h/cpp     Win32 窗口过程
    ├── render_overlay.h/cpp  SDL overlay 渲染
    ├── gdi_text.h/cpp    GDI 文字渲染
    ├── svgicon.h/cpp     SVG 图标光栅化
    ├── helpers.h/cpp     通用工具函数
    ├── dialogs.h/cpp     Win32 对话框
    ├── ulw.h             UpdateLayeredWindow 封装
    └── primitives.h/cpp  绘图基元
```
