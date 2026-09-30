// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick
import Trackknife.Style

// PlaybackTabBar: tabs drawn by the bar itself, the same under every style.
// The current tab is filled with the list's own ground so it reads as the
// top of the list below it; the others are plain, quieter text. The playing
// tab carries an accent dot; a remote tab its engine's icon.
Item {
    id: bar

    signal contextMenuRequested(int index, point position)
    signal newListRequested()

    // Room either side of what a tab shows, and for its close button --
    // kept when a pinned tab hides it, so pinning does not resize the tab.
    readonly property int lead: 12
    readonly property int trail: 10
    readonly property int closeRoom: 13
    readonly property int slack: 12
    readonly property int dotWidth: 11
    readonly property int iconGap: 5
    readonly property int closeInset: 14
    readonly property int heightPadding: 16

    implicitHeight: metrics.height + heightPadding

    FontMetrics {
        id: metrics
    }

    ToolTip.visible: emptyHover.hovered && !tabs.hoveredTab
    ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
    ToolTip.text: qsTr("Drop local tracks on a tab to transfer, or on empty tab-bar space to create a tab. Hold Ctrl to copy.")
    HoverHandler {
        id: emptyHover
    }
    // Double-clicking empty tab-bar space makes a new list.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: bar.newListRequested()
    }

    // Tracks, a library's selection, folders or files dropped on a tab go
    // to that list (moved, or copied with Ctrl); on empty space, into a new
    // one.
    DropArea {
        id: drops
        anchors.fill: parent
        keys: ["application/x-trackknife-drag", "text/uri-list"]
        property int over: -1
        function tabAt(x, y) {
            return tabs.indexAt(x + tabs.contentX, y);
        }
        function show(x, y) {
            over = tabAt(x, y);
            const item = over >= 0 ? tabs.itemAtIndex(over) : null;
            marker.visible = true;
            if (item) {
                marker.x = item.x - tabs.contentX;
                marker.width = item.width;
            } else {
                marker.x = Math.min(tabs.contentWidth - tabs.contentX, bar.width - 40);
                marker.width = 36;
            }
        }
        onEntered: drag => show(drag.x, drag.y)
        onPositionChanged: drag => show(drag.x, drag.y)
        onExited: marker.visible = false
        onDropped: drop => {
            marker.visible = false;
            const taken = Tk.draggedKind() !== ""
                          ? Tk.dropOnTab(over, -1, drop.action === Qt.CopyAction)
                          : Tk.dropUrls(drop.urls, over, -1);
            if (taken)
                drop.accept(drop.action);
        }
    }
    DropMarker {
        id: marker
        whole: true
        y: 2
        height: bar.height - 4
    }

    ListView {
        id: tabs

        property bool hoveredTab: false

        anchors.fill: parent
        orientation: ListView.Horizontal
        interactive: contentWidth > width
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        model: Tk.tabs
        currentIndex: Tk.currentTab
        highlightFollowsCurrentItem: false
        onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

        // After the last tab: a new list.
        footer: Item {
            width: 30
            height: tabs.height
            Item {
                id: plus
                x: 4
                y: 3 + (parent.height - 3 - height) / 2
                width: 22
                height: 22
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("New list")
                HoverHandler {
                    id: plusHover
                }
                ToolTip.visible: plusHover.hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("New list")
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    color: Theme.hovered(bar.palette)
                    opacity: plusHover.hovered ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation { duration: Theme.quick }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    text: "+"
                    font.pixelSize: 16
                    color: plusHover.hovered ? bar.palette.windowText : bar.palette.placeholderText
                }
                TapHandler {
                    onTapped: bar.newListRequested()
                }
            }
        }

        delegate: Item {
            id: tab

            required property int index
            required property string text
            required property string tooltip
            required property bool playing
            required property bool remote
            required property bool pinned

            readonly property bool current: index === Tk.currentTab
            readonly property int iconWidth: remote ? 14 + bar.iconGap : 0
            readonly property int dot: playing ? bar.dotWidth : 0

            width: bar.lead + label.implicitWidth + dot + iconWidth + bar.slack + bar.closeRoom
                   + bar.trail
            height: tabs.height
            z: current ? 1 : 0

            HoverHandler {
                id: hover
                onHoveredChanged: tabs.hoveredTab = hovered
            }
            ToolTip.visible: hover.hovered && !closeHover.hovered
            ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: tab.tooltip

            // The fill: Base, rounded at the top only; a hovered tab at
            // alpha 110.
            Item {
                x: 1
                y: 3
                width: parent.width - 2
                height: parent.height - 3
                clip: true
                visible: tab.current || hover.hovered
                Rectangle {
                    width: parent.width
                    height: parent.height + 6
                    radius: 5
                    color: tab.current ? bar.palette.base : Shade.alpha(bar.palette.base, 110 / 255)
                }
            }

            // What it shows, centred in the room left of the close button.
            Row {
                x: bar.lead + Math.max(0, (tab.width - bar.lead - bar.trail - bar.closeRoom
                                           - width) / 2)
                y: 3
                height: parent.height - 3
                Item {
                    visible: tab.playing
                    width: bar.dotWidth
                    height: parent.height
                    Rectangle {
                        x: 0
                        y: parent.height / 2 + 1 - 3.5
                        width: 7
                        height: 7
                        radius: 3.5
                        color: bar.palette.highlight
                    }
                }
                Item {
                    visible: tab.remote
                    width: tab.iconWidth
                    height: parent.height
                    Image {
                        y: parent.height / 2 - 7 + 1
                        width: 14
                        height: 14
                        sourceSize: Qt.size(14, 14)
                        source: "image://icon/network-server" + (tab.current ? "" : "?disabled")
                    }
                }
                Label {
                    id: label
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: tab.text
                    color: tab.current ? bar.palette.text : bar.palette.placeholderText
                }
            }

            // TabCloseButton: a small cross, brighter when pointed at; hidden,
            // but its room kept, for a pinned tab.
            Item {
                id: close
                visible: !tab.pinned
                width: 16
                height: 16
                x: tab.width - bar.closeInset - width / 2
                y: (tab.height - height) / 2
                Accessible.role: Accessible.Button
                Accessible.name: "Close tab"
                HoverHandler {
                    id: closeHover
                }
                ToolTip.visible: closeHover.hovered
                ToolTip.text: "Close tab"
                Canvas {
                    anchors.fill: parent
                    property color ink: closeHover.hovered ? bar.palette.text
                                                           : bar.palette.placeholderText
                    onInkChanged: requestPaint()
                    onPaint: {
                        const context = getContext("2d");
                        context.reset();
                        context.strokeStyle = ink;
                        context.lineWidth = 1.8;
                        context.lineCap = "round";
                        const cx = width / 2, cy = height / 2, arm = 3;
                        context.beginPath();
                        context.moveTo(cx - arm, cy - arm);
                        context.lineTo(cx + arm, cy + arm);
                        context.moveTo(cx - arm, cy + arm);
                        context.lineTo(cx + arm, cy - arm);
                        context.stroke();
                    }
                }
                TapHandler {
                    onTapped: Tk.closeTab(tab.index)
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressedChanged: {
                    if (pressed)
                        Tk.currentTab = tab.index;
                }
                onTapped: (eventPoint, button) => {
                    if (button === Qt.RightButton)
                        bar.contextMenuRequested(tab.index, tab.mapToItem(bar, eventPoint.position));
                }
            }
            TapHandler {
                acceptedButtons: Qt.MiddleButton
                onTapped: Tk.closeTab(tab.index)
            }
            // Dragged along the bar, a tab moves; it stays among its
            // engine's tabs (ADR-0234).
            DragHandler {
                id: drag
                target: null
                yAxis.enabled: false
                onActiveTranslationChanged: {
                    if (!active)
                        return;
                    const point = tab.mapToItem(tabs.contentItem, tab.width / 2
                                                + activeTranslation.x, tab.height / 2);
                    const over = tabs.indexAt(point.x, point.y);
                    if (over >= 0 && over !== tab.index)
                        Tk.moveTab(tab.index, over);
                }
            }
        }
    }
}
