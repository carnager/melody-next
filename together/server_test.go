// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
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
	made, _ := rooms.make("pw", "", "")
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
	made, _ := rooms.make("pw", "", "")
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

// Phase 4: in a party, friends suggest tracks and vote; the host takes
// the most wanted. A listener sends a copy only for a suggestion of the
// party's.
func TestParty(t *testing.T) {
	rooms := newRooms("pw")
	served := httptest.NewServer((&server{rooms: rooms, tracks: keptIn(t), wait: 200 * time.Millisecond}).handler())
	defer served.Close()
	c := client{t, served.URL}
	made, _ := rooms.make("pw", "", "")
	base := "/rooms/" + made.Room
	offer := func(who, title string) (int, map[string]any) {
		return c.do("POST", base+"/suggestions", made.Invite,
			`{"name":"`+who+`","track":{"title":"`+title+`","artist":"A"}}`)
	}
	if status, _ := offer("Ana", "One"); status != 409 {
		t.Fatal("no suggestions but in a party")
	}
	if status, _ := c.do("PUT", base+"/party", made.Invite, `{"party":true}`); status != 403 {
		t.Fatal("only the host makes a party")
	}
	if status, _ := c.do("PUT", base+"/party", made.HostKey, `{"party":true}`); status != 200 {
		t.Fatal("the host does")
	}
	_, first := offer("Ana", "One")
	_, second := offer("Bo", "Two")
	offer("Ana", "Three")
	offer("Ana", "Four")
	if status, _ := offer("Ana", "Five"); status != 409 {
		t.Fatal("three of one friend's wait at most")
	}
	// A listener's copy: of a suggestion, and nothing else.
	if status, _ := c.do("PUT", base+"/tracks/e1", made.Invite, "OggS"); status != 403 {
		t.Fatal("a listener sends no track of the host's")
	}
	if status, _ := c.do("PUT", base+"/tracks/"+second["file"].(string), made.Invite, "OggS"); status != 200 {
		t.Fatal("but the copy of a suggestion")
	}
	// Votes order the list; a vote given again is taken back.
	twoID := strconv.Itoa(int(second["id"].(float64)))
	c.do("POST", base+"/suggestions/"+twoID+"/vote", made.Invite, `{"name":"Cy"}`)
	c.do("POST", base+"/suggestions/"+twoID+"/vote", made.Invite, `{"name":"Di"}`)
	c.do("POST", base+"/suggestions/"+twoID+"/vote", made.Invite, `{"name":"Di"}`)
	_, state := c.do("GET", base+"/state?after=0", made.HostKey, "")
	if state["party"] != true {
		t.Fatal("the room says it is a party")
	}
	listed := state["suggestions"].([]any)
	top := listed[0].(map[string]any)
	if len(listed) != 4 || top["id"] != second["id"] || len(top["votes"].([]any)) != 1 || top["ready"] != true {
		t.Fatalf("the most wanted first, its copy there: %v", listed)
	}
	if listed[1].(map[string]any)["id"] != first["id"] || listed[1].(map[string]any)["ready"] != false {
		t.Fatal("then the oldest, its copy not yet sent")
	}
	// The host takes it to play; its copy stays.
	if status, taken := c.do("POST", base+"/suggestions/"+twoID+"/take", made.HostKey, ""); status != 200 || taken["file"] != second["file"] {
		t.Fatal("the host takes the most wanted")
	}
	if status, _ := c.do("POST", base+"/suggestions/"+twoID+"/take", made.Invite, ""); status != 403 {
		t.Fatal("only the host takes")
	}
	_, state = c.do("GET", base+"/state?after=0", made.HostKey, "")
	if len(state["suggestions"].([]any)) != 3 || !slices.Contains(state["files"].([]any), any(second["file"])) {
		t.Fatal("gone from the list, its copy kept")
	}
	// The party over, what waited is gone.
	c.do("PUT", base+"/party", made.HostKey, `{"party":false}`)
	if _, state = c.do("GET", base+"/state?after=0", made.HostKey, ""); len(state["suggestions"].([]any)) != 0 {
		t.Fatal("no party, no suggestions")
	}
}

// A room has a name: the host's, or two words made here; its invite's key
// is the secret.
func TestNamedRooms(t *testing.T) {
	rooms := newRooms("pw")
	served := httptest.NewServer((&server{rooms: rooms, tracks: keptIn(t), wait: time.Second}).handler())
	defer served.Close()
	c := client{t, served.URL}
	status, named := c.do("POST", "/rooms", "pw", `{"name":"Friday-Doom"}`)
	if status != 200 || named["room"] != "friday-doom" || len(named["invite"].(string)) != 16 {
		t.Fatalf("a room of the name asked: %d %v", status, named)
	}
	if status, _ := c.do("POST", "/rooms", "pw", `{"name":"friday-doom"}`); status != 409 {
		t.Fatal("one of a name at a time")
	}
	for _, bad := range []string{`{"name":"a"}`, `{"name":"two words"}`, `{"name":"-dash"}`, `{"name":"x/y"}`} {
		if status, _ := c.do("POST", "/rooms", "pw", bad); status != 400 {
			t.Fatalf("refused: %s", bad)
		}
	}
	_, made := c.do("POST", "/rooms", "pw", "")
	if !regexp.MustCompile(`^[a-z]+-[a-z]+(-\d+)?$`).MatchString(made["room"].(string)) {
		t.Fatalf("a name made of two words: %v", made["room"])
	}
}

// What played before what plays, newest first.
func TestHistory(t *testing.T) {
	rooms := newRooms("pw")
	made, _ := rooms.make("pw", "", "")
	for index, title := range []string{"One", "Two", "Two", "Two", "Three"} {
		// "Two" said again -- a seek, a pause -- is one track; and so is
		// "Two" begun again, a new instance of the same entry.
		entry := map[string]string{"One": "e1", "Two": "e2", "Three": "e3"}[title]
		instance := strconv.Itoa(1 + index/3)
		_, _ = rooms.describe(made.Room, json.RawMessage(`{"entry":"`+entry+`","instance":`+instance+`,"track":{"title":"`+title+`"},"position_ms":`+strconv.Itoa(index)+`}`))
	}
	seen, _ := rooms.next(context.Background(), made.Room, 0, "", time.Millisecond)
	if len(seen.History) != 2 || !strings.Contains(string(seen.History[0].Track), "Two") ||
		!strings.Contains(string(seen.History[1].Track), "One") || seen.History[0].File != "e2" {
		t.Fatalf("the tracks before, newest first: %+v", seen.History)
	}
}

// Rooms and their copies outlive the server's restart.
func TestKeptAcrossRestart(t *testing.T) {
	directory := t.TempDir()
	file := filepath.Join(directory, "rooms.json")
	rooms := newRooms("pw")
	made, _ := rooms.make("pw", "night-owl", "")
	_, _ = rooms.describe(made.Room, json.RawMessage(`{"entry":"e1","instance":1,"track":{"title":"One"}}`))
	_ = rooms.say(made.Room, "Ana", "still here?")
	_ = rooms.setParty(made.Room, true)
	kept, _ := newTracks(filepath.Join(directory, "tracks"), 1<<20)
	if err := kept.put(made.Room, "e1", strings.NewReader("OggS")); err != nil {
		t.Fatal(err)
	}
	if err := rooms.keep(file); err != nil {
		t.Fatal(err)
	}
	again := newRooms("pw")
	if err := again.restore(file); err != nil {
		t.Fatal(err)
	}
	if again.holder(made.Room, made.Invite) != listener || again.holder(made.Room, made.HostKey) != host {
		t.Fatal("the room opens with the same keys")
	}
	seen, _ := again.next(context.Background(), made.Room, 0, "", time.Millisecond)
	if !strings.Contains(string(seen.State), "One") || len(seen.Chat) != 1 || !seen.Party {
		t.Fatalf("what played, said and was: %+v", seen)
	}
	keptAgain, _ := newTracks(filepath.Join(directory, "tracks"), 1<<20)
	if names := keptAgain.names(made.Room); len(names) != 1 || names[0] != "e1" {
		t.Fatalf("and its copies: %v", names)
	}
}

// A room joined by the password its host chose; a server without a
// password of its own makes rooms for anyone.
func TestRoomPasswords(t *testing.T) {
	open := newRooms("")
	made, err := open.make("", "doom", "letmein")
	if err != nil || made.Invite != "letmein" {
		t.Fatalf("a server without a password makes rooms for anyone: %v", err)
	}
	if open.holder("doom", "letmein") != listener || open.holder("doom", "guess") != nobody {
		t.Fatal("the room's password lets in, nothing else")
	}
	if _, err := open.make("", "x-room", "abc"); !errors.Is(err, errBadSecret) {
		t.Fatal("a password of four characters at least")
	}
	closed := newRooms("pw")
	if _, err := closed.make("", "", ""); !errors.Is(err, errRefused) {
		t.Fatal("one with a password makes rooms only with it")
	}
}

func TestSweep(t *testing.T) {
	now := time.Now()
	rooms := newRooms("pw")
	rooms.now = func() time.Time { return now }
	made, _ := rooms.make("pw", "", "")
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

// A track's cover, sent by the host, listed with the room's state and shown
// to whoever has the invite -- an image, and nothing else served as one.
func TestCovers(t *testing.T) {
	rooms := newRooms("pw")
	covers, err := newCovers(filepath.Join(t.TempDir(), "covers"), 1<<20)
	if err != nil {
		t.Fatal(err)
	}
	served := httptest.NewServer((&server{rooms: rooms, tracks: keptIn(t), covers: covers, wait: 100 * time.Millisecond}).handler())
	defer served.Close()
	c := client{t, served.URL}
	made, _ := rooms.make("pw", "", "")
	cover := "/rooms/" + made.Room + "/covers/entry-1"
	jpeg := "\xff\xd8\xff\xe0\x00\x10JFIF\x00" + strings.Repeat("x", 200)

	if status, _ := c.do("PUT", cover, made.Invite, jpeg); status != 403 {
		t.Fatal("a listener sends no covers")
	}
	if status, _ := c.do("PUT", cover, made.HostKey, "<html><script>alert(1)</script>"); status != 400 {
		t.Fatalf("a cover is an image: %d", status)
	}
	if status, _ := c.do("PUT", cover, made.HostKey, jpeg); status != 200 {
		t.Fatal("the host sends one")
	}
	if _, state := c.do("GET", "/rooms/"+made.Room+"/state?after=0", made.Invite, ""); len(state["covers"].([]any)) != 1 || state["covers"].([]any)[0] != "entry-1" {
		t.Fatalf("listed with the room: %v", state["covers"])
	}
	response, err := http.Get(served.URL + cover + "?key=" + made.Invite)
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != 200 || response.Header.Get("Content-Type") != "image/jpeg" ||
		response.Header.Get("X-Content-Type-Options") != "nosniff" {
		t.Fatalf("shown as what it is: %d %s", response.StatusCode, response.Header.Get("Content-Type"))
	}
	if status, _ := c.do("DELETE", "/rooms/"+made.Room, made.HostKey, ""); status != 200 {
		t.Fatal("closed")
	}
	if len(covers.names(made.Room)) != 0 {
		t.Fatal("a closed room's covers go")
	}
}

// A reaction: one of a few emoji, floated up on every page.
func TestReactions(t *testing.T) {
	rooms := newRooms("pw")
	served := httptest.NewServer((&server{rooms: rooms, tracks: keptIn(t), wait: 100 * time.Millisecond}).handler())
	defer served.Close()
	c := client{t, served.URL}
	made, _ := rooms.make("pw", "", "")
	react := "/rooms/" + made.Room + "/react"
	if status, _ := c.do("POST", react, made.Invite, `{"name":"Ana","emoji":"<b>"}`); status != 400 {
		t.Fatal("only the room's emoji")
	}
	if status, _ := c.do("POST", react, made.Invite, `{"name":"","emoji":"🔥"}`); status != 400 {
		t.Fatal("from someone")
	}
	for range keptReactions + 3 {
		if status, _ := c.do("POST", react, made.Invite, `{"name":"Ana","emoji":"🔥"}`); status != 200 {
			t.Fatal("a reaction")
		}
	}
	_, state := c.do("GET", "/rooms/"+made.Room+"/state?after=0", made.Invite, "")
	kept := state["reactions"].([]any)
	if len(kept) != keptReactions || kept[0].(map[string]any)["emoji"] != "🔥" ||
		kept[len(kept)-1].(map[string]any)["id"].(float64) != keptReactions+3 {
		t.Fatalf("the last few, oldest first: %v", kept)
	}
}
