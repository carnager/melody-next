// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Style

// Text that explains or reports rather than asks: quieter, wrapped.
Label {
    Layout.fillWidth: true
    wrapMode: Text.WordWrap
    color: Theme.dim(palette)
    linkColor: palette.highlight
    onLinkActivated: link => Qt.openUrlExternally(link)
}
