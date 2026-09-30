// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// The "Transport" toolbar (bench-transport): one row, as players read --
// what is playing on the left, the controls and the position in the middle,
// where the sound goes on the right.
ToolBar {
    id: bar

    // The Up Next dock, which its pill shows and hides.
    property bool upNextShown: false
    signal toggleUpNext()

    readonly property var transport: Tk.transport
    readonly property color ground: palette.window
    readonly property color ink: palette.text

    // bench-player-header: margins 10,6,10,6, spacing 10.
    leftPadding: 10
    rightPadding: 10
    topPadding: 6
    bottomPadding: 6

    contentItem: RowLayout {
        spacing: 10

        // bench-now-playing-cover: 44 x 44, the album's cover or a note.
        Item {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            Accessible.name: "Cover of the current album"
            // Playing, and no cover: the album's tile, as in the list.
            InitialsTile {
                anchors.fill: parent
                visible: (cover.status !== Image.Ready || cover.implicitWidth <= 1)
                         && (bar.transport.album ?? "") !== ""
                name: bar.transport.album ?? ""
            }
            Rectangle {
                anchors.fill: parent
                visible: (cover.status !== Image.Ready || cover.implicitWidth <= 1)
                         && (bar.transport.album ?? "") === ""
                radius: 3
                color: Shade.mix(bar.ground, bar.ink, 0.10)
                Text {
                    anchors.centerIn: parent
                    text: "♪"
                    font.pointSize: Qt.application.font.pointSize * 1.5
                    color: bar.palette.placeholderText
                }
            }
            Image {
                id: cover
                anchors.fill: parent
                asynchronous: false
                fillMode: Image.PreserveAspectFit
                sourceSize: Qt.size(88, 88)
                source: (bar.transport.coverKey ?? "") === "" ? ""
                        : "image://cover/" + encodeURIComponent(bar.transport.coverKey) + "#"
                          + Tk.coverRevision
                visible: status === Image.Ready && implicitWidth > 1
            }
        }

        // bench-track-display: title over "Artist — Album (Year)".
        // A set width: the seek line takes the rest, as long as the window.
        ColumnLayout {
            // Not filling, though its labels do.
            Layout.fillWidth: false
            Layout.minimumWidth: 140
            Layout.preferredWidth: Math.min(260, Math.max(140, bar.width * 0.2))
            spacing: 1
            Item { Layout.fillHeight: true }
            Label {
                objectName: "bench-now-playing"
                Layout.fillWidth: true
                text: bar.transport.title ?? ""
                elide: Text.ElideRight
                font.weight: Font.DemiBold
                font.pointSize: Qt.application.font.pointSize * 1.08
                ToolTip.visible: titleHover.hovered && (bar.transport.tooltip ?? "") !== ""
                ToolTip.text: bar.transport.tooltip ?? ""
                HoverHandler { id: titleHover }
            }
            Label {
                objectName: "bench-now-playing-context"
                Layout.fillWidth: true
                text: bar.transport.context ?? ""
                elide: Text.ElideRight
                color: bar.palette.placeholderText
                ToolTip.visible: contextHover.hovered && (bar.transport.tooltip ?? "") !== ""
                ToolTip.text: bar.transport.tooltip ?? ""
                HoverHandler { id: contextHover }
            }
            Item { Layout.fillHeight: true }
        }

        Item { Layout.preferredWidth: 6 }

        // bench-transport-buttons: previous, play/pause, next; spacing 4.
        // Stop is only in the Playback menu.
        RowLayout {
            spacing: 4
            ToolButton {
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                flat: true
                display: AbstractButton.IconOnly
                enabled: bar.transport.canPrevious ?? false
                text: "Previous"
                icon.source: "image://icon/media-skip-backward|sp:SP_MediaSkipBackward"
                icon.width: 18
                icon.height: 18
                onClicked: Tk.previous()
                ToolTip.visible: hovered
                ToolTip.text: text
            }
            // bench-play: a filled Highlight circle.
            AbstractButton {
                id: play
                objectName: "bench-play"
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                hoverEnabled: true
                enabled: bar.transport.canPlayPause ?? false
                text: bar.transport.playLabel ?? "Play"
                Accessible.name: text
                ToolTip.visible: hovered
                ToolTip.text: text
                onClicked: Tk.playPause()
                background: Rectangle {
                    radius: 18
                    color: play.enabled ? play.palette.highlight
                                        : Shade.mix(bar.ground, bar.ink, 0.12)
                    border.width: play.hovered && play.enabled ? 1 : 0
                    border.color: play.palette.light
                }
                contentItem: Item {
                    Image {
                        anchors.centerIn: parent
                        width: 18
                        height: 18
                        sourceSize: Qt.size(18, 18)
                        source: bar.transport.playing
                                ? "image://icon/media-playback-pause|sp:SP_MediaPause"
                                : "image://icon/media-playback-start|sp:SP_MediaPlay"
                    }
                }
            }
            ToolButton {
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                flat: true
                display: AbstractButton.IconOnly
                enabled: bar.transport.canNext ?? false
                text: "Next"
                icon.source: "image://icon/media-skip-forward|sp:SP_MediaSkipForward"
                icon.width: 18
                icon.height: 18
                onClicked: Tk.next()
                ToolTip.visible: hovered
                ToolTip.text: text
            }
        }

        TextMetrics {
            id: clockWidth
            text: "00:00:00"
        }
        Label {
            Layout.preferredWidth: clockWidth.advanceWidth
            horizontalAlignment: Text.AlignRight
            color: bar.palette.placeholderText
            text: bar.transport.elapsed ?? "0:00"
        }
        // bench-seek: seeks when let go, not while dragged.
        LineSlider {
            id: seek
            objectName: "bench-seek"
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            enabled: bar.transport.seekEnabled ?? false
            from: 0
            to: Math.max(1, bar.transport.durationMs ?? 0)
            Binding on value {
                when: !seek.pressed
                value: bar.transport.positionMs ?? 0
            }
            onPressedChanged: {
                if (!pressed)
                    Tk.seek(value);
            }
        }
        Label {
            Layout.preferredWidth: clockWidth.advanceWidth
            horizontalAlignment: Text.AlignLeft
            color: bar.palette.placeholderText
            text: bar.transport.duration ?? "0:00"
        }

        Item { Layout.preferredWidth: 6 }

        // The volume box: mute, then the level; spacing 4.
        RowLayout {
            spacing: 4
            ToolButton {
                objectName: "bench-mute"
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                flat: true
                checkable: true
                display: AbstractButton.IconOnly
                enabled: bar.transport.engine ?? false
                readonly property bool muted: (bar.transport.volume ?? 0) === 0
                checked: muted
                icon.source: muted ? "image://icon/audio-volume-muted|sp:SP_MediaVolumeMuted"
                                   : "image://icon/audio-volume-high|sp:SP_MediaVolume"
                icon.width: 18
                icon.height: 18
                text: muted ? qsTr("Unmute") : qsTr("Mute")
                ToolTip.visible: hovered
                ToolTip.text: text
                onClicked: Tk.toggleMute()
            }
            LineSlider {
                id: volume
                objectName: "bench-volume"
                Layout.preferredWidth: 90
                from: 0
                to: 100
                stepSize: 1
                enabled: bar.transport.engine ?? false
                ToolTip.visible: hovered
                ToolTip.text: "Volume"
                // What the engine has, including a change another client
                // made -- not while this window's own change is on its way.
                Binding on value {
                    when: !volume.pressed && !(bar.transport.settling ?? false)
                    value: bar.transport.volume ?? 0
                }
                onMoved: Tk.setVolume(Math.round(value))
            }
        }

        // bench-device: where the sound goes, named when there is a choice.
        PillButton {
            id: device
            objectName: "bench-device"
            readonly property string shown: bar.transport.output ?? ""
            readonly property bool named: shown !== ""
            chevron: named
            Layout.preferredWidth: named ? Math.min(implicitWidth, 280 + 60) : 34
            Layout.preferredHeight: 26
            enabled: bar.transport.engine ?? false
            Accessible.name: "Audio output device"
            Accessible.description: bar.transport.outputDescription ?? ""
            ToolTip.visible: hovered
            ToolTip.text: bar.transport.outputTooltip ?? ""
            contentItem: RowLayout {
                spacing: 5
                Image {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    sourceSize: Qt.size(14, 14)
                    source: "image://icon/audio-speakers-symbolic|audio-speakers|sp:SP_ComputerIcon"
                        + (device.enabled ? "" : "?disabled")
                }
                Label {
                    visible: device.named
                    Layout.maximumWidth: 280
                    text: device.shown
                    elide: Text.ElideMiddle
                    font: device.font
                }
            }
            Image {
                objectName: "bench-device-chevron"
                visible: device.named
                anchors.right: parent.right
                anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                width: 10
                height: 10
                sourceSize: Qt.size(10, 10)
                source: "image://icon/pan-down-symbolic|arrow-down|sp:SP_ArrowDown"
            }
            onClicked: deviceMenu.popup(device, 0, device.height)
            OutputMenu { id: deviceMenu }
        }

        // action-up-next: "Up Next" and how many wait.
        PillButton {
            id: upNext
            objectName: "action-up-next"
            readonly property int count: Tk.upNext.count ?? 0
            Layout.preferredHeight: 26
            leftPadding: 11
            rightPadding: 7
            checked: bar.upNextShown
            Accessible.name: "Up Next, %1 waiting".arg(count)
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            contentItem: RowLayout {
                spacing: 6
                Label {
                    text: "Up Next"
                    font: upNext.font
                }
                Rectangle {
                    objectName: "bench-up-next-count"
                    visible: upNext.count > 0
                    implicitHeight: 18
                    implicitWidth: Math.max(18, badge.implicitWidth + 10)
                    radius: 9
                    color: upNext.palette.highlight
                    Label {
                        id: badge
                        anchors.centerIn: parent
                        text: upNext.count
                        color: upNext.palette.highlightedText
                        font.pointSize: Qt.application.font.pointSize * 0.85
                    }
                }
            }
            onClicked: bar.toggleUpNext()
            // Tracks or a library's albums dropped on it join the end of
            // Up Next.
            DropArea {
                id: pillDrop
                anchors.fill: parent
                keys: ["application/x-trackknife-drag"]
                onEntered: drag => drag.accepted = Tk.draggedKind() !== "" && Tk.draggedKind() !== "upnext"
                onDropped: drop => {
                    if (Tk.dropOnUpNext(-1))
                        drop.accept(Qt.CopyAction);
                }
            }
            DropMarker {
                whole: true
                anchors.fill: parent
                visible: pillDrop.containsDrag
            }
        }

    }
}
