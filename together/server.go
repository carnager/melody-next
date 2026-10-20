// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode"
)

// ADR-0302: what a melody-together server answers -- plain HTTP, for the web
// proxy in front of it to give TLS.
//
//	GET    /time                          the server's clock, in ms
//	POST   /rooms                         a room          (the server's password)
//	PUT    /rooms/ROOM/state              what plays      (the host key)
//	GET    /rooms/ROOM/state?after=N&name=WHO&wait=MS
//	                                      the next state  (the invite, or the host key),
//	                                      waited for MS at most (25 s)
//	POST   /rooms/ROOM/leave?name=WHO     a listener goes (the invite)
//	DELETE /rooms/ROOM                    the room closes (the host key)
//
// Each key travels as "Authorization: Bearer KEY"; every answer is JSON.

// A state is a track's description; anything near this is not one.
const maximumState = 256 << 10

func serverMs() int64 { return time.Now().UnixMilli() }

func answer(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}

func refuse(w http.ResponseWriter, status int, message string) {
	answer(w, status, map[string]string{"error": message})
}

func bearer(r *http.Request) string {
	return strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
}

// A listener's name as the others see it: printable, and short.
func listenerName(r *http.Request) (string, bool) {
	name := r.URL.Query().Get("name")
	if len(name) > 64 || strings.IndexFunc(name, func(c rune) bool { return !unicode.IsPrint(c) }) >= 0 {
		return "", false
	}
	return name, true
}

type server struct {
	rooms *rooms
	// How long a listener's ask for the next state is held.
	wait time.Duration
}

func (s *server) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /time", func(w http.ResponseWriter, _ *http.Request) {
		answer(w, http.StatusOK, map[string]int64{"ms": serverMs()})
	})
	mux.HandleFunc("POST /rooms", func(w http.ResponseWriter, r *http.Request) {
		made, err := s.rooms.make(bearer(r))
		if err != nil {
			refuse(w, http.StatusForbidden, err.Error())
			return
		}
		answer(w, http.StatusOK, made)
	})
	mux.HandleFunc("PUT /rooms/{room}/state", s.opened(host, func(w http.ResponseWriter, r *http.Request, room string, _ holder) {
		body, err := io.ReadAll(io.LimitReader(r.Body, maximumState+1))
		if err != nil || len(body) > maximumState || !json.Valid(body) || !strings.HasPrefix(strings.TrimSpace(string(body)), "{") {
			refuse(w, http.StatusBadRequest, "the state is a JSON object")
			return
		}
		revision, err := s.rooms.describe(room, json.RawMessage(body))
		if err != nil {
			refuse(w, http.StatusNotFound, err.Error())
			return
		}
		answer(w, http.StatusOK, map[string]uint64{"revision": revision})
	}))
	mux.HandleFunc("GET /rooms/{room}/state", s.opened(listener, func(w http.ResponseWriter, r *http.Request, room string, who holder) {
		var after uint64
		if given := r.URL.Query().Get("after"); given != "" {
			parsed, err := strconv.ParseUint(given, 10, 64)
			if err != nil {
				refuse(w, http.StatusBadRequest, "after is a revision")
				return
			}
			after = parsed
		}
		name, ok := listenerName(r)
		if !ok {
			refuse(w, http.StatusBadRequest, "a name is at most 64 printable characters")
			return
		}
		// The host reads the room too -- who listens -- without being one.
		if who == host {
			name = ""
		}
		// A shorter wait for whoever asks one: an engine keeping in step looks
		// again within a second whatever comes.
		wait := s.wait
		if given := r.URL.Query().Get("wait"); given != "" {
			ms, err := strconv.ParseUint(given, 10, 32)
			if err != nil {
				refuse(w, http.StatusBadRequest, "wait is milliseconds")
				return
			}
			wait = min(wait, time.Duration(ms)*time.Millisecond)
		}
		found, err := s.rooms.next(r.Context(), room, after, name, wait)
		if err != nil {
			if errors.Is(err, errNoRoom) {
				refuse(w, http.StatusNotFound, err.Error())
			}
			return
		}
		answer(w, http.StatusOK, struct {
			seen
			Ms int64 `json:"ms"`
		}{found, serverMs()})
	}))
	mux.HandleFunc("POST /rooms/{room}/leave", s.opened(listener, func(w http.ResponseWriter, r *http.Request, room string, _ holder) {
		if name, ok := listenerName(r); ok && name != "" {
			s.rooms.leave(room, name)
		}
		answer(w, http.StatusOK, map[string]bool{"left": true})
	}))
	mux.HandleFunc("DELETE /rooms/{room}", s.opened(host, func(w http.ResponseWriter, _ *http.Request, room string, _ holder) {
		s.rooms.close(room)
		answer(w, http.StatusOK, map[string]bool{"closed": true})
	}))
	return mux
}

// A handler for whoever holds at least `needed` in the room the path names.
func (s *server) opened(needed holder, handle func(http.ResponseWriter, *http.Request, string, holder)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		room := r.PathValue("room")
		who := s.rooms.holder(room, bearer(r))
		switch {
		case who == nobody:
			refuse(w, http.StatusForbidden, "that key opens no such room")
		case needed == host && who != host:
			refuse(w, http.StatusForbidden, "only the host does that")
		default:
			handle(w, r, room, who)
		}
	}
}
