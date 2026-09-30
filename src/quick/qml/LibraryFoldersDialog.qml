// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// The library's folders, in a dialog of their own.
Dialog {
    id: dialog
    objectName: "local-library-folders-dialog"

    property var browser

    title: browser && browser.remote() ? qsTr("Library folders on %1").arg(browser.name())
                                       : qsTr("Local library folders")
    anchors.centerIn: Overlay.overlay
    width: 560
    height: 320
    modal: false
    standardButtons: Dialog.Close

    onOpened: if (browser) browser.loadRoots()

    LibraryFolders {
        anchors.fill: parent
        browser: dialog.browser
    }
}
