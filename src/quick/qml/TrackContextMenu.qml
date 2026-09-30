// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// The track list's context menu, as the widgets window's: play and queue,
// the tools, locating, rating, copying and moving, the list edits, undo
// and Last.fm.
Menu {
    id: contextMenu

    signal notYet(string what)
    signal newTabRequested(bool move)
    signal customSortRequested()

    property var lists: []
    property var rating: ({})
    property var lastFm: ({})

    onAboutToShow: {
        lists = Tk.otherLists();
        rating = Tk.ratingState();
        lastFm = Tk.lastFmTrack();
    }

    readonly property bool selection: (Tk.selection.count ?? 0) > 0

    MenuItem {
        objectName: "action-play-selected-track"
        text: "Play"
        enabled: Tk.rows.currentRow >= 0
        onTriggered: Tk.activateRow(Tk.rows.currentRow)
    }
    MenuItem {
        objectName: "action-queue-next"
        text: qsTr("Queue next")
        enabled: contextMenu.selection
        onTriggered: Tk.queueSelection(true)
    }
    MenuItem {
        objectName: "action-queue-end"
        text: qsTr("Queue at end")
        enabled: contextMenu.selection
        onTriggered: Tk.queueSelection(false)
    }
    MenuSeparator {}
    Menu {
        title: qsTr("Tools")
        MenuItem {
            text: "Edit tags…"
            enabled: contextMenu.selection
            onTriggered: contextMenu.notYet("The tagger")
        }
        MenuItem {
            text: "ReplayGain…"
            onTriggered: contextMenu.notYet("ReplayGain scanning")
        }
        MenuItem {
            text: "Convert files…"
            enabled: contextMenu.selection
            onTriggered: contextMenu.notYet("Converting")
        }
    }
    MenuSeparator {}
    MenuItem {
        objectName: "action-local-locate-artist"
        text: "Locate artist"
        onTriggered: contextMenu.notYet("The library")
    }
    MenuItem {
        objectName: "action-local-locate-album"
        text: "Locate album"
        onTriggered: contextMenu.notYet("The library")
    }
    MenuSeparator {}
    Menu {
        objectName: "bench-local-rate-menu"
        title: "Rate"
        enabled: contextMenu.rating.canRate ?? false
        Repeater {
            model: [0, 2, 4, 6, 8, 10]
            delegate: RatingMenuItem {
                required property int modelData
                objectName: "action-local-rate-" + modelData
                rating: modelData
                shared: (contextMenu.rating.rating ?? -1) === modelData
                onTriggered: Tk.rateSelection(false, modelData)
            }
        }
    }
    Menu {
        objectName: "bench-local-album-rate-menu"
        title: "Rate album"
        enabled: contextMenu.rating.canRateAlbum ?? false
        Repeater {
            model: [0, 2, 4, 6, 8, 10]
            delegate: RatingMenuItem {
                required property int modelData
                objectName: "action-local-album-rate-" + modelData
                rating: modelData
                shared: (contextMenu.rating.albumRating ?? -1) === modelData
                onTriggered: Tk.rateSelection(true, modelData)
            }
        }
    }
    MenuSeparator {}
    Menu {
        objectName: "bench-track-copy-menu"
        title: "Copy to list"
        MenuItem {
            objectName: "action-copy-to-new-tab"
            text: qsTr("New tab…")
            onTriggered: contextMenu.newTabRequested(false)
        }
        MenuSeparator {
            visible: contextMenu.lists.length > 0
        }
        Repeater {
            model: contextMenu.lists
            delegate: MenuItem {
                required property var modelData
                text: modelData.name
                onTriggered: Tk.transferSelection(modelData.id, false)
            }
        }
    }
    Menu {
        objectName: "bench-track-move-menu"
        title: "Move to list"
        MenuItem {
            objectName: "action-move-to-new-tab"
            text: qsTr("New tab…")
            onTriggered: contextMenu.newTabRequested(true)
        }
        MenuSeparator {
            visible: contextMenu.lists.length > 0
        }
        Repeater {
            model: contextMenu.lists
            delegate: MenuItem {
                required property var modelData
                text: modelData.name
                onTriggered: Tk.transferSelection(modelData.id, true)
            }
        }
    }
    MenuItem {
        text: "Remove selected"
        enabled: contextMenu.selection
        onTriggered: Tk.removeSelectedRows()
    }
    MenuSeparator {}
    SortMenu {
        onCustomRequested: contextMenu.customSortRequested()
    }
    MenuItem {
        objectName: "action-reverse-list"
        text: qsTr("Reverse list")
        enabled: Tk.history.editable ?? false
        onTriggered: Tk.edit.reverse()
    }
    MenuItem {
        objectName: "action-shuffle-albums"
        text: qsTr("Shuffle albums")
        enabled: Tk.history.editable ?? false
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Reorder the whole list by album; retain each album's existing track order. Random playback is unchanged.")
        onTriggered: Tk.edit.shuffleAlbums()
    }
    MenuItem {
        objectName: "action-deduplicate-list"
        text: qsTr("Remove duplicate entries")
        enabled: Tk.history.editable ?? false
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Keep the first occurrence of each exact local source and logical track")
        onTriggered: Tk.edit.removeDuplicates()
    }
    MenuSeparator {}
    MenuItem {
        text: Tk.history.undoText ?? "Undo list edit"
        enabled: Tk.history.canUndo ?? false
        onTriggered: Tk.undoListEdit()
    }
    MenuItem {
        text: Tk.history.redoText ?? "Redo list edit"
        enabled: Tk.history.canRedo ?? false
        onTriggered: Tk.redoListEdit()
    }
    MenuSeparator {}
    Menu {
        title: "Last.fm"
        enabled: (contextMenu.lastFm.artist ?? "") !== "" && (contextMenu.lastFm.title ?? "") !== ""
        onAboutToShow: Tk.askLastFm()
        MenuItem {
            enabled: false
            text: Tk.lastFmState
        }
        MenuItem {
            text: "Love track"
            onTriggered: Tk.loveOnLastFm(true)
        }
        MenuItem {
            text: "Unlove track"
            onTriggered: Tk.loveOnLastFm(false)
        }
    }
}
