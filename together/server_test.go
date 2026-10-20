// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
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
	served := httptest.NewServer((&server{rooms: rooms, wait: 2 * time.Second}).handler())
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
