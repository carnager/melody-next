// SPDX-License-Identifier: GPL-3.0-only

// melody-together is the server friends listen together through (ADR-0302):
// run by one of them, behind the web proxy that gives it TLS. One static
// binary, nothing else to install.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

func main() {
	listen := flag.String("listen", "127.0.0.1:6099", "where to listen: a web proxy in front gives it TLS and a name")
	passwordFile := flag.String("password-file", "", "the password that makes rooms; whoever hosts needs it, listeners need only an invite")
	data := flag.String("data", defaultData(), "where rooms and the copies of their tracks are kept, to outlive a restart")
	filesMB := flag.Int64("files-mb", 2048, "the most the kept tracks may take, in MB")
	flag.Parse()
	if *passwordFile == "" {
		fmt.Fprintln(os.Stderr, "melody-together: -password-file is needed")
		flag.Usage()
		os.Exit(2)
	}
	read, err := os.ReadFile(*passwordFile)
	password := strings.TrimSpace(string(read))
	if err != nil || password == "" {
		fmt.Fprintln(os.Stderr, "melody-together: the password file is missing or empty")
		os.Exit(1)
	}

	if err := os.MkdirAll(*data, 0o700); err != nil {
		fmt.Fprintln(os.Stderr, "melody-together: the data folder:", err)
		os.Exit(1)
	}
	rooms := newRooms(password)
	roomsFile := filepath.Join(*data, "rooms.json")
	if err := rooms.restore(roomsFile); err != nil {
		fmt.Fprintln(os.Stderr, "melody-together: the rooms kept:", err)
		os.Exit(1)
	}
	kept, err := newTracks(filepath.Join(*data, "tracks"), *filesMB<<20)
	if err != nil {
		fmt.Fprintln(os.Stderr, "melody-together: the folder for tracks:", err)
		os.Exit(1)
	}
	// Stopping ends whoever waits on a long poll at once.
	stopping, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	served := &http.Server{
		Handler: (&server{rooms: rooms, tracks: kept, wait: 25 * time.Second}).handler(),
		// A track's upload or download takes as long as the line allows.
		ReadTimeout:       0,
		ReadHeaderTimeout: 10 * time.Second,
		BaseContext:       func(net.Listener) context.Context { return stopping },
	}
	listener, err := net.Listen("tcp", *listen)
	if err != nil {
		fmt.Fprintln(os.Stderr, "melody-together:", err)
		os.Exit(1)
	}
	go func() {
		sweeping := time.NewTicker(30 * time.Second)
		keeping := time.NewTicker(time.Second)
		defer sweeping.Stop()
		defer keeping.Stop()
		for {
			select {
			case <-sweeping.C:
				for _, gone := range rooms.sweep() {
					kept.forget(gone)
				}
				kept.sweep()
			case <-keeping.C:
				if err := rooms.keep(roomsFile); err != nil {
					fmt.Fprintln(os.Stderr, "melody-together: keeping the rooms:", err)
				}
			case <-stopping.Done():
				return
			}
		}
	}()
	go func() {
		<-stopping.Done()
		ended, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = served.Shutdown(ended)
	}()
	fmt.Fprintln(os.Stderr, "melody-together: listening on", listener.Addr())
	if err := served.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
		fmt.Fprintln(os.Stderr, "melody-together:", err)
		os.Exit(1)
	}
	// Stopped: what changed in the last second kept too.
	if err := rooms.keep(roomsFile); err != nil {
		fmt.Fprintln(os.Stderr, "melody-together: keeping the rooms:", err)
	}
}

// $XDG_STATE_HOME/melody-together, else ~/.local/state/melody-together.
func defaultData() string {
	if state := os.Getenv("XDG_STATE_HOME"); state != "" {
		return filepath.Join(state, "melody-together")
	}
	if home, err := os.UserHomeDir(); err == nil {
		return filepath.Join(home, ".local", "state", "melody-together")
	}
	return "melody-together"
}
