// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick

// Fake catalogue. A handful of real-looking albums up front, then a few
// hundred generated ones so long lists can be judged for scrolling.
QtObject {
    id: data

    readonly property var albums: build()
    readonly property var folders: [
        { name: "Music", depth: 0 }, { name: "Albums", depth: 1 },
        { name: "[LAW]", depth: 2 }, { name: "Badlands", depth: 2 },
        { name: "Russian Circles", depth: 2 }, { name: "Schandmaul", depth: 2 },
        { name: "Steven Wilson", depth: 2 }, { name: "The Ocean", depth: 2 },
        { name: "Vennart", depth: 2 }, { name: "Wilderun", depth: 2 },
        { name: "Wolf Alice", depth: 2 }, { name: "Compilations", depth: 1 },
        { name: "Singles", depth: 1 }, { name: "Incoming", depth: 0 },
        { name: "Podcasts", depth: 0 }
    ]

    function random(seed) {
        let s = seed >>> 0;
        return function () {
            s = (s + 0x6D2B79F5) >>> 0;
            let t = s;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }

    function build() {
        const rnd = random(20260929);
        const pick = list => list[Math.floor(rnd() * list.length)];
        const words = ["Silent", "Glass", "Northern", "Hollow", "Ember", "Winter", "Paper", "Iron",
                       "Velvet", "Distant", "Broken", "Golden", "Salt", "Lantern", "Echo", "Signal",
                       "Harbour", "Static", "Violet", "Stone", "Mirror", "Tide", "Cinder", "Orbit"];
        const nouns = ["Rivers", "Machines", "Gardens", "Ghosts", "Satellites", "Horses", "Cathedrals",
                       "Tapes", "Wolves", "Lights", "Engines", "Islands", "Letters", "Towers", "Fields"];
        const phrases = ["Under the", "Beyond the", "Songs for", "Nothing but", "Letters to",
                         "Return of the", "Before the", "All the"];

        const seeded = [
            ["In My Head", "[LAW]", 2005, [["Whatever You Say", 238], ["Something in My Head", 204],
             ["You Should Have It All", 179], ["The One", 242], ["Believe", 231], ["Falling Down Again", 245],
             ["Bad Manners", 185], ["The Ordinary", 252], ["Giving Under", 234], ["Gotta Know", 274],
             ["Minx", 205], ["Punch", 227], ["Cumshot", 556]]],
            ["Cover Version", "Steven Wilson", 2014, 12], ["Solaris", "The Ocean", 2018, 12],
            ["Sternensegler", "Schandmaul", 2011, 19], ["Nine", "Russian Circles", 2021, 7],
            ["Hands of Time", "Badlands", 1998, 10], ["The Doors", "The Doors", 1967, 11],
            ["Sea of Dust (single)", "Yuri Gagarin", 2019, 2], ["Blue Weekend", "Wolf Alice", 2021, 11],
            ["Epigone", "Wilderun", 2022, 11], ["Illusion of Choice", "Whoopie Cat", 2020, 8],
            ["The Caravan EP", "Warsong", 2016, 5], ["Forgiveness & The Grain", "Vennart", 2023, 8],
            ["In The Dead, Dead Wood", "Vennart", 2021, 8], ["Copeland EP", "Vennart", 2017, 5],
            ["Perception", "The Sonic Dawn", 2017, 10], ["Second Thoughts", "The Old Dead Tree", 2005, 14],
            ["Songs of Last Resort", "The Haunted", 2025, 12], ["The Estranged", "The Estranged", 2008, 9],
            ["Svartanatt", "Svartanatt", 2016, 10], ["Verde", "Tei Shi", 2015, 5],
            ["Himmelfahrt", "Subway to Sally", 2017, 18], ["Labyrinth of Veins", "Static Abyss", 2024, 10],
            ["To the Bone", "Steven Wilson", 2017, 11]
        ];

        const makeTracks = count => {
            const tracks = [];
            for (let n = 1; n <= count; ++n)
                tracks.push({ n: n, title: rnd() < 0.5 ? pick(words) + " " + pick(nouns)
                                                       : pick(phrases) + " " + pick(nouns),
                              len: 120 + Math.floor(rnd() * 330), rating: rnd() < 0.3 ? 1 + Math.floor(rnd() * 5) : 0 });
            return tracks;
        };

        const result = [];
        for (const s of seeded) {
            const tracks = Array.isArray(s[3])
                ? s[3].map((t, i) => ({ n: i + 1, title: t[0], len: t[1], rating: 0 }))
                : makeTracks(s[3]);
            result.push({ title: s[0], artist: s[1], year: s[2], hue: rnd(), tracks: tracks });
        }
        for (let i = 0; i < 380; ++i) {
            result.push({ title: rnd() < 0.5 ? pick(words) + " " + pick(nouns) : pick(phrases) + " " + pick(words) + " " + pick(nouns),
                          artist: (rnd() < 0.4 ? "The " : "") + pick(words) + " " + pick(nouns),
                          year: 1965 + Math.floor(rnd() * 61), hue: rnd(),
                          tracks: makeTracks(4 + Math.floor(rnd() * 12)) });
        }
        return result;
    }

    function duration(seconds) {
        const total = Math.max(0, Math.floor(seconds));
        const h = Math.floor(total / 3600), m = Math.floor(total / 60) % 60, s = total % 60;
        const mm = h > 0 && m < 10 ? "0" + m : "" + m;
        return (h > 0 ? h + ":" : "") + mm + ":" + (s < 10 ? "0" : "") + s;
    }

    function albumLength(album) {
        return album.tracks.reduce((sum, t) => sum + t.len, 0);
    }

    // One header row per album followed by its tracks.
    function rowsFor(albumIndices) {
        const rows = [];
        for (const a of albumIndices) {
            rows.push({ kind: "album", a: a, t: -1 });
            for (let t = 0; t < albums[a].tracks.length; ++t)
                rows.push({ kind: "track", a: a, t: t });
        }
        return rows;
    }
}
