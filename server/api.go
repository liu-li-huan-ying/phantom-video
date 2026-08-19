package main

import (
	"encoding/json"
	"net/http"
	"sort"
	"time"
)

func handleHistory(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, loadHistory())
	case http.MethodPost:
		var e HistoryEntry
		if err := json.NewDecoder(r.Body).Decode(&e); err != nil || e.Path == "" {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		h := loadHistory()
		keep := h[:0]
		for _, x := range h {
			if x.Path != e.Path {
				keep = append(keep, x)
			}
		}
		e.LastPlayed = time.Now().Unix()
		keep = append(keep, e)
		sort.Slice(keep, func(i, j int) bool { return keep[i].LastPlayed > keep[j].LastPlayed })
		if len(keep) > 200 {
			keep = keep[:200]
		}
		if err := writeJSONAtomic("history.json", keep); err != nil {
			http.Error(w, "write failed", http.StatusInternalServerError)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func handlePlaylist(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, loadPlaylist())
	case http.MethodPost:
		var p []PlaylistEntry
		if err := json.NewDecoder(r.Body).Decode(&p); err != nil {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		if err := writeJSONAtomic("playlist.json", p); err != nil {
			http.Error(w, "write failed", http.StatusInternalServerError)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func handleState(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, loadState())
	case http.MethodPost:
		var s AppState
		if err := json.NewDecoder(r.Body).Decode(&s); err != nil {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		if s.ResumeMode == "" {
			s.ResumeMode = "ask"
		}
		if err := writeJSONAtomic("state.json", s); err != nil {
			http.Error(w, "write failed", http.StatusInternalServerError)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}