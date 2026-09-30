// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// "Identify with MusicBrainz" (ADR-0090): search by artist and album (or by
// audio fingerprint), pick the exact release version, then pair the files
// with its tracks and stage the pairs as ordinary coloured drafts. Nothing
// is written here.
ApplicationWindow {
    id: dialog
    objectName: "bench-musicbrainz-identify"

    required property QuickIdentify identify
    readonly property var state: identify.state
    readonly property var match: identify.match
    property int matchRow: -1

    title: qsTr("Identify with MusicBrainz")
    width: 940
    height: 520
    minimumWidth: 640
    minimumHeight: 400
    visible: true
    modality: Qt.WindowModal
    color: palette.window

    Component.onDestruction: identify.release()
    onClosing: Qt.callLater(() => dialog.destroy())

    Connections {
        target: dialog.identify
        function onAccepted() {
            dialog.close();
        }
        function onMatchChanged(select) {
            if (select >= 0)
                dialog.matchRow = select;
            else if (!dialog.identify.matching)
                dialog.matchRow = -1;
        }
        function onCandidatesChanged() {
            if (results.currentIndex < 0 || results.currentIndex >= results.count)
                results.currentIndex = dialog.state.suggested ?? -1;
        }
        function onChanged() {
            if (results.currentIndex < 0 && !(dialog.state.busy ?? false))
                results.currentIndex = dialog.state.suggested ?? -1;
        }
    }

    // A header cell and a text cell, for both pages' tables.
    component HeaderCell: Label {
        font.bold: true
        opacity: 0.8
        elide: Text.ElideRight
        leftPadding: 8
    }
    component Cell: Label {
        property string tip: ""
        elide: Text.ElideRight
        leftPadding: 8
        verticalAlignment: Text.AlignVCenter
        HoverHandler {
            id: cellHover
        }
        ToolTip.visible: cellHover.hovered && tip !== ""
        ToolTip.delay: 700
        ToolTip.text: tip
    }

    StackLayout {
        anchors.fill: parent
        anchors.margins: 10
        currentIndex: dialog.identify.matching ? 1 : 0

        // The release versions.
        ColumnLayout {
            spacing: 8
            RowLayout {
                spacing: 10
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    Label {
                        text: qsTr("Artist:")
                    }
                    TextField {
                        id: artist
                        objectName: "bench-musicbrainz-identify-artist"
                        Layout.fillWidth: true
                        text: dialog.identify.initialArtist
                        placeholderText: qsTr("Artist name (optional if you enter an album)")
                        onAccepted: dialog.identify.search(artist.text, album.text)
                    }
                    Label {
                        text: qsTr("Album:")
                    }
                    TextField {
                        id: album
                        objectName: "bench-musicbrainz-identify-release"
                        Layout.fillWidth: true
                        text: dialog.identify.initialRelease
                        placeholderText: qsTr("Type an album title — existing tags are not required")
                        onAccepted: dialog.identify.search(artist.text, album.text)
                    }
                }
                Button {
                    objectName: "bench-musicbrainz-identify-search"
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Search")
                    highlighted: true
                    enabled: dialog.state.canSearch ?? true
                    onClicked: dialog.identify.search(artist.text, album.text)
                }
                Button {
                    objectName: "bench-musicbrainz-identify-scan"
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Fingerprint files")
                    enabled: dialog.state.canScan ?? false
                    onClicked: dialog.identify.scan()
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Identify by audio fingerprint (AcoustID) — works with no usable tags at all; candidates are ranked by how many selected files match each release")
                }
            }
            Label {
                objectName: "bench-musicbrainz-identify-status"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: dialog.state.status ?? ""
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: palette.base
                border.color: Shade.mix(palette.window, palette.windowText, 0.15)
                radius: 4
                clip: true
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 1
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 26
                        color: Shade.mix(palette.base, palette.window, 0.6)
                        RowLayout {
                            anchors.fill: parent
                            spacing: 0
                            HeaderCell { text: qsTr("Match"); Layout.preferredWidth: 150 }
                            HeaderCell { text: qsTr("Album"); Layout.fillWidth: true }
                            HeaderCell { text: qsTr("Artist"); Layout.preferredWidth: 170 }
                            HeaderCell { text: qsTr("Tracks"); Layout.preferredWidth: 60 }
                            HeaderCell { text: qsTr("Media"); Layout.preferredWidth: 90 }
                            HeaderCell { text: qsTr("Version"); Layout.preferredWidth: 220 }
                        }
                    }
                    ListView {
                        id: results
                        objectName: "bench-musicbrainz-identify-results"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        currentIndex: -1
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        model: dialog.identify.candidates
                        delegate: Rectangle {
                            id: row
                            required property var modelData
                            required property int index
                            width: ListView.view.width
                            height: 28
                            color: ListView.isCurrentItem ? Shade.alpha(palette.highlight, 0.35)
                                   : (index % 2 ? palette.alternateBase : palette.base)
                            TapHandler {
                                onTapped: results.currentIndex = row.index
                                onDoubleTapped: dialog.identify.use(row.index)
                            }
                            RowLayout {
                                anchors.fill: parent
                                spacing: 0
                                Cell { text: row.modelData.match; tip: row.modelData.matchToolTip; Layout.preferredWidth: 150 }
                                Cell { text: row.modelData.album; tip: row.modelData.albumToolTip; Layout.fillWidth: true }
                                Cell { text: row.modelData.artist; Layout.preferredWidth: 170 }
                                Cell { text: row.modelData.tracks; Layout.preferredWidth: 60 }
                                Cell { text: row.modelData.media; Layout.preferredWidth: 90 }
                                Cell { text: row.modelData.version; tip: row.modelData.version; Layout.preferredWidth: 220 }
                            }
                        }
                    }
                }
            }
            RowLayout {
                Item {
                    Layout.fillWidth: true
                }
                Button {
                    objectName: "bench-musicbrainz-identify-use"
                    text: qsTr("Match this version…")
                    enabled: !(dialog.state.busy ?? false) && results.currentIndex >= 0
                    onClicked: dialog.identify.use(results.currentIndex)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Load the release tracks and review their assignments to local files")
                }
                Button {
                    text: qsTr("Close")
                    onClicked: dialog.close()
                }
            }
        }

        // The chosen version's tracks, paired with the files.
        ColumnLayout {
            objectName: "bench-musicbrainz-track-match"
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.bold: true
                text: dialog.match.heading ?? ""
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.8
                text: dialog.match.help ?? ""
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: palette.base
                border.color: Shade.mix(palette.window, palette.windowText, 0.15)
                radius: 4
                clip: true
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 1
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 26
                        color: Shade.mix(palette.base, palette.window, 0.6)
                        RowLayout {
                            anchors.fill: parent
                            spacing: 0
                            HeaderCell { text: qsTr("Local filename"); Layout.fillWidth: true }
                            HeaderCell { text: qsTr("Length"); Layout.preferredWidth: 64 }
                            HeaderCell { text: qsTr("Pairing"); Layout.preferredWidth: 90 }
                            HeaderCell { text: qsTr("MusicBrainz track"); Layout.fillWidth: true }
                            HeaderCell { text: qsTr("Length"); Layout.preferredWidth: 64 }
                        }
                    }
                    ListView {
                        id: pairs
                        objectName: "bench-musicbrainz-match-files"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        focus: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        model: dialog.identify.matchRows
                        currentIndex: dialog.matchRow
                        onCurrentIndexChanged: dialog.matchRow = currentIndex
                        // The row a dragged file is dropped before; -1 when
                        // nothing is dragged.
                        property int dropBefore: -1
                        Keys.onPressed: event => {
                            if (event.modifiers & Qt.AltModifier) {
                                if (event.key === Qt.Key_Up) {
                                    dialog.identify.move(dialog.matchRow, -1);
                                    event.accepted = true;
                                } else if (event.key === Qt.Key_Down) {
                                    dialog.identify.move(dialog.matchRow, 1);
                                    event.accepted = true;
                                }
                            }
                        }
                        delegate: Rectangle {
                            id: pair
                            required property var modelData
                            required property int index
                            width: ListView.view.width
                            height: 30
                            color: ListView.isCurrentItem ? Shade.alpha(palette.highlight, 0.35)
                                   : modelData.paired ? Shade.mix(palette.base, palette.highlight, 0.1)
                                   : (index % 2 ? palette.alternateBase : palette.base)
                            Rectangle {
                                visible: pairs.dropBefore === pair.index
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                height: 2
                                color: palette.highlight
                            }
                            MouseArea {
                                id: grab
                                anchors.fill: parent
                                property int from: -1
                                property real startY: 0
                                onPressed: mouse => {
                                    pairs.currentIndex = pair.index;
                                    pairs.forceActiveFocus();
                                    from = pair.index;
                                    startY = mouse.y;
                                }
                                onPositionChanged: mouse => {
                                    if (from < 0 || Math.abs(mouse.y - startY) < 6)
                                        return;
                                    const y = pair.mapToItem(pairs.contentItem, 0, mouse.y).y;
                                    const target = pairs.indexAt(0, y);
                                    let before = target < 0 ? pairs.count : target;
                                    if (target >= 0 && y - pairs.itemAtIndex(target).y > pair.height / 2)
                                        ++before;
                                    pairs.dropBefore = before;
                                }
                                onReleased: {
                                    if (pairs.dropBefore >= 0 && from >= 0) {
                                        let boundary = pairs.dropBefore;
                                        if (boundary > from)
                                            --boundary;
                                        if (boundary !== from)
                                            dialog.identify.moveFile(from, Math.min(boundary, pairs.count - 1));
                                    }
                                    pairs.dropBefore = -1;
                                    from = -1;
                                }
                            }
                            RowLayout {
                                anchors.fill: parent
                                spacing: 0
                                Cell { text: pair.modelData.file; tip: pair.modelData.fileToolTip; Layout.fillWidth: true; elide: Text.ElideMiddle }
                                Cell { text: pair.modelData.length; Layout.preferredWidth: 64 }
                                Cell {
                                    text: pair.modelData.pairing
                                    tip: pair.modelData.pairingToolTip
                                    Layout.preferredWidth: 90
                                    font.bold: pair.modelData.paired
                                    color: pair.modelData.paired ? palette.link : palette.text
                                }
                                Cell { text: pair.modelData.track; tip: pair.modelData.track; Layout.fillWidth: true; elide: Text.ElideMiddle }
                                Cell { text: pair.modelData.trackLength; Layout.preferredWidth: 64 }
                            }
                        }
                    }
                }
            }
            RowLayout {
                Button {
                    objectName: "bench-musicbrainz-match-up"
                    text: qsTr("Move file up")
                    enabled: {
                        dialog.match;
                        return dialog.identify.matching && dialog.identify.canMoveUp(dialog.matchRow);
                    }
                    onClicked: dialog.identify.move(dialog.matchRow, -1)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Swap local files with the row above (Alt+Up)")
                }
                Button {
                    objectName: "bench-musicbrainz-match-down"
                    text: qsTr("Move file down")
                    enabled: {
                        dialog.match;
                        return dialog.identify.matching && dialog.identify.canMoveDown(dialog.matchRow);
                    }
                    onClicked: dialog.identify.move(dialog.matchRow, 1)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Swap local files with the row below (Alt+Down)")
                }
                Button {
                    objectName: "bench-musicbrainz-match-unmatch"
                    text: qsTr("Leave unmatched")
                    enabled: {
                        dialog.match;
                        return dialog.identify.matching && dialog.identify.canUnmatch(dialog.matchRow);
                    }
                    onClicked: dialog.identify.unmatch(dialog.matchRow)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Move this file below the album tracks, leaving a gap. It will receive no tags.")
                }
                Button {
                    objectName: "bench-musicbrainz-match-sort"
                    text: qsTr("Match by filename")
                    enabled: dialog.match.ready ?? false
                    onClicked: dialog.identify.resetOrder(true)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Pair files in natural filename order with the album tracks. This replaces the current pairings.")
                }
                Button {
                    objectName: "bench-musicbrainz-match-order"
                    text: qsTr("Reset file order")
                    enabled: dialog.match.ready ?? false
                    onClicked: dialog.identify.resetOrder(false)
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Pair files in their original selection order with the album tracks.")
                }
            }
            Label {
                objectName: "bench-musicbrainz-match-status"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: dialog.match.status ?? ""
            }
            RowLayout {
                Button {
                    objectName: "bench-musicbrainz-match-back"
                    text: qsTr("Back to releases")
                    onClicked: dialog.identify.back()
                }
                Item {
                    Layout.fillWidth: true
                }
                Button {
                    objectName: "bench-musicbrainz-match-stage"
                    text: qsTr("Stage matches")
                    highlighted: true
                    enabled: dialog.match.canStage ?? false
                    onClicked: dialog.identify.stage()
                    ToolTip.visible: hovered
                    ToolTip.delay: 800
                    ToolTip.text: qsTr("Confirm the displayed assignments and stage tags for matched files. Unmatched files are untouched; Apply writes the draft later.")
                }
            }
        }
    }
}
