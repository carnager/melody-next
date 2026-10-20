// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"crypto/rand"
	"errors"
	"math/big"
	"regexp"
)

// A room's name: what its address says, chosen by its host or made here --
// two words, easy to say over the phone. It is no secret: the invite's key
// is (ADR-0302).

var (
	errBadName   = errors.New("a room's name is 3 to 40 of a-z, 0-9 and -")
	errNameTaken = errors.New("a room of that name is open on this server")
	namePattern  = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$`)
)

var (
	moods = []string{
		"quiet", "loud", "golden", "velvet", "electric", "midnight", "sunday", "rusty",
		"lucky", "dusty", "crimson", "silver", "hazy", "wild", "gentle", "neon",
		"lazy", "cosmic", "smoky", "bright", "deep", "brave", "sleepy", "groovy",
		"humble", "mellow", "stormy", "sunny", "frosty", "secret", "royal", "fuzzy",
	}
	creatures = []string{
		"otter", "blackbird", "fox", "heron", "badger", "moth", "lynx", "raven",
		"wren", "hare", "owl", "beetle", "falcon", "marten", "finch", "toad",
		"swift", "magpie", "salmon", "bison", "gecko", "robin", "wolf", "crane",
		"lark", "seal", "stoat", "kestrel", "newt", "dove", "elk", "puffin",
	}
)

func pick(from []string) (string, error) {
	index, err := rand.Int(rand.Reader, big.NewInt(int64(len(from))))
	if err != nil {
		return "", err
	}
	return from[index.Int64()], nil
}

// A name made of two words, free on this server; `taken` says whether one
// is not.
func madeName(taken func(string) bool) (string, error) {
	for attempt := 0; attempt < 50; attempt++ {
		mood, err := pick(moods)
		if err != nil {
			return "", err
		}
		creature, err := pick(creatures)
		if err != nil {
			return "", err
		}
		name := mood + "-" + creature
		if attempt >= 20 {
			// Most are taken: a number makes one free.
			number, err := rand.Int(rand.Reader, big.NewInt(900))
			if err != nil {
				return "", err
			}
			name += "-" + number.Add(number, big.NewInt(100)).String()
		}
		if !taken(name) {
			return name, nil
		}
	}
	return "", errNameTaken
}
