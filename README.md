# VPlayer — 轻量级 Windows 视频播放器

个人自用的功能齐全、轻量级 Windows 视频播放器。

- **技术栈**：Go 1.24.5（后端，零第三方依赖）+ TypeScript（前端）+ 系统 Edge（播放内核）
- **平台**：Windows 10/11（依赖 Edge 自带 H.264/AAC 专有解码器）
- **构建**：单二进制 `vplayer.exe`（前端由 `go:embed` 打包进二进制）

## 快速开始

```powershell
go build -o vplayer.exe ./server
npx tsc -p web/tsconfig.json
.\vplayer.exe
```

运行后自动启动本地 HTTP 服务并拉起 Edge（`--app=` 无地址栏窗口）。

## 文档

- `开发文档.md` — 架构、技术决策、功能规划、依赖清单（唯一权威文档）
- `AGENTS.md` — 开发强制规则（AI 协作约定）

## 技术背景

| 组件 | 说明 |
|---|---|
| Go 1.24.5 | 本地 HTTP 服务：媒体 Range 直读、历史/播放列表/配置持久化、ffmpeg 元数据解析 |
| TypeScript + 原生 CSS | 播放器 UI（控制条/快捷键/拖拽/断点续播/播放列表） |
| Edge（系统浏览器） | 唯一播放内核，`--app=` 窗口模式；自带 H.264/AAC 专有解码器，无转码 |
| ffmpeg.exe（只读调用） | 仅作 probe 元数据（时长/分辨率/编码），不参与播放 |

> 架构沿革：PyQt5 + QtMultimedia 因本机 WMF 视频管线原生崩溃（0xC0000409）弃用；
> QtWebEngine 因 conda 构建缺专有解码器弃用；Edge 原生解码为最终方案（详见开发文档.md）。