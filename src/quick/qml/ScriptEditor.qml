// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// The tagging script editor (ADR-0178): a script's steps -- built one at a
// time or compiled from raw Picard-style source -- previewed on the
// selected files as they change, saved for reuse, and added to the draft.
ApplicationWindow {
    id: editor
    objectName: "bench-metadata-transformation"

    required property QuickScript script
    readonly property var state: script.state
    property int stepRow: -1
    property bool closingConfirmed: false
    // The raw source is being replaced, not typed.
    property bool settingRaw: false

    title: qsTr("Tagging script editor") + ((state.unsaved ?? false) ? " *" : "")
    width: 1080
    height: 620
    minimumWidth: 760
    minimumHeight: 420
    visible: true
    modality: Qt.WindowModal
    color: palette.window

    Component.onDestruction: script.release()
    onClosing: close => {
        if (closingConfirmed) {
            Qt.callLater(() => editor.destroy());
            return;
        }
        const answer = script.requestClose();
        if (answer === "wait") {
            close.accepted = false;
        } else if (answer === "confirm") {
            close.accepted = false;
            discardConfirm.open();
        } else {
            Qt.callLater(() => editor.destroy());
        }
    }

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: editor.close()
    }

    function focusField(name) {
        const fields = {target: target, input: input, argument: captureArgument, name: scriptName};
        if (fields[name])
            fields[name].forceActiveFocus();
    }
    readonly property int currentKind: {
        const entry = kindChoice.model[kindChoice.currentIndex];
        return entry ? entry.kind : -1;
    }
    readonly property var form: script.stepForm(currentKind, captureSource.currentIndex)

    function addStep() {
        const before = script.steps.length;
        const focus = script.addStep({
            kind: editor.currentKind,
            target: target.text,
            input: input.text,
            replacement: replacement.text,
            numberStart: numberStart.value,
            numberPadding: numberPadding.value,
            characterCount: characterCount.value,
            captureSource: captureSource.currentIndex,
            captureArgument: captureArgument.text,
            ratingScale: ratingScale.currentIndex
        });
        if (script.steps.length !== before) {
            target.text = "";
            input.text = "";
            replacement.text = "";
        }
        focusField(focus);
    }

    Connections {
        target: editor.script
        function onStepsChanged(select) {
            const count = editor.script.steps.length;
            editor.stepRow = count === 0 ? -1 : Math.max(0, Math.min(select, count - 1));
        }
        function onRawChanged() {
            if (rawSource.text !== editor.script.rawSource) {
                editor.settingRaw = true;
                rawSource.text = editor.script.rawSource;
                editor.settingRaw = false;
            }
        }
        function onAccepted() {
            editor.closingConfirmed = true;
            editor.close();
        }
        function onCloseRequested() {
            editor.close();
        }
    }
    // QA hook: a step, for a picture of the preview.
    function stageForScreenshot() {
        target.text = "Title";
        kindChoice.currentIndex = 10;
        addStep();
    }
    Component.onCompleted: {
        editor.settingRaw = true;
        rawSource.text = script.rawSource;
        editor.settingRaw = false;
        stepRow = script.steps.length > 0 ? 0 : -1;
    }

    // Names offered below a text field as it is typed; a click (or Enter on
    // the list) takes one.
    component Suggestions: Popup {
        id: popup
        property var names: []
        signal chosen(string name)
        padding: 1
        width: parent ? parent.width : 200
        height: Math.min(12, names.length) * 26 + 2
        y: parent ? parent.height : 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        ListView {
            anchors.fill: parent
            clip: true
            model: popup.names
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property string modelData
                width: ListView.view.width
                height: 26
                text: modelData
                onClicked: {
                    popup.chosen(modelData);
                    popup.close();
                }
            }
        }
    }

    header: ToolBar {
        topPadding: Theme.gap
        bottomPadding: Theme.gap
        leftPadding: Theme.margin
        rightPadding: Theme.margin
        RowLayout {
            anchors.fill: parent
            spacing: 6
            Label {
                text: qsTr("Script:")
            }
            ComboBox {
                objectName: "bench-metadata-transformation-saved"
                Layout.fillWidth: true
                model: editor.script.savedNames
                currentIndex: editor.script.savedIndex
                enabled: editor.state.canSelectSaved ?? false
                onActivated: index => editor.script.selectSaved(index)
            }
            Button {
                objectName: "bench-metadata-transformation-save"
                text: editor.state.saveText ?? qsTr("Save")
                enabled: editor.state.canSave ?? false
                onClicked: editor.focusField(editor.script.save(false))
            }
            Button {
                objectName: "bench-metadata-transformation-save-as-new"
                text: qsTr("Save as new")
                enabled: editor.state.canSaveAsNew ?? false
                onClicked: editor.focusField(editor.script.save(true))
            }
            Button {
                objectName: "bench-metadata-transformation-delete"
                text: qsTr("Delete")
                enabled: editor.state.canDelete ?? false
                onClicked: editor.script.deleteSaved()
            }
            Button {
                objectName: "bench-metadata-transformation-import-native"
                text: qsTr("Import…")
                enabled: editor.state.canImport ?? false
                onClicked: {
                    if (editor.state.unsaved)
                        importConfirm.open();
                    else
                        importFile.open();
                }
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Open a complete native Trackknife tagging script as an unsaved definition for review")
            }
            Button {
                objectName: "bench-metadata-transformation-export-native"
                text: qsTr("Export…")
                enabled: editor.state.canExport ?? false
                onClicked: {
                    exportFile.currentFile = "file:" + editor.script.suggestedExportName();
                    exportFile.open();
                }
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Write the complete current typed definition as versioned native JSON; saved identity and automatic state are not included")
            }
        }
    }

    SplitView {
        objectName: "bench-metadata-transformation-splitter"
        anchors.fill: parent
        anchors.margins: Theme.margin
        orientation: Qt.Horizontal
        // Room either side of the line between steps and preview.
        handle: Item {
            implicitWidth: 2 * Theme.gap + 1
            Rectangle {
                x: Theme.gap
                width: 1
                height: parent.height
                color: SplitHandle.pressed ? palette.highlight : Theme.hairline(palette)
            }
        }

        ColumnLayout {
            SplitView.preferredWidth: 460
            SplitView.minimumWidth: 360
            spacing: 6
            RowLayout {
                Label {
                    text: qsTr("Name:")
                }
                TextField {
                    id: scriptName
                    objectName: "bench-metadata-transformation-name"
                    Layout.fillWidth: true
                    text: editor.state.name ?? ""
                    enabled: editor.state.editing ?? true
                    onTextEdited: editor.script.setName(text)
                }
            }
            TabBar {
                id: editorTabs
                objectName: "bench-metadata-transformation-editor-tabs"
                Layout.fillWidth: true
                TabButton {
                    text: qsTr("Steps")
                    width: implicitWidth + 24
                }
                TabButton {
                    text: qsTr("Raw script")
                    width: implicitWidth + 24
                }
            }
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: editorTabs.currentIndex

                // The steps, one at a time.
                ColumnLayout {
                    spacing: 6
                    enabled: editor.state.editing ?? true
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 8
                        Label {
                            text: qsTr("New step:")
                        }
                        ComboBox {
                            id: kindChoice
                            objectName: "bench-metadata-transformation-kind"
                            Layout.fillWidth: true
                            model: editor.script.stepKinds
                            textRole: "label"
                            currentIndex: editor.script.initialStepKind
                            delegate: ItemDelegate {
                                required property var modelData
                                required property int index
                                width: ListView.view ? ListView.view.width : implicitWidth
                                text: modelData.label
                                enabled: modelData.kind >= 0
                                font.bold: modelData.kind < 0
                                highlighted: kindChoice.highlightedIndex === index
                                ToolTip.visible: hovered && modelData.toolTip !== ""
                                ToolTip.delay: 600
                                ToolTip.text: modelData.toolTip
                            }
                        }
                        Label {
                            text: qsTr("Target field:")
                            visible: editor.form.target
                        }
                        TextField {
                            id: target
                            objectName: "bench-metadata-transformation-target"
                            Layout.fillWidth: true
                            visible: editor.form.target
                            placeholderText: qsTr("For example: Title or ALBUM ARTIST")
                            onTextEdited: {
                                targetSuggestions.names = editor.script.targetSuggestions(text);
                                if (text.trim() !== "" && targetSuggestions.names.length > 0)
                                    targetSuggestions.open();
                                else
                                    targetSuggestions.close();
                            }
                            onAccepted: editor.addStep()
                            Suggestions {
                                id: targetSuggestions
                                onChosen: name => target.text = name
                            }
                        }
                        Label {
                            text: editor.form.inputLabel
                            visible: editor.form.input
                        }
                        TextField {
                            id: input
                            objectName: "bench-metadata-transformation-input"
                            Layout.fillWidth: true
                            visible: editor.form.input
                            placeholderText: editor.form.inputPlaceholder
                            onTextEdited: {
                                if (!editor.form.fieldList)
                                    return;
                                fieldSuggestions.names = editor.script.fieldListSuggestions(text);
                                if (fieldSuggestions.names.length > 0)
                                    fieldSuggestions.open();
                            }
                            onAccepted: editor.addStep()
                            Suggestions {
                                id: fieldSuggestions
                                onChosen: name => {
                                    input.text = editor.script.completeFieldList(input.text, name);
                                    input.forceActiveFocus();
                                }
                            }
                        }
                        Label {
                            text: qsTr("Replacement:")
                            visible: editor.form.replacement
                        }
                        TextField {
                            id: replacement
                            objectName: "bench-metadata-transformation-replacement"
                            Layout.fillWidth: true
                            visible: editor.form.replacement
                            onAccepted: editor.addStep()
                        }
                        Label {
                            text: qsTr("Start at:")
                            visible: editor.form.numbering
                        }
                        SpinBox {
                            id: numberStart
                            objectName: "bench-metadata-transformation-number-start"
                            visible: editor.form.numbering
                            from: 1
                            to: 1000000000
                            value: 1
                            editable: true
                        }
                        Label {
                            text: qsTr("Minimum width:")
                            visible: editor.form.numbering
                        }
                        SpinBox {
                            id: numberPadding
                            objectName: "bench-metadata-transformation-number-padding"
                            visible: editor.form.numbering
                            from: 0
                            to: 32
                            value: 0
                            editable: true
                        }
                        Label {
                            text: qsTr("Characters to keep:")
                            visible: editor.form.characters
                        }
                        SpinBox {
                            id: characterCount
                            objectName: "bench-metadata-transformation-character-count"
                            visible: editor.form.characters
                            from: 1
                            to: 1000000
                            value: 4
                            editable: true
                        }
                        Label {
                            text: qsTr("Capture source:")
                            visible: editor.form.captureSource
                        }
                        ComboBox {
                            id: captureSource
                            objectName: "bench-metadata-transformation-capture-source"
                            Layout.fillWidth: true
                            visible: editor.form.captureSource
                            model: editor.script.captureSources
                        }
                        Label {
                            text: qsTr("Its scale:")
                            visible: editor.form.ratingScale
                        }
                        ComboBox {
                            id: ratingScale
                            objectName: "bench-metadata-transformation-rating-scale"
                            Layout.fillWidth: true
                            visible: editor.form.ratingScale
                            model: editor.script.ratingScales
                            ToolTip.visible: hovered
                            ToolTip.delay: 800
                            ToolTip.text: qsTr("The scale the other player kept the rating on; it becomes 0.0-1.0")
                        }
                        Label {
                            text: editor.form.captureArgumentLabel
                            visible: editor.form.captureArgument
                        }
                        TextField {
                            id: captureArgument
                            objectName: "bench-metadata-transformation-capture-argument"
                            Layout.fillWidth: true
                            visible: editor.form.captureArgument
                            placeholderText: editor.form.captureArgumentPlaceholder
                            onAccepted: editor.addStep()
                        }
                    }
                    RowLayout {
                        Button {
                            objectName: "bench-metadata-transformation-add"
                            text: qsTr("Add step")
                            enabled: editor.state.canAdd ?? false
                            onClicked: editor.addStep()
                        }
                        Button {
                            objectName: "bench-metadata-transformation-import-script"
                            text: qsTr("Paste script…")
                            onClicked: pasteScript.open()
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: palette.base
                        border.color: Theme.hairline(palette)
                        radius: 4
                        ListView {
                            id: steps
                            objectName: "bench-metadata-transformation-steps"
                            anchors.fill: parent
                            anchors.margins: 1
                            clip: true
                            model: editor.script.steps
                            currentIndex: editor.stepRow
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}
                            delegate: ItemDelegate {
                                required property string modelData
                                required property int index
                                width: ListView.view.width
                                text: modelData
                                highlighted: ListView.isCurrentItem
                                onClicked: editor.stepRow = index
                            }
                            ToolTip.visible: stepsHover.hovered
                            ToolTip.delay: 1200
                            ToolTip.text: qsTr("Steps run in order; each step sees the result of every earlier step")
                            HoverHandler {
                                id: stepsHover
                            }
                        }
                    }
                    RowLayout {
                        Button {
                            objectName: "bench-metadata-transformation-remove"
                            text: qsTr("Remove step")
                            enabled: {
                                editor.state;
                                return editor.script.canRemove(editor.stepRow);
                            }
                            onClicked: editor.script.removeStep(editor.stepRow)
                        }
                        Button {
                            objectName: "bench-metadata-transformation-up"
                            text: qsTr("Move up")
                            enabled: {
                                editor.state;
                                return editor.script.canMoveUp(editor.stepRow);
                            }
                            onClicked: editor.script.moveStep(editor.stepRow, -1)
                        }
                        Button {
                            objectName: "bench-metadata-transformation-down"
                            text: qsTr("Move down")
                            enabled: {
                                editor.state;
                                return editor.script.canMoveDown(editor.stepRow);
                            }
                            onClicked: editor.script.moveStep(editor.stepRow, 1)
                        }
                    }
                }

                // The raw source.
                ColumnLayout {
                    spacing: 6
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        TextArea {
                            id: rawSource
                            objectName: "bench-metadata-transformation-raw-source"
                            readOnly: editor.state.rawReadOnly ?? false
                            enabled: editor.state.editing ?? true
                            font.family: "monospace"
                            placeholderText: "$if($eq(%totaldiscs%,1),$delete(discnumber)$delete(totaldiscs))"
                            onTextChanged: if (!editor.settingRaw) editor.script.setRawSource(text)
                            ToolTip.visible: hovered
                            ToolTip.delay: 1200
                            ToolTip.text: qsTr("Valid source compiles into the steps on the Steps tab; arbitrary script is never executed")
                        }
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 110
                        TextArea {
                            objectName: "bench-metadata-transformation-raw-diagnostics"
                            readOnly: true
                            wrapMode: TextEdit.Wrap
                            text: editor.state.rawDiagnostics ?? ""
                        }
                    }
                    // What can be written here, in the browser.
                    RowLayout {
                        Layout.fillWidth: true
                        Item {
                            Layout.fillWidth: true
                        }
                        Button {
                            objectName: "bench-metadata-transformation-script-reference"
                            text: qsTr("Script steps")
                            flat: true
                            onClicked: Tk.openReference(1)
                            ToolTip.visible: hovered
                            ToolTip.delay: 800
                            ToolTip.text: qsTr("Every step a script can have, written as text")
                        }
                        Button {
                            objectName: "bench-metadata-transformation-tkfmt-reference"
                            text: qsTr("tkfmt-1 reference")
                            flat: true
                            onClicked: Tk.openReference(0)
                            ToolTip.visible: hovered
                            ToolTip.delay: 800
                            ToolTip.text: qsTr("The language of values and conditions")
                        }
                    }
                }
            }
        }

        // The preview.
        ColumnLayout {
            SplitView.fillWidth: true
            spacing: 6
            Label {
                text: qsTr("Preview")
                font.bold: true
                ToolTip.visible: previewHover.hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Updates automatically as you edit; nothing enters the draft until you add the previewed changes")
                HoverHandler {
                    id: previewHover
                }
            }
            Label {
                objectName: "bench-metadata-transformation-summary"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: editor.state.summary ?? ""
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: palette.base
                border.color: Theme.hairline(palette)
                radius: 4
                clip: true
                HorizontalHeaderView {
                    id: previewHeader
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 1
                    syncView: preview
                    clip: true
                    visible: editor.script.preview !== null
                    delegate: Rectangle {
                        required property string display
                        implicitHeight: 24
                        color: Shade.mix(palette.base, palette.window, 0.6)
                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            verticalAlignment: Text.AlignVCenter
                            text: parent.display
                            font.bold: true
                            opacity: 0.8
                            elide: Text.ElideRight
                        }
                    }
                }
                TreeView {
                    id: preview
                    objectName: "bench-metadata-transformation-table"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: previewHeader.bottom
                    anchors.bottom: parent.bottom
                    anchors.margins: 1
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    model: editor.script.preview
                    columnWidthProvider: column => column === 0 ? 170 : column === 1 ? 200
                                                                 : Math.max(160, preview.width - 370)
                    onWidthChanged: forceLayout()
                    delegate: TreeViewDelegate {
                        required property var toolTip
                        ToolTip.visible: hovered && (toolTip ?? "") !== ""
                        ToolTip.delay: 700
                        ToolTip.text: toolTip ?? ""
                    }
                }
            }
        }
    }

    footer: ToolBar {
        topPadding: Theme.gap
        bottomPadding: Theme.gap
        leftPadding: Theme.margin
        rightPadding: Theme.margin
        RowLayout {
            anchors.fill: parent
            spacing: 8
            Label {
                objectName: "bench-metadata-transformation-catalog-status"
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: editor.state.catalogStatus ?? ""
            }
            Button {
                text: qsTr("Close")
                onClicked: editor.close()
            }
            Button {
                objectName: "bench-metadata-transformation-stage"
                text: qsTr("Add to draft")
                highlighted: true
                enabled: editor.state.canStage ?? false
                onClicked: editor.script.stage()
            }
        }
    }

    Dialog {
        id: pasteScript
        objectName: "bench-metadata-rule-script-import"
        title: qsTr("Generate rules from script")
        anchors.centerIn: Overlay.overlay
        width: Math.min(760, editor.width - 40)
        height: Math.min(560, editor.height - 40)
        modal: true
        property var translated: ({diagnostics: "", ready: false})
        onOpened: {
            pasteSource.text = "";
            pasteSource.forceActiveFocus();
        }
        footer: DialogButtonBox {
            Button {
                objectName: "bench-metadata-rule-script-append"
                text: qsTr("Append generated rules")
                enabled: pasteScript.translated.ready
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    editor.script.importRuleScript(pasteSource.text, true);
                    pasteScript.close();
                }
            }
            Button {
                objectName: "bench-metadata-rule-script-replace"
                text: qsTr("Replace rules")
                enabled: pasteScript.translated.ready
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    editor.script.importRuleScript(pasteSource.text, false);
                    pasteScript.close();
                }
            }
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            onRejected: pasteScript.close()
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Paste a Picard-style cleanup script. Trackknife translates the supported $unset/$delete, $set, $if, $and/$or, $eq/$ne, $not, and $left subset into editable typed rules; it does not store or execute the pasted script. Here, $unset generates an actual Remove field rule. Imported removals match the exact native field name, ignoring ASCII case but preserving separators.")
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextArea {
                    id: pasteSource
                    objectName: "bench-metadata-rule-script-source"
                    font.family: "monospace"
                    placeholderText: "$unset(comment)\n$set(date,$left(%date%,4))"
                    onTextChanged: pasteScript.translated = editor.script.translateRuleScript(text)
                }
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                TextArea {
                    objectName: "bench-metadata-rule-script-diagnostics"
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    text: pasteScript.translated.diagnostics
                }
            }
        }
    }

    FileDialog {
        id: importFile
        title: qsTr("Import Trackknife tagging script")
        nameFilters: [qsTr("Trackknife tagging scripts (*.tbtags.json *.json)"), qsTr("All files (*)")]
        onAccepted: editor.script.importNative(selectedFile)
    }
    FileDialog {
        id: exportFile
        title: qsTr("Export Trackknife tagging script")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Trackknife tagging scripts (*.tbtags.json)"), qsTr("JSON files (*.json)"), qsTr("All files (*)")]
        onAccepted: editor.script.exportNative(selectedFile)
    }
    Dialog {
        id: importConfirm
        title: qsTr("Discard unsaved script changes?")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label {
            width: 400
            wrapMode: Text.WordWrap
            text: qsTr("Importing a native tagging script replaces the current unsaved editor contents. Explicitly discard those changes to continue.")
        }
        onDiscarded: {
            close();
            importFile.open();
        }
    }
    Dialog {
        id: discardConfirm
        title: qsTr("Discard unsaved script changes?")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label {
            width: 400
            wrapMode: Text.WordWrap
            text: qsTr("This script differs from its saved version. Save it before closing, or explicitly discard the changes.")
        }
        onDiscarded: {
            close();
            editor.closingConfirmed = true;
            editor.close();
        }
    }
}
