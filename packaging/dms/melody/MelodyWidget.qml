import QtQuick
import Quickshell
import Quickshell.Io
import qs.Common
import qs.Modules.Plugins
import qs.Services
import qs.Widgets

// What Melody plays, from `melody-cli --json watch`: a line for each change.
// The buttons are melody-cli commands too.
PluginComponent {
    id: root

    property string cli: String(pluginData.cli || "melody-cli").trim() || "melody-cli"
    property string engine: String(pluginData.engine || "").trim()
    property string password: String(pluginData.password || "")
    property int maxWidthSetting: Math.max(0, parseInt(String(pluginData.maxWidth || "320"), 10) || 0)

    // The engine the last line was about, when following all of them: what
    // the buttons act on.
    property string shownServer: ""
    property bool connected: false
    property string status: "stopped"
    property var track: null
    // 0-10, as the engine keeps it; the heart is five stars. A click shows
    // at once, for that track, until the engine reports it back.
    property int ratingShown: -1
    property string ratingShownFor: ""
    readonly property int rating: ratingShown >= 0 && track && track.path === ratingShownFor
        ? ratingShown
        : (track && track.rating !== undefined ? track.rating : 0)
    readonly property bool hearted: rating >= 10
    readonly property bool rateable: track !== null && track.rating_hash !== undefined
    readonly property bool playing: status === "playing"
    readonly property string displayText: {
        if (!connected)
            return "Melody";
        if (!track)
            return "Nothing playing";
        return track.artist ? track.artist + " — " + track.title : track.title;
    }

    // Which engine: a name, HOST:PORT or a socket path -- or, with none
    // set, whichever one plays, as the watcher last said.
    function engineArguments() {
        const args = [];
        if (engine.length === 0 && shownServer.length > 0) {
            args.push("--server", shownServer);
        } else if (engine.length > 0) {
            if (engine.indexOf(":") >= 0 || engine.startsWith("/"))
                args.push("--server", engine);
            else
                args.push("--engine", engine);
        }
        if (password.length > 0)
            args.push("--password", password);
        return args;
    }

    // Commands run one after another, so "rate, then love" keeps its order.
    property var pending: []

    function run(words) {
        pending = pending.concat([[cli].concat(engineArguments(), words)]);
        runNext();
    }

    function runNext() {
        if (action.running || pending.length === 0)
            return;
        action.command = pending[0];
        pending = pending.slice(1);
        action.running = true;
    }

    function toggleHeart() {
        if (!rateable)
            return;
        ratingShownFor = track.path;
        if (hearted) {
            ratingShown = 0;
            run(["rate", "0"]);
            run(["unlove"]);
        } else {
            ratingShown = 10;
            run(["rate", "5"]);
            run(["love"]);
        }
    }

    function startWatcher() {
        shownServer = "";
        watcher.command = engine.length > 0
            ? [cli].concat(engineArguments(), ["--json", "watch"])
            : [cli].concat(password.length > 0 ? ["--password", password] : [], ["--json", "watch", "--all"]);
        watcher.running = true;
    }

    Component.onCompleted: startWatcher()

    onPluginDataChanged: {
        watcher.running = false;
        restartTimer.restart();
    }

    Process {
        id: watcher

        running: false

        stdout: SplitParser {
            splitMarker: "\n"
            onRead: data => {
                try {
                    const state = JSON.parse(String(data));
                    root.connected = true;
                    root.status = String(state.status || "stopped");
                    root.track = state.track || null;
                    root.shownServer = state.engine && state.engine.server ? String(state.engine.server) : "";
                } catch (error) {
                }
            }
        }

        onExited: {
            root.connected = false;
            root.track = null;
            restartTimer.restart();
        }
    }

    // Engine gone or not started yet: try again shortly.
    Timer {
        id: restartTimer

        interval: 3000
        onTriggered: {
            if (!watcher.running)
                root.startWatcher();
        }
    }

    Process {
        id: action

        running: false

        stderr: SplitParser {
            splitMarker: "\n"
            onRead: data => {
                const text = String(data || "").replace(/^melody-cli: /, "").trim();
                if (text.length > 0)
                    ToastService.showError("Melody: " + text);
            }
        }

        onExited: root.runNext()
    }

    component ControlButton: Rectangle {
        id: button

        property string icon: ""
        property int iconSize: 12
        property color iconColor: Theme.widgetTextColor
        property bool filled: false
        signal activated()

        width: 20
        height: 20
        radius: width / 2
        color: area.containsMouse ? Theme.widgetBaseHoverColor : "transparent"
        opacity: enabled ? 1 : 0.35

        DankIcon {
            anchors.centerIn: parent
            name: button.icon
            size: button.iconSize
            filled: button.filled
            color: button.iconColor
        }

        MouseArea {
            id: area

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.activated()
        }
    }

    horizontalBarPill: Component {
        Item {
            id: pill

            // The heart, then previous, play and next, and the gaps between.
            readonly property int fixedWidth: 20 + (20 + 2 + 24 + 2 + 20) + 2 * Theme.spacingXS
            readonly property int textWidth: Math.ceil(measure.implicitWidth) + 2
            readonly property int shownTextWidth: root.maxWidthSetting > 0
                ? Math.max(60, Math.min(textWidth, root.maxWidthSetting - fixedWidth))
                : textWidth

            implicitWidth: fixedWidth + shownTextWidth
            implicitHeight: row.implicitHeight

            Row {
                id: row

                spacing: Theme.spacingXS
                anchors.verticalCenter: parent.verticalCenter

                ControlButton {
                    icon: "favorite"
                    filled: root.hearted
                    iconColor: root.hearted ? Theme.primary : Theme.widgetTextColor
                    enabled: root.rateable
                    anchors.verticalCenter: parent.verticalCenter
                    onActivated: root.toggleHeart()
                }

                StyledText {
                    width: pill.shownTextWidth
                    text: root.displayText
                    font.pixelSize: Theme.barTextSize(root.barThickness, root.barConfig ? root.barConfig.fontScale : undefined)
                    color: Theme.widgetTextColor
                    opacity: root.playing ? 1 : 0.6
                    elide: Text.ElideRight
                    wrapMode: Text.NoWrap
                    anchors.verticalCenter: parent.verticalCenter
                }

                Row {
                    spacing: 2
                    anchors.verticalCenter: parent.verticalCenter

                    ControlButton {
                        icon: "skip_previous"
                        enabled: root.connected
                        anchors.verticalCenter: parent.verticalCenter
                        onActivated: root.run(["prev"])
                    }

                    Rectangle {
                        width: 24
                        height: 24
                        radius: 12
                        color: root.playing ? Theme.primary : Theme.primaryHover
                        opacity: root.connected ? 1 : 0.35
                        anchors.verticalCenter: parent.verticalCenter

                        DankIcon {
                            anchors.centerIn: parent
                            name: root.playing ? "pause" : "play_arrow"
                            size: 14
                            color: root.playing ? Theme.background : Theme.primary
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.connected
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.run(["toggle"])
                        }
                    }

                    ControlButton {
                        icon: "skip_next"
                        enabled: root.connected
                        anchors.verticalCenter: parent.verticalCenter
                        onActivated: root.run(["next"])
                    }
                }
            }

            StyledText {
                id: measure

                visible: false
                text: root.displayText
                font.pixelSize: Theme.barTextSize(root.barThickness, root.barConfig ? root.barConfig.fontScale : undefined)
                wrapMode: Text.NoWrap
            }
        }
    }

    verticalBarPill: Component {
        Column {
            spacing: 2

            ControlButton {
                icon: "favorite"
                filled: root.hearted
                iconColor: root.hearted ? Theme.primary : Theme.widgetTextColor
                enabled: root.rateable
                anchors.horizontalCenter: parent.horizontalCenter
                onActivated: root.toggleHeart()
            }

            ControlButton {
                icon: root.playing ? "pause" : "play_arrow"
                iconSize: 14
                enabled: root.connected
                anchors.horizontalCenter: parent.horizontalCenter
                onActivated: root.run(["toggle"])
            }
        }
    }
}
