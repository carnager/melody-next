// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// What this window can set. Each engine keeps its own library and
// playback settings; the engines themselves are chosen in Trackknife's
// settings, which this window reads.
Dialog {
    id: dialog

    property var appWindow: null
    // The engine whose library is being set.
    property int libraryEngine: 0
    readonly property var librarySession: Engine.sessions[Math.min(libraryEngine, Engine.sessions.length - 1)]
    readonly property var player: Engine.current.player

    title: "Settings"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(700, parent ? parent.width - 40 : 700)
    height: Math.min(560, parent ? parent.height - 40 : 560)
    standardButtons: Dialog.Close
    onOpened: librarySession.refreshRoots()

    background: Rectangle {
        color: Theme.panel
        radius: 8
        border.color: Theme.line
    }

    FolderPicker { id: folderPicker }

    component Label: Text {
        color: Theme.text
        font.pixelSize: Theme.fontSize
    }
    component Hint: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.dim
        font.pixelSize: Theme.smallFontSize
    }

    component Tab: TabButton {
        id: tab
        contentItem: Text {
            text: tab.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: tab.checked ? Theme.text : Theme.dim
            font.pixelSize: Theme.fontSize
        }
        background: Rectangle {
            color: tab.hovered && !tab.checked ? Theme.hover : "transparent"
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 2
                color: tab.checked ? Theme.accent : "transparent"
            }
        }
    }

    header: ColumnLayout {
        spacing: 0
        Text {
            Layout.margins: 14
            Layout.bottomMargin: 6
            text: dialog.title
            color: Theme.text
            font.pixelSize: 15
            font.weight: Font.DemiBold
        }
        TabBar {
            id: tabs
            Layout.fillWidth: true
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.line
                }
            }
            Tab { text: "Library" }
            Tab { text: "Playback" }
            Tab { text: "Window" }
            Tab { text: "Engines" }
        }
    }

    contentItem: StackLayout {
        currentIndex: tabs.currentIndex

        // Library folders, per engine
        ColumnLayout {
            spacing: 10
            RowLayout {
                Label { text: "Library of" }
                ComboBox {
                    Layout.preferredWidth: 240
                    model: Engine.sessions.map(session => session.name)
                    currentIndex: dialog.libraryEngine
                    onActivated: index => {
                        dialog.libraryEngine = index;
                        dialog.librarySession.refreshRoots();
                    }
                }
            }
            Hint {
                text: (dialog.librarySession.local ? "Folders on this computer"
                                                   : "Folders on " + dialog.librarySession.name + "'s machine")
                      + ". Adding or removing one takes effect "
                      + "at once; a scan brings the library up to date with what is in them."
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.base
                radius: 5
                border.color: Theme.line
                ListView {
                    anchors.fill: parent
                    anchors.margins: 4
                    clip: true
                    model: dialog.librarySession.roots
                    delegate: Rectangle {
                        id: root
                        required property var modelData
                        width: ListView.view.width
                        height: 34
                        radius: 4
                        color: rootHover.hovered ? Theme.hover : "transparent"
                        HoverHandler { id: rootHover }
                        Icon {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 15
                            height: 15
                            name: "folder"
                            color: root.modelData.available ? Theme.dim : Theme.error
                        }
                        Column {
                            x: 32
                            width: parent.width - x - 40
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                width: parent.width
                                text: root.modelData.name
                                elide: Text.ElideMiddle
                                color: Theme.text
                                font.pixelSize: Theme.fontSize
                            }
                            Text {
                                visible: !root.modelData.available || root.modelData.error !== ""
                                width: parent.width
                                text: root.modelData.error !== "" ? root.modelData.error : "Not reachable"
                                elide: Text.ElideRight
                                color: Theme.error
                                font.pixelSize: Theme.smallFontSize
                            }
                        }
                        IconButton {
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            icon: "close"
                            iconSize: 11
                            tip: "Remove from the library"
                            opacity: rootHover.hovered ? 1 : 0
                            onClicked: dialog.librarySession.removeRoot(root.modelData.path)
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: parent.count === 0
                        text: dialog.librarySession.connected ? "No folders yet" : "Not connected"
                        color: Theme.dim
                        font.pixelSize: Theme.fontSize
                    }
                }
            }
            RowLayout {
                Button {
                    text: "Add folder…"
                    enabled: dialog.librarySession.connected
                    onClicked: {
                        const session = dialog.librarySession;
                        folderPicker.choose(session, path => session.addRoot(path));
                    }
                }
                Item { Layout.fillWidth: true }
                Text {
                    Layout.maximumWidth: 320
                    text: dialog.librarySession.scanProgress
                    elide: Text.ElideRight
                    color: Theme.dim
                    font.pixelSize: Theme.smallFontSize
                }
                Button {
                    text: dialog.librarySession.scanning ? "Stop" : "Scan now"
                    enabled: dialog.librarySession.connected
                    onClicked: dialog.librarySession.scanning ? dialog.librarySession.cancelScan()
                                                               : dialog.librarySession.scan()
                }
            }
        }

        // Playback, on the current engine
        ColumnLayout {
            spacing: 12
            Hint {
                text: "For " + Engine.current.name + ", the engine of the list on show. Each engine keeps its own."
            }
            RowLayout {
                spacing: 12
                Label { text: "ReplayGain" }
                Repeater {
                    model: [["off", "Off"], ["track", "Track"], ["album", "Album"]]
                    delegate: RadioButton {
                        required property var modelData
                        text: modelData[1]
                        checked: dialog.player.replayGain === modelData[0]
                        onClicked: dialog.player.setReplayGainMode(modelData[0])
                        palette.windowText: Theme.text
                    }
                }
            }
            GridLayout {
                columns: 3
                columnSpacing: 12
                Label { text: "Preamp with ReplayGain" }
                TkSlider {
                    id: withGain
                    Layout.preferredWidth: 280
                    from: -15
                    to: 15
                    stepSize: 0.5
                    Binding on value {
                        when: !withGain.pressed
                        value: dialog.player.preampWithGain
                        restoreMode: Binding.RestoreNone
                    }
                    onPressedChanged: {
                        if (!pressed)
                            dialog.player.setPreamps(value, withoutGain.value);
                    }
                }
                Label { text: (withGain.value > 0 ? "+" : "") + withGain.value.toFixed(1) + " dB" }
                Label { text: "Preamp without" }
                TkSlider {
                    id: withoutGain
                    Layout.preferredWidth: 280
                    from: -15
                    to: 15
                    stepSize: 0.5
                    Binding on value {
                        when: !withoutGain.pressed
                        value: dialog.player.preampWithoutGain
                        restoreMode: Binding.RestoreNone
                    }
                    onPressedChanged: {
                        if (!pressed)
                            dialog.player.setPreamps(withGain.value, value);
                    }
                }
                Label { text: (withoutGain.value > 0 ? "+" : "") + withoutGain.value.toFixed(1) + " dB" }
            }
            Hint {
                text: "Tracks without ReplayGain are played at the second level, so they do not jump out between "
                      + "ones that have it."
            }
            Item { Layout.fillHeight: true }
        }

        // This window
        ColumnLayout {
            spacing: 10
            RowLayout {
                spacing: 12
                Label { text: "Appearance" }
                Repeater {
                    model: ["As the system", "Light", "Dark"]
                    delegate: RadioButton {
                        required property string modelData
                        required property int index
                        text: modelData
                        checked: dialog.appWindow && dialog.appWindow.appearance === index
                        onClicked: dialog.appWindow.appearance = index
                        palette.windowText: Theme.text
                    }
                }
            }
            CheckBox {
                text: "Notify on each new track"
                checked: dialog.appWindow && dialog.appWindow.notifications
                onClicked: dialog.appWindow.notifications = checked
                palette.windowText: Theme.text
            }
            CheckBox {
                leftPadding: 28
                text: "Only while this window is in the background"
                enabled: dialog.appWindow && dialog.appWindow.notifications
                checked: dialog.appWindow && dialog.appWindow.notifyInBackgroundOnly
                onClicked: dialog.appWindow.notifyInBackgroundOnly = checked
                palette.windowText: Theme.text
            }
            CheckBox {
                text: "Keep what plays in view"
                checked: dialog.appWindow && dialog.appWindow.followPlayback
                onClicked: dialog.appWindow.followPlayback = checked
                palette.windowText: Theme.text
            }
            Item { Layout.fillHeight: true }
        }

        // Engines
        ColumnLayout {
            spacing: 10
            Repeater {
                model: Engine.sessions
                delegate: RowLayout {
                    required property var modelData
                    spacing: 10
                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: parent.modelData.connected ? "#6cc28b" : Theme.error
                    }
                    Column {
                        Label { text: parent.parent.modelData.name }
                        Text {
                            text: (parent.parent.modelData.local ? "This computer's engine"
                                                                 : parent.parent.modelData.key)
                                  + (parent.parent.modelData.connected ? "" : " · " + parent.parent.modelData.failure)
                            color: Theme.dim
                            font.pixelSize: Theme.smallFontSize
                        }
                    }
                }
            }
            Hint {
                Layout.topMargin: 8
                text: "Engines elsewhere are chosen in Trackknife's settings (Settings → Engine), which this window "
                      + "reads when it starts: after a change there, open it again."
            }
            Item { Layout.fillHeight: true }
        }
    }
}
