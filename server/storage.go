package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
)

var storeMu sync.Mutex

type HistoryEntry struct {
	Path       string `json:"path"`
	Title      string `json:"title"`
	PositionMs int64  `json:"position_ms"`
	DurationMs int64  `json:"duration_ms"`
	LastPlayed int64  `json:"last_played"`
}

type PlaylistEntry struct {
	Path  string `json:"path"`
	Title string `json:"title"`
}

type AppState struct {
	ResumeMode      string  `json:"resume_mode"` // ask / auto / never
	OpenLastOnStart bool    `json:"open_last_on_start"`
	Volume          int     `json:"volume"`
	Rate            float64 `json:"rate"`
}

func dataPath(name string) string {
	return filepath.Join(dataDir, name)
}

func readJSON(name string, v interface{}) {
	storeMu.Lock()
	defer storeMu.Unlock()
	data, err := os.ReadFile(dataPath(name))
	if err != nil {
		return
	}
	_ = json.Unmarshal(data, v)
}

func writeJSONAtomic(name string, v interface{}) error {
	storeMu.Lock()
	defer storeMu.Unlock()
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	tmp := dataPath(name + ".tmp")
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, dataPath(name))
}

func loadHistory() []HistoryEntry {
	var h []HistoryEntry
	readJSON("history.json", &h)
	return h
}

func loadPlaylist() []PlaylistEntry {
	var p []PlaylistEntry
	readJSON("playlist.json", &p)
	return p
}

func loadState() AppState {
	s := AppState{ResumeMode: "ask", OpenLastOnStart: true, Volume: 80, Rate: 1.0}
	readJSON("state.json", &s)
	return s
}