// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick

// The window's colours: light or dark as the system says, unless chosen.
QtObject {
    // 0 as the system says, 1 light, 2 dark.
    property int mode: 0
    readonly property bool dark: mode === 2
                                 || (mode === 0 && Application.styleHints.colorScheme !== Qt.ColorScheme.Light)

    readonly property color window: dark ? "#23272f" : "#e8ebef"
    readonly property color panel: dark ? "#2a2f38" : "#f3f4f6"
    readonly property color base: dark ? "#2e333d" : "#ffffff"
    readonly property color raised: dark ? "#3a404c" : "#dde1e7"
    readonly property color line: dark ? "#3c4250" : "#d3d8df"
    readonly property color hover: dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.045)
    readonly property color text: dark ? "#dde2ea" : "#1d2129"
    readonly property color dim: dark ? "#939cac" : "#586170"
    readonly property color faint: dark ? "#646c7b" : "#8c94a1"
    readonly property color accent: dark ? "#4c95e8" : "#2a74dc"
    readonly property color selection: dark ? Qt.rgba(0.30, 0.58, 0.91, 0.26) : Qt.rgba(0.16, 0.45, 0.86, 0.16)
    readonly property color playingTint: dark ? Qt.rgba(0.30, 0.58, 0.91, 0.10) : Qt.rgba(0.16, 0.45, 0.86, 0.07)
    readonly property color playingLine: dark ? Qt.rgba(0.30, 0.58, 0.91, 0.45) : Qt.rgba(0.16, 0.45, 0.86, 0.40)
    // Text on the selection and on what plays.
    readonly property color accentText: dark ? Qt.lighter(accent, 1.25) : Qt.darker(accent, 1.15)
    readonly property color error: dark ? "#e08a7a" : "#b8412c"
    readonly property color star: "#e3b54c"
    readonly property color scrim: dark ? Qt.rgba(0.08, 0.09, 0.11, 0.85) : Qt.rgba(0.93, 0.94, 0.96, 0.88)

    readonly property int fontSize: 13
    readonly property int smallFontSize: 11
    readonly property int rowHeight: 22
}
