// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// The list on show (ui::QueueTableView): a flat header over rows drawn by
// the album view's rules (ui::trackCell), as the widgets delegate draws them.
FocusScope {
    id: table

    signal contextMenuRequested(int row, point position)
    signal headerMenuRequested(point position)

    readonly property var rows: Tk.rows
    readonly property var columns: rows.columns
    readonly property bool grouped: rows.grouped
    readonly property bool side: rows.sideArtwork
    readonly property color base: palette.base
    readonly property color ink: palette.text
    // The first visible column holds the covers: album headers sit beside
    // them, and start where the rows' text does.
    readonly property bool coverLeads: side && columns.length > 0 && columns[0].logical === 0
    readonly property int coverWidth: coverLeads ? columns[0].width : 0

    function columnX(logical) {
        let x = 0;
        for (const column of columns) {
            if (column.logical === logical)
                return x;
            x += column.width;
        }
        return -1;
    }
    function columnWidth(logical) {
        for (const column of columns)
            if (column.logical === logical)
                return column.width;
        return 0;
    }
    function jumpTo(row) {
        list.positionViewAtIndex(row, ListView.Center);
    }

    onWidthChanged: rows.setViewportWidth(list.width)
    Component.onCompleted: rows.setViewportWidth(list.width)

    FontMetrics {
        id: metrics
    }

    // FlatHeaderStyle: Base ground, bold labels 58.6 % of the way to Text,
    // 18 px separators at 15.7 %, no rule below.
    Rectangle {
        id: header
        width: parent.width
        height: metrics.height + 14
        color: table.base
        Row {
            Repeater {
                model: table.columns
                delegate: Item {
                    id: section
                    required property var modelData
                    required property int index
                    width: modelData.width
                    height: header.height
                    Label {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        text: section.modelData.header
                        font.bold: true
                        elide: Text.ElideRight
                        color: Shade.mix(table.base, table.ink, 0.586)
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 1
                        height: 18
                        color: Shade.mix(table.base, table.ink, 0.157)
                    }
                    // A column is widened or narrowed by its right edge; the
                    // width chosen is the one it prefers from then on.
                    MouseArea {
                        anchors.right: parent.right
                        anchors.rightMargin: -3
                        width: 6
                        height: parent.height
                        cursorShape: Qt.SplitHCursor
                        property real pressX: 0
                        property int startWidth: 0
                        onPressed: mouse => {
                            pressX = mapToItem(header, mouse.x, 0).x;
                            startWidth = section.modelData.width;
                        }
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return;
                            const x = mapToItem(header, mouse.x, 0).x;
                            Tk.setColumnWidth(section.modelData.id,
                                              Math.max(section.modelData.minimum,
                                                       startWidth + x - pressX));
                        }
                    }
                }
            }
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: eventPoint => table.headerMenuRequested(
                          header.mapToItem(table, eventPoint.position))
        }
    }

    Rectangle {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        color: table.base
    }

    ListView {
        id: list
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        clip: true
        focus: true
        model: table.rows
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true
        currentIndex: table.rows.currentRow
        highlightFollowsCurrentItem: false
        keyNavigationEnabled: false
        ScrollBar.vertical: ScrollBar {}
        onWidthChanged: table.rows.setViewportWidth(width)

        delegate: Item {
            id: row

            required property int index
            required property var cells
            required property int spacing
            required property bool groupStart
            required property bool looseRun
            required property string disc
            required property string headerAlbum
            required property string headerDetails
            required property string coverKey
            required property int coverOffset
            required property int groupHeight
            required property bool selected
            required property bool currentTrack
            required property string rowTooltip

            readonly property int discHeight: disc !== "" ? 26 : 0
            readonly property int headerHeight: groupStart ? 30 : looseRun ? 10 : 0

            width: list.width
            height: 22 + spacing

            // Plain columns stripe their rows; albums need no stripes.
            Rectangle {
                y: row.spacing
                width: parent.width
                height: 22
                visible: !table.grouped && row.index % 2 === 1
                color: row.palette.alternateBase
            }

            // An album's header: its name, then "artist · year · N tracks ·
            // length", quieter, beside its cover.
            Item {
                visible: row.groupStart
                width: parent.width
                height: 30
                // In the header strip, when covers are not beside the rows.
                Image {
                    visible: !table.side && table.columnX(0) >= 0
                    x: table.columnX(0) + 6
                    y: 15 - 11
                    width: 22
                    height: 22
                    asynchronous: false
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(44, 44)
                    source: visible && row.coverKey !== ""
                            ? "image://cover/" + encodeURIComponent(row.coverKey) + "#"
                              + Tk.coverRevision : ""
                }
                Row {
                    readonly property int start: table.side
                                                ? (table.coverLeads && table.columns.length > 1
                                                   ? table.columns[0].width : 0)
                                                : Math.max(0, table.columnX(3))
                    readonly property int room: table.side ? row.width - start
                                                           : table.columnWidth(3)
                    x: start + 6
                    width: Math.max(0, room - 14)
                    y: 30 - 6 - albumName.baselineOffset
                    spacing: 10
                    clip: true
                    Label {
                        id: albumName
                        width: Math.min(implicitWidth, parent.width)
                        text: row.headerAlbum
                        elide: Text.ElideRight
                        font.weight: Font.DemiBold
                        font.pointSize: Qt.application.font.pointSize * 1.08
                        color: table.ink
                    }
                    Label {
                        anchors.baseline: albumName.baseline
                        width: Math.max(0, parent.width - albumName.width - 12)
                        visible: width > 0
                        text: row.headerDetails
                        elide: Text.ElideRight
                        color: row.palette.placeholderText
                    }
                }
            }
            Rectangle {
                visible: row.looseRun
                y: 5
                width: parent.width
                height: 1
                color: Shade.alpha(table.ink, 38 / 255)
            }
            // A disc's name where its tracks start, in line with their titles.
            Label {
                visible: row.disc !== ""
                x: Math.max(0, table.columnX(3)) + 6
                y: row.headerHeight
                width: Math.max(0, table.columnWidth(3) - 14)
                height: 26 - 4
                verticalAlignment: Text.AlignBottom
                text: row.disc
                elide: Text.ElideRight
                font.weight: Font.DemiBold
                font.pointSize: Qt.application.font.pointSize * 0.92
                color: row.palette.placeholderText
            }

            // The track.
            Row {
                y: row.spacing
                height: 22
                Repeater {
                    model: row.cells
                    delegate: Item {
                        id: cell
                        required property var modelData
                        required property int index
                        readonly property var column: table.columns[index] ?? ({width: 0})
                        readonly property bool artwork: modelData.artworkCell ?? false
                        readonly property bool tinted: row.selected && !artwork
                        width: column.width
                        height: 22

                        Rectangle {
                            anchors.fill: parent
                            visible: cell.tinted
                            color: table.grouped ? Shade.mix(table.base, row.palette.highlight, 0.32)
                                                 : row.palette.highlight
                        }
                        Image {
                            id: playing
                            visible: cell.modelData.playingIcon ?? false
                            x: 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: visible ? 14 : 0
                            height: 14
                            sourceSize: Qt.size(14, 14)
                            source: visible ? "image://icon/media-playback-start|sp:SP_MediaPlay" : ""
                        }
                        Row {
                            x: playing.visible ? playing.x + playing.width + 4 : 4
                            width: parent.width - x - 4
                            height: parent.height
                            readonly property color accent: Shade.lighter(row.palette.highlight, 115)
                            readonly property color shade:
                                (cell.modelData.foreground ?? "") !== "" ? cell.modelData.foreground
                                : cell.modelData.current ? accent
                                : !table.grouped && row.selected ? row.palette.highlightedText
                                : cell.modelData.quiet ? row.palette.placeholderText : table.ink
                            Label {
                                id: text
                                width: Math.min(implicitWidth, parent.width)
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: cell.modelData.rightAligned ? Text.AlignRight
                                                                                 : Text.AlignLeft
                                text: cell.modelData.text ?? ""
                                elide: Text.ElideRight
                                color: parent.shade
                                font.weight: cell.modelData.current ? Font.DemiBold : Font.Normal
                                Component.onCompleted: if (cell.modelData.rightAligned)
                                    width = Qt.binding(() => parent.width)
                            }
                            Label {
                                readonly property real room: parent.width - text.width
                                visible: (cell.modelData.suffix ?? "") !== "" && room > 12
                                width: room
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                text: " — " + (cell.modelData.suffix ?? "")
                                elide: Text.ElideRight
                                color: cell.modelData.current ? parent.shade
                                                              : row.palette.placeholderText
                            }
                        }
                        ToolTip.visible: cellHover.hovered && (cell.modelData.tooltip ?? "") !== ""
                        ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
                        ToolTip.text: cell.modelData.tooltip ?? ""
                        HoverHandler {
                            id: cellHover
                        }
                    }
                }
            }

            // The album's cover at its top left, the same size for every
            // album, drawn by each row it reaches, clipped to that row.
            Item {
                id: sideCover
                readonly property int columnLeft: table.columnX(0)
                visible: table.side && columnLeft >= 0 && row.groupHeight > 0
                         && row.coverOffset < 6 + 44
                x: columnLeft
                width: table.columnWidth(0)
                height: row.height
                clip: true
                Item {
                    x: 12
                    y: 6 - row.coverOffset
                    width: 44
                    height: 44
                    Rectangle {
                        anchors.fill: parent
                        visible: art.status !== Image.Ready || art.implicitWidth <= 1
                        radius: 3
                        color: row.palette.mid
                        Image {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            sourceSize: Qt.size(16, 16)
                            source: "image://icon/media-optical-audio|sp:SP_FileIcon?disabled"
                        }
                    }
                    Image {
                        id: art
                        anchors.fill: parent
                        asynchronous: false
                        fillMode: Image.PreserveAspectCrop
                        sourceSize: Qt.size(88, 88)
                        source: sideCover.visible && row.coverKey !== ""
                                ? "image://cover/" + encodeURIComponent(row.coverKey) + "#"
                                  + Tk.coverRevision : ""
                    }
                }
            }

            // The keyboard's place: one outline around the current row,
            // after a leading cover column, while the list has focus.
            Rectangle {
                visible: list.activeFocus && row.index === table.rows.currentRow
                x: table.coverLeads ? table.coverWidth : 0
                y: row.spacing
                width: parent.width - x
                height: 22
                color: "transparent"
                border.width: 1
                border.color: Shade.alpha(row.palette.highlight, 200 / 255)
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onPressedChanged: {
                    if (pressed) {
                        list.forceActiveFocus();
                        table.rows.press(row.index, point.modifiers);
                    }
                }
                onDoubleTapped: Tk.activateRow(row.index)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: eventPoint => {
                    list.forceActiveFocus();
                    // Right-clicking an album's header selects the album.
                    if (row.groupStart && eventPoint.position.y < 30)
                        table.rows.selectGroup(row.index);
                    else if (!row.selected)
                        table.rows.press(row.index, 0);
                    table.contextMenuRequested(row.index, row.mapToItem(table, eventPoint.position));
                }
            }
        }

        Keys.onPressed: event => {
            const current = table.rows.currentRow;
            const page = Math.max(1, Math.floor(height / 22));
            let target = -1;
            switch (event.key) {
            case Qt.Key_Up: target = current < 0 ? 0 : current - 1; break;
            case Qt.Key_Down: target = current + 1; break;
            case Qt.Key_PageUp: target = current - page; break;
            case Qt.Key_PageDown: target = current + page; break;
            case Qt.Key_Home: target = 0; break;
            case Qt.Key_End: target = count - 1; break;
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (current >= 0 && !(event.modifiers & Qt.ControlModifier)) {
                    Tk.activateRow(current);
                    event.accepted = true;
                }
                return;
            case Qt.Key_A:
                if (event.modifiers & Qt.ControlModifier) {
                    table.rows.selectAll();
                    event.accepted = true;
                }
                return;
            default:
                return;
            }
            table.rows.moveCurrent(Math.max(0, Math.min(count - 1, target)), event.modifiers);
            positionViewAtIndex(table.rows.currentRow, ListView.Contain);
            event.accepted = true;
        }

        // An empty list says what can go in it, two fifths of the way down.
        Column {
            visible: list.count === 0
            x: 24
            width: list.width - 48
            y: list.height * 2 / 5 - height / 2
            spacing: 4
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: Tk.list.emptyTitle ?? ""
                font.weight: Font.DemiBold
                font.pointSize: Qt.application.font.pointSize * 1.15
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: Tk.list.emptyHint ?? ""
                color: table.palette.placeholderText
            }
        }
    }

    Connections {
        target: Tk
        function onRevealRow(row) {
            table.rows.selectRows([row], row);
            list.positionViewAtIndex(row, ListView.Center);
        }
        function onFindDismissed() {
            list.forceActiveFocus();
        }
        function onPlaybackCursor(row, jump) {
            table.rows.selectRows([row], row);
            list.positionViewAtIndex(row, ListView.Center);
            if (jump)
                list.forceActiveFocus();
        }
    }
}
