// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Style

// A window's bottom bar: a hairline over it, what it reports on the left,
// its buttons on the right -- the closing ones last.
ColumnLayout {
    id: footer
    default property alias content: row.data
    Layout.fillWidth: true
    spacing: 0
    Rectangle {
        Layout.fillWidth: true
        height: 1
        color: Theme.hairline(palette)
    }
    RowLayout {
        id: row
        Layout.fillWidth: true
        Layout.leftMargin: Theme.margin
        Layout.rightMargin: Theme.margin
        Layout.topMargin: Theme.gap + 4
        Layout.bottomMargin: Theme.gap + 4
        spacing: Theme.gap
    }
}
