// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick

// Pretend engine state. Everything here is local and fake.
QtObject {
    id: app

    property int playAlbum: 0
    property int playTrack: 7
    property bool playing: true
    property real elapsed: 87
    property real volume: 0.7
    property bool upNextOpen: true
    property bool repeat: false
    property bool shuffle: false
    property bool single: false
    property bool consume: false
    property int replayGain: 2
    readonly property var replayGainNames: ["Off", "Track", "Album"]
    property string selectionText: "Library selection loaded."

    readonly property var album: MockData.albums[playAlbum]
    readonly property var track: album.tracks[playTrack]

    property var tabs: [
        { name: "Local", engine: "This computer", dirty: true, albums: range(1, 24) },
        { name: "gemenon", engine: "gemenon", dirty: true, albums: [0] },
        { name: "Everything", engine: "gemenon", dirty: false, albums: range(0, MockData.albums.length) }
    ]
    property int currentTab: 1

    property ListModel queue: ListModel {}

    property Timer ticker: Timer {
        interval: 250
        repeat: true
        running: app.playing
        onTriggered: {
            app.elapsed += 0.25;
            if (app.elapsed >= app.track.len)
                app.next();
        }
    }

    function range(from, to) {
        const out = [];
        for (let i = from; i < to; ++i)
            out.push(i);
        return out;
    }

    function play(a, t) {
        playAlbum = a;
        playTrack = t;
        elapsed = 0;
        playing = true;
    }

    function next() {
        if (queue.count > 0) {
            const entry = queue.get(0);
            const a = entry.a, t = entry.t;
            queue.remove(0);
            play(a, t);
        } else if (playTrack + 1 < album.tracks.length) {
            play(playAlbum, playTrack + 1);
        } else {
            play((playAlbum + 1) % MockData.albums.length, 0);
        }
    }

    function previous() {
        if (elapsed > 3 || playTrack === 0)
            elapsed = 0;
        else
            play(playAlbum, playTrack - 1);
    }

    function enqueue(a, t) {
        queue.append({ a: a, t: t });
    }

    function enqueueAlbum(a) {
        for (let t = 0; t < MockData.albums[a].tracks.length; ++t)
            enqueue(a, t);
    }

    function updateTab(index, change) {
        const copy = tabs.slice();
        copy[index] = Object.assign({}, copy[index], change);
        tabs = copy;
    }

    function appendAlbum(a) {
        updateTab(currentTab, { albums: tabs[currentTab].albums.concat([a]), dirty: true });
    }

    function newTab() {
        tabs = tabs.concat([{ name: "New list", engine: "This computer", dirty: false, albums: [] }]);
        currentTab = tabs.length - 1;
    }

    function closeTab(index) {
        if (tabs.length === 1)
            return;
        const copy = tabs.slice();
        copy.splice(index, 1);
        if (currentTab >= index && currentTab > 0)
            currentTab -= 1;
        tabs = copy;
    }

    Component.onCompleted: {
        enqueue(8, 2);
        enqueue(8, 3);
        enqueue(2, 0);
        enqueue(5, 4);
    }
}
