// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// ADR-0156: measure the selected tracks' loudness and save track and album
// volume adjustments, without the tag editor. Audio samples are not changed.
ApplicationWindow {
    id: dialog
    objectName: "bench-replaygain-dialog"

    required property QuickReplayGain replayGain
    readonly property var state: replayGain.state

    title: replayGain.title
    width: 640
    height: 500
    minimumWidth: 480
    minimumHeight: 380
    visible: true
    color: palette.window

    Component.onDestruction: replayGain.release()
    onClosing: Qt.callLater(() => dialog.destroy())

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: dialog.close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.margin
            spacing: Theme.gap

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.RichText
                text: qsTr("<b>Calculate ReplayGain tags</b><br>Measure loudness and save track and album volume adjustments. Audio samples are not changed.")
            }
            RowLayout {
                Label {
                    text: qsTr("Scan mode:")
                }
                ComboBox {
                    objectName: "bench-replaygain-dialog-grouping"
                    Layout.fillWidth: true
                    model: dialog.replayGain.groupings
                    currentIndex: dialog.state.grouping ?? 0
                    enabled: !(dialog.state.running ?? false)
                    onActivated: index => dialog.replayGain.setGrouping(index)
                }
            }
            TextField {
                objectName: "bench-replaygain-dialog-expression"
                Layout.fillWidth: true
                visible: (dialog.state.grouping ?? 0) === 4
                enabled: !(dialog.state.running ?? false)
                placeholderText: qsTr("tkfmt-1, e.g. %album%")
                text: dialog.state.expression ?? ""
                onTextEdited: dialog.replayGain.setExpression(text)
            }
            RowLayout {
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Album groups in the selected files")
                }
                Button {
                    objectName: "bench-replaygain-dialog-preview"
                    text: qsTr("Preview groups")
                    enabled: !(dialog.state.running ?? false)
                    onClicked: dialog.replayGain.preview()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: palette.base
                border.color: Theme.hairline(palette)
                radius: 4
                ListView {
                    objectName: "bench-replaygain-dialog-groups"
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    model: dialog.state.groups ?? []
                    delegate: Label {
                        required property string modelData
                        required property int index
                        width: ListView.view.width
                        height: 26
                        leftPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: modelData
                        background: Rectangle {
                            color: index % 2 ? palette.alternateBase : palette.base
                        }
                    }
                }
            }
            GroupBox {
                Layout.fillWidth: true
                title: qsTr("Storage and peak measurement")
                enabled: !(dialog.state.running ?? false)
                ColumnLayout {
                    CheckBox {
                        objectName: "bench-replaygain-dialog-sidecar-only"
                        text: qsTr("Keep audio files untouched — store in sidecar files")
                        checked: dialog.state.sidecarOnly ?? false
                        onToggled: dialog.replayGain.setSidecarOnly(checked)
                        ToolTip.visible: hovered
                        ToolTip.delay: 800
                        ToolTip.text: qsTr("Otherwise write tags where safely supported, with sidecar fallback. CUE tracks store gains in their CUE sheet.")
                    }
                    CheckBox {
                        objectName: "bench-replaygain-dialog-true-peak"
                        text: qsTr("Use true peak for clipping protection")
                        checked: dialog.state.truePeak ?? false
                        onToggled: dialog.replayGain.setTruePeak(checked)
                        ToolTip.visible: hovered
                        ToolTip.delay: 800
                        ToolTip.text: qsTr("Uses inter-sample peak estimates instead of sample peaks for the saved ReplayGain peak values.")
                    }
                }
            }
            ProgressBar {
                objectName: "bench-replaygain-dialog-progress"
                Layout.fillWidth: true
                visible: dialog.state.running ?? false
                indeterminate: (dialog.state.progressMaximum ?? 0) === 0
                from: 0
                to: Math.max(1, dialog.state.progressMaximum ?? 1)
                value: dialog.state.progress ?? 0
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                visible: (dialog.state.problems ?? "") !== ""
                TextArea {
                    objectName: "bench-replaygain-dialog-problems"
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    text: dialog.state.problems ?? ""
                }
            }
        }
        // What it is doing on the left, its buttons on the right.
        WindowFooter {
            Label {
                objectName: "bench-replaygain-dialog-status"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: dialog.state.status ?? ""
            }
            Button {
                objectName: "bench-replaygain-dialog-run"
                text: qsTr("Scan and write tags")
                highlighted: true
                enabled: !(dialog.state.running ?? false)
                onClicked: dialog.replayGain.run()
            }
            Button {
                objectName: "bench-replaygain-dialog-stop"
                text: qsTr("Stop")
                visible: dialog.state.running ?? false
                enabled: dialog.state.canStop ?? false
                onClicked: dialog.replayGain.stop()
            }
            Button {
                objectName: "bench-replaygain-dialog-close"
                text: qsTr("Close")
                onClicked: dialog.close()
            }
        }
    }
}
