// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// The exact ordered value list of the current field on the selected files:
// one row per value, empty ones kept, reordered, added and removed; OK
// stages it as the draft.
Dialog {
    id: dialog
    objectName: "bench-metadata-exact-values"

    required property QuickTagger tagger
    property int row: -1
    property string heading: ""
    property string context: ""

    title: qsTr("Edit exact metadata values")
    anchors.centerIn: Overlay.overlay
    width: 640
    height: 420
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel

    readonly property int maximumValues: 16384

    function edit() {
        const exact = tagger.exactValues();
        if (exact.row === undefined)
            return;
        row = exact.row;
        heading = exact.heading;
        context = exact.context;
        values.clear();
        for (const value of exact.values)
            values.append({value: value});
        list.currentIndex = values.count > 0 ? 0 : -1;
        tagger.setExactValuesOpen(true);
        open();
    }

    onAccepted: {
        const result = [];
        for (let at = 0; at < values.count; ++at)
            result.push(values.get(at).value);
        tagger.replaceValues(row, result);
    }
    onClosed: tagger.setExactValuesOpen(false)
    Component.onCompleted: {
        const ok = standardButton(Dialog.Ok);
        if (ok)
            ok.enabled = Qt.binding(() => values.count > 0);
    }

    ListModel {
        id: values
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        Label {
            objectName: "bench-metadata-exact-values-heading"
            text: dialog.heading
            font.bold: true
        }
        Label {
            objectName: "bench-metadata-exact-values-context"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.context
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: palette.base
            border.color: Shade.mix(palette.window, palette.windowText, 0.15)
            radius: 4
            ListView {
                id: list
                objectName: "bench-metadata-exact-values-table"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: values
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: entry
                    required property int index
                    required property string value
                    width: ListView.view.width
                    height: 28
                    color: ListView.isCurrentItem ? Shade.alpha(palette.highlight, 0.3)
                                                  : (index % 2 ? palette.alternateBase : palette.base)
                    TextField {
                        anchors.fill: parent
                        anchors.margins: 1
                        text: entry.value
                        placeholderText: qsTr("(empty value)")
                        background: null
                        onActiveFocusChanged: if (activeFocus) list.currentIndex = entry.index
                        onTextEdited: values.setProperty(entry.index, "value", text)
                        ToolTip.visible: hovered && text === ""
                        ToolTip.text: qsTr("This is one explicit empty metadata value")
                    }
                }
            }
        }
        RowLayout {
            Button {
                objectName: "bench-metadata-exact-values-add"
                text: qsTr("Add value")
                enabled: values.count < dialog.maximumValues
                onClicked: {
                    values.append({value: ""});
                    list.currentIndex = values.count - 1;
                    list.positionViewAtEnd();
                    Qt.callLater(() => {
                        if (list.currentItem)
                            list.currentItem.children[0].forceActiveFocus();
                    });
                }
            }
            Button {
                objectName: "bench-metadata-exact-values-remove"
                text: qsTr("Remove value")
                enabled: list.currentIndex >= 0
                onClicked: {
                    const at = list.currentIndex;
                    values.remove(at);
                    list.currentIndex = Math.min(at, values.count - 1);
                }
            }
            Button {
                objectName: "bench-metadata-exact-values-up"
                text: qsTr("Move up")
                enabled: list.currentIndex > 0
                onClicked: {
                    values.move(list.currentIndex, list.currentIndex - 1, 1);
                    list.currentIndex -= 1;
                }
            }
            Button {
                objectName: "bench-metadata-exact-values-down"
                text: qsTr("Move down")
                enabled: list.currentIndex >= 0 && list.currentIndex + 1 < values.count
                onClicked: {
                    values.move(list.currentIndex, list.currentIndex + 1, 1);
                    list.currentIndex += 1;
                }
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("Each row is one value. Empty rows are preserved; use Delete in Properties to remove the field.")
        }
    }
}
