// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// Dynamic playlists (ADR-0145): saved rules or a Last.fm source, refreshed
// into the tracks they match in the library chosen; played from, or kept as
// a snapshot.
ApplicationWindow {
    id: dynamicWindow
    objectName: "bench-dynamic-playlists"

    required property QuickDynamic dynamic
    readonly property var state: dynamic.state
    // The rows chosen, by index, and the one the cursor is on.
    property var chosen: ({})
    property int anchorRow: -1
    property int cursorRow: -1

    title: qsTr("Dynamic playlists")
    width: 900
    height: 720
    minimumWidth: 600
    minimumHeight: 480
    visible: true
    color: palette.window

    Component.onCompleted: dynamic.setShown(true)
    Component.onDestruction: dynamic.release()
    onClosing: {
        dynamic.setShown(false);
        Qt.callLater(() => dynamicWindow.destroy());
    }

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: dynamicWindow.close()
    }

    function chosenRows() {
        return Object.keys(chosen).map(Number).sort((a, b) => a - b);
    }
    function choose(row, modifiers) {
        const next = Object.assign({}, chosen);
        if ((modifiers & Qt.ShiftModifier) && anchorRow >= 0) {
            for (let index = Math.min(anchorRow, row); index <= Math.max(anchorRow, row); ++index)
                next[index] = true;
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
        cursorRow = row;
        results.currentIndex = row;
    }

    component Field: TextField {
        Layout.fillWidth: true
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.margin
            spacing: Theme.gap + 4

            Hint {
                text: qsTr("Save rules or a Last.fm source, then refresh to see the matching tracks. Rules update while this window is open. Last.fm refreshes draw a fresh selection when requested. Opening a snapshot keeps that list stable while you listen.")
            }
            RowLayout {
                spacing: Theme.gap
                // As in Search: the library searched, and so the engine that
                // plays what is found (ADR-0227).
                ComboBox {
                    objectName: "dynamic-library"
                    model: dynamicWindow.dynamic.libraries
                    currentIndex: dynamicWindow.state.library ?? 0
                    onActivated: index => dynamicWindow.dynamic.chooseLibrary(index)
                    Accessible.name: qsTr("Library")
                }
                ComboBox {
                    objectName: "dynamic-catalog"
                    Layout.fillWidth: true
                    model: dynamicWindow.dynamic.catalog
                    currentIndex: dynamicWindow.state.catalogIndex ?? 0
                    onActivated: index => dynamicWindow.dynamic.selectDefinition(index)
                    Accessible.name: qsTr("Saved dynamic playlists")
                }
                Button {
                    objectName: "dynamic-save"
                    text: qsTr("Save definition")
                    enabled: dynamicWindow.state.writable ?? false
                    onClicked: dynamicWindow.dynamic.save()
                }
                Button {
                    objectName: "dynamic-remove"
                    text: qsTr("Remove")
                    enabled: dynamicWindow.state.writable ?? false
                    onClicked: dynamicWindow.dynamic.remove()
                }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.gap + 4
                rowSpacing: Theme.gap
                readonly property string source: dynamicWindow.state.source ?? "rules"

                FormLabel {
                    columnWidth: 130
                    text: qsTr("Name:")
                }
                Field {
                    objectName: "dynamic-name"
                    text: dynamicWindow.state.name ?? ""
                    onTextEdited: dynamicWindow.dynamic.setName(text)
                }
                FormLabel {
                    columnWidth: 130
                    text: qsTr("Source:")
                }
                ComboBox {
                    objectName: "dynamic-source"
                    Layout.preferredWidth: 280
                    model: dynamicWindow.dynamic.sources
                    textRole: "label"
                    currentIndex: {
                        const sources = dynamicWindow.dynamic.sources;
                        for (let i = 0; i < sources.length; ++i) {
                            if (sources[i].value === parent.source)
                                return i;
                        }
                        return 0;
                    }
                    onActivated: index => dynamicWindow.dynamic.setSource(model[index].value)
                }
                FormLabel {
                    columnWidth: 130
                    visible: parent.source === "rules"
                    text: qsTr("Rules:")
                }
                Field {
                    objectName: "dynamic-query"
                    visible: parent.source === "rules"
                    placeholderText: qsTr("genre HAS rock AND rating GREATER 6")
                    text: dynamicWindow.state.query ?? ""
                    onTextEdited: dynamicWindow.dynamic.setQuery(text)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Ratings use 0–10; 8 means four stars. Example: genre HAS jazz SORT BY %album%")
                }
                FormLabel {
                    columnWidth: 130
                    visible: parent.source === "similar"
                    text: qsTr("Seed artist:")
                }
                Field {
                    objectName: "dynamic-artist"
                    visible: parent.source === "similar"
                    text: dynamicWindow.state.artist ?? ""
                    onTextEdited: dynamicWindow.dynamic.setArtist(text)
                }
                FormLabel {
                    columnWidth: 130
                    visible: parent.source === "similar"
                    text: qsTr("Seed track:")
                }
                Field {
                    objectName: "dynamic-track"
                    visible: parent.source === "similar"
                    text: dynamicWindow.state.track ?? ""
                    onTextEdited: dynamicWindow.dynamic.setTrack(text)
                }
                FormLabel {
                    columnWidth: 130
                    visible: parent.source === "loved" || parent.source === "top"
                    text: qsTr("Last.fm user:")
                }
                Field {
                    objectName: "dynamic-user"
                    visible: parent.source === "loved" || parent.source === "top"
                    text: dynamicWindow.state.user ?? ""
                    onTextEdited: dynamicWindow.dynamic.setUser(text)
                }
                FormLabel {
                    columnWidth: 130
                    visible: parent.source === "tag"
                    text: qsTr("Last.fm tag:")
                }
                Field {
                    objectName: "dynamic-tag"
                    visible: parent.source === "tag"
                    text: dynamicWindow.state.tag ?? ""
                    onTextEdited: dynamicWindow.dynamic.setTag(text)
                }
                FormLabel {
                    columnWidth: 130
                    text: qsTr("Maximum tracks:")
                }
                SpinBox {
                    objectName: "dynamic-limit"
                    from: 1
                    to: 500
                    editable: true
                    value: dynamicWindow.state.limit ?? 100
                    onValueModified: dynamicWindow.dynamic.setLimit(value)
                }
                FormLabel {
                    columnWidth: 130
                }
                CheckBox {
                    objectName: "dynamic-shuffle"
                    text: dynamicWindow.state.shuffleText ?? ""
                    checked: dynamicWindow.state.shuffle ?? false
                    onToggled: {
                        dynamicWindow.dynamic.setShuffle(checked);
                        checked = Qt.binding(() => dynamicWindow.state.shuffle ?? false);
                    }
                    ToolTip.visible: hovered && (dynamicWindow.state.shuffleTip ?? "") !== ""
                    ToolTip.delay: 800
                    ToolTip.text: dynamicWindow.state.shuffleTip ?? ""
                }
            }
            RowLayout {
                Layout.leftMargin: 130 + Theme.gap + 4
                spacing: Theme.gap
                Button {
                    objectName: "dynamic-refresh"
                    text: qsTr("Refresh")
                    enabled: dynamicWindow.state.canRefresh ?? false
                    onClicked: dynamicWindow.dynamic.refresh()
                }
                Button {
                    objectName: "dynamic-stop"
                    text: qsTr("Stop")
                    onClicked: dynamicWindow.dynamic.stop()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: palette.base
                border.color: Theme.hairline(palette)
                radius: Theme.radius
                clip: true
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 1
                    spacing: 0
                    // The flat columns the snapshot keeps.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.preferredHeight: 28
                        spacing: 8
                        Label {
                            Layout.preferredWidth: 36
                            text: "#"
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 3
                            text: qsTr("Title")
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 2
                            text: qsTr("Artist")
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 2
                            text: qsTr("Album")
                            font.bold: true
                        }
                        Label {
                            Layout.preferredWidth: 56
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("Length")
                            font.bold: true
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Shade.mix(palette.base, palette.text, 0.12)
                    }
                    ListView {
                        id: results
                        objectName: "dynamic-tracks"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: dynamicWindow.dynamic.results
                        currentIndex: -1
                        highlightMoveDuration: 0
                        ScrollBar.vertical: ScrollBar {}
                        Keys.onPressed: event => {
                            const step = event.key === Qt.Key_Down ? 1 : event.key === Qt.Key_Up ? -1 : 0;
                            if (step !== 0) {
                                const row = Math.max(0, Math.min(count - 1, dynamicWindow.cursorRow + step));
                                if (count > 0)
                                    dynamicWindow.choose(row, event.modifiers);
                                event.accepted = true;
                            } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                       && (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier) {
                                if (dynamicWindow.cursorRow >= 0)
                                    dynamicWindow.dynamic.play(dynamicWindow.cursorRow);
                                event.accepted = true;
                            }
                        }
                        delegate: ItemDelegate {
                            id: trackRow
                            required property var modelData
                            required property int index
                            width: results.width
                            height: 26
                            highlighted: dynamicWindow.chosen[index] === true
                            font.bold: modelData.playing
                            onClicked: {
                                results.forceActiveFocus();
                                dynamicWindow.choose(index, Qt.application.keyboardModifiers);
                            }
                            onDoubleClicked: dynamicWindow.dynamic.play(index)
                            // Dragged, the tracks chosen are copied where they
                            // are dropped; the definition stays as it is.
                            DragSource {
                                copyOnly: true
                                label: {
                                    const count = dynamicWindow.chosenRows().length;
                                    return count > 1 ? qsTr("%1 tracks").arg(count) : qsTr("1 track");
                                }
                                onBegan: {
                                    if (!dynamicWindow.chosen[trackRow.index])
                                        dynamicWindow.choose(trackRow.index, 0);
                                    dynamicWindow.dynamic.drag(dynamicWindow.chosenRows());
                                }
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: {
                                    if (!dynamicWindow.chosen[trackRow.index])
                                        dynamicWindow.choose(trackRow.index, 0);
                                    trackMenu.popup();
                                }
                            }
                            contentItem: RowLayout {
                                spacing: 8
                                Label {
                                    Layout.preferredWidth: 36
                                    opacity: 0.7
                                    text: trackRow.modelData.playing ? "▶" : trackRow.modelData.number
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 3
                                    elide: Text.ElideRight
                                    text: trackRow.modelData.title
                                    font: trackRow.font
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 2
                                    elide: Text.ElideRight
                                    text: trackRow.modelData.artist
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 2
                                    elide: Text.ElideRight
                                    text: trackRow.modelData.album
                                }
                                Label {
                                    Layout.preferredWidth: 56
                                    horizontalAlignment: Text.AlignRight
                                    text: trackRow.modelData.length
                                }
                            }
                        }
                    }
                }
            }
        }

        WindowFooter {
            Hint {
                objectName: "dynamic-status"
                text: dynamicWindow.state.status ?? ""
            }
            Button {
                objectName: "dynamic-open"
                text: qsTr("Open snapshot in new tab")
                enabled: dynamicWindow.state.canOpen ?? false
                onClicked: dynamicWindow.dynamic.openSnapshot()
            }
        }
    }

    // Queue next and Queue at end, with the keys the lists use, on the
    // tracks chosen here.
    Shortcut {
        enabled: results.activeFocus
        sequence: Tk.shortcutRevision >= 0 ? Tk.shortcut("action-queue-next", "Ctrl+Return") : ""
        onActivated: dynamicWindow.dynamic.queue(dynamicWindow.chosenRows(), true)
    }
    Shortcut {
        enabled: results.activeFocus
        sequence: Tk.shortcutRevision >= 0 ? Tk.shortcut("action-queue-end", "Ctrl+Shift+Return") : ""
        onActivated: dynamicWindow.dynamic.queue(dynamicWindow.chosenRows(), false)
    }

    Connections {
        target: dynamicWindow.dynamic
        function onResultsChanged() {
            // Keep only what is still there.
            const count = dynamicWindow.dynamic.results.length;
            const next = {};
            for (const key of Object.keys(dynamicWindow.chosen)) {
                if (Number(key) < count)
                    next[key] = true;
            }
            dynamicWindow.chosen = next;
            if (dynamicWindow.cursorRow >= count)
                dynamicWindow.cursorRow = -1;
        }
    }

    Menu {
        id: trackMenu
        objectName: "dynamic-track-menu"
        property var lists: []
        property var rating: ({})
        property var lastFm: ({})
        onAboutToShow: {
            lists = Tk.otherLists();
            rating = dynamicWindow.dynamic.ratingState(dynamicWindow.chosenRows());
            lastFm = dynamicWindow.dynamic.lastFmTrack(dynamicWindow.chosenRows());
        }
        MenuItem {
            objectName: "dynamic-play"
            text: qsTr("Play")
            onTriggered: dynamicWindow.dynamic.play(dynamicWindow.cursorRow)
        }
        MenuItem {
            text: qsTr("Queue next")
            onTriggered: dynamicWindow.dynamic.queue(dynamicWindow.chosenRows(), true)
        }
        MenuItem {
            text: qsTr("Queue at end")
            onTriggered: dynamicWindow.dynamic.queue(dynamicWindow.chosenRows(), false)
        }
        MenuItem {
            text: qsTr("Go to artist")
            onTriggered: dynamicWindow.dynamic.locate(dynamicWindow.cursorRow, false)
        }
        MenuItem {
            text: qsTr("Go to album")
            onTriggered: dynamicWindow.dynamic.locate(dynamicWindow.cursorRow, true)
        }
        Menu {
            title: qsTr("Tools")
            MenuItem {
                text: qsTr("Edit tags…")
                onTriggered: dynamicWindow.dynamic.editTags(dynamicWindow.chosenRows())
            }
            MenuItem {
                text: qsTr("ReplayGain…")
                onTriggered: dynamicWindow.dynamic.replayGain(dynamicWindow.chosenRows())
            }
            MenuItem {
                text: qsTr("Convert files…")
                onTriggered: dynamicWindow.dynamic.convertFiles(dynamicWindow.chosenRows())
            }
        }
        Menu {
            title: qsTr("Rate")
            enabled: trackMenu.rating.canRate ?? false
            Repeater {
                model: [0, 2, 4, 6, 8, 10]
                delegate: RatingMenuItem {
                    required property int modelData
                    rating: modelData
                    shared: (trackMenu.rating.rating ?? -1) === modelData
                    onTriggered: dynamicWindow.dynamic.rate(dynamicWindow.chosenRows(), false, modelData)
                }
            }
        }
        Menu {
            title: qsTr("Rate album")
            enabled: trackMenu.rating.canRateAlbum ?? false
            Repeater {
                model: [0, 2, 4, 6, 8, 10]
                delegate: RatingMenuItem {
                    required property int modelData
                    rating: modelData
                    shared: (trackMenu.rating.albumRating ?? -1) === modelData
                    onTriggered: dynamicWindow.dynamic.rate(dynamicWindow.chosenRows(), true, modelData)
                }
            }
        }
        Menu {
            title: qsTr("Copy to list")
            MenuItem {
                text: qsTr("New tab…")
                onTriggered: newTabDialog.open()
            }
            MenuSeparator {
                visible: trackMenu.lists.length > 0
            }
            Repeater {
                model: trackMenu.lists
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    onTriggered: dynamicWindow.dynamic.copyTo(dynamicWindow.chosenRows(), modelData.id)
                }
            }
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Open editable snapshot…")
            onTriggered: dynamicWindow.dynamic.openSnapshot()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Manual edits apply to a separate list; the dynamic definition stays unchanged.")
        }
        MenuSeparator {}
        Menu {
            title: "Last.fm"
            enabled: (trackMenu.lastFm.artist ?? "") !== "" && (trackMenu.lastFm.title ?? "") !== ""
            onAboutToShow: dynamicWindow.dynamic.askLastFm(dynamicWindow.chosenRows())
            MenuItem {
                enabled: false
                text: Tk.lastFmState
            }
            MenuItem {
                text: qsTr("Love track")
                onTriggered: dynamicWindow.dynamic.loveOnLastFm(dynamicWindow.chosenRows(), true)
            }
            MenuItem {
                text: qsTr("Unlove track")
                onTriggered: dynamicWindow.dynamic.loveOnLastFm(dynamicWindow.chosenRows(), false)
            }
        }
    }

    Dialog {
        id: newTabDialog
        title: qsTr("New tab")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label {
                text: qsTr("Name:")
            }
            TextField {
                id: newTabName
                Layout.preferredWidth: 300
                onAccepted: newTabDialog.accept()
            }
        }
        onOpened: {
            newTabName.text = qsTr("Selection");
            newTabName.forceActiveFocus();
            newTabName.selectAll();
        }
        onAccepted: dynamicWindow.dynamic.copyToNewTab(dynamicWindow.chosenRows(), newTabName.text)
    }
}
