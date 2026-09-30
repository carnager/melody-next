// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// ADR-0238: what Apply does, all in view at once -- tags, renaming with a
// naming layout, moving to a destination, ReplayGain, and the scripts that
// stage their edits automatically. Choices are remembered for the next
// editor.
Popup {
    id: actions

    required property QuickTagger tagger
    readonly property var state: tagger.state

    signal provenance()
    // The script editor, on the saved script `id` (empty: a new one).
    signal scriptEditor(string id)
    signal settings(string page)
    signal destinations()
    signal expression()

    padding: Theme.gapLarge
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    component LinkLabel: Label {
        id: link
        signal activated()
        textFormat: Text.RichText
        linkColor: palette.highlight
        onLinkActivated: {
            actions.close();
            activated();
        }
        opacity: enabled ? 1 : 0.5
        HoverHandler {
            cursorShape: link.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }

    contentItem: GridLayout {
        columns: 2
        columnSpacing: Theme.gap + 4
        rowSpacing: Theme.gap

        CheckBox {
            objectName: "bench-actions-save-tags"
            Layout.columnSpan: 2
            text: qsTr("Save tags")
            checked: actions.state.saveTags ?? true
            onClicked: {
                actions.tagger.setSaveTags(checked);
                checked = Qt.binding(() => actions.state.saveTags ?? true);
            }
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Write the drafted tag edits into the files")
        }

        CheckBox {
            objectName: "bench-actions-rename-files"
            text: qsTr("Rename files")
            enabled: actions.state.renameAvailable ?? false
            checked: actions.state.renameFiles ?? false
            onClicked: {
                actions.tagger.chooseRename(checked);
                checked = Qt.binding(() => actions.state.renameFiles ?? false);
            }
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: actions.state.renameTooltip ?? ""
        }
        ComboBox {
            objectName: "bench-actions-layout"
            Layout.fillWidth: true
            Layout.minimumWidth: 240
            enabled: actions.state.layoutsAvailable ?? false
            model: actions.tagger.layouts
            currentIndex: actions.state.layoutIndex ?? -1
            displayText: currentIndex < 0 ? qsTr("None saved yet") : currentText
            onActivated: index => actions.tagger.selectLayout(index)
            Accessible.name: qsTr("Naming layout")
        }

        CheckBox {
            objectName: "bench-actions-move-files"
            text: qsTr("Move files")
            enabled: actions.state.moveAvailable ?? false
            checked: actions.state.moveFiles ?? false
            onClicked: {
                actions.tagger.chooseMove(checked);
                checked = Qt.binding(() => actions.state.moveFiles ?? false);
            }
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: actions.state.moveTooltip ?? ""
        }
        ComboBox {
            objectName: "bench-actions-destination"
            Layout.fillWidth: true
            enabled: actions.state.destinationsAvailable ?? false
            model: actions.tagger.destinations
            currentIndex: actions.state.destinationIndex ?? -1
            displayText: currentIndex >= 0 ? currentText
                         : ((actions.state.destinationsOn ?? "") !== ""
                            ? qsTr("None saved on %1 yet").arg(actions.state.destinationsOn)
                            : qsTr("None saved yet"))
            onActivated: index => actions.tagger.selectDestination(index)
            Accessible.name: qsTr("Move destination")
            ToolTip.visible: hovered && (actions.state.destinationsOn ?? "") !== ""
            ToolTip.delay: 800
            ToolTip.text: qsTr("Move destinations on %1").arg(actions.state.destinationsOn ?? "")
        }

        Item {
            Layout.preferredWidth: 1
        }
        RowLayout {
            spacing: 16
            LinkLabel {
                objectName: "bench-actions-manage-layouts"
                text: "<a href=\"#\">" + qsTr("Manage naming layouts…") + "</a>"
                onActivated: actions.settings("naming")
            }
            LinkLabel {
                objectName: "bench-actions-manage-destinations"
                text: "<a href=\"#\">" + qsTr("Manage move destinations…") + "</a>"
                onActivated: actions.destinations()
            }
        }

        Label {
            leftPadding: 4
            text: qsTr("ReplayGain")
        }
        RowLayout {
            ComboBox {
                objectName: "bench-actions-replaygain-grouping"
                Layout.fillWidth: true
                model: [qsTr("Album by release"), qsTr("Album merging discs"),
                        qsTr("Selection as one album"), qsTr("Track gains only"),
                        qsTr("Group by expression")]
                currentIndex: actions.state.grouping ?? 0
                onActivated: index => {
                    actions.tagger.setReplayGainGrouping(index);
                    if (index === 4)
                        actions.expression();
                }
                Accessible.name: qsTr("ReplayGain grouping")
            }
            Button {
                objectName: "bench-actions-replaygain-scan"
                text: qsTr("Scan now")
                enabled: actions.state.canScan ?? false
                onClicked: {
                    actions.close();
                    actions.tagger.scanReplayGain();
                }
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("Measure EBU R128 loudness for the selected files and stage the ReplayGain tags as colored draft edits — nothing is written until you apply")
            }
        }
        Item {
            Layout.preferredWidth: 1
        }
        RowLayout {
            spacing: 16
            LinkLabel {
                objectName: "bench-actions-loudness-sources"
                enabled: actions.state.canShowSources ?? false
                text: "<a href=\"#\">" + qsTr("Loudness sources…") + "</a>"
                onActivated: actions.provenance()
            }
            LinkLabel {
                objectName: "bench-actions-replaygain-settings"
                text: "<a href=\"#\">" + qsTr("ReplayGain settings…") + "</a>"
                onActivated: actions.settings("replaygain")
            }
        }

        Label {
            Layout.alignment: Qt.AlignTop
            leftPadding: 4
            text: qsTr("Scripts")
            ToolTip.visible: scriptsHover.hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Checked scripts stage their edits as colored drafts when files load and when suggestions arrive; Apply writes exactly what the grid shows")
            HoverHandler {
                id: scriptsHover
            }
        }
        ColumnLayout {
            spacing: 2
            Label {
                visible: actions.tagger.scripts.length === 0
                text: qsTr("No saved scripts yet")
                opacity: 0.6
            }
            Repeater {
                model: actions.tagger.scripts
                RowLayout {
                    id: scriptRow
                    required property var modelData
                    required property int index
                    spacing: 4
                    CheckBox {
                        objectName: "bench-actions-script-" + scriptRow.index
                        Layout.fillWidth: true
                        text: scriptRow.modelData.name
                        checked: scriptRow.modelData.automatic
                        enabled: !(actions.state.scriptsLoading ?? false) && (actions.state.canTransform ?? false)
                        onClicked: actions.tagger.toggleScript(scriptRow.modelData.id, checked)
                        ToolTip.visible: hovered
                        ToolTip.delay: 800
                        ToolTip.text: scriptRow.modelData.automatic
                                      ? qsTr("Staged automatically as colored draft edits")
                                      : qsTr("Not staged automatically")
                    }
                    ToolButton {
                        text: qsTr("Edit…")
                        enabled: actions.state.canTransform ?? false
                        onClicked: {
                            actions.close();
                            actions.scriptEditor(scriptRow.modelData.id);
                        }
                    }
                }
            }
            Label {
                visible: text !== ""
                text: actions.state.scriptStatus ?? ""
                color: Theme.dim(palette)
                font.pointSize: Theme.smallSize
            }
            LinkLabel {
                objectName: "bench-actions-script-editor"
                enabled: actions.state.canTransform ?? false
                text: "<a href=\"#\">" + qsTr("Open script editor…") + "</a>"
                onActivated: actions.scriptEditor("")
            }
        }

        Label {
            Layout.columnSpan: 2
            Layout.maximumWidth: 460
            visible: text !== ""
            wrapMode: Text.WordWrap
            leftPadding: 4
            color: Theme.dim(palette)
            font.pointSize: Theme.smallSize
            text: actions.state.profileStatus ?? ""
        }
    }
}
