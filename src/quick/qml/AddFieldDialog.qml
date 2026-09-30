// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// Adds a metadata field to the selected files, suggesting names as they are
// typed: the fields already there, those added lately, then the common ones.
Dialog {
    id: dialog
    objectName: "bench-metadata-add-field-dialog"

    required property QuickTagger tagger

    title: qsTr("Add metadata field")
    anchors.centerIn: Overlay.overlay
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel

    onOpened: {
        name.text = "";
        suggestions.model = tagger.fieldNameSuggestions("");
        tagger.setFieldNameOpen(true);
        name.forceActiveFocus();
    }
    onClosed: tagger.setFieldNameOpen(false)
    onAccepted: {
        if (name.text.trim() !== "")
            tagger.addField(name.text);
    }

    ColumnLayout {
        spacing: 6
        Label {
            text: qsTr("Field name:")
        }
        TextField {
            id: name
            objectName: "bench-metadata-add-field-name"
            Layout.preferredWidth: 360
            onTextEdited: {
                suggestions.model = dialog.tagger.fieldNameSuggestions(text);
                suggestions.currentIndex = -1;
            }
            onAccepted: dialog.accept()
            Keys.onDownPressed: {
                suggestions.forceActiveFocus();
                suggestions.currentIndex = 0;
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(12, suggestions.count) * 26 + 2
            visible: suggestions.count > 0
            color: palette.base
            border.color: Theme.hairline(palette)
            radius: 4
            ListView {
                id: suggestions
                objectName: "bench-metadata-field-completions"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                keyNavigationEnabled: true
                Keys.onReturnPressed: {
                    name.text = model[currentIndex];
                    dialog.accept();
                }
                Keys.onUpPressed: event => {
                    if (currentIndex <= 0)
                        name.forceActiveFocus();
                    else
                        decrementCurrentIndex();
                }
                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: ListView.view.width
                    height: 26
                    text: modelData
                    highlighted: ListView.isCurrentItem
                    onClicked: {
                        name.text = modelData;
                        name.forceActiveFocus();
                    }
                    onDoubleClicked: {
                        name.text = modelData;
                        dialog.accept();
                    }
                }
            }
        }
    }
}
