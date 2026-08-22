# M30 开发计划：控件动画 + 缩略图优化 + OSD 增强

## 一、控件栏动画（缓动系统）

### 目标
hover / 点击 / 展开 / 收起 时有平滑过渡动画，不依赖 SDL3。

### 方案
1. **新建 `src/ui/easing.h`**：纯头文件缓动函数库
   - 线性、ease-in、ease-out、ease-in-out、弹性（overshoot）
   - `float ease(float t, EaseFunc func)` — t ∈ [0,1]，返回 [0,1] 或 overshoot
   - 零依赖，纯数学

2. **控件 alpha 动画**（`video_renderer.h` 新增成员）：
   ```
   float controlsAlpha_ = 1.0f;      // 当前 alpha（0~1 浮点）
   float controlsAlphaTarget_ = 1.0f; // 目标 alpha
   Uint32 controlsFadeStart_ = 0;     // 动画起始时间
   static constexpr int kFadeMs = 200; // 过渡时长
   ```
   - 每帧计算：`elapsed = now - fadeStart; t = min(elapsed / kFadeMs, 1.0); controlsAlpha_ = lerp(from, to, ease(t))`
   - hover 时 target=1.0，鼠标离开时 target=0.0（延迟隐藏）

3. **按钮 hover 缩放**：
   - hover 时 scale 从 1.0 → 1.15，200ms ease-out
   - 点击时 scale 从 1.0 → 0.9 → 1.0，150ms ease-out（弹性回弹）

4. **进度条展开/收起**：
   - hover 进度条时高度从 4px → 8px，thumb 从 12px → 18px
   - 收起时反向，200ms ease-out

### 改动文件
- `src/ui/easing.h`（新建）
- `src/video/video_renderer.h` — 新增动画状态成员
- `src/video/video_renderer.cpp` — drawControls() 中应用动画值
- `src/main.cpp` — onMouseMove 时触发 hover 动画

---

## 二、缩略图性能优化

### 目标
- LRU 缓存：避免重复 seek 提取同一时间点缩略图
- 多线程提取：不阻塞主循环（当前 `thumbnail.getFrame()` 是同步调用）

### 方案

#### 2.1 LRU 缓存
```
新建 src/core/thumbnail_cache.h:
  struct CacheEntry { SDL_Texture* tex; int w, h; double time; Uint32 lastAccess; };
  class ThumbnailCache {
    std::deque<CacheEntry> entries_;  // 最近使用在前
    int maxEntries_ = 30;
  public:
    SDL_Texture* get(double time, int toleranceMs = 500);
    void put(double time, SDL_Texture* tex, int w, int h);
    void evict();  // 删除最旧条目
    void clear();  // 切视频时清空
  };
```

- `get(time)`：遍历找 `abs(entry.time - time) < tolerance` → 命中则更新 lastAccess 并返回
- `put(time, tex)`：插入新条目，超过 maxEntries_ 时 evict 尾部
- 切视频时调 `clear()` 释放所有纹理

#### 2.2 多线程提取
```
新建 src/core/thumbnail_worker.h:
  class ThumbnailWorker {
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    struct Request { double time; int seq; };
    std::deque<Request> pending_;
    int currentSeq_ = 0;
  public:
    void start(ThumbnailExtractor* extractor, ThumbnailCache* cache);
    void request(double time);  // 主线程调用，入队
    void cancel();              // 取消当前请求
    // 线程函数：循环 wait → 提取 → cache.put()
  };
```

- 主线程 `onMouseMove` 时调 `worker.request(targetTime)`
- worker 线程消费请求，调 `extractor.getFrame()` 提取 → `cache.put()` 存入
- 主线程渲染时调 `cache.get()` 获取纹理（命中则直接用，未命中则显示上一帧）
- `cancel()` 在新请求入队时自动取消旧请求（通过 seq 号丢弃）

### 改动文件
- `src/core/thumbnail_cache.h`（新建）
- `src/core/thumbnail_worker.h`（新建）
- `src/core/thumbnail_extractor.h` — 线程安全封装
- `src/main.cpp` — 替换同步调用为 worker.request + cache.get
- `src/video/video_renderer.cpp` — setThumbnail 改为从 cache 获取

---

## 三、OSD 信息增强

### 目标
在画面上显示：当前码率、分辨率、帧率、音频采样率、硬解状态。

### 方案

#### 3.1 数据采集（`player.h` 新增查询接口）
```cpp
// 新增接口
int videoBitrate() const;      // kbps
int audioBitrate() const;      // kbps
int videoWidth() const;        // 像素
int videoHeight() const;       // 像素
float videoFps() const;        // 帧率
int audioSampleRate() const;   // Hz
bool hwDecoding() const;       // 硬解状态
```

- 从 `AVCodecParameters` 和 `AVStream` 读取，openFile() 时缓存
- 码率从 `bit_rate` 读取（kbps = bit_rate / 1000）
- 帧率从 `avg_frame_rate` 计算（`av_q2d()`）
- 硬解状态从 `hwDecode_` 原子量读取

#### 3.2 OSD 显示（`video_renderer.cpp`）
- 按 `I` 键切换 OSD 显示/隐藏（`bool osdInfoVisible_`）
- 显示格式（左上角半透明背景）：
  ```
  1920×1080 | 23.976fps | 码率: 5120kbps
  AAC 44100Hz | H/W: No
  ```
- 字体用现有 `drawFontText()`，scale=1.5
- 背景：半透明黑色圆角矩形

#### 3.3 快捷键
- `I`：切换 OSD 信息显示
- `Shift+I`：切换详细模式（含解码器名称、profile/level）

### 改动文件
- `src/core/player.h` + `player.cpp` — 新增查询接口 + 缓存 AVCodecParameters
- `src/video/video_renderer.h` — 新增 osdInfoVisible_ 成员
- `src/video/video_renderer.cpp` — drawOsdInfo() 新函数
- `src/main.cpp` — I 键绑定 + RenderStats 传递

---

## 实施顺序

1. **阶段 M30a**：控件动画（easing + alpha 动画 + 按钮缩放 + 进度条展开）
2. **阶段 M30b**：缩略图 LRU 缓存 + 多线程 worker
3. **阶段 M30c**：OSD 信息增强（player 查询接口 + OSD 渲染 + I 键绑定）
