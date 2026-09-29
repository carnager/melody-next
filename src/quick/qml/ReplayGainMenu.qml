// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// ReplayGain: Off, Track, Album or Automatic; then the preamps. The same
// menu in the Playback menu and on the status bar.
Menu {
    id: menu
    title: "ReplayGain"

    signal preampRequested()

    Instantiator {
        model: Tk.modes.replaygainMode !== undefined ? Tk.replayGainModes() : []
        delegate: MenuItem {
            required property var modelData
            objectName: "action-local-replaygain-" + modelData.value
            text: modelData.label
            checkable: true
            checked: Tk.modes.replaygainMode === modelData.value
            enabled: Tk.modes.enabled ?? false
            onTriggered: Tk.setReplayGain(modelData.value)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.takeItem(index)
    }
    MenuSeparator {}
    MenuItem {
        text: "Preamp…"
        onTriggered: menu.preampRequested()
    }
}
