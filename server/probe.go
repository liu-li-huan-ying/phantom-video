package main

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

var (
	reDuration = regexp.MustCompile(`Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)`)
	reBitrate  = regexp.MustCompile(`bitrate:\s*(\d+)\s*kb/s`)
	reVideo    = regexp.MustCompile(`Video:\s*([^,\s]+)`)
	reAudio    = regexp.MustCompile(`Audio:\s*([^,\s]+)`)
	reSize     = regexp.MustCompile(`(\d{3,5})x(\d{3,5})`)
)

type ProbeInfo struct {
	Path        string `json:"path"`
	Title       string `json:"title"`
	DurationMs  int64  `json:"duration_ms"`
	VideoCodec  string `json:"video_codec"`
	AudioCodec  string `json:"audio_codec"`
	Width       int    `json:"width"`
	Height      int    `json:"height"`
	Bitrate     int    `json:"bitrate"`
	FormatBrief string `json:"format_brief"`
}

func probeFile(path string) ProbeInfo {
	info := ProbeInfo{Path: path}
	info.Title = filepath.Base(path)
	if ffmpeg == "" {
		return info
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, ffmpeg, "-hide_banner", "-i", path).CombinedOutput()
	if err != nil && ctx.Err() != nil {
		return info
	}
	stderr := string(out)
	if m := reDuration.FindStringSubmatch(stderr); m != nil {
		h, _ := strconv.Atoi(m[1])
		mi, _ := strconv.Atoi(m[2])
		s, _ := strconv.ParseFloat(m[3], 64)
		info.DurationMs = int64((h*3600+mi*60)*1000) + int64(s*1000)
	}
	if m := reBitrate.FindStringSubmatch(stderr); m != nil {
		info.Bitrate, _ = strconv.Atoi(m[1])
	}
	for _, line := range strings.Split(stderr, "\n") {
		if strings.Contains(line, "Video:") && info.VideoCodec == "" {
			if vm := reVideo.FindStringSubmatch(line); vm != nil {
				info.VideoCodec = vm[1]
			}
			if sm := reSize.FindStringSubmatch(line); sm != nil {
				info.Width, _ = strconv.Atoi(sm[1])
				info.Height, _ = strconv.Atoi(sm[2])
			}
		} else if strings.Contains(line, "Audio:") && info.AudioCodec == "" {
			if am := reAudio.FindStringSubmatch(line); am != nil {
				info.AudioCodec = am[1]
			}
		}
	}
	parts := []string{}
	if info.Width > 0 && info.Height > 0 {
		parts = append(parts, strconv.Itoa(info.Width)+"x"+strconv.Itoa(info.Height))
	}
	if info.VideoCodec != "" {
		parts = append(parts, strings.ToUpper(info.VideoCodec))
	}
	if info.AudioCodec != "" {
		parts = append(parts, strings.ToUpper(info.AudioCodec))
	}
	if info.Bitrate > 0 {
		parts = append(parts, strconv.Itoa(info.Bitrate)+" kbps")
	}
	info.FormatBrief = strings.Join(parts, " · ")
	return info
}

func handleProbe(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	if path == "" {
		http.Error(w, "missing path", http.StatusBadRequest)
		return
	}
	if _, err := os.Stat(path); err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	writeJSON(w, probeFile(path))
}

func writeJSON(w http.ResponseWriter, v interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	_ = json.NewEncoder(w).Encode(v)
}