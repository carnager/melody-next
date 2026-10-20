// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"slices"
	"sort"
	"strconv"
	"sync"
	"time"
)

// ADR-0302: the rooms of a melody-together server. A room is made with the
// server's password and answers with a host key -- whose holder alone says
// what plays -- and an invite, which lets a friend listen. What plays is the
// host's to describe; the server keeps it whole, numbers each change, and
// wakes whoever waits for the next.

var (
	errRefused  = errors.New("the server's password is not that")
	errNoRoom   = errors.New("there is no such room")
	errNoParty  = errors.New("the room is no party: only the host chooses")
	errTooMany  = errors.New("three suggestions of yours are waiting already")
	errNoSuch   = errors.New("there is no such suggestion")
	maximumWait = 3
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
	Chat      []said          `json:"chat"`
	Closed    bool            `json:"closed"`
	Party     bool            `json:"party"`
	// Most wanted first; the oldest first among as wanted.
	Suggestions []suggestion `json:"suggestions"`
	// Newest first.
	History []played `json:"history"`
}

// A line of the room's chat: who, what, when on the server's clock.
type said struct {
	ID   uint64 `json:"id"`
	Name string `json:"name"`
	Text string `json:"text"`
	Ms   int64  `json:"ms"`
}

// The chat a room keeps: enough to read back on arriving.
const keptChat = 100

// ADR-0302 phase 4: a track a friend suggests in a party room -- described
// as the host's tracks are, its copy sent by whoever suggested it, played
// in turn with the host's by the votes it has.
type suggestion struct {
	ID    uint64          `json:"id"`
	Name  string          `json:"name"`
	Track json.RawMessage `json:"track"`
	// The name its copy is kept by, among the room's tracks.
	File  string   `json:"file"`
	Votes []string `json:"votes"`
	Ms    int64    `json:"ms"`
}

func suggestionFile(id uint64) string { return "s" + strconv.FormatUint(id, 10) }

type room struct {
	hostKey   string
	invite    string
	revision  uint64
	state     json.RawMessage
	listeners map[string]time.Time
	touched   time.Time
	closed    bool
	chat      []said
	chatID    uint64
	party     bool
	suggested []suggestion
	suggestID uint64
	// What played before what plays, newest first; and what plays, by its
	// entry and instance, to tell when it changes.
	history []played
	playing string
	current played
	// Closed, and a new one made, whenever the room changes: whoever waits
	// on it wakes.
	changed chan struct{}
}

// A track the room played: what it was, its copy's name, and when it began
// on the server's clock.
type played struct {
	Track json.RawMessage `json:"track"`
	File  string          `json:"file"`
	Ms    int64           `json:"ms"`
}

// The tracks a room remembers having played.
const keptHistory = 30

type rooms struct {
	password string
	now      func() time.Time
	mutex    sync.Mutex
	rooms    map[string]*room
	// Changed since last kept on disk.
	dirty bool
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

// A room, named as asked or with a name made here.
func (r *rooms) make(password, wanted string) (made, error) {
	// No password set is no rooms made: a server open to anyone is not one
	// for friends.
	if r.password == "" || !same(password, r.password) {
		return made{}, errRefused
	}
	if wanted != "" && !namePattern.MatchString(wanted) {
		return made{}, errBadName
	}
	hostKey, err := token(32)
	if err != nil {
		return made{}, err
	}
	// The secret of a room is its invite's key, not its name: 64 bits, for
	// a server of friends.
	invite, err := token(8)
	if err != nil {
		return made{}, err
	}
	r.mutex.Lock()
	defer r.mutex.Unlock()
	taken := func(name string) bool { return r.rooms[name] != nil }
	name := wanted
	if name == "" {
		if name, err = madeName(taken); err != nil {
			return made{}, err
		}
	} else if taken(name) {
		return made{}, errNameTaken
	}
	r.dirty = true
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
// with the lock held -- by the rooms, which then have something to keep.
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
	chat := make([]said, len(found.chat))
	copy(chat, found.chat)
	suggested := make([]suggestion, len(found.suggested))
	copy(suggested, found.suggested)
	sort.SliceStable(suggested, func(a, b int) bool {
		if len(suggested[a].Votes) != len(suggested[b].Votes) {
			return len(suggested[a].Votes) > len(suggested[b].Votes)
		}
		return suggested[a].ID < suggested[b].ID
	})
	history := make([]played, len(found.history))
	copy(history, found.history)
	return seen{Revision: found.revision, State: found.state, Listeners: names, Chat: chat,
		Closed: found.closed, Party: found.party, Suggestions: suggested, History: history}
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
	// A new track: the one before it goes into the room's history.
	var said struct {
		Entry    string          `json:"entry"`
		Instance uint64          `json:"instance"`
		File     string          `json:"file"`
		Track    json.RawMessage `json:"track"`
	}
	if json.Unmarshal(state, &said) == nil && len(said.Track) > 0 && string(said.Track) != "null" {
		playing := said.Entry + "/" + strconv.FormatUint(said.Instance, 10)
		if playing != found.playing {
			if found.playing != "" {
				found.history = append([]played{found.current}, found.history...)
				if len(found.history) > keptHistory {
					found.history = found.history[:keptHistory]
				}
			}
			file := said.File
			if file == "" {
				file = said.Entry
			}
			found.playing = playing
			found.current = played{Track: said.Track, File: file, Ms: r.now().UnixMilli()}
		}
	}
	r.dirty = true
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

// Something of the room changed that is not its state -- a track kept --
// and whoever waits is told.
func (r *rooms) touch(name string) {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	if found := r.rooms[name]; found != nil && !found.closed {
		found.touched = r.now()
		found.moved()
	}
}

// A line of chat, said to everyone waiting.
func (r *rooms) say(name, who, text string) error {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.dirty = true
	found := r.rooms[name]
	if found == nil || found.closed {
		return errNoRoom
	}
	found.chatID++
	found.chat = append(found.chat, said{ID: found.chatID, Name: who, Text: text, Ms: r.now().UnixMilli()})
	if len(found.chat) > keptChat {
		found.chat = found.chat[len(found.chat)-keptChat:]
	}
	found.touched = r.now()
	found.moved()
	return nil
}

// The host makes the room a party, or ends it -- and with it what waits.
func (r *rooms) setParty(name string, on bool) error {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.dirty = true
	found := r.rooms[name]
	if found == nil || found.closed {
		return errNoRoom
	}
	found.party = on
	if !on {
		found.suggested = nil
	}
	found.touched = r.now()
	found.moved()
	return nil
}

// A friend's suggestion: the name its copy goes by.
func (r *rooms) suggest(name, who string, track json.RawMessage) (suggestion, error) {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.dirty = true
	found := r.rooms[name]
	if found == nil || found.closed {
		return suggestion{}, errNoRoom
	}
	if !found.party {
		return suggestion{}, errNoParty
	}
	waiting := 0
	for _, each := range found.suggested {
		if each.Name == who {
			waiting++
		}
	}
	if waiting >= maximumWait {
		return suggestion{}, errTooMany
	}
	found.suggestID++
	made := suggestion{ID: found.suggestID, Name: who, Track: track, File: suggestionFile(found.suggestID),
		Votes: []string{}, Ms: r.now().UnixMilli()}
	found.suggested = append(found.suggested, made)
	found.touched = r.now()
	found.moved()
	return made, nil
}

// A vote for a suggestion, or -- given again -- taken back.
func (r *rooms) vote(name string, id uint64, who string) error {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.dirty = true
	found := r.rooms[name]
	if found == nil || found.closed {
		return errNoRoom
	}
	for index := range found.suggested {
		each := &found.suggested[index]
		if each.ID != id {
			continue
		}
		if at := slices.Index(each.Votes, who); at >= 0 {
			each.Votes = slices.Delete(each.Votes, at, at+1)
		} else {
			each.Votes = append(each.Votes, who)
		}
		found.touched = r.now()
		found.moved()
		return nil
	}
	return errNoSuch
}

// A suggestion gone from the list: taken by the host to play, or removed.
func (r *rooms) unsuggest(name string, id uint64) (suggestion, error) {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.dirty = true
	found := r.rooms[name]
	if found == nil || found.closed {
		return suggestion{}, errNoRoom
	}
	for index, each := range found.suggested {
		if each.ID == id {
			found.suggested = slices.Delete(found.suggested, index, index+1)
			found.touched = r.now()
			found.moved()
			return each, nil
		}
	}
	return suggestion{}, errNoSuch
}

// Whether `file` is the copy of a suggestion waiting in a party: the one
// upload a listener may make.
func (r *rooms) awaits(name, file string) bool {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	found := r.rooms[name]
	if found == nil || found.closed || !found.party {
		return false
	}
	for _, each := range found.suggested {
		if each.File == file {
			return true
		}
	}
	return false
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
	r.dirty = true
	if found := r.rooms[name]; found != nil && !found.closed {
		found.closed = true
		found.touched = r.now()
		found.moved()
	}
}

// Closes the rooms idle too long, and forgets the listeners gone; run now
// and then. The rooms gone, by name.
func (r *rooms) sweep() []string {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	now := r.now()
	r.dirty = true
	var gone []string
	for name, found := range r.rooms {
		// Closed, it stays a while to say so to those still waiting.
		if (found.closed && now.Sub(found.touched) > presence) ||
			(!found.closed && now.Sub(found.touched) > idle) {
			delete(r.rooms, name)
			gone = append(gone, name)
			continue
		}
		left := false
		for who, last := range found.listeners {
			if now.Sub(last) > presence {
				delete(found.listeners, who)
				left = true
			}
		}
		if left {
			found.moved()
		}
	}
	return gone
}

// What is kept of a room on disk: everything but who is listening this
// moment, who comes back by asking.
type keptRoom struct {
	HostKey     string          `json:"host_key"`
	Invite      string          `json:"invite"`
	Revision    uint64          `json:"revision"`
	State       json.RawMessage `json:"state"`
	Touched     time.Time       `json:"touched"`
	Closed      bool            `json:"closed"`
	Chat        []said          `json:"chat"`
	ChatID      uint64          `json:"chat_id"`
	Party       bool            `json:"party"`
	Suggestions []suggestion    `json:"suggestions"`
	SuggestID   uint64          `json:"suggest_id"`
	History     []played        `json:"history"`
	Playing     string          `json:"playing"`
	Current     played          `json:"current"`
}

// Writes the rooms to `path`, whole or not at all, when they changed since.
func (r *rooms) keep(path string) error {
	r.mutex.Lock()
	if !r.dirty {
		r.mutex.Unlock()
		return nil
	}
	all := map[string]keptRoom{}
	for name, found := range r.rooms {
		all[name] = keptRoom{HostKey: found.hostKey, Invite: found.invite, Revision: found.revision,
			State: found.state, Touched: found.touched, Closed: found.closed, Chat: found.chat,
			ChatID: found.chatID, Party: found.party, Suggestions: found.suggested,
			SuggestID: found.suggestID, History: found.history, Playing: found.playing,
			Current: found.current}
	}
	r.dirty = false
	r.mutex.Unlock()
	text, err := json.Marshal(map[string]any{"version": 1, "rooms": all})
	if err != nil {
		return err
	}
	partial := path + ".partial"
	if err := os.WriteFile(partial, text, 0o600); err != nil {
		return err
	}
	return os.Rename(partial, path)
}

// The rooms kept at `path`, if any.
func (r *rooms) restore(path string) error {
	text, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var kept struct {
		Rooms map[string]keptRoom `json:"rooms"`
	}
	if err := json.Unmarshal(text, &kept); err != nil {
		return err
	}
	r.mutex.Lock()
	defer r.mutex.Unlock()
	for name, each := range kept.Rooms {
		state := each.State
		if len(state) == 0 {
			state = json.RawMessage("{}")
		}
		r.rooms[name] = &room{hostKey: each.HostKey, invite: each.Invite, revision: each.Revision,
			state: state, listeners: map[string]time.Time{}, touched: each.Touched,
			closed: each.Closed, chat: each.Chat, chatID: each.ChatID, party: each.Party,
			suggested: each.Suggestions, suggestID: each.SuggestID, history: each.History,
			playing: each.Playing, current: each.Current, changed: make(chan struct{})}
	}
	return nil
}
