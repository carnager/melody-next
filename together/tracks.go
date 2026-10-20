// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

// ADR-0302 phase 2: the host's copy of a track, for whoever lacks one --
// sent once by the host, fetched by each listener without it. Kept on disk
// an hour, within a size for the whole server, and gone with its room.

const (
	maximumTrack = 64 << 20
	// A cover is shown no larger than a phone's screen: the host's engine
	// sends it fitted, well within this.
	maximumCover = 1 << 20
	keptFor      = time.Hour
)

var (
	errTooLarge  = errors.New("a track is at most 64 MB, a cover 1 MB")
	errNoImage   = errors.New("a cover is a JPEG, PNG or WebP image")
	errBadTrack  = errors.New("a track's name is letters, digits, - and _")
	trackPattern = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)
)

type kept struct {
	path  string
	size  int64
	added time.Time
}

// The copies a room's tracks have -- and, a store of their own, their
// covers -- kept alike: an hour, a folder a room.
type tracks struct {
	directory string
	capacity  int64
	// What each file ends in, and the most one may take.
	suffix  string
	maximum int64
	now     func() time.Time
	mutex   sync.Mutex
	// By room, then track.
	files map[string]map[string]kept
}

// The tracks kept in `directory`, a folder each room, those a previous run
// kept among them: rooms outlive a restart, and so do their copies.
func newTracks(directory string, capacity int64) (*tracks, error) {
	return newStore(directory, capacity, ".opus", maximumTrack)
}

// The covers of the tracks, kept as the tracks are.
func newCovers(directory string, capacity int64) (*tracks, error) {
	return newStore(directory, capacity, ".img", maximumCover)
}

func newStore(directory string, capacity int64, suffix string, maximum int64) (*tracks, error) {
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return nil, err
	}
	found := &tracks{directory: directory, capacity: capacity, suffix: suffix, maximum: maximum,
		now: time.Now, files: map[string]map[string]kept{}}
	rooms, err := os.ReadDir(directory)
	if err != nil {
		return nil, err
	}
	for _, room := range rooms {
		if !room.IsDir() {
			// An upload a stop cut short.
			_ = os.Remove(filepath.Join(directory, room.Name()))
			continue
		}
		copies, _ := os.ReadDir(filepath.Join(directory, room.Name()))
		for _, copy := range copies {
			track, isOurs := strings.CutSuffix(copy.Name(), found.suffix)
			info, err := copy.Info()
			if !isOurs || err != nil || !trackPattern.MatchString(track) {
				continue
			}
			if found.files[room.Name()] == nil {
				found.files[room.Name()] = map[string]kept{}
			}
			found.files[room.Name()][track] = kept{path: filepath.Join(directory, room.Name(), copy.Name()),
				size: info.Size(), added: info.ModTime()}
		}
	}
	found.evictLocked()
	return found, nil
}

func (t *tracks) path(room, track string) string {
	return filepath.Join(t.directory, room, track+t.suffix)
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
	written, err := io.Copy(partial, io.LimitReader(body, t.maximum+1))
	if closed := partial.Close(); err == nil {
		err = closed
	}
	if err != nil {
		return err
	}
	if written > t.maximum {
		return errTooLarge
	}
	// A cover is an image a browser shows, and nothing else served as one.
	if t.suffix == ".img" && imageType(partial.Name()) == "" {
		return errNoImage
	}
	target := t.path(room, track)
	if err := os.MkdirAll(filepath.Dir(target), 0o700); err != nil {
		return err
	}
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
	_ = os.RemoveAll(filepath.Join(t.directory, room))
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

// What image a kept cover is, from its first bytes: one of those a page
// shows, or "".
func imageType(path string) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	head := make([]byte, 512)
	read, _ := io.ReadFull(file, head)
	switch kind := http.DetectContentType(head[:read]); kind {
	case "image/jpeg", "image/png", "image/webp":
		return kind
	default:
		return ""
	}
}
