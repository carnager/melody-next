// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    readonly property var rows: MockData.rowsFor(App.tabs[App.currentTab].albums)
    property var selected: ({})
    property int selectionRevision: 0
    property int anchorRow: -1

    readonly property int gutter: 66
    readonly property int numberWidth: 26
    readonly property int ratingWidth: 86
    readonly property int lengthWidth: 56

    onRowsChanged: clearSelection()

    function clearSelection() {
        selected = {};
        anchorRow = -1;
        ++selectionRevision;
        summarize();
    }

    function select(row, modifiers) {
        const extend = modifiers & Qt.ShiftModifier;
        const toggle = modifiers & Qt.ControlModifier;
        if (extend && anchorRow >= 0) {
            if (!toggle)
                selected = {};
            const lo = Math.min(anchorRow, row), hi = Math.max(anchorRow, row);
            for (let i = lo; i <= hi; ++i)
                if (rows[i].kind === "track")
                    selected[i] = true;
        } else if (toggle) {
            selected[row] = !selected[row];
            anchorRow = row;
        } else {
            selected = {};
            selected[row] = true;
            anchorRow = row;
        }
        ++selectionRevision;
        summarize();
    }

    function selectAlbum(headerRow) {
        selected = {};
        let i = headerRow + 1;
        for (; i < rows.length && rows[i].kind === "track"; ++i)
            selected[i] = true;
        anchorRow = headerRow + 1;
        ++selectionRevision;
        summarize();
    }

    function summarize() {
        const picked = Object.keys(selected).filter(k => selected[k]).map(Number);
        if (picked.length === 0) {
            App.selectionText = rows.length === 0 ? "Empty list" : "Nothing selected";
        } else if (picked.length === 1) {
            const r = rows[picked[0]], album = MockData.albums[r.a], track = album.tracks[r.t];
            App.selectionText = album.artist + " — " + track.title + " · " + album.title
                              + " (" + album.year + ") · " + MockData.duration(track.len);
        } else {
            const total = picked.reduce((sum, i) => sum + MockData.albums[rows[i].a].tracks[rows[i].t].len, 0);
            App.selectionText = picked.length + " tracks selected · " + MockData.duration(total);
        }
    }

    function step(delta, modifiers) {
        let row = anchorRow < 0 ? 0 : anchorRow;
        do {
            row += delta;
        } while (row >= 0 && row < rows.length && rows[row].kind !== "track");
        if (row < 0 || row >= rows.length)
            return;
        const keepAnchor = anchorRow;
        select(row, modifiers & Qt.ShiftModifier ? Qt.ShiftModifier : Qt.NoModifier);
        if (modifiers & Qt.ShiftModifier)
            anchorRow = keepAnchor;
        else
            anchorRow = row;
        view.positionViewAtIndex(row, ListView.Contain);
    }

    color: Theme.base

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Tabs
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 34
            color: Theme.panel

            Row {
                id: tabRow
                anchors.left: parent.left
                anchors.leftMargin: 6
                anchors.bottom: parent.bottom
                spacing: 2

                Repeater {
                    model: App.tabs
                    delegate: Rectangle {
                        id: tab
                        required property var modelData
                        required property int index
                        readonly property bool current: App.currentTab === index

                        width: tabLabel.implicitWidth + 50
                        height: 30
                        radius: 5
                        color: current ? Theme.base : tabHover.hovered ? Theme.hover : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        // Square off the bottom corners so the tab joins the list.
                        Rectangle {
                            visible: tab.current
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 6
                            color: Theme.base
                        }
                        HoverHandler { id: tabHover }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                            onClicked: mouse => {
                                if (mouse.button === Qt.MiddleButton)
                                    App.closeTab(tab.index);
                                else
                                    App.currentTab = tab.index;
                            }
                        }
                        Rectangle {
                            id: engineDot
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: 7
                            height: 7
                            radius: 3.5
                            color: tab.modelData.engine === "gemenon" ? Theme.accent : "#6cc28b"
                        }
                        Text {
                            id: tabLabel
                            anchors.left: engineDot.right
                            anchors.leftMargin: 7
                            anchors.verticalCenter: parent.verticalCenter
                            text: tab.modelData.name + (tab.modelData.dirty ? " *" : "")
                            color: tab.current ? Theme.text : Theme.dim
                            font.pixelSize: Theme.fontSize
                        }
                        IconButton {
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            icon: "close"
                            iconSize: 11
                            opacity: tab.current || tabHover.hovered ? 1 : 0
                            onClicked: App.closeTab(tab.index)
                        }
                    }
                }

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    icon: "plus"
                    iconSize: 13
                    tip: "New list"
                    onClicked: App.newTab()
                }
            }
        }

        // Column header
        Item {
            Layout.fillWidth: true
            implicitHeight: 26

            Text {
                x: pane.gutter
                width: pane.numberWidth
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: "#"
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
            }
            Text {
                x: pane.gutter + pane.numberWidth + 10
                anchors.verticalCenter: parent.verticalCenter
                text: "Title"
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
            }
            Text {
                x: parent.width - pane.lengthWidth - pane.ratingWidth - 12
                anchors.verticalCenter: parent.verticalCenter
                text: "Rating"
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
            }
            Text {
                x: parent.width - pane.lengthWidth - 12
                width: pane.lengthWidth
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: "Length"
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.line
            }
        }

        ListView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: pane.rows
            reuseItems: true
            cacheBuffer: 400
            boundsBehavior: Flickable.StopAtBounds
            topMargin: 6
            bottomMargin: 12
            ScrollBar.vertical: ScrollBar {}

            Keys.onUpPressed: event => pane.step(-1, event.modifiers)
            Keys.onDownPressed: event => pane.step(1, event.modifiers)
            Keys.onReturnPressed: {
                const r = pane.rows[pane.anchorRow];
                if (r && r.kind === "track")
                    App.play(r.a, r.t);
            }

            delegate: Item {
                id: row
                required property var modelData
                required property int index
                readonly property bool isHeader: modelData.kind === "album"
                readonly property var album: MockData.albums[modelData.a]
                readonly property var track: isHeader ? null : album.tracks[modelData.t]
                readonly property bool isSelected: pane.selectionRevision >= 0 && !!pane.selected[index]
                readonly property bool isPlaying: !isHeader && App.playAlbum === modelData.a && App.playTrack === modelData.t

                width: ListView.view.width
                height: isHeader ? 30 : Theme.rowHeight
                // The album cover hangs into the rows below its header.
                z: isHeader ? 2 : 1

                HoverHandler { id: rowHover }
                MouseArea {
                    anchors.fill: parent
                    onClicked: mouse => {
                        view.forceActiveFocus();
                        if (row.isHeader)
                            pane.selectAlbum(row.index);
                        else
                            pane.select(row.index, mouse.modifiers);
                    }
                    onDoubleClicked: {
                        if (!row.isHeader)
                            App.play(row.modelData.a, row.modelData.t);
                    }
                }

                Rectangle {
                    visible: !row.isHeader
                    x: pane.gutter - 6
                    width: parent.width - x - 6
                    height: parent.height
                    radius: 3
                    color: row.isSelected ? Theme.selection : row.isPlaying ? Theme.playingTint
                         : rowHover.hovered ? Theme.hover : "transparent"
                    border.color: row.isSelected || row.isPlaying ? Theme.playingLine : "transparent"
                }

                // Album header
                Cover {
                    visible: row.isHeader
                    x: 12
                    y: 6
                    width: 44
                    height: 44
                    album: row.album
                }
                Row {
                    visible: row.isHeader
                    x: pane.gutter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    spacing: 10
                    Text {
                        text: row.album.title
                        color: Theme.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Text {
                        anchors.baseline: parent.children[0].baseline
                        text: row.album.artist + " · " + row.album.year + " · " + row.album.tracks.length
                              + " tracks · " + MockData.duration(MockData.albumLength(row.album))
                        color: Theme.dim
                        font.pixelSize: Theme.smallFontSize + 1
                    }
                }

                // Track row
                Item {
                    visible: !row.isHeader
                    x: pane.gutter
                    width: pane.numberWidth
                    height: parent.height
                    Text {
                        anchors.fill: parent
                        visible: !row.isPlaying
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: row.track ? row.track.n : ""
                        color: Theme.faint
                        font.pixelSize: Theme.fontSize
                    }
                    Icon {
                        visible: row.isPlaying
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        height: 12
                        name: App.playing ? "play" : "pause"
                        color: Theme.accent
                    }
                }
                Text {
                    visible: !row.isHeader
                    x: pane.gutter + pane.numberWidth + 10
                    width: parent.width - x - pane.ratingWidth - pane.lengthWidth - 24
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: row.track ? row.track.title : ""
                    color: row.isPlaying || row.isSelected ? Qt.lighter(Theme.accent, 1.25) : Theme.text
                    font.pixelSize: Theme.fontSize
                    elide: Text.ElideRight
                }
                Row {
                    visible: !row.isHeader && (row.track.rating > 0 || rowHover.hovered)
                    x: parent.width - pane.lengthWidth - pane.ratingWidth - 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Repeater {
                        model: 5
                        delegate: Text {
                            required property int index
                            text: "★"
                            color: row.track && index < row.track.rating ? Theme.star : Theme.line
                            font.pixelSize: 12
                        }
                    }
                }
                Text {
                    visible: !row.isHeader
                    x: parent.width - pane.lengthWidth - 12
                    width: pane.lengthWidth
                    height: parent.height
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                    text: row.track ? MockData.duration(row.track.len) : ""
                    color: row.isPlaying || row.isSelected ? Qt.lighter(Theme.accent, 1.25) : Theme.dim
                    font.pixelSize: Theme.fontSize
                    font.features: { "tnum": 1 }
                }
            }

            Column {
                anchors.centerIn: parent
                visible: pane.rows.length === 0
                spacing: 6
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "This list is empty"
                    color: Theme.text
                    font.pixelSize: 15
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Double-click an album in the library to add it."
                    color: Theme.dim
                    font.pixelSize: Theme.fontSize
                }
            }
        }
    }
}
