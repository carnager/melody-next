// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Style

// A form's label: right-aligned in a column of one width, so the fields of
// every form in a window start in one line.
Label {
    property int columnWidth: Theme.labelWidth
    Layout.preferredWidth: columnWidth
    Layout.minimumWidth: columnWidth
    Layout.alignment: Qt.AlignVCenter
    horizontalAlignment: Text.AlignRight
    wrapMode: Text.WordWrap
}
