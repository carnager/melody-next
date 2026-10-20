// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// ADR-0302: rooms made with the server's password, described by the host
// alone, followed by whoever holds the invite -- over HTTP.

type client struct {
	t    *testing.T
	base string
}

func (c client) do(method, path, key, body string) (int, map[string]any) {
	c.t.Helper()
	request, err := http.NewRequest(method, c.base+path, strings.NewReader(body))
	if err != nil {
		c.t.Fatal(err)
	}
	if key != "" {
		request.Header.Set("Authorization", "Bearer "+key)
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		c.t.Fatal(err)
	}
	defer response.Body.Close()
	read, _ := io.ReadAll(response.Body)
	answered := map[string]any{}
	_ = json.Unmarshal(read, &answered)
	return response.StatusCode, answered
}

func TestRooms(t *testing.T) {
	rooms := newRooms("server secret")
	served := httptest.NewServer((&server{rooms: rooms, tracks: keptIn(t), wait: 2 * time.Second}).handler())
	defer served.Close()
	c := client{t, served.URL}

	if status, _ := c.do("GET", "/time", "", ""); status != 200 {
		t.Fatal("the time is answered")
	}
	// Made only with the server's password.
	if status, _ := c.do("POST", "/rooms", "wrong", ""); status != 403 {
		t.Fatal("a wrong password makes no room")
	}
	_, made := c.do("POST", "/rooms", "server secret", "")
	room, hostKey, invite := made["room"].(string), made["host_key"].(string), made["invite"].(string)
	if room == "" || hostKey == invite {
		t.Fatal("a room, its host key and its invite")
	}
	state := "/rooms/" + room + "/state"

	// The host alone says what plays; nothing else opens the room.
	said := `{"track":{"title":"Time Machine"},"anchor_ms":1000}`
	if status, _ := c.do("PUT", state, invite, said); status != 403 {
		t.Fatal("a listener cannot say what plays")
	}
	if status, _ := c.do("GET", state, "guess", ""); status != 403 {
		t.Fatal("a guess opens nothing")
	}
	if status, _ := c.do("PUT", state, hostKey, "[1]"); status != 400 {
		t.Fatal("a state is a JSON object")
	}
	if status, body := c.do("PUT", state, hostKey, said); status != 200 || body["revision"] != 1.0 {
		t.Fatal("the first change")
	}

	// A listener asking from the start has it at once, and is counted.
	_, seen := c.do("GET", state+"?after=0&name=Ana", invite, "")
	if seen["state"].(map[string]any)["track"].(map[string]any)["title"] != "Time Machine" {
		t.Fatal("what plays is told")
	}
	if names := seen["listeners"].([]any); len(names) != 1 || names[0] != "Ana" {
		t.Fatal("who listens is told")
	}
	if seen["ms"].(float64) <= 0 || seen["revision"] != 2.0 {
		t.Fatal("the server's clock, and her arriving as a change")
	}

	// Asking for what follows waits for it, and wakes when the host speaks.
	woke := make(chan map[string]any)
	go func() {
		_, body := c.do("GET", state+"?after=2&name=Ana", invite, "")
		woke <- body
	}()
	select {
	case <-woke:
		t.Fatal("nothing new: it waits")
	case <-time.After(200 * time.Millisecond):
	}
	c.do("PUT", state, hostKey, `{"paused":true}`)
	select {
	case body := <-woke:
		if body["state"].(map[string]any)["paused"] != true {
			t.Fatal("the waiter has the pause")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the waiter wakes")
	}
	// Nothing new within the wait: the same state again -- after the wait
	// asked for, when it is shorter.
	asked := time.Now()
	if _, body := c.do("GET", state+"?after=3&wait=100", invite, ""); body["revision"] != 3.0 {
		t.Fatal("an empty wait answers what is")
	}
	if waited := time.Since(asked); waited < 100*time.Millisecond || waited > time.Second {
		t.Fatalf("the wait asked for is kept: %v", waited)
	}

	// Leaving and closing.
	c.do("POST", "/rooms/"+room+"/leave?name=Ana", invite, "")
	if _, body := c.do("GET", state+"?after=0", hostKey, ""); len(body["listeners"].([]any)) != 0 {
		t.Fatal("the host sees her go")
	}
	if status, _ := c.do("DELETE", "/rooms/"+room, invite, ""); status != 403 {
		t.Fatal("a listener does not close the room")
	}
	if status, _ := c.do("DELETE", "/rooms/"+room, hostKey, ""); status != 200 {
		t.Fatal("the host does")
	}
	if _, body := c.do("GET", state+"?after=99", invite, ""); body["closed"] != true {
		t.Fatal("a listener is told")
	}
	if status, _ := c.do("PUT", state, hostKey, said); status != 404 {
		t.Fatal("a closed room says nothing more")
	}
}

// An invite opened in a browser is a page saying how to join -- the same
// for any room, real or not.
func TestInvitePage(t *testing.T) {
	served := httptest.NewServer((&server{rooms: newRooms("pw"), tracks: keptIn(t), wait: time.Second}).handler())
	defer served.Close()
	for _, room := range []string{"real", "made-up"} {
		response, err := http.Get(served.URL + "/rooms/" + room)
		if err != nil {
			t.Fatal(err)
		}
		page, _ := io.ReadAll(response.Body)
		response.Body.Close()
		if response.StatusCode != 200 || !strings.HasPrefix(response.Header.Get("Content-Type"), "text/html") {
			t.Fatalf("an invite opens a page: %d %s", response.StatusCode, response.Header.Get("Content-Type"))
		}
		if !strings.Contains(string(page), "Listen here") ||
			!strings.Contains(string(page), "melody://together/join?invite=") {
			t.Fatal("to listen in the browser, or to open Melody")
		}
	}
}

func keptIn(t *testing.T) *tracks {
	kept, err := newTracks(filepath.Join(t.TempDir(), "tracks"), 1<<20)
	if err != nil {
		t.Fatal(err)
	}
	return kept
}

// Phase 2: the host's copy of a track, kept for whoever lacks one, fetched
// with the invite -- as a header, or in the address as a browser's <audio>
// asks -- and listed with the room's state.
func TestTracks(t *testing.T) {
	rooms := newRooms("pw")
	kept := keptIn(t)
	served := httptest.NewServer((&server{rooms: rooms, tracks: kept, wait: 100 * time.Millisecond}).handler())
	defer served.Close()
	c := client{t, served.URL}
	made, _ := rooms.make("pw")
	track := "/rooms/" + made.Room + "/tracks/entry-1"
	opus := strings.Repeat("OggS", 1000)

	if status, _ := c.do("PUT", track, made.Invite, opus); status != 403 {
		t.Fatal("a listener sends no tracks")
	}
	if status, _ := c.do("GET", track, made.Invite, ""); status != 404 {
		t.Fatal("none sent, none fetched")
	}
	if status, _ := c.do("PUT", "/rooms/"+made.Room+"/tracks/..%2Fx", made.HostKey, opus); status != 400 && status != 404 {
		t.Fatalf("a track's name is plain: %d", status)
	}
	if status, _ := c.do("PUT", track, made.HostKey, opus); status != 200 {
		t.Fatal("the host sends one")
	}
	if _, state := c.do("GET", "/rooms/"+made.Room+"/state?after=0", made.Invite, ""); len(state["files"].([]any)) != 1 || state["files"].([]any)[0] != "entry-1" {
		t.Fatal("and the room lists it")
	}
	fetched, err := http.Get(served.URL + track + "?key=" + made.Invite)
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(fetched.Body)
	fetched.Body.Close()
	if fetched.StatusCode != 200 || string(body) != opus || fetched.Header.Get("Content-Type") != "audio/ogg" {
		t.Fatal("a listener fetches it, the key in the address")
	}
	ranged, _ := http.NewRequest("GET", served.URL+track, nil)
	ranged.Header.Set("Authorization", "Bearer "+made.Invite)
	ranged.Header.Set("Range", "bytes=4-7")
	part, err := http.DefaultClient.Do(ranged)
	if err != nil {
		t.Fatal(err)
	}
	body, _ = io.ReadAll(part.Body)
	part.Body.Close()
	if part.StatusCode != 206 || string(body) != "OggS" {
		t.Fatal("a part of it, for a seek")
	}
	if status, _ := c.do("GET", track, "guess", ""); status != 403 {
		t.Fatal("nothing without the invite")
	}
	// Over the server's size, the oldest go; with the room, all of them.
	big := strings.Repeat("x", 700<<10)
	c.do("PUT", "/rooms/"+made.Room+"/tracks/a", made.HostKey, big)
	c.do("PUT", "/rooms/"+made.Room+"/tracks/b", made.HostKey, big)
	if names := kept.names(made.Room); len(names) != 1 || names[0] != "b" {
		t.Fatalf("the newest is kept within the size: %v", names)
	}
	c.do("DELETE", "/rooms/"+made.Room, made.HostKey, "")
	if len(kept.names(made.Room)) != 0 {
		t.Fatal("a closed room's tracks go")
	}
}

func TestTracksKeptAnHour(t *testing.T) {
	kept := keptIn(t)
	now := time.Now()
	kept.now = func() time.Time { return now }
	if err := kept.put("r", "old", strings.NewReader("x")); err != nil {
		t.Fatal(err)
	}
	now = now.Add(keptFor + time.Minute)
	kept.sweep()
	if len(kept.names("r")) != 0 {
		t.Fatal("a track older than an hour goes")
	}
}

// The room's chat: said by whoever holds the invite, to everyone waiting,
// the last hundred kept for those arriving.
func TestChat(t *testing.T) {
	rooms := newRooms("pw")
	served := httptest.NewServer((&server{rooms: rooms, tracks: keptIn(t), wait: 2 * time.Second}).handler())
	defer served.Close()
	c := client{t, served.URL}
	made, _ := rooms.make("pw")
	chat := "/rooms/" + made.Room + "/chat"
	if status, _ := c.do("POST", chat, "guess", `{"name":"Ana","text":"hi"}`); status != 403 {
		t.Fatal("nothing said without the invite")
	}
	for _, bad := range []string{`{"name":"","text":"hi"}`, `{"name":"Ana","text":"  "}`, `nope`,
		`{"name":"Ana","text":"` + strings.Repeat("x", 501) + `"}`} {
		if status, _ := c.do("POST", chat, made.Invite, bad); status != 400 {
			t.Fatalf("refused: %s", bad)
		}
	}
	woke := make(chan map[string]any)
	go func() {
		_, body := c.do("GET", "/rooms/"+made.Room+"/state?after=0&wait=1500", made.HostKey, "")
		woke <- body
	}()
	time.Sleep(100 * time.Millisecond)
	if status, _ := c.do("POST", chat, made.Invite, `{"name":"Ana","text":"this riff!"}`); status != 200 {
		t.Fatal("a line is said")
	}
	body := <-woke
	lines := body["chat"].([]any)
	if len(lines) != 1 || lines[0].(map[string]any)["text"] != "this riff!" || lines[0].(map[string]any)["name"] != "Ana" {
		t.Fatalf("and everyone waiting has it: %v", body["chat"])
	}
	for index := 0; index < keptChat+5; index++ {
		_ = rooms.say(made.Room, "Bo", "line")
	}
	if seen, _ := rooms.next(context.Background(), made.Room, 0, "", time.Millisecond); len(seen.Chat) != keptChat {
		t.Fatal("the last hundred are kept")
	}
}

func TestMascot(t *testing.T) {
	served := httptest.NewServer((&server{rooms: newRooms("pw"), tracks: keptIn(t), wait: time.Second}).handler())
	defer served.Close()
	response, err := http.Get(served.URL + "/assets/mascot.svg")
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(response.Body)
	response.Body.Close()
	if response.StatusCode != 200 || response.Header.Get("Content-Type") != "image/svg+xml" ||
		!strings.Contains(string(body), "<svg") {
		t.Fatal("the blackbird is there for the page")
	}
}

func TestNoPasswordNoRooms(t *testing.T) {
	if _, err := newRooms("").make(""); err == nil {
		t.Fatal("a server without a password makes no rooms")
	}
}

func TestSweep(t *testing.T) {
	now := time.Now()
	rooms := newRooms("pw")
	rooms.now = func() time.Time { return now }
	made, _ := rooms.make("pw")
	if _, err := rooms.next(context.Background(), made.Room, 0, "Bo", time.Millisecond); err != nil {
		t.Fatal(err)
	}
	// Bo is not heard from for longer than presence: gone.
	now = now.Add(presence + time.Second)
	rooms.sweep()
	if seen, _ := rooms.next(context.Background(), made.Room, 0, "", time.Millisecond); len(seen.Listeners) != 0 {
		t.Fatal("a listener not heard from has gone")
	}
	// Idle past a day's half: the room is no more.
	now = now.Add(idle + time.Second)
	rooms.sweep()
	if _, err := rooms.next(context.Background(), made.Room, 0, "", time.Millisecond); err != errNoRoom {
		t.Fatal("an idle room is closed")
	}
}
