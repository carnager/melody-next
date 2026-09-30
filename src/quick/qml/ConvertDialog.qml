// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// Converts the selection below a destination folder (ADR-0107): a preset, a
// naming layout (or the source folders mirrored) with a live preview of the
// targets, resampling, depth, channels, an optional permanent volume change
// and cover art, converted in parallel with problems-only feedback.
ApplicationWindow {
    id: dialog
    objectName: "bench-convert-dialog"

    required property QuickConvert convert
    readonly property var state: convert.state

    title: convert.title
    width: 720
    height: 620
    minimumWidth: 560
    minimumHeight: 460
    visible: true
    color: palette.window

    Component.onDestruction: convert.release()
    onClosing: close => {
        if (!convert.requestClose())
            close.accepted = false;
        else
            Qt.callLater(() => dialog.destroy());
    }

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: dialog.close()
    }

    function indexOfValue(choices, value) {
        for (let at = 0; at < choices.length; ++at) {
            if (choices[at].value === value)
                return at;
        }
        return 0;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 10
            rowSpacing: 6

            Label {
                text: qsTr("Preset:")
            }
            RowLayout {
                ComboBox {
                    id: preset
                    objectName: "bench-convert-preset"
                    Layout.fillWidth: true
                    model: dialog.convert.presets
                    textRole: "name"
                    currentIndex: dialog.state.presetIndex ?? 0
                    onActivated: index => {
                        const entry = dialog.convert.presets[index];
                        if (entry && !entry.separator)
                            dialog.convert.selectPreset(entry.id);
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        width: ListView.view ? ListView.view.width : implicitWidth
                        height: modelData.separator ? 9 : implicitHeight
                        text: modelData.separator ? "" : modelData.name
                        enabled: !modelData.separator && modelData.available
                        highlighted: preset.highlightedIndex === index
                        background: modelData.separator ? separatorLine : null
                        ToolTip.visible: hovered && !modelData.available && modelData.detail !== ""
                        ToolTip.text: modelData.detail
                        Rectangle {
                            id: separatorLine
                            visible: false
                        }
                        Rectangle {
                            visible: modelData.separator
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 6
                            height: 1
                            color: Shade.mix(palette.window, palette.windowText, 0.25)
                        }
                    }
                }
                Button {
                    objectName: "bench-convert-preset-new"
                    text: qsTr("New…")
                    visible: dialog.state.canAddPreset ?? false
                    onClicked: presetEditor.open()
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Save a new encoder preset starting from the selected one")
                }
                Button {
                    objectName: "bench-convert-preset-export"
                    text: qsTr("Export…")
                    onClicked: {
                        exportPreset.currentFile = "file:" + dialog.convert.suggestedPresetExportName();
                        exportPreset.open();
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Export the selected encoder preset as JSON")
                }
                Button {
                    objectName: "bench-convert-preset-delete"
                    text: qsTr("Delete")
                    visible: dialog.state.canDeletePreset ?? false
                    onClicked: dialog.convert.deletePreset()
                }
            }

            Label {
                text: qsTr("Into:")
            }
            RowLayout {
                ComboBox {
                    objectName: "bench-convert-destination-choice"
                    visible: dialog.convert.destinations.length > 1
                    model: dialog.convert.destinations
                    currentIndex: dialog.state.destinationChoice ?? 0
                    onActivated: index => dialog.convert.selectDestination(index)
                }
                TextField {
                    objectName: "bench-convert-destination"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Destination folder")
                    text: dialog.state.destination ?? ""
                    onTextEdited: dialog.convert.setDestination(text, true)
                }
                Button {
                    objectName: "bench-convert-browse"
                    text: qsTr("Browse…")
                    onClicked: destinationFolder.open()
                }
            }

            Label {
                text: qsTr("Layout:")
                visible: dialog.convert.layouts.length > 1
            }
            ComboBox {
                objectName: "bench-convert-layout"
                Layout.fillWidth: true
                visible: dialog.convert.layouts.length > 1
                enabled: !(dialog.state.mirror ?? false)
                model: dialog.convert.layouts
                currentIndex: dialog.state.layoutChoice ?? 0
                onActivated: index => dialog.convert.selectLayout(index)
            }

            Item {
                Layout.preferredWidth: 1
            }
            CheckBox {
                objectName: "bench-convert-mirror"
                text: qsTr("Mirror source folders")
                checked: dialog.state.mirror ?? false
                onToggled: dialog.convert.setMirror(checked)
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Recreates each source's complete folder path beneath the destination, keeping source file names instead of the expressions")
            }

            Label {
                text: qsTr("Folders:")
            }
            TextField {
                objectName: "bench-convert-directory-expression"
                Layout.fillWidth: true
                enabled: !(dialog.state.mirror ?? false)
                text: dialog.state.directoryExpression ?? ""
                onTextEdited: dialog.convert.setDirectoryExpression(text)
            }
            Label {
                text: qsTr("Names:")
            }
            TextField {
                objectName: "bench-convert-basename-expression"
                Layout.fillWidth: true
                enabled: !(dialog.state.mirror ?? false)
                text: dialog.state.basenameExpression ?? ""
                onTextEdited: dialog.convert.setBasenameExpression(text)
            }

            Label {
                text: qsTr("Resample:")
            }
            ComboBox {
                objectName: "bench-convert-resample"
                Layout.fillWidth: true
                model: dialog.convert.resampleChoices
                textRole: "label"
                currentIndex: dialog.indexOfValue(dialog.convert.resampleChoices, dialog.state.resample)
                onActivated: index => dialog.convert.setResample(dialog.convert.resampleChoices[index].value)
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Encoders that only speak certain rates still constrain the result — Opus maps every choice into its 48 kHz family")
            }
            Label {
                text: qsTr("Bit depth:")
            }
            ComboBox {
                objectName: "bench-convert-bit-depth"
                Layout.fillWidth: true
                model: dialog.convert.bitDepthChoices
                textRole: "label"
                currentIndex: dialog.indexOfValue(dialog.convert.bitDepthChoices, dialog.state.bitDepth)
                onActivated: index => dialog.convert.setBitDepth(dialog.convert.bitDepthChoices[index].value)
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Stored bit depth for lossless output; Opus and other float-based encoders have no stored depth and ignore this")
            }
            Label {
                text: qsTr("Channels:")
            }
            ComboBox {
                objectName: "bench-convert-channels"
                Layout.fillWidth: true
                model: dialog.convert.channelChoices
                textRole: "label"
                currentIndex: dialog.indexOfValue(dialog.convert.channelChoices, dialog.state.channels)
                onActivated: index => dialog.convert.setChannels(dialog.convert.channelChoices[index].value)
            }
            Label {
                text: qsTr("Permanent volume adjustment:")
            }
            ComboBox {
                objectName: "bench-convert-gain"
                Layout.fillWidth: true
                model: dialog.convert.gainChoices
                textRole: "label"
                currentIndex: dialog.indexOfValue(dialog.convert.gainChoices, dialog.state.gain)
                onActivated: index => dialog.convert.setGain(dialog.convert.gainChoices[index].value)
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Permanently changes PCM before encoding; stale ReplayGain tags are removed")
            }
            Item {
                Layout.preferredWidth: 1
            }
            Label {
                objectName: "bench-convert-gain-warning"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.RichText
                text: dialog.state.gainWarning ?? ""
            }
            Item {
                Layout.preferredWidth: 1
            }
            CheckBox {
                objectName: "bench-convert-artwork"
                text: qsTr("Embed cover art")
                checked: dialog.state.embedArtwork ?? true
                onToggled: dialog.convert.setEmbedArtwork(checked)
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Carries each source's cover image (embedded pictures first, then cover/folder/front siblings) into the converted file")
            }
            Label {
                text: qsTr("Parallel files:")
            }
            SpinBox {
                objectName: "bench-convert-parallelism"
                from: 1
                to: dialog.convert.maximumParallelism
                value: dialog.state.parallelism ?? 4
                editable: true
                onValueModified: dialog.convert.setParallelism(value)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: palette.base
            border.color: Theme.hairline(palette)
            radius: 4
            ListView {
                objectName: "bench-convert-preview"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                model: dialog.state.preview ?? []
                delegate: Label {
                    required property string modelData
                    required property int index
                    width: ListView.view.width
                    height: 24
                    leftPadding: 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideMiddle
                    text: modelData
                    background: Rectangle {
                        color: index % 2 ? palette.alternateBase : palette.base
                    }
                }
            }
        }
        ProgressBar {
            objectName: "bench-convert-progress"
            Layout.fillWidth: true
            visible: dialog.state.running ?? false
            from: 0
            to: Math.max(1, dialog.state.progressMaximum ?? 1)
            value: dialog.state.progress ?? 0
        }
        Label {
            objectName: "bench-convert-status"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.state.status ?? ""
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 110
            visible: (dialog.state.problems ?? "") !== ""
            TextArea {
                objectName: "bench-convert-problems"
                readOnly: true
                wrapMode: TextEdit.Wrap
                text: dialog.state.problems ?? ""
            }
        }
        RowLayout {
            Item {
                Layout.fillWidth: true
            }
            Button {
                objectName: "bench-convert-run"
                text: qsTr("Convert")
                highlighted: true
                visible: !(dialog.state.running ?? false)
                enabled: dialog.state.canRun ?? false
                onClicked: {
                    if ((dialog.state.gain ?? 0) !== 0)
                        gainConfirmation.open();
                    else
                        dialog.convert.run();
                }
            }
            Button {
                objectName: "bench-convert-stop"
                text: qsTr("Stop")
                visible: dialog.state.running ?? false
                onClicked: dialog.convert.stop()
            }
            Button {
                objectName: "bench-convert-close"
                text: qsTr("Close")
                onClicked: dialog.close()
            }
        }
    }

    FolderDialog {
        id: destinationFolder
        title: qsTr("Choose the conversion destination")
        onAccepted: dialog.convert.chooseDestinationFolder(selectedFolder)
    }
    FileDialog {
        id: exportPreset
        title: qsTr("Export encoder preset")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON (*.json)")]
        onAccepted: dialog.convert.exportPreset(selectedFile)
    }
    Dialog {
        id: gainConfirmation
        objectName: "bench-convert-gain-confirmation"
        title: qsTr("Permanently change audio volume?")
        anchors.centerIn: Overlay.overlay
        modal: true
        footer: DialogButtonBox {
            Button {
                text: qsTr("Convert and change volume")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            onAccepted: gainConfirmation.accept()
            onRejected: gainConfirmation.reject()
        }
        Label {
            width: 440
            wrapMode: Text.WordWrap
            text: dialog.state.gainConfirmation ?? ""
        }
        onAccepted: dialog.convert.run()
    }
    Dialog {
        id: presetEditor
        objectName: "bench-preset-editor"
        title: qsTr("New encoder preset")
        anchors.centerIn: Overlay.overlay
        modal: true
        onOpened: {
            const draft = dialog.convert.presetDraft();
            presetName.text = "";
            presetFormat.currentIndex = draft.format;
            presetRate.currentIndex = draft.qualityMode ? 1 : 0;
            presetBitrate.value = draft.bitrate;
            presetQuality.value = draft.quality;
            presetName.forceActiveFocus();
        }
        readonly property bool lossless: dialog.convert.formatLossless(presetFormat.currentIndex)
        footer: DialogButtonBox {
            Button {
                text: qsTr("Save")
                enabled: presetName.text.trim() !== ""
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            onAccepted: presetEditor.accept()
            onRejected: presetEditor.reject()
        }
        onAccepted: dialog.convert.savePreset({
            name: presetName.text,
            format: presetFormat.currentIndex,
            qualityMode: presetRate.currentIndex === 1,
            bitrate: presetBitrate.value,
            quality: presetQuality.value
        })
        GridLayout {
            columns: 2
            Label {
                text: qsTr("Name:")
            }
            TextField {
                id: presetName
                objectName: "bench-preset-editor-name"
                Layout.preferredWidth: 260
                placeholderText: qsTr("Preset name")
            }
            Label {
                text: qsTr("Format:")
            }
            ComboBox {
                id: presetFormat
                objectName: "bench-preset-editor-format"
                Layout.fillWidth: true
                model: dialog.convert.editorFormats
            }
            Label {
                text: qsTr("Rate control:")
            }
            ComboBox {
                id: presetRate
                objectName: "bench-preset-editor-rate-mode"
                Layout.fillWidth: true
                enabled: !presetEditor.lossless
                model: [qsTr("Bit rate"), qsTr("VBR quality")]
            }
            Label {
                text: qsTr("Bit rate:")
            }
            SpinBox {
                id: presetBitrate
                objectName: "bench-preset-editor-bitrate"
                enabled: !presetEditor.lossless && presetRate.currentIndex === 0
                from: 8
                to: 2000
                editable: true
                textFromValue: value => qsTr("%1 kbps").arg(value)
                valueFromText: text => parseInt(text)
            }
            Label {
                text: qsTr("Quality:")
            }
            SpinBox {
                id: presetQuality
                objectName: "bench-preset-editor-quality"
                enabled: !presetEditor.lossless && presetRate.currentIndex === 1
                from: -2
                to: 12
                editable: true
            }
        }
    }
}
