// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick

QtObject {
    readonly property color window: "#23272f"
    readonly property color panel: "#2a2f38"
    readonly property color base: "#2e333d"
    readonly property color raised: "#3a404c"
    readonly property color line: "#3c4250"
    readonly property color hover: Qt.rgba(1, 1, 1, 0.05)
    readonly property color text: "#dde2ea"
    readonly property color dim: "#939cac"
    readonly property color faint: "#646c7b"
    readonly property color accent: "#4c95e8"
    readonly property color selection: Qt.rgba(0.30, 0.58, 0.91, 0.26)
    readonly property color playingTint: Qt.rgba(0.30, 0.58, 0.91, 0.10)
    readonly property color playingLine: Qt.rgba(0.30, 0.58, 0.91, 0.45)
    readonly property color star: "#e3b54c"

    readonly property int fontSize: 13
    readonly property int smallFontSize: 11
    readonly property int rowHeight: 22
}
