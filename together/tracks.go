// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"errors"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"sync"
	"time"
)

// ADR-0302 phase 2: the host's copy of a track, for whoever lacks one --
// sent once by the host, fetched by each listener without it. Kept on disk
// an hour, within a size for the whole server, and gone with its room.

const (
	maximumTrack = 64 << 20
	keptFor      = time.Hour
)

var (
	errTooLarge  = errors.New("a track is at most 64 MB")
	errBadTrack  = errors.New("a track's name is letters, digits, - and _")
	trackPattern = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)
)

type kept struct {
	path  string
	size  int64
	added time.Time
}

type tracks struct {
	directory string
	capacity  int64
	now       func() time.Time
	mutex     sync.Mutex
	// By room, then track.
	files map[string]map[string]kept
}

func newTracks(directory string, capacity int64) (*tracks, error) {
	// Whatever a previous run left is no one's now.
	if err := os.RemoveAll(directory); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return nil, err
	}
	return &tracks{directory: directory, capacity: capacity, now: time.Now,
		files: map[string]map[string]kept{}}, nil
}

func (t *tracks) path(room, track string) string {
	return filepath.Join(t.directory, room+"-"+track+".opus")
}

// Keeps what `body` holds as the room's track, replacing one of that name.
func (t *tracks) put(room, track string, body io.Reader) error {
	if !trackPattern.MatchString(track) {
		return errBadTrack
	}
	partial, err := os.CreateTemp(t.directory, "upload-*")
	if err != nil {
		return err
	}
	defer os.Remove(partial.Name())
	written, err := io.Copy(partial, io.LimitReader(body, maximumTrack+1))
	if closed := partial.Close(); err == nil {
		err = closed
	}
	if err != nil {
		return err
	}
	if written > maximumTrack {
		return errTooLarge
	}
	target := t.path(room, track)
	if err := os.Rename(partial.Name(), target); err != nil {
		return err
	}
	t.mutex.Lock()
	defer t.mutex.Unlock()
	if t.files[room] == nil {
		t.files[room] = map[string]kept{}
	}
	t.files[room][track] = kept{path: target, size: written, added: t.now()}
	t.evictLocked()
	return nil
}

// The file of a room's track, if it is kept.
func (t *tracks) open(room, track string) (*os.File, time.Time, error) {
	t.mutex.Lock()
	found, there := t.files[room][track]
	t.mutex.Unlock()
	if !there {
		return nil, time.Time{}, os.ErrNotExist
	}
	file, err := os.Open(found.path)
	return file, found.added, err
}

// Every track a room has, by name.
func (t *tracks) names(room string) []string {
	t.mutex.Lock()
	defer t.mutex.Unlock()
	names := make([]string, 0, len(t.files[room]))
	for name := range t.files[room] {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func (t *tracks) forget(room string) {
	t.mutex.Lock()
	defer t.mutex.Unlock()
	for _, found := range t.files[room] {
		_ = os.Remove(found.path)
	}
	delete(t.files, room)
}

// What is older than an hour goes; then the oldest, until the rest fit.
func (t *tracks) evictLocked() {
	type entry struct {
		room, track string
		kept
	}
	var all []entry
	var total int64
	now := t.now()
	for room, files := range t.files {
		for track, found := range files {
			if now.Sub(found.added) > keptFor {
				_ = os.Remove(found.path)
				delete(files, track)
				continue
			}
			all = append(all, entry{room, track, found})
			total += found.size
		}
	}
	sort.Slice(all, func(a, b int) bool { return all[a].added.Before(all[b].added) })
	for _, oldest := range all {
		if total <= t.capacity {
			break
		}
		_ = os.Remove(oldest.path)
		delete(t.files[oldest.room], oldest.track)
		total -= oldest.size
	}
}

func (t *tracks) sweep() {
	t.mutex.Lock()
	defer t.mutex.Unlock()
	t.evictLocked()
}
