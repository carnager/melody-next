// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// "Edit tags" (MetadataPropertiesDialog), in Qt Quick: the selected files on
// the left, their fields with original and draft values on the right, and
// the footer's Actions, status and Apply. Everything it decides is the
// tagger's session, shared with the widgets window.
ApplicationWindow {
    id: taggerWindow
    objectName: "bench-metadata-properties"

    required property QuickTagger tagger
    readonly property var state: tagger.state
    // Asked once: the draft or pending covers were given up, so it closes.
    property bool closingConfirmed: false

    title: tagger.title
    width: 1020
    height: 620
    minimumWidth: 640
    minimumHeight: 360
    visible: true
    color: palette.window

    signal settingsRequested(string page)
    signal destinationsRequested()

    // Its window gone, so is the editor.
    Component.onDestruction: tagger.release()
    function closed() {
        tagger.storeWindowState({
            width: taggerWindow.visibility === Window.Maximized ? restoredWidth : width,
            height: taggerWindow.visibility === Window.Maximized ? restoredHeight : height,
            maximized: taggerWindow.visibility === Window.Maximized,
            listWidth: fileList.width,
        });
        Qt.callLater(() => taggerWindow.destroy());
    }

    // Opened as it was last left: its size, maximized or not, and the file
    // list's width. The size before maximizing is the one kept.
    property int restoredWidth: width
    property int restoredHeight: height
    onWidthChanged: if (visibility !== Window.Maximized) restoredWidth = width
    onHeightChanged: if (visibility !== Window.Maximized) restoredHeight = height
    Component.onCompleted: tagger.loadWindowState()
    Connections {
        target: taggerWindow.tagger
        function onWindowStateLoaded(state) {
            if ((state.width ?? 0) >= taggerWindow.minimumWidth)
                taggerWindow.width = state.width;
            if ((state.height ?? 0) >= taggerWindow.minimumHeight)
                taggerWindow.height = state.height;
            if ((state.listWidth ?? 0) > 0)
                fileList.SplitView.preferredWidth = state.listWidth;
            if (state.maximized)
                taggerWindow.showMaximized();
        }
    }

    onClosing: close => {
        if (closingConfirmed) {
            closed();
            return;
        }
        const answer = tagger.requestClose();
        if (answer === "wait") {
            close.accepted = false;
        } else if (answer === "confirm-drafts") {
            close.accepted = false;
            discardDraft.open();
        } else if (answer === "confirm-artwork") {
            close.accepted = false;
            discardArtwork.open();
        } else {
            tagger.closing(false);
            closed();
        }
    }

    // What has not been ported yet says so in the status line for a moment.
    property string notice: ""
    function notYet(what) {
        notice = qsTr("%1 is not in the Qt Quick window yet").arg(what);
        noticeTimer.restart();
    }
    Timer {
        id: noticeTimer
        interval: 3000
        onTriggered: taggerWindow.notice = ""
    }

    function reallyClose(discard) {
        tagger.closing(discard);
        closingConfirmed = true;
        taggerWindow.close();
    }

    Connections {
        target: taggerWindow.tagger
        function onCloseRequested() {
            taggerWindow.close();
        }
        function onFeedbackRequested(title, summary, rows, retryOffered, applyCommitted) {
            feedback.show(title, summary, rows, retryOffered, applyCommitted);
        }
        function onFolderImagesRequested(note, rows) {
            folderReview.show(qsTr("Review folder covers"), note,
                              [qsTr("Destination"), qsTr("Change"), qsTr("Incoming image")], rows,
                              true);
        }
        function onSettingsRequested(page) {
            taggerWindow.settingsRequested(page);
        }
        function onDestinationsRequested() {
            taggerWindow.destinationsRequested();
        }
        function onRevealField(row) {
            fieldTable.reveal(row);
        }
    }

    header: ToolBar {
        topPadding: Theme.gap + 2
        bottomPadding: Theme.gap + 2
        leftPadding: Theme.margin
        rightPadding: Theme.margin
        RowLayout {
            anchors.fill: parent
            spacing: Theme.gapLarge
            Label {
                objectName: "bench-metadata-summary"
                text: taggerWindow.state.summary ?? ""
                font.bold: true
            }
            Label {
                objectName: "bench-metadata-technical"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                color: Theme.dim(palette)
                text: taggerWindow.state.technical ?? ""
                ToolTip.visible: technicalHover.hovered && text !== ""
                ToolTip.text: text
                HoverHandler {
                    id: technicalHover
                }
            }
        }
    }

    // Until the files are read.
    Label {
        objectName: "bench-metadata-loading"
        anchors.centerIn: parent
        visible: !taggerWindow.tagger.ready
        text: taggerWindow.state.loadingText ?? ""
        opacity: 0.7
    }

    SplitView {
        id: split
        objectName: "bench-metadata-splitter"
        anchors.fill: parent
        visible: taggerWindow.tagger.ready
        orientation: Qt.Horizontal

        TaggerFileList {
            id: fileList
            tagger: taggerWindow.tagger
            visible: showFiles.checked
            SplitView.preferredWidth: 260
            SplitView.minimumWidth: 180
            enabled: taggerWindow.state.filesEnabled ?? true
        }

        Item {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 360
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.margin
                anchors.topMargin: Theme.gap
                spacing: Theme.gap

                TabBar {
                    id: sections
                    objectName: "bench-metadata-sections"
                    Layout.fillWidth: true
                    TabButton {
                        text: qsTr("Fields")
                        width: implicitWidth + 24
                    }
                    TabButton {
                        text: qsTr("Artwork")
                        width: implicitWidth + 24
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: sections.currentIndex

                    // Fields.
                    ColumnLayout {
                        spacing: Theme.gap

                        // The review bar: presentation only; hidden fields keep
                        // their edits.
                        RowLayout {
                            objectName: "bench-metadata-field-review"
                            Layout.fillWidth: true
                            TextField {
                                objectName: "bench-metadata-field-filter"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Filter fields…")
                                text: taggerWindow.tagger.filterText
                                onTextEdited: taggerWindow.tagger.filterText = text
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Match display or canonical field names; values are not searched.")
                            }
                            CheckBox {
                                objectName: "bench-metadata-changed-only"
                                text: qsTr("Changed fields only")
                                checked: taggerWindow.tagger.changedOnly
                                onToggled: taggerWindow.tagger.changedOnly = checked
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Show fields with staged edits in the selected files.")
                            }
                            CheckBox {
                                id: showFiles
                                objectName: "bench-metadata-show-files"
                                text: qsTr("Show files")
                                checked: true
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Hide the file list to make more room for fields. The selected files stay selected.")
                            }
                        }
                        Label {
                            objectName: "bench-metadata-field-filter-status"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.dim(palette)
                            font.pointSize: Theme.smallSize
                            text: taggerWindow.tagger.filterStatus
                        }

                        // The grid tools.
                        RowLayout {
                            objectName: "bench-metadata-grid-tools"
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Button {
                                objectName: "bench-metadata-add-field"
                                text: qsTr("Add field…")
                                enabled: taggerWindow.state.canAddField ?? false
                                onClicked: addField.open()
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Add an arbitrary metadata field (Insert)")
                            }
                            Button {
                                objectName: "bench-metadata-remove-field"
                                text: qsTr("Remove field")
                                enabled: taggerWindow.state.canRemoveFields ?? false
                                onClicked: taggerWindow.tagger.removeFields()
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Remove the selected fields from the selected files (Delete)")
                            }
                            Button {
                                objectName: "bench-metadata-edit-values"
                                text: qsTr("Edit values…")
                                enabled: taggerWindow.state.canEditValues ?? false
                                onClicked: exactValues.edit()
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Edit the exact ordered value list (Ctrl+Enter)")
                            }
                            Label {
                                text: qsTr("Fields:")
                                visible: taggerWindow.tagger.fieldLayouts.length > 0
                            }
                            ComboBox {
                                id: fieldLayout
                                objectName: "bench-metadata-field-layout"
                                visible: taggerWindow.tagger.fieldLayouts.length > 0
                                textRole: "name"
                                valueRole: "id"
                                model: [{id: "", name: qsTr("All fields")}].concat(taggerWindow.tagger.fieldLayouts)
                                currentIndex: Math.max(0, indexOfValue(taggerWindow.tagger.activeFieldLayout))
                                onActivated: taggerWindow.tagger.selectFieldLayout(currentValue)
                            }
                            Button {
                                objectName: "bench-metadata-identify"
                                text: qsTr("Identify…")
                                enabled: taggerWindow.state.canIdentify ?? false
                                onClicked: {
                                    const identify = taggerWindow.tagger.identify();
                                    if (identify)
                                        identifyComponent.createObject(taggerWindow, {identify: identify});
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Search MusicBrainz by artist and album — no MusicBrainz tags needed — pick the exact release version, and stage the match as ordinary colored draft edits")
                            }
                            Button {
                                id: moreButton
                                objectName: "bench-metadata-more"
                                text: qsTr("More")
                                onClicked: moreMenu.popup(moreButton, 0, moreButton.height)
                                Menu {
                                    id: moreMenu
                                    MenuItem {
                                        text: qsTr("Suggest album totals and artist")
                                        enabled: taggerWindow.state.canSuggest ?? false
                                        onTriggered: taggerWindow.tagger.suggest()
                                    }
                                    MenuItem {
                                        text: qsTr("Save visible fields as set…")
                                        onTriggered: fieldSetName.open()
                                    }
                                    MenuItem {
                                        text: qsTr("Delete current field set")
                                        enabled: taggerWindow.tagger.activeFieldLayout !== ""
                                        onTriggered: taggerWindow.tagger.removeFieldLayout()
                                    }
                                }
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            ToolButton {
                                objectName: "bench-metadata-undo"
                                icon.source: Tk.iconBase + "edit-undo|sp:SP_ArrowBack"
                                enabled: taggerWindow.state.canUndo ?? false
                                onClicked: taggerWindow.tagger.undo()
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Undo the last draft edit (Ctrl+Z)")
                            }
                            ToolButton {
                                objectName: "bench-metadata-redo"
                                icon.source: Tk.iconBase + "edit-redo|sp:SP_ArrowForward"
                                enabled: taggerWindow.state.canRedo ?? false
                                onClicked: taggerWindow.tagger.redo()
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Redo the last undone draft edit (Ctrl+Shift+Z)")
                            }
                            ToolButton {
                                objectName: "bench-metadata-discard"
                                icon.source: Tk.iconBase + "edit-clear|sp:SP_DialogDiscardButton"
                                enabled: (taggerWindow.state.draftCount ?? 0) > 0
                                onClicked: taggerWindow.tagger.discardAll()
                                ToolTip.visible: hovered
                                ToolTip.delay: 800
                                ToolTip.text: qsTr("Throw away every pending draft edit")
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: Theme.gap
                            TaggerFieldTable {
                                id: fieldTable
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                tagger: taggerWindow.tagger
                                onEditValues: exactValues.edit()
                                onAddField: addField.open()
                            }
                            CompactCover {
                                Layout.fillHeight: true
                                artwork: taggerWindow.tagger.artwork
                                onOpenArtwork: sections.currentIndex = 1
                                onCoverSettings: taggerWindow.settingsRequested("covers")
                                onFetch: artworkPage.openPicker()
                            }
                        }
                    }

                    // Artwork.
                    ArtworkPage {
                        id: artworkPage
                        artwork: taggerWindow.tagger.artwork
                        onFeedback: (title, summary, rows) => artworkFeedback.show(title, summary, rows, false, false)
                        onFolderReview: (note, rows) => artworkFolderReview.show(
                                            qsTr("Review folder covers"), note,
                                            [qsTr("Destination"), qsTr("Change"), qsTr("Incoming image")],
                                            rows, true)
                    }
                }
            }
        }
    }

    footer: ToolBar {
        objectName: "bench-metadata-footer"
        topPadding: Theme.gap
        bottomPadding: Theme.gap
        leftPadding: Theme.margin
        rightPadding: Theme.margin
        RowLayout {
            anchors.fill: parent
            spacing: Theme.gap
            Button {
                id: actionsButton
                objectName: "bench-metadata-actions"
                text: qsTr("Actions")
                // A toggle: shown as on while they are, a click closes them.
                checked: actions.visible
                onClicked: actions.visible ? actions.close() : actions.open()
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("What Apply does: tags, renaming and moving, ReplayGain, scripts")
                TaggerActions {
                    id: actions
                    objectName: "bench-metadata-actions-popover"
                    tagger: taggerWindow.tagger
                    y: -height - 6
                    onProvenance: taggerWindow.showLoudnessSources()
                    onScriptEditor: id => taggerWindow.openScriptEditor(id)
                    onSettings: page => taggerWindow.settingsRequested(page)
                    onDestinations: taggerWindow.destinationsRequested()
                    onExpression: expressionDialog.open()
                }
            }
            Label {
                objectName: "bench-metadata-apply-summary"
                text: taggerWindow.state.applySummary ?? ""
                font.bold: true
            }
            Label {
                objectName: "bench-metadata-read-only"
                Layout.fillWidth: true
                elide: Text.ElideRight
                textFormat: taggerWindow.notice === "" && taggerWindow.state.statusRich
                            ? Text.RichText : Text.PlainText
                text: taggerWindow.notice !== "" ? taggerWindow.notice : (taggerWindow.state.status ?? "")
                onLinkActivated: link => {
                    if (link === "export-replaygain")
                        exportDialog.open();
                    else
                        taggerWindow.tagger.statusLink(link);
                }
                HoverHandler {
                    cursorShape: parent.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
            }
            ProgressBar {
                objectName: "bench-metadata-apply-progress"
                visible: taggerWindow.state.applying ?? false
                Layout.preferredWidth: 170
                from: 0
                to: Math.max(1, taggerWindow.state.progressMaximum ?? 1)
                value: taggerWindow.state.progress ?? 0
            }
            Button {
                objectName: "bench-metadata-apply-stop"
                text: qsTr("Stop")
                visible: taggerWindow.state.applying ?? false
                enabled: taggerWindow.state.canStop ?? false
                onClicked: taggerWindow.tagger.stopApply()
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Stop after the files already in progress are safe")
            }
            Button {
                text: qsTr("Close")
                onClicked: taggerWindow.close()
            }
            Button {
                objectName: "bench-metadata-apply-changes"
                text: qsTr("Apply")
                highlighted: true
                enabled: taggerWindow.state.canApply ?? false
                onClicked: taggerWindow.tagger.apply()
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Recheck the files, then make every enabled change; problems stop the run and are shown")
            }
        }
    }

    Shortcut {
        sequence: StandardKey.Undo
        onActivated: taggerWindow.tagger.undo()
    }
    Shortcut {
        sequence: StandardKey.Redo
        onActivated: taggerWindow.tagger.redo()
    }
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: taggerWindow.close()
    }

    // QA hook (--open tagger-edit): a draft staged and the Actions shown,
    // for a picture of both.
    function stageForScreenshot(name) {
        if (name === "tagger-art") {
            sections.currentIndex = 1;
            return;
        }
        if (name === "tagger-script") {
            const script = tagger.scriptEditor("");
            if (script) {
                const opened = scriptComponent.createObject(taggerWindow, {script: script});
                opened.stageForScreenshot();
            }
            return;
        }
        if (name === "tagger-identify") {
            const identify = tagger.identify();
            if (identify)
                identifyComponent.createObject(taggerWindow, {identify: identify});
            return;
        }
        tagger.setField(0, "Edited title");
        actions.open();
    }

    function openScriptEditor(id) {
        const script = tagger.scriptEditor(id);
        if (script)
            scriptComponent.createObject(taggerWindow, {script: script});
    }

    function showLoudnessSources() {
        const sources = tagger.loudnessSources();
        if (sources.rows.length > 0)
            provenance.show(qsTr("Loudness sources"), "", sources.headers, sources.rows, false);
    }

    Component {
        id: scriptComponent
        ScriptEditor {}
    }
    Component {
        id: identifyComponent
        IdentifyDialog {}
    }
    FeedbackDialog {
        id: feedback
        onRetry: taggerWindow.tagger.retry()
        onDone: taggerWindow.tagger.feedbackFinished()
    }
    TableDialog {
        id: provenance
        objectName: "bench-replaygain-provenance-dialog"
    }
    TableDialog {
        id: folderReview
        objectName: "bench-folder-cover-review"
        onAccepted: taggerWindow.tagger.folderImagesReviewed(true)
        onRejected: taggerWindow.tagger.folderImagesReviewed(false)
    }
    FeedbackDialog {
        id: artworkFeedback
    }
    TableDialog {
        id: artworkFolderReview
        objectName: "bench-folder-cover-review"
        onAccepted: taggerWindow.tagger.artwork.folderImagesReviewed(true)
        onRejected: taggerWindow.tagger.artwork.folderImagesReviewed(false)
    }
    ExactValuesDialog {
        id: exactValues
        tagger: taggerWindow.tagger
    }
    AddFieldDialog {
        id: addField
        tagger: taggerWindow.tagger
    }

    Dialog {
        id: fieldSetName
        title: qsTr("Save field set")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label {
                text: qsTr("Field set name:")
            }
            TextField {
                id: fieldSetField
                Layout.preferredWidth: 320
                onAccepted: fieldSetName.accept()
            }
        }
        onOpened: {
            fieldSetField.text = "";
            fieldSetField.forceActiveFocus();
        }
        onAccepted: {
            if (fieldSetField.text.trim() !== "")
                taggerWindow.tagger.saveFieldLayout(fieldSetField.text.trim());
        }
    }

    Dialog {
        id: expressionDialog
        title: qsTr("Group by expression")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label {
                text: qsTr("tkfmt-1 expression:")
            }
            TextField {
                id: expressionField
                objectName: "bench-replaygain-expression"
                Layout.preferredWidth: 320
                placeholderText: qsTr("tkfmt-1, e.g. %album%")
                onAccepted: expressionDialog.accept()
            }
        }
        onOpened: {
            expressionField.text = taggerWindow.state.expression ?? "";
            expressionField.forceActiveFocus();
        }
        onAccepted: taggerWindow.tagger.setReplayGainExpression(expressionField.text)
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Export ReplayGain results")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "csv"
        nameFilters: [qsTr("CSV files (*.csv)")]
        currentFile: "replaygain-results.csv"
        onAccepted: taggerWindow.tagger.exportReplayGain(selectedFile)
    }

    Dialog {
        id: discardDraft
        title: qsTr("Discard metadata draft?")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label {
            width: 380
            wrapMode: Text.WordWrap
            text: {
                const count = taggerWindow.state.draftCount ?? 0;
                return qsTr("%1 staged %2 exist only in memory and have not been written to files.")
                    .arg(count).arg(count === 1 ? qsTr("change") : qsTr("changes"));
            }
        }
        onDiscarded: {
            close();
            taggerWindow.reallyClose(true);
        }
    }
    Dialog {
        id: discardArtwork
        title: qsTr("Discard artwork changes?")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label {
            text: qsTr("The pending artwork changes have not been saved.")
        }
        onDiscarded: {
            close();
            taggerWindow.tagger.discardArtwork();
            taggerWindow.close();
        }
    }
}
