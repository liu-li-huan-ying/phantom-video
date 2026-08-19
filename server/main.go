package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

//go:embed all:dist
var distFS embed.FS

var (
	token   string
	dataDir string
	ffmpeg  string
)

func initEnv() {
	token = randomToken()
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = os.TempDir()
	}
	dataDir = filepath.Join(dir, "VPlayer")
	_ = os.MkdirAll(dataDir, 0o755)
	ffmpeg = findFFmpeg()
}

func randomToken() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func findFFmpeg() string {
	candidates := []string{
		`C:\Users\31697\AppData\Roaming\Python\Python311\site-packages\imageio_ffmpeg\binaries\ffmpeg-win64-v4.2.2.exe`,
	}
	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	if exe, err := exec.LookPath("ffmpeg"); err == nil {
		return exe
	}
	return ""
}

func edgeExe() string {
	candidates := []string{
		`C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe`,
		`C:\Program Files\Microsoft\Edge\Application\msedge.exe`,
	}
	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	return ""
}

func launchEdge(url string) {
	exe := edgeExe()
	args := []string{"--app=" + url, "--window-size=1280,800"}
	if runtime.GOOS == "windows" {
		args = append(args, "--no-first-run", "--no-default-browser-check")
	}
	if exe == "" {
		log.Printf("Edge not found, open manually: %s", url)
		return
	}
	cmd := exec.Command(exe, args...)
	_ = cmd.Start()
}

func handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/media", handleMedia)
	mux.HandleFunc("/api/probe", handleProbe)
	mux.HandleFunc("/api/history", handleHistory)
	mux.HandleFunc("/api/playlist", handlePlaylist)
	mux.HandleFunc("/api/state", handleState)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		p := strings.TrimPrefix(r.URL.Path, "/")
		if p == "" {
			p = "index.html"
		}
		data, err := distFS.ReadFile("dist/" + p)
		if err != nil {
			http.NotFound(w, r)
			return
		}
		ct := "text/html; charset=utf-8"
		switch {
		case strings.HasSuffix(p, ".js"):
			ct = "application/javascript"
		case strings.HasSuffix(p, ".css"):
			ct = "text/css"
		case strings.HasSuffix(p, ".json"):
			ct = "application/json"
		case strings.HasSuffix(p, ".svg"):
			ct = "image/svg+xml"
		}
		w.Header().Set("Content-Type", ct)
		w.Write(data)
	})
	return withToken(mux)
}

func withToken(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/media" || strings.HasPrefix(r.URL.Path, "/api/") {
			if r.URL.Query().Get("token") != token {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
		}
		h.ServeHTTP(w, r)
	})
}

func main() {
	smoke := flag.Bool("smoke", false, "smoke test: start server, print URL, exit")
	flag.Parse()

	initEnv()

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatal(err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	base := fmt.Sprintf("http://127.0.0.1:%d/?token=%s", port, token)

	if *smoke {
		h := handler()
		_ = h
		fmt.Printf("SMOKE OK url=%s\n", base)
		return
	}

	log.Printf("VPlayer listening on %s", base)
	go func() {
		srv := &http.Server{Handler: handler()}
		_ = srv.Serve(ln)
	}()
	launchEdge(base)

	select {}
}