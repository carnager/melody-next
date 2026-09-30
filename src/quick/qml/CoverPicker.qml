// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// One picker for a cover: the images beside the files, then what the Cover
// Art Archive has for the release, their thumbnails arriving as they come.
Dialog {
    id: picker
    objectName: "bench-metadata-artwork-picker"

    required property QuickArtwork artwork

    title: qsTr("Choose a cover")
    anchors.centerIn: Overlay.overlay
    width: Math.min(600, Overlay.overlay ? Overlay.overlay.width - 40 : 600)
    height: Math.min(440, Overlay.overlay ? Overlay.overlay.height - 40 : 440)
    modal: true
    standardButtons: Dialog.Close

    onOpened: list.currentIndex = artwork.pickerSuggested
    onClosed: artwork.closePicker()

    Connections {
        target: picker.artwork
        function onPickerClosed() {
            picker.close();
        }
        function onPickerChanged() {
            if (list.currentIndex < 0)
                list.currentIndex = picker.artwork.pickerSuggested;
        }
    }

    footer: DialogButtonBox {
        Button {
            objectName: "bench-metadata-artwork-picker-use"
            text: qsTr("Use this image")
            enabled: list.currentIndex >= 0
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: picker.artwork.usePicker(list.currentIndex)
        }
        Button {
            text: qsTr("Close")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        onRejected: picker.close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("A front cover replaces the files' existing one; other images are added with their type.")
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: palette.base
            border.color: Theme.hairline(palette)
            radius: 4
            ListView {
                id: list
                objectName: "bench-metadata-artwork-picker-list"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                currentIndex: -1
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                model: picker.artwork.pickerRows
                delegate: ItemDelegate {
                    id: entry
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 84
                    highlighted: ListView.isCurrentItem
                    onClicked: list.currentIndex = index
                    onDoubleClicked: picker.artwork.usePicker(index)
                    ToolTip.visible: hovered && modelData.toolTip !== ""
                    ToolTip.delay: 800
                    ToolTip.text: modelData.toolTip
                    contentItem: RowLayout {
                        spacing: 12
                        Rectangle {
                            Layout.preferredWidth: 72
                            Layout.preferredHeight: 72
                            color: Shade.mix(palette.base, palette.window, 0.5)
                            radius: 3
                            Image {
                                anchors.fill: parent
                                fillMode: Image.PreserveAspectFit
                                visible: entry.modelData.hasThumbnail
                                cache: false
                                source: entry.modelData.hasThumbnail
                                        ? "image://artwork/%1/picker/%2?%3".arg(picker.artwork.id)
                                              .arg(entry.index).arg(Date.now())
                                        : ""
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: entry.modelData.from
                                font.bold: true
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Label {
                                text: entry.modelData.type
                            }
                            Label {
                                visible: text !== ""
                                text: entry.modelData.details
                                opacity: 0.7
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
        Label {
            objectName: "bench-metadata-artwork-picker-status"
            visible: text !== ""
            text: picker.artwork.pickerStatus
            opacity: 0.8
        }
    }
}
