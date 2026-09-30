// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// ADR-0153: the standalone search. Words or a tkq query, in a library or
// the current tab; saved searches and presets; what is found put in a tab.
ApplicationWindow {
    id: searchWindow
    objectName: "bench-search-dialog"

    required property QuickSearch search
    readonly property var state: search.state
    readonly property var saved: search.saved
    // The rows chosen, by index; and the one the keyboard is on.
    property var chosen: ({})
    property int anchorRow: -1

    title: qsTr("Search")
    width: 720
    height: 480
    minimumWidth: 520
    minimumHeight: 320
    visible: true
    color: palette.window

    Component.onDestruction: search.release()
    onClosing: Qt.callLater(() => searchWindow.destroy())

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: searchWindow.close()
    }

    function focusInput() {
        input.forceActiveFocus();
    }
    function chosenRows() {
        return Object.keys(chosen).map(Number).sort((a, b) => a - b);
    }
    function choose(row, modifiers) {
        const next = Object.assign({}, chosen);
        if ((modifiers & Qt.ShiftModifier) && anchorRow >= 0) {
            const from = Math.min(anchorRow, row);
            const to = Math.max(anchorRow, row);
            for (let index = from; index <= to; ++index) {
                if (!searchWindow.search.results[index].heading)
                    next[index] = true;
            }
        } else if (modifiers & Qt.ControlModifier) {
            if (next[row])
                delete next[row];
            else
                next[row] = true;
            anchorRow = row;
        } else {
            for (const key of Object.keys(next))
                delete next[key];
            next[row] = true;
            anchorRow = row;
        }
        chosen = next;
        results.currentIndex = row;
    }
    function openChosen(action) {
        const rows = chosenRows();
        if (rows.length > 0)
            search.openRows(rows, action);
    }

    Connections {
        target: searchWindow.search
        function onResultsChanged() {
            searchWindow.chosen = {};
            searchWindow.anchorRow = -1;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.margin
            spacing: Theme.gap

            // Two rows in three columns: what is searched and how it is
            // started line up with each other.
            GridLayout {
                Layout.fillWidth: true
                columns: 3
                columnSpacing: Theme.gap
                rowSpacing: Theme.gap
                ComboBox {
                    objectName: "bench-search-scope"
                    Layout.preferredWidth: 160
                    model: searchWindow.search.scopes
                    currentIndex: searchWindow.state.scope ?? 0
                    onActivated: index => searchWindow.search.setScope(index)
                }
                TextField {
                    id: input
                    objectName: "bench-search-input"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search…")
                    text: searchWindow.state.text ?? ""
                    onTextEdited: searchWindow.search.setText(text)
                    Keys.onDownPressed: {
                        results.forceActiveFocus();
                        if (results.count > 0)
                            searchWindow.choose(firstRow(), 0);
                    }
                    function firstRow() {
                        const rows = searchWindow.search.results;
                        for (let index = 0; index < rows.length; ++index) {
                            if (!rows[index].heading)
                                return index;
                        }
                        return 0;
                    }
                }
                CheckBox {
                    objectName: "bench-search-query-mode"
                    text: qsTr("Query")
                    checked: searchWindow.state.queryMode ?? false
                    onToggled: {
                        searchWindow.search.setQueryMode(checked);
                        checked = Qt.binding(() => searchWindow.state.queryMode ?? false);
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Interpret the input as a tkq query, e.g. genre HAS jazz AND date GREATER 1990. Library history: HISTORY(albumplaycount) EQUAL 0, or HISTORY(albumdayssinceplayed) GREATER 180.")
                }
                Button {
                    id: presetsButton
                    Layout.preferredWidth: 160
                    objectName: "bench-search-presets"
                    text: qsTr("Browse presets")
                    onClicked: presetMenu.popup(presetsButton, 0, presetsButton.height)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Choose a starting point; adjust the query and Save as… to keep it")
                    Menu {
                        id: presetMenu
                        objectName: "bench-search-presets-menu"
                        Instantiator {
                            model: searchWindow.search.presetGroups
                            delegate: Menu {
                                id: groupMenu
                                required property var modelData
                                title: modelData.topic
                                Repeater {
                                    model: groupMenu.modelData.presets
                                    MenuItem {
                                        required property var modelData
                                        objectName: "search-preset-" + modelData.id
                                        text: modelData.title
                                        onTriggered: searchWindow.askPreset(modelData.index)
                                    }
                                }
                            }
                            onObjectAdded: (index, object) => presetMenu.insertMenu(index, object)
                            onObjectRemoved: (index, object) => presetMenu.removeMenu(object)
                        }
                        MenuItem {
                            visible: searchWindow.search.presetGroups.length === 0
                            height: visible ? implicitHeight : 0
                            enabled: false
                            text: qsTr("No presets supported in this scope")
                        }
                    }
                }
                ComboBox {
                    objectName: "bench-search-saved"
                    Layout.fillWidth: true
                    model: searchWindow.saved.names ?? []
                    currentIndex: searchWindow.saved.index ?? 0
                    enabled: searchWindow.saved.available ?? false
                    onActivated: index => searchWindow.search.useSaved(index)
                    Accessible.name: qsTr("Saved searches")
                    ToolTip.visible: hovered && currentIndex > 0
                    ToolTip.delay: 800
                    ToolTip.text: (searchWindow.saved.tooltips ?? [])[currentIndex] ?? ""
                }
                RowLayout {
                    Layout.fillWidth: false
                    spacing: Theme.gap
                    Button {
                        objectName: "bench-search-save"
                        text: qsTr("Save as…")
                        enabled: searchWindow.saved.canSave ?? false
                        onClicked: nameDialog.ask(qsTr("Save search"), searchWindow.saved.suggestedName,
                                                  name => searchWindow.search.saveAs(name))
                    }
                    // The saved search chosen: replaced, renamed or deleted.
                    ToolButton {
                        id: savedMore
                        objectName: "bench-search-saved-more"
                        text: "⋯"
                        enabled: (searchWindow.saved.canUpdate ?? false)
                                 || (searchWindow.saved.canRename ?? false)
                        onClicked: savedMenu.popup(savedMore, 0, savedMore.height)
                        ToolTip.visible: hovered
                        ToolTip.delay: 800
                        ToolTip.text: qsTr("Update, rename or delete the saved search")
                        Menu {
                            id: savedMenu
                            MenuItem {
                                objectName: "bench-search-update"
                                text: qsTr("Update with this query")
                                enabled: searchWindow.saved.canUpdate ?? false
                                onTriggered: searchWindow.search.update()
                            }
                            MenuItem {
                                objectName: "bench-search-rename"
                                text: qsTr("Rename…")
                                enabled: searchWindow.saved.canRename ?? false
                                onTriggered: nameDialog.ask(qsTr("Rename search"),
                                                            searchWindow.saved.selectedName,
                                                            name => searchWindow.search.rename(name))
                            }
                            MenuSeparator {}
                            MenuItem {
                                objectName: "bench-search-delete"
                                text: qsTr("Delete…")
                                enabled: searchWindow.saved.canRename ?? false
                                onTriggered: deleteConfirm.open()
                            }
                        }
                    }
                }
            }
            Hint {
                objectName: "bench-search-saved-status"
                textFormat: Text.PlainText
                text: searchWindow.saved.status ?? ""
            }
            Label {
                objectName: "bench-search-error"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: text !== ""
                color: "firebrick"
                text: searchWindow.state.error ?? ""
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: Theme.gapSmall
                color: palette.base
                border.color: Theme.hairline(palette)
                radius: Theme.radius
                ListView {
                    id: results
                    objectName: "bench-search-results"
                    anchors.fill: parent
                    anchors.margins: Theme.gapSmall
                    clip: true
                    model: searchWindow.search.results
                    currentIndex: -1
                    highlightMoveDuration: 0
                    ScrollBar.vertical: ScrollBar {}
                    Keys.onPressed: event => {
                        const step = event.key === Qt.Key_Down ? 1 : event.key === Qt.Key_Up ? -1 : 0;
                        if (step !== 0) {
                            let row = currentIndex + step;
                            while (row >= 0 && row < count && searchWindow.search.results[row].heading)
                                row += step;
                            if (row >= 0 && row < count)
                                searchWindow.choose(row, event.modifiers);
                            event.accepted = true;
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            searchWindow.openChosen(0);
                            event.accepted = true;
                        }
                    }
                    delegate: ItemDelegate {
                        id: resultRow
                        required property var modelData
                        required property int index
                        width: results.width
                        height: modelData.heading ? 30 : Theme.rowHeight
                        hoverEnabled: !modelData.heading
                        highlighted: searchWindow.chosen[index] === true
                        // A heading: artists, albums or tracks found, set above
                        // them, not chosen itself.
                        contentItem: Label {
                            text: resultRow.modelData.label
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignBottom
                            font.bold: resultRow.modelData.heading
                            font.pointSize: resultRow.modelData.heading ? Theme.smallSize
                                                                        : Qt.application.font.pointSize
                            color: resultRow.modelData.heading ? Theme.dim(palette) : palette.text
                            bottomPadding: resultRow.modelData.heading ? 2 : 0
                        }
                        onClicked: {
                            if (modelData.heading)
                                return;
                            results.forceActiveFocus();
                            searchWindow.choose(index, Qt.application.keyboardModifiers);
                        }
                        onDoubleClicked: if (!modelData.heading) searchWindow.openChosen(0)
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            enabled: !resultRow.modelData.heading
                            onTapped: {
                                if (!searchWindow.chosen[resultRow.index])
                                    searchWindow.choose(resultRow.index, 0);
                                resultMenu.popup();
                            }
                        }
                    }
                }
            }
        }

        WindowFooter {
            Hint {
                objectName: "bench-search-status"
                text: searchWindow.state.status ?? ""
            }
            Button {
                objectName: "bench-search-open-tab"
                text: qsTr("Open results in tab")
                enabled: searchWindow.state.canOpen ?? false
                onClicked: searchWindow.search.openAll(3)
            }
            Button {
                objectName: "bench-search-close"
                text: qsTr("Close")
                onClicked: searchWindow.close()
            }
        }
    }

    // The standard destinations, selection-scoped (ADR-0153).
    Menu {
        id: resultMenu
        objectName: "bench-search-context-menu"
        MenuItem {
            objectName: "action-search-append"
            text: qsTr("Add to current tab")
            onTriggered: searchWindow.openChosen(0)
        }
        MenuItem {
            objectName: "action-search-next"
            text: qsTr("Play next")
            onTriggered: searchWindow.openChosen(1)
        }
        MenuItem {
            objectName: "action-search-replace"
            text: qsTr("Replace current tab")
            onTriggered: searchWindow.openChosen(2)
        }
        MenuItem {
            objectName: "action-search-new-tab"
            text: qsTr("Open in new tab")
            onTriggered: searchWindow.openChosen(3)
        }
    }

    // A preset that asks something first.
    property int presetAsked: -1
    function askPreset(index) {
        const input = search.presetInput(index);
        if (input.kind === "none") {
            search.usePreset(index, "");
            focusInput();
            return;
        }
        presetAsked = index;
        presetDialog.title = input.title;
        presetPrompt.text = input.prompt;
        presetNumber.visible = input.kind === "integer";
        presetText.visible = input.kind === "text";
        presetNumber.from = input.minimum;
        presetNumber.to = input.maximum;
        presetNumber.stepSize = input.step;
        presetNumber.value = parseInt(input.value);
        presetText.text = input.value;
        presetDialog.open();
    }
    Dialog {
        id: presetDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label {
                id: presetPrompt
            }
            SpinBox {
                id: presetNumber
                editable: true
            }
            TextField {
                id: presetText
                Layout.preferredWidth: 280
            }
        }
        onAccepted: {
            searchWindow.search.usePreset(searchWindow.presetAsked,
                                          presetNumber.visible ? String(presetNumber.value) : presetText.text);
            searchWindow.focusInput();
        }
    }

    Dialog {
        id: nameDialog
        property var done
        function ask(heading, initial, then) {
            title = heading;
            nameField.text = initial;
            done = then;
            open();
        }
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label {
                text: qsTr("Name:")
            }
            TextField {
                id: nameField
                Layout.preferredWidth: 320
                onAccepted: nameDialog.accept()
            }
        }
        onOpened: {
            nameField.forceActiveFocus();
            nameField.selectAll();
        }
        onAccepted: {
            const name = nameField.text.trim();
            if (name !== "" && done)
                done(name);
        }
    }
    MessageDialog {
        id: deleteConfirm
        title: qsTr("Delete saved search")
        text: searchWindow.saved.deleteQuestion ?? ""
        buttons: MessageDialog.Yes | MessageDialog.No
        onButtonClicked: (button, role) => {
            if (button === MessageDialog.Yes)
                searchWindow.search.remove();
        }
    }
}
