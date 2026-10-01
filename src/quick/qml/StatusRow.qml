// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// The status bar, as the header's counterpart: a hairline above; the
// selection on the left -- or a passing message in its place -- and the
// modes, a divider and ReplayGain on the right. The modes that are on are
// tinted in the accent.
Pane {
    id: status

    readonly property color ground: palette.window
    readonly property color ink: palette.text
    property string message

    function showMessage(text, timeout) {
        message = text;
        if (timeout > 0)
            messageTimer.interval = timeout;
        messageTimer.running = timeout > 0;
    }

    padding: 0
    topPadding: 1
    Rectangle {
        parent: status.background
        width: parent.width
        height: 1
        color: Shade.mix(status.ground, status.ink, 0.12)
    }
    Timer {
        id: messageTimer
        onTriggered: status.message = ""
    }

    component ModeButton: AbstractButton {
        id: mode
        property var state: ({})
        property string iconName
        implicitWidth: 16 + 6
        implicitHeight: 16 + 6
        padding: 3
        hoverEnabled: true
        checkable: true
        checked: state.checked ?? false
        enabled: Tk.modes.enabled ?? false
        text: state.text ?? ""
        Accessible.name: text
        ToolTip.visible: hovered
        ToolTip.text: state.tooltip ?? ""
        background: Rectangle {
            radius: 4
            color: mode.checked ? Shade.mix(status.ground, mode.palette.highlight, 0.35)
                 : mode.hovered ? Shade.mix(status.ground, status.ink, 0.08) : "transparent"
        }
        contentItem: Image {
            sourceSize: Qt.size(16, 16)
            source: Tk.iconBase + mode.iconName + "|sp:SP_BrowserReload"
                    + (mode.state.oneshot ? "?oneshot" : "")
                    + (mode.enabled ? "" : "?disabled")
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 2
        // bench-selection-status
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            text: status.message !== "" ? status.message : (Tk.selection.text ?? "")
            elide: Text.ElideRight
            color: status.palette.placeholderText
            ToolTip.visible: selectionHover.hovered && status.message === ""
                             && (Tk.selection.tooltip ?? "") !== ""
            ToolTip.text: Tk.selection.tooltip ?? ""
            HoverHandler {
                id: selectionHover
            }
        }
        ModeButton {
            objectName: "bench-local-repeat"
            state: Tk.modes.repeat ?? ({})
            iconName: "media-playlist-repeat"
            onClicked: Tk.setRepeat(checked)
        }
        ModeButton {
            objectName: "bench-local-random"
            state: Tk.modes.random ?? ({})
            iconName: "media-playlist-shuffle"
            onClicked: Tk.setRandom(checked)
        }
        ModeButton {
            objectName: "bench-local-single"
            state: Tk.modes.single ?? ({})
            iconName: "media-playlist-repeat-song"
            onClicked: Tk.cycleSingle()
        }
        ModeButton {
            objectName: "bench-local-album-random"
            state: Tk.modes.albumRandom ?? ({})
            iconName: "tk:album-shuffle"
            onClicked: Tk.setAlbumRandom(checked)
        }
        ModeButton {
            objectName: "bench-local-consume"
            state: Tk.modes.consume ?? ({})
            iconName: "edit-clear-list"
            onClicked: Tk.cycleConsume()
        }
        // bench-status-divider
        Rectangle {
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            implicitWidth: 1
            implicitHeight: 16
            color: Shade.mix(status.ground, status.ink, 0.18)
        }
        // bench-local-replaygain: as quiet as the modes; a gain in use is
        // told by the text's colour.
        AbstractButton {
            id: replaygain
            objectName: "bench-local-replaygain"
            Layout.rightMargin: 4
            hoverEnabled: true
            enabled: Tk.modes.enabled ?? false
            leftPadding: 6
            rightPadding: 6
            topPadding: 3
            bottomPadding: 3
            text: Tk.modes.replaygain ?? ""
            Accessible.name: "ReplayGain mode"
            ToolTip.visible: hovered
            ToolTip.text: Tk.modes.replaygainTooltip ?? ""
            background: Rectangle {
                radius: 4
                color: replaygain.hovered ? Shade.mix(status.ground, status.ink, 0.08)
                                          : "transparent"
            }
            contentItem: Label {
                text: replaygain.text
                color: (Tk.modes.replaygainActive ?? false) ? replaygain.palette.text
                                                            : replaygain.palette.placeholderText
            }
            onClicked: replayGainMenu.popup(replaygain, 0, -replayGainMenu.implicitHeight)
            ReplayGainMenu {
                id: replayGainMenu
            }
        }
    }
}
