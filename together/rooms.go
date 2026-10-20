// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"sort"
	"sync"
	"time"
)

// ADR-0302: the rooms of a melody-together server. A room is made with the
// server's password and answers with a host key -- whose holder alone says
// what plays -- and an invite, which lets a friend listen. What plays is the
// host's to describe; the server keeps it whole, numbers each change, and
// wakes whoever waits for the next.

var (
	errRefused = errors.New("the server's password is not that")
	errNoRoom  = errors.New("there is no such room")
)

// Listeners not heard from for this long have gone; a room no one has
// touched for idle is closed.
const (
	presence = 60 * time.Second
	idle     = 12 * time.Hour
)

type holder int

const (
	nobody holder = iota
	host
	listener
)

// What a room was made with.
type made struct {
	Room    string `json:"room"`
	HostKey string `json:"host_key"`
	Invite  string `json:"invite"`
}

// A room as a listener receives it: what plays, who listens, which change
// it is, and whether it has closed.
type seen struct {
	Revision  uint64          `json:"revision"`
	State     json.RawMessage `json:"state"`
	Listeners []string        `json:"listeners"`
	Closed    bool            `json:"closed"`
}

type room struct {
	hostKey   string
	invite    string
	revision  uint64
	state     json.RawMessage
	listeners map[string]time.Time
	touched   time.Time
	closed    bool
	// Closed, and a new one made, whenever the room changes: whoever waits
	// on it wakes.
	changed chan struct{}
}

type rooms struct {
	password string
	now      func() time.Time
	mutex    sync.Mutex
	rooms    map[string]*room
}

func newRooms(password string) *rooms {
	return &rooms{password: password, now: time.Now, rooms: map[string]*room{}}
}

func token(bytes int) (string, error) {
	random := make([]byte, bytes)
	if _, err := rand.Read(random); err != nil {
		return "", err
	}
	return hex.EncodeToString(random), nil
}

// Compares without an early exit, so how long a wrong guess takes says
// nothing about how much of it was right.
func same(offered, expected string) bool {
	return len(offered) == len(expected) &&
		subtle.ConstantTimeCompare([]byte(offered), []byte(expected)) == 1
}

func (r *rooms) make(password string) (made, error) {
	// No password set is no rooms made: a server open to anyone is not one
	// for friends.
	if r.password == "" || !same(password, r.password) {
		return made{}, errRefused
	}
	name, err := token(8)
	if err != nil {
		return made{}, err
	}
	hostKey, err := token(32)
	if err != nil {
		return made{}, err
	}
	invite, err := token(32)
	if err != nil {
		return made{}, err
	}
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.rooms[name] = &room{
		hostKey:   hostKey,
		invite:    invite,
		state:     json.RawMessage("{}"),
		listeners: map[string]time.Time{},
		touched:   r.now(),
		changed:   make(chan struct{}),
	}
	return made{Room: name, HostKey: hostKey, Invite: invite}, nil
}

func (r *rooms) holder(name, key string) holder {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	found := r.rooms[name]
	switch {
	case found == nil || key == "":
		return nobody
	case same(key, found.hostKey):
		return host
	case same(key, found.invite):
		return listener
	}
	return nobody
}

// Moved: the revision is a new one, and everyone waiting wakes. Called
// with the lock held.
func (found *room) moved() {
	found.revision++
	close(found.changed)
	found.changed = make(chan struct{})
}

func (found *room) seen() seen {
	names := make([]string, 0, len(found.listeners))
	for name := range found.listeners {
		names = append(names, name)
	}
	sort.Strings(names)
	return seen{Revision: found.revision, State: found.state, Listeners: names, Closed: found.closed}
}

// The host's: a new state, numbered and handed to everyone waiting.
func (r *rooms) describe(name string, state json.RawMessage) (uint64, error) {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	found := r.rooms[name]
	if found == nil || found.closed {
		return 0, errNoRoom
	}
	found.state = state
	found.touched = r.now()
	found.moved()
	return found.revision, nil
}

// The next state after `after` -- at once if there is one, else when it
// comes or `wait` has passed, when the same one is answered again. A
// listener naming itself is counted present; one arriving or going is a
// change too, so the host sees who listens.
func (r *rooms) next(ctx context.Context, name string, after uint64, who string, wait time.Duration) (seen, error) {
	deadline := time.NewTimer(wait)
	defer deadline.Stop()
	for {
		r.mutex.Lock()
		found := r.rooms[name]
		if found == nil {
			r.mutex.Unlock()
			return seen{}, errNoRoom
		}
		if !found.closed {
			found.touched = r.now()
			if who != "" {
				_, there := found.listeners[who]
				found.listeners[who] = r.now()
				if !there {
					found.moved()
				}
			}
		}
		if found.closed || found.revision > after {
			answer := found.seen()
			r.mutex.Unlock()
			return answer, nil
		}
		changed := found.changed
		r.mutex.Unlock()
		select {
		case <-changed:
		case <-deadline.C:
			r.mutex.Lock()
			answer := found.seen()
			r.mutex.Unlock()
			return answer, nil
		case <-ctx.Done():
			return seen{}, ctx.Err()
		}
	}
}

func (r *rooms) leave(name, who string) {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	if found := r.rooms[name]; found != nil {
		if _, there := found.listeners[who]; there {
			delete(found.listeners, who)
			found.moved()
		}
	}
}

func (r *rooms) close(name string) {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	if found := r.rooms[name]; found != nil && !found.closed {
		found.closed = true
		found.touched = r.now()
		found.moved()
	}
}

// Closes the rooms idle too long, and forgets the listeners gone; run now
// and then.
func (r *rooms) sweep() {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	now := r.now()
	for name, found := range r.rooms {
		// Closed, it stays a while to say so to those still waiting.
		if (found.closed && now.Sub(found.touched) > presence) ||
			(!found.closed && now.Sub(found.touched) > idle) {
			delete(r.rooms, name)
			continue
		}
		gone := false
		for who, last := range found.listeners {
			if now.Sub(last) > presence {
				delete(found.listeners, who)
				gone = true
			}
		}
		if gone {
			found.moved()
		}
	}
}
