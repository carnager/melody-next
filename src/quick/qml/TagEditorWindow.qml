// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// The tagger: its own window, as the product keeps it. Edits are staged,
// previewed with every change and every problem, and written by the engine
// only when asked and only when nothing blocks them.
ApplicationWindow {
    id: tagger

    readonly property var editor: Engine.tagEditor
    readonly property string stage: editor.stage

    function edit(session, paths) {
        editor.open(session, paths);
        show();
        raise();
        requestActivate();
    }

    width: 760
    height: 640
    title: editor.files > 0 ? "Tags — " + editor.files + (editor.files === 1 ? " file" : " files") : "Tags"
    color: Theme.window
    palette.window: Theme.window
    palette.windowText: Theme.text
    palette.base: Theme.base
    palette.text: Theme.text
    palette.button: Theme.raised
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: "white"
    palette.placeholderText: Theme.faint
    palette.mid: Theme.line
    font.pixelSize: Theme.fontSize

    onClosing: editor.close()

    component Field: Rectangle {
        id: field
        required property var modelData
        width: ListView.view.width
        height: 38
        radius: 4
        color: modelData.edited ? Theme.playingTint : "transparent"

        Text {
            x: 10
            width: 150
            anchors.verticalCenter: parent.verticalCenter
            text: field.modelData.name
            elide: Text.ElideRight
            color: field.modelData.edited ? Theme.accentText : Theme.dim
            font.pixelSize: Theme.fontSize
        }
        TextField {
            id: input
            x: 170
            width: parent.width - x - 44
            anchors.verticalCenter: parent.verticalCenter
            text: field.modelData.value
            placeholderText: field.modelData.state === "mixed" ? "Different in each file — typing sets them all"
                           : field.modelData.state === "partial" ? "In some files only"
                           : "Not set"
            color: Theme.text
            placeholderTextColor: Theme.faint
            font.pixelSize: Theme.fontSize
            enabled: tagger.stage === "editing"
            // Written only when it changed: tabbing through leaves every
            // field as it was.
            onEditingFinished: {
                if (text !== field.modelData.value || (text !== "" && field.modelData.state !== "common"))
                    tagger.editor.setField(field.modelData.index, text);
            }
            background: Rectangle {
                radius: 5
                color: Theme.base
                border.color: input.activeFocus ? Theme.accent : Theme.line
            }
        }
        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            icon: "history"
            iconSize: 14
            tip: "Undo this field's change"
            visible: field.modelData.edited && tagger.stage === "editing"
            onClicked: tagger.editor.revertField(field.modelData.index)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        Text {
            Layout.fillWidth: true
            visible: tagger.editor.status !== ""
            text: tagger.editor.status
            wrapMode: Text.WordWrap
            color: tagger.stage === "reviewing" && !tagger.editor.planReady ? Theme.error : Theme.dim
            font.pixelSize: Theme.fontSize
        }

        // Editing
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: tagger.stage === "editing" || tagger.stage === "planning"
            clip: true
            spacing: 2
            model: tagger.editor.fields
            delegate: Field {}
            ScrollBar.vertical: ScrollBar {}
        }

        // Reviewing
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: tagger.stage === "reviewing" || tagger.stage === "writing"
            clip: true
            spacing: 10
            model: tagger.editor.plan
            ScrollBar.vertical: ScrollBar {}
            delegate: Column {
                id: source
                required property var modelData
                width: ListView.view.width
                spacing: 2
                Text {
                    text: source.modelData.name
                    color: source.modelData.ready ? Theme.text : Theme.error
                    font.pixelSize: Theme.fontSize
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: source.modelData.changes
                    delegate: Text {
                        required property var modelData
                        leftPadding: 14
                        width: source.width
                        elide: Text.ElideRight
                        text: modelData.field + ":  " + (modelData.before !== "" ? modelData.before : "—")
                              + "  →  " + (modelData.after !== "" ? modelData.after : "—")
                        color: modelData.conflicting ? Theme.error : Theme.dim
                        font.pixelSize: Theme.smallFontSize + 1
                    }
                }
                Repeater {
                    model: source.modelData.issues
                    delegate: Text {
                        required property string modelData
                        leftPadding: 14
                        width: source.width
                        wrapMode: Text.WordWrap
                        text: "⚠ " + modelData
                        color: Theme.error
                        font.pixelSize: Theme.smallFontSize + 1
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
            visible: tagger.stage === "reading" || tagger.stage === "done" || tagger.stage === "idle"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                text: "Back"
                visible: tagger.stage === "reviewing"
                onClicked: tagger.editor.back()
            }
            Item { Layout.fillWidth: true }
            Button {
                text: "Stop"
                visible: tagger.stage === "writing" || tagger.stage === "reading" || tagger.stage === "planning"
                onClicked: tagger.editor.cancel()
            }
            Button {
                text: "Preview changes"
                visible: tagger.stage === "editing"
                enabled: tagger.editor.edited
                onClicked: tagger.editor.preview()
            }
            Button {
                text: "Write " + tagger.editor.plan.filter(source => source.ready).length + " files"
                visible: tagger.stage === "reviewing"
                enabled: tagger.editor.planReady
                onClicked: tagger.editor.apply()
            }
            Button {
                text: "Close"
                onClicked: tagger.close()
            }
        }
    }
}
