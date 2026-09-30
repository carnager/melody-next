// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// A command whose key Settings › Shortcuts may change: `defaultKey`, unless
// another is saved under its object name.
Action {
    property string defaultKey
    shortcut: Tk.shortcutRevision >= 0 ? Tk.shortcut(objectName, defaultKey) : ""
}
