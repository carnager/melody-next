// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// Up Next (bench-up-next): the asks waiting to play before the list goes
// on -- a heading, the tracks, and the edits under a hairline with the way
// back to the list.
Pane {
    id: panel

    signal closeRequested()

    readonly property color ground: palette.window
    readonly property color ink: palette.text
    readonly property var rows: Tk.upNext.rows ?? []
    property var selected: []
    property int current: -1

    padding: 0

    function select(row, modifiers) {
        if (modifiers & Qt.ControlModifier) {
            const at = selected.indexOf(row);
            selected = at >= 0 ? selected.filter(each => each !== row) : selected.concat([row]);
        } else if ((modifiers & Qt.ShiftModifier) && current >= 0) {
            const range = [];
            for (let each = Math.min(current, row); each <= Math.max(current, row); ++each)
                range.push(each);
            selected = range;
            return;
        } else {
            selected = [row];
        }
        current = row;
    }
    function edit(operation) {
        Tk.editUpNextRows(selected, operation);
    }
    onRowsChanged: selected = selected.filter(row => row < rows.length)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The heading: margins 12,10,6,8.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.topMargin: 10
            Layout.rightMargin: 6
            Layout.bottomMargin: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: "Up Next"
                    font.weight: Font.DemiBold
                    font.pointSize: Qt.application.font.pointSize * 1.08
                }
                Label {
                    objectName: "up-next-status"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: Tk.upNext.status ?? ""
                    color: panel.palette.placeholderText
                    ToolTip.visible: statusHover.hovered && (Tk.upNext.statusTooltip ?? "") !== ""
                    ToolTip.text: Tk.upNext.statusTooltip ?? ""
                    HoverHandler {
                        id: statusHover
                    }
                }
            }
            ToolButton {
                objectName: "up-next-close"
                Layout.alignment: Qt.AlignTop
                flat: true
                display: AbstractButton.IconOnly
                icon.source: "image://icon/window-close"
                text: "Close Up Next"
                ToolTip.visible: hovered
                ToolTip.text: text
                onClicked: panel.closeRequested()
            }
        }

        // up-next-tracks
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: panel.palette.base
            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: panel.rows
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                focus: true
                Keys.onDeletePressed: panel.edit(1)
                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool chosen: panel.selected.indexOf(index) >= 0
                    width: list.width
                    height: 40
                    Rectangle {
                        anchors.fill: parent
                        color: row.chosen ? Shade.mix(panel.palette.base, panel.palette.highlight, 0.32)
                                          : panel.palette.base
                    }
                    Item {
                        id: area
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                    }
                    Rectangle {
                        id: coverBox
                        x: area.x
                        y: area.y + (area.height - 32) / 2
                        width: 32
                        height: 32
                        radius: 2
                        color: Shade.mix(panel.palette.base, panel.ink, 0.10)
                        clip: true
                        Image {
                            anchors.fill: parent
                            asynchronous: false
                            sourceSize: Qt.size(64, 64)
                            source: (row.modelData.coverKey ?? "") === "" ? ""
                                    : "image://cover/" + encodeURIComponent(row.modelData.coverKey)
                                      + "#" + Tk.coverRevision
                        }
                    }
                    Label {
                        id: length
                        anchors.right: area.right
                        anchors.verticalCenter: area.verticalCenter
                        text: row.modelData.length ?? ""
                        font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                        color: panel.palette.placeholderText
                    }
                    Column {
                        x: coverBox.x + coverBox.width + 10
                        width: Math.max(0, length.x - x - 8)
                        anchors.verticalCenter: area.verticalCenter
                        Label {
                            width: parent.width
                            text: row.modelData.title ?? ""
                            elide: Text.ElideRight
                        }
                        Label {
                            width: parent.width
                            visible: text !== ""
                            text: row.modelData.artist ?? ""
                            elide: Text.ElideRight
                            font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                            color: panel.palette.placeholderText
                        }
                    }
                    // A press on a chosen row keeps the choice, to be dragged.
                    property bool pressPending: false
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onPressedChanged: if (pressed) {
                            list.forceActiveFocus();
                            if (row.chosen && point.modifiers === Qt.NoModifier) {
                                row.pressPending = true;
                            } else {
                                row.pressPending = false;
                                panel.select(row.index, point.modifiers);
                            }
                        }
                        onTapped: {
                            if (row.pressPending)
                                panel.select(row.index, 0);
                            row.pressPending = false;
                        }
                        onCanceled: row.pressPending = false
                        // Activated, an ask plays now.
                        onDoubleTapped: Tk.playUpNextRow(row.index)
                    }
                    // Dragged, the asks chosen move within Up Next.
                    DragSource {
                        label: panel.selected.length === 1 ? qsTr("1 track")
                                                           : qsTr("%1 tracks").arg(panel.selected.length)
                        onBegan: {
                            row.pressPending = false;
                            if (!row.chosen)
                                panel.select(row.index, 0);
                            Tk.dragUpNext();
                        }
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: eventPoint => {
                            if (!row.chosen)
                                panel.select(row.index, 0);
                            menu.row = row.index;
                            menu.popup();
                        }
                    }
                }
                // Tracks and a library's albums dropped here are asked for,
                // before the row under the pointer's upper half.
                DropArea {
                    parent: list
                    anchors.fill: parent
                    keys: ["application/x-trackknife-drag"]
                    property int position: -1
                    function show(y) {
                        const at = Math.floor((y + list.contentY + 20) / 40);
                        position = at >= list.count ? -1 : at;
                        upNextMarker.visible = true;
                        upNextMarker.y = Math.min(at, list.count) * 40 - list.contentY - 1;
                    }
                    onEntered: drag => show(drag.y)
                    onPositionChanged: drag => show(drag.y)
                    onExited: upNextMarker.visible = false
                    onDropped: drop => {
                        upNextMarker.visible = false;
                        let taken = false;
                        if (Tk.draggedKind() === "upnext") {
                            Tk.editUpNextRows(panel.selected, 4,
                                              position < 0 ? list.count : position);
                            taken = true;
                        } else {
                            taken = Tk.dropOnUpNext(position);
                        }
                        if (taken)
                            drop.accept(Qt.CopyAction);
                    }
                }
                DropMarker {
                    id: upNextMarker
                    parent: list
                    x: 4
                    width: list.width - 8
                }
                Column {
                    visible: list.count === 0
                    x: 16
                    width: list.width - 32
                    y: list.height * 2 / 5 - height / 2
                    spacing: 4
                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Nothing waiting")
                        font.weight: Font.DemiBold
                        font.pointSize: Qt.application.font.pointSize * 1.15
                    }
                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("Drag tracks here from a list or the library.")
                        color: panel.palette.placeholderText
                    }
                }
            }
        }

        // up-next-footer: the edits, then the way back to the list.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: footer.implicitHeight + 8
            color: panel.ground
            Rectangle {
                width: parent.width
                height: 1
                color: Shade.mix(panel.ground, panel.ink, 0.12)
            }
            RowLayout {
                id: footer
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 8
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                spacing: 0
                component FooterButton: ToolButton {
                    flat: true
                    display: AbstractButton.IconOnly
                    icon.width: 16
                    icon.height: 16
                    ToolTip.visible: hovered
                    ToolTip.text: text
                }
                readonly property int first: panel.selected.length ? Math.min(...panel.selected) : -1
                readonly property int last: panel.selected.length ? Math.max(...panel.selected) : -1
                FooterButton {
                    objectName: "up-next-remove"
                    text: "Remove from Up Next"
                    icon.source: "image://icon/list-remove"
                    enabled: footer.first >= 0
                    onClicked: panel.edit(1)
                }
                FooterButton {
                    objectName: "up-next-move-up"
                    text: "Move up"
                    icon.source: "image://icon/go-up"
                    enabled: footer.first > 0
                    onClicked: panel.edit(2)
                }
                FooterButton {
                    objectName: "up-next-move-down"
                    text: "Move down"
                    icon.source: "image://icon/go-down"
                    enabled: footer.first >= 0 && footer.last + 1 < panel.rows.length
                    onClicked: panel.edit(3)
                }
                ToolSeparator {}
                FooterButton {
                    objectName: "up-next-clear"
                    text: "Clear pending tracks"
                    icon.source: "image://icon/edit-clear"
                    enabled: panel.rows.length > 0
                    onClicked: Tk.editUpNext(0)
                }
                FooterButton {
                    objectName: "up-next-undo"
                    text: "Undo"
                    icon.source: "image://icon/edit-undo"
                    enabled: Tk.upNext.canUndo ?? false
                    onClicked: Tk.undoUpNext()
                }
                Item {
                    Layout.fillWidth: true
                }
                ToolButton {
                    objectName: "up-next-return"
                    Layout.maximumWidth: panel.width - footer.x - 180
                    flat: true
                    LayoutMirroring.enabled: true
                    icon.source: "image://icon/go-next"
                    text: Tk.upNext.back ?? ""
                    enabled: Tk.upNext.backEnabled ?? false
                    ToolTip.visible: hovered
                    ToolTip.text: Tk.upNext.backTooltip ?? ""
                    onClicked: Tk.returnToList()
                }
            }
        }
    }

    Menu {
        id: menu
        property int row: -1
        MenuItem {
            text: qsTr("Play")
            icon.source: "image://icon/media-playback-start"
            onTriggered: Tk.playUpNextRow(menu.row)
        }
        MenuSeparator {}
        MenuItem {
            text: "Remove from Up Next"
            onTriggered: panel.edit(1)
        }
        MenuItem {
            text: "Move up"
            enabled: footer.first > 0
            onTriggered: panel.edit(2)
        }
        MenuItem {
            text: "Move down"
            enabled: footer.first >= 0 && footer.last + 1 < panel.rows.length
            onTriggered: panel.edit(3)
        }
        MenuSeparator {}
        MenuItem {
            text: "Clear pending tracks"
            onTriggered: Tk.editUpNext(0)
        }
        MenuItem {
            text: "Undo"
            enabled: Tk.upNext.canUndo ?? false
            onTriggered: Tk.undoUpNext()
        }
    }
}
