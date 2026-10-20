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
	files := flag.String("files", filepath.Join(os.TempDir(), "melody-together"), "where the hosts' copies of tracks are kept, an hour each; emptied at start")
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

	rooms := newRooms(password)
	kept, err := newTracks(*files, *filesMB<<20)
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
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				for _, gone := range rooms.sweep() {
					kept.forget(gone)
				}
				kept.sweep()
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
}
