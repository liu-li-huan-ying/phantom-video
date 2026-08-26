# 幻影视频 设计文档

## 1. 总体架构

```
                 ┌─────────────────────────────────────────┐
                 │              main (UI 线程)              │
                 │  SDL 事件循环 / 快捷键 / OSD / 渲染       │
                 └──────────────┬──────────────────────────┘
                                │ 状态查询/控制指令
                                ▼
                 ┌─────────────────────────────────────────┐
                 │              Player (总控)               │
                 │  解码线程 / 同步策略 / seek / 生命周期    │
                 └──┬────────────┬────────────┬───────────┘
                    │            │            │
        ┌───────────▼──┐  ┌──────▼──────┐  ┌─▼──────────────┐
        │  Demuxer     │  │  Decoder    │  │  VideoQueue    │
        │  解封装/流表  │  │  音/视解码  │  │  (FramePtr)    │
        └───────────┬──┘  └──────┬──────┘  └───────┬────────┘
                    │            │                  │
              ┌─────▼─────┐  ┌───▼────┐   ┌────────▼────────┐
              │ AudioOutput│  │AudioQ  │   │  VideoRenderer  │
              │ SDL 回调   │  │(PCM)   │   │  SDL 纹理       │
              │ 音频时钟   │  └────────┘   └─────────────────┘
              └───────────┘
              时钟实现：Player::clock() = 音频时钟（writeHead_）
              或视频时钟（首帧 pts + 累计播放时长）回退
```

**线程模型（3 线程）：**

| 线程 | 职责 |
|---|---|
| 解码线程 | 循环 `av_read_frame` → 按流分发到视频/音频解码器 → 帧入队（有界队列满则阻塞）→ EOF 后发哨兵 |
| SDL 音频回调线程 | 从音频队列取 PCM 写入设备，同时推进音频时钟 |
| 主线程 | 事件循环、视频帧渲染与同步、OSD、控制指令 |

## 2. 数据流

### 2.1 视频

```
AVPacket ──avcodec_send_packet──▶ AVCodecContext ──avcodec_receive_frame──▶ AVFrame (YUV420P)
    │                                                                          │
    └── 解码线程内完成，仅帧指针入队                                              ▼
                                                                  VideoQueue<FramePtr>（FramePtr = shared_ptr<AVFrame>，makeFramePtr 构造）
                                                                          │
                                                    主线程取出，pts 对齐主时钟后：
                                                    SDL_UpdateYUVTexture(3 平面) → RenderCopy
```

- 视频纹理固定使用 `SDL_PIXELFORMAT_IYUV`（YUV420P 三平面直传，免 swscale 转 RGB，性能最优）。
- 窗口缩放：手动计算保持宽高比的 `SDL_Rect`（`SDL_RenderSetLogicalSize` 会拉伸变形，不用它）。

### 2.2 音频

```
AVFrame (FLTP/原始格式)
    │  swr_convert（swr_alloc + av_opt_set_*，目标 = SDL 实际打开的设备参数）
    ▼
AudioChunk { pts, 字节缓冲 }  ──▶  AudioQueue（按字节数限长，70560B ≈ 0.4 秒）
                                      │
                     SDL 回调 fill()：零填充 → 逐块拷入 → 推进音频时钟
```

- 设备请求：`S16 / 44100Hz / 2ch`，实际参数以 `SDL_OpenAudioDevice` 返回为准，swr 按实际参数转换。
- 音量在回调内用 `SDL_MixAudioFormat` 就地混合（预分配临时缓冲，回调内禁止动态分配）。
- 静音/暂停用 `SDL_PauseAudioDevice`，暂停期间时钟冻结。

## 3. 时钟与同步

### 3.1 主时钟 Clock

| 模式 | 取值 |
|---|---|
| 有音频流 | 音频时钟：`已消费字节数 / 每秒字节数 + 当前帧 pts`（AudioOutput 内部维护，回调内推进） |
| 纯视频 | 视频时钟：`首帧 pts + 累计播放时长`（主线程用 `SDL_GetPerformanceCounter` 累计，暂停不计） |

### 3.2 视频帧调度（主线程）

```
取出队首帧 F：
  F.pts <= 主时钟 + 0.05s  → 出队；F.pts 落后主时钟 > 0.05s → 直接丢弃（追帧，不重放）
  F.pts >  主时钟          → SDL_Delay(F.pts - 时钟)，再渲染
```

- 有音频时，音频队列空（解码线程被限流）前视频不会超前太多；音频尾部播完时若视频未完，切到视频时钟收尾。

### 3.3 拖动（Seek）

```
UI 请求 seek(target) → Player 置 m_seekReq
解码线程在安全点处理：
  1. 清空视频/音频队列（audio_ 额外清 current_）
  2. av_seek_frame(video 流, BACKWARD, target)  —— 保证跳到关键帧
  3. 两个解码器 avcodec_flush_buffers
  4. 置 m_dropUntil = target，随后解码线程丢弃 pts < target 的帧，直到赶上目标
  5. 音频时钟：不显式复位——seek 后新数据 pts 与旧写头偏差 >0.5s，fill() 自动重设写头
```

拖动期间视频渲染循环照常运行（队列空则等），UI 不阻塞。seek 精度受关键帧间隔限制（如 testdata 4.mp4 每 10 秒一个关键帧，seek(16.9) 会落到 10 秒处）。

## 4. 生命周期与状态机

```
IDLE ──打开文件──▶ DECODING ──首帧就绪──▶ PLAYING ⇄ 暂停: PAUSED
                    │                        │
                    └── EOF 后队列空──▶ ENDED（显示最后一帧）
                                          │ 空格 → seek(0) 重新播放
```

| 状态 | 含义 |
|---|---|
| IDLE | 未打开文件，显示黑屏 + 提示 |
| PLAYING | 正常播放 |
| PAUSED | SDL 暂停音频设备 + 视频时钟冻结，画面停留 |
| ENDED | 播放完毕，显示末帧，可重播 |

## 5. 模块说明

### 5.1 BlockingQueue\<T\>（core/blocking_queue.h）

- 互斥量 + 条件变量，`max` 上限（视频 8 帧 / 音频按字节 0.4s），满则生产阻塞。
- `close()` 后 push 失败、pop 排空返回 false（EOF 哨兵用 `shared_ptr` 空指针实现）。
- `reopen()`：重置 closed_ 并清空队列（**openFile 在 close() 后必须调用**，否则解码线程 push 立即失败——M5 修复）。
- `clear()` 供 seek 使用。

### 5.2 Demuxer

- `open(path)`：`avformat_open_input` + `avformat_find_stream_info`，探测最优视频流/音频流。
- `readPacket()`：返回 `shared_ptr<AVPacket>`，EOF 返回 nullptr。
- `seek(targetSec)`：按视频流时间基换算调用 `av_seek_frame`。

### 5.3 Decoder

- 统一封装 `VideoDecoder` / `AudioDecoder`：`open(codecpar)` → `send(pkt)` → `receive()`（receive 用 av_frame_move_ref 转移后经 makeFramePtr 持有，避免跨堆 free——见 AGENTS.md M4 教训）。
- 硬件解码（M7）：`open(codecpar, hwDeviceCtx)` 重载——`avcodec_get_hw_config` 检查 codec 是否支持设备类型，支持则设置 `ctx_->hw_device_ctx`；`receive()` 检测硬件帧（hw_frames_ctx）后 `av_hwframe_transfer_data` 转回软件帧（复制 pts/best_effort_timestamp），渲染层零改动。不支持的 codec 自动回退软解。
- 内部维护 `AVCodecContext`（线程安全由解码线程独占保证）。
- `flush()`：send(nullptr) 排空所有剩余帧（EOF 用）；`flushBuffers()`：`avcodec_flush_buffers`（seek 用）。

### 5.4 Player（总控）

- `openFile(path)`：创建 Demuxer/Decoder/AudioOutput/队列（close() 后必须 `videoQueue_.reopen()`），启动解码线程。
- `togglePause()` / `seek(double)` / `setVolume(double)` / `mute()`：线程安全指令。
- `clock()`：返回主时钟（供渲染同步）。
- 解码线程主循环（伪代码）：

```
while (true) {
    if (m_seekReq) 执行 seek 流程; continue
    pkt = demuxer->readPacket()          // EOF → nullptr
    if (!pkt) { flush 两个解码器; 队列 close; break }
    if (pkt->stream_index == videoIdx) {
        videoDecoder->send(pkt)
        while (frame = videoDecoder->receive()) {
            if (pts < m_dropUntil) continue        // seek 追赶
            videoQueue->push(frame)
        }
    } else if (audio) {
        audioDecoder->send(pkt)
        while (frame = audioDecoder->receive()) {
            if (pts < m_dropUntil) continue
            audioOutput->push(swr 转码后的 chunk)    // 内部按字节限流阻塞
        }
    }
}
```

### 5.5 AudioOutput

- `open(stream, spec)`：swr（av_opt_set_* 配置解码参数 → SDL 设备参数），打开设备。
- `push(AVFrame)`：swr 转换 → 组装 `AudioChunk` → 入队（超过 0.4s 阻塞）。
- 回调 `fill(stream, len)`：零填充 → 循环拷入 → 累加写头 pts → 更新时钟（检测到 pts 跳变 >0.5s 时自动重设写头，seek 后无需手动复位）。
- `resetClock()`：写头置 -1（备用，当前 seek 流程依赖 fill 自动重设）。

### 5.6 VideoRenderer

- 持有 `SDL_Renderer` 与 IYUV 纹理（纹理尺寸 = 视频帧尺寸，帧尺寸变化时重建）。
- `render(AVFrame*)`：`SDL_UpdateYUVTexture` + 按窗口尺寸等比缩放 + `RenderPresent`。
- `clear()`：黑屏。
- 字幕渲染（M8）：Windows GDI（CreateFontW 微软雅黑 + DrawTextW 黑描边白字 → DIB → SDL 纹理），底部居中，文本变化才重建（subtitleCache_ 缓存），零新增依赖。

### 5.7 字幕（subtitle/subtitle.h，M8）

- `SubtitleTrack`：事件列表（start/end/text），`loadSrt()`/`loadAss()`（去 BOM、归一 CRLF、去 ASS 标签），`textAt(t)` 二分查询。
- `SubtitleDecoder`：FFmpeg 字幕解码器（avcodec_decode_subtitle2）。**坑**：srt 解码器不设 AVSubtitle.pts（NOPTS）、start/end_display_time 恒 0，时间须取 packet pts × stream time_base；MKV 内嵌 srt 的 rect->ass 为 9 字段无 "Dialogue:" 前缀格式，需剥离前 9 字段取 Text。
- Player：内嵌字幕流在解码线程逐包解码入 SubtitleTrack；seek 时清空 + flush；外挂字幕 `loadExternalSubtitle()` 优先于内嵌。
- main.cpp：打开视频自动查找同名 .srt/.ass/.ssa/.sub 外挂字幕。

### 5.8 OSD（ui/osd.h）

- 进度条：底部 4px 高，已播部分亮色填充。
- 时间文本：内置 5x7 位图数字字体（'0'-'9' ':' '.'），白字黑边，预渲染到纹理。
- 音量条：拖动/按键时显示 2 秒后消失。
- 暂停图标：画面中央双竖条。

## 6. 关键约定与坑

1. **SDL main 宏**：`SDL_main.h` 会把 `main` 宏替换为 `SDL_main`，C++ 下会因符号修饰导致链接错误。本工程统一 `#define SDL_MAIN_HANDLED` 后写普通 `main`，链接时不带 `-lSDL2main`（保留控制台便于调试；发布版可换 `WinMain`）。
2. **FFmpeg 新版 API**：音频使用新版 `AVChannelLayout`（`ch_layout` 字段）；swr 用 `swr_alloc` + `av_opt_set_*`（`av_opt_set_chlayout`/`av_opt_set_sample_fmt`）配置，不再用旧 API。
3. **DLL 分发**：运行需要 FFmpeg / SDL2 / libmpv 的运行时 DLL，CMake 已自动拷贝到 build 目录；发布时需随 exe 分发。
4. **零第三方模块**：不使用 vcpkg/conan，依赖全部用 pkg-config 定位。
5. **磁盘约束**：构建产物一律放 build 目录，不装任何东西到系统盘。