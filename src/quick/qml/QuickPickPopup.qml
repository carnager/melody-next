// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// Quick album and Quick track: type words -- every one must appear in its
// artist, title, album or date -- then put it somewhere without reaching
// for the mouse.
Popup {
    id: popup
    objectName: pick && pick.albums ? "bench-quick-album" : "bench-quick-track"

    property QuickPick pick

    x: Math.round((parent.width - width) / 2)
    y: 90
    width: Math.min(640, Math.max(420, parent.width - 80))
    height: 420
    padding: 10
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        input.text = "";
        input.forceActiveFocus();
        results.currentIndex = 0;
        pick.search();
    }
    onClosed: {
        if (pick) {
            pick.release();
            pick = null;
        }
    }

    Connections {
        target: popup.pick
        function onChosen() {
            popup.close();
        }
        function onRowsChanged() {
            results.currentIndex = popup.pick.rows.length > 0 ? 0 : -1;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        RowLayout {
            TextField {
                id: input
                objectName: "bench-quick-album-input"
                Layout.fillWidth: true
                font.pointSize: Qt.application.font.pointSize * 1.1
                placeholderText: popup.pick ? popup.pick.placeholder : ""
                onTextEdited: popup.pick.setText(text)
                Keys.onPressed: event => {
                    const count = results.count;
                    const move = by => {
                        if (count > 0)
                            results.currentIndex = Math.max(0, Math.min(count - 1, results.currentIndex + by));
                        event.accepted = true;
                    };
                    if (event.key === Qt.Key_Down)
                        move(1);
                    else if (event.key === Qt.Key_Up)
                        move(-1);
                    else if (event.key === Qt.Key_PageDown)
                        move(8);
                    else if (event.key === Qt.Key_PageUp)
                        move(-8);
                    else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        popup.pick.choose(results.currentIndex, event.modifiers);
                        event.accepted = true;
                    }
                }
            }
            // Which library is searched, so a remote tab's albums are not
            // expected from this computer's.
            Label {
                objectName: "bench-quick-album-scope"
                text: popup.pick ? popup.pick.scope : ""
                opacity: 0.6
            }
        }
        ListView {
            id: results
            objectName: "bench-quick-album-results"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: popup.pick ? popup.pick.rows : []
            highlightMoveDuration: 0
            delegate: ItemDelegate {
                id: row
                required property var modelData
                required property int index
                width: results.width
                height: 30
                highlighted: ListView.isCurrentItem
                onClicked: results.currentIndex = index
                onDoubleClicked: popup.pick.choose(index, 0)
                contentItem: RowLayout {
                    spacing: 10
                    Label {
                        Layout.maximumWidth: row.width * 3 / 5
                        elide: Text.ElideRight
                        font.weight: Font.DemiBold
                        text: row.modelData.name
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        opacity: 0.6
                        text: row.modelData.details
                    }
                }
            }
        }
        Label {
            objectName: "bench-quick-album-status"
            Layout.fillWidth: true
            opacity: 0.6
            font.pointSize: Qt.application.font.pointSize * 0.88
            text: popup.pick ? popup.pick.status : ""
        }
        Label {
            objectName: "bench-quick-album-keys"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.6
            font.pointSize: Qt.application.font.pointSize * 0.88
            text: popup.pick ? popup.pick.keys : ""
        }
    }
}
