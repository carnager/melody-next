// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic

Rectangle {
    id: button

    property string icon
    property int iconSize: 16
    property bool checked: false
    property string tip
    signal clicked

    implicitWidth: iconSize + 10
    implicitHeight: iconSize + 10
    radius: 4
    color: mouse.pressed ? Theme.raised : mouse.containsMouse ? Theme.hover : "transparent"
    Behavior on color { ColorAnimation { duration: 90 } }

    Icon {
        anchors.centerIn: parent
        width: button.iconSize
        height: button.iconSize
        name: button.icon
        color: button.checked ? Theme.accent : mouse.containsMouse ? Theme.text : Theme.dim
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }

    ToolTip.visible: button.tip !== "" && mouse.containsMouse
    ToolTip.delay: 600
    ToolTip.text: button.tip
}
