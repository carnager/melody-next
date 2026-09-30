// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// Settings (ADR-0112, ADR-0185): a draft of every value, written on Save;
// the pages that act at once -- library folders, naming layouts and move
// destinations, the Last.fm account -- say so under the pages.
ApplicationWindow {
    id: settingsWindow
    objectName: "bench-settings-dialog"

    required property QuickSettings settings
    property int page: 0
    readonly property var state: settings.state
    readonly property var values: state.values ?? ({})
    readonly property var options: settings.options
    readonly property var profiles: settings.profiles
    readonly property var lastFm: settings.lastFm

    // Pages, as SettingsSession::Page numbers them.
    readonly property int generalPage: 0
    readonly property int playbackPage: 1
    readonly property int libraryPage: 2
    readonly property int enginePage: 3
    readonly property int namingPage: 4
    readonly property int replayGainPage: 5
    readonly property int coversPage: 6
    readonly property int servicesPage: 7
    readonly property int lastFmPage: 8
    readonly property int shortcutsPage: 9

    title: qsTr("Settings")
    width: 940
    height: 640
    minimumWidth: 640
    minimumHeight: 420
    visible: true
    color: palette.window

    Component.onDestruction: settings.release()
    onClosing: Qt.callLater(() => settingsWindow.destroy())

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: settingsWindow.close()
    }

    function showPage(index) {
        page = index;
        raise();
        requestActivate();
    }
    function showNamingLayouts() {
        showPage(namingPage);
        namingSections.currentIndex = 0;
    }
    function showDestinationsOf(key) {
        showPage(namingPage);
        const place = settings.placeOf(key);
        if (place >= 0)
            settings.selectPlace(place);
        namingSections.currentIndex = 1;
    }
    function focusReplayGainPreamp() {
        showPage(playbackPage);
        preampWith.forceActiveFocus();
    }
    function editCustomBuffer() {
        showPage(playbackPage);
        settings.setValue("playback/buffer-profile", "custom");
        bufferCapacity.forceActiveFocus();
    }

    function choiceIndex(choices, value) {
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].value == value)
                return i;
        }
        return 0;
    }

    // Where fields begin: a form's labels are a column this wide.
    readonly property int fieldIndent: Theme.labelWidth + Theme.gap + 4

    component Note: Label {
        // A page's opening words span it; a hint under fields starts where
        // they do.
        property bool intro: false
        Layout.fillWidth: true
        Layout.leftMargin: intro ? 0 : settingsWindow.fieldIndent
        color: Theme.dim(palette)
        linkColor: palette.highlight
        wrapMode: Text.WordWrap
        onLinkActivated: link => Qt.openUrlExternally(link)
        HoverHandler {
            cursorShape: parent.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }
    component FieldLabel: Label {
        Layout.preferredWidth: Theme.labelWidth
        Layout.minimumWidth: Theme.labelWidth
        Layout.alignment: Qt.AlignVCenter
        horizontalAlignment: Text.AlignRight
        wrapMode: Text.WordWrap
    }
    component Form: GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.gap + 4
        rowSpacing: Theme.gap
    }
    component SettingCheck: CheckBox {
        required property string key
        property string tip
        // Beside a form's fields, or under them.
        Layout.leftMargin: parent && parent.columns === undefined ? settingsWindow.fieldIndent : 0
        checked: settingsWindow.values[key] ?? false
        onToggled: {
            settingsWindow.settings.setValue(key, checked);
            checked = Qt.binding(() => settingsWindow.values[key] ?? false);
        }
        ToolTip.visible: hovered && tip !== ""
        ToolTip.delay: 800
        ToolTip.text: tip
    }
    component SettingCombo: ComboBox {
        required property string key
        property var choices: []
        property string tip
        Layout.preferredWidth: 260
        model: choices
        textRole: "label"
        currentIndex: settingsWindow.choiceIndex(choices, settingsWindow.values[key])
        onActivated: index => settingsWindow.settings.setValue(key, choices[index].value)
        ToolTip.visible: hovered && tip !== ""
        ToolTip.delay: 800
        ToolTip.text: tip
    }
    component SettingText: TextField {
        required property string key
        property string tip
        Layout.fillWidth: true
        Layout.maximumWidth: 460
        text: settingsWindow.values[key] ?? ""
        onTextEdited: settingsWindow.settings.setValue(key, text)
        ToolTip.visible: hovered && tip !== ""
        ToolTip.delay: 800
        ToolTip.text: tip
    }
    component SettingSpin: SpinBox {
        required property string key
        property string suffix
        property string special
        property string tip
        Layout.preferredWidth: 180
        editable: true
        value: settingsWindow.values[key] ?? 0
        textFromValue: (number, locale) => special !== "" && number === from ? special
                                                                             : number + suffix
        valueFromText: (text, locale) => text === special ? from : parseInt(text)
        onValueModified: settingsWindow.settings.setValue(key, value)
        ToolTip.visible: hovered && tip !== ""
        ToolTip.delay: 800
        ToolTip.text: tip
    }
    // Decibels to one decimal, as tenths.
    component PreampSpin: SpinBox {
        required property string key
        readonly property int limit: Math.round((settingsWindow.options.maximumPreamp ?? 0) * 10)
        Layout.preferredWidth: 180
        editable: true
        from: -limit
        to: limit
        stepSize: 5
        value: Math.round((settingsWindow.values[key] ?? 0) * 10)
        textFromValue: (number, locale) => (number / 10).toFixed(1) + " dB"
        valueFromText: (text, locale) => Math.round(parseFloat(text) * 10)
        onValueModified: settingsWindow.settings.setValue(key, value / 10)
    }
    component SettingsPage: ScrollView {
        id: settingsPage
        property string heading
        default property alias content: column.data
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            id: column
            x: Theme.margin + Theme.gap
            width: settingsPage.availableWidth - 2 * (Theme.margin + Theme.gap)
            spacing: Theme.gap + 4
            Label {
                objectName: "bench-settings-page-title"
                topPadding: Theme.margin
                bottomPadding: Theme.gapSmall
                text: settingsPage.heading
                font.pointSize: Qt.application.font.pointSize * 1.35
                font.weight: Font.DemiBold
            }
        }
    }
    // A shortcut: clicked, the next keys pressed are it.
    component KeyField: Button {
        id: keyField
        required property int row
        required property string shown
        property bool listening: false
        Layout.preferredWidth: 220
        text: listening ? qsTr("Press keys…") : (shown === "" ? qsTr("None") : shown)
        background: Rectangle {
            implicitHeight: Theme.controlHeight
            radius: Theme.radius
            color: keyField.listening ? Theme.alpha(keyField.palette.highlight, 0.12)
                                      : keyField.palette.base
            border.width: 1
            border.color: keyField.listening || keyField.activeFocus ? keyField.palette.highlight
                                                                    : Theme.border(keyField.palette)
        }
        onClicked: {
            listening = true;
            forceActiveFocus();
        }
        onActiveFocusChanged: if (!activeFocus) listening = false
        // While keys are awaited, Esc stops that rather than closing Settings.
        Keys.onShortcutOverride: event => event.accepted = listening
        Keys.onPressed: event => {
            if (!listening)
                return;
            event.accepted = true;
            if (event.key === Qt.Key_Escape) {
                listening = false;
                return;
            }
            const pressed = settingsWindow.settings.keyText(event.key, event.modifiers);
            if (pressed !== "") {
                settingsWindow.settings.setShortcut(row, pressed);
                listening = false;
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // The pages, down a side bar a shade off the window.
        Rectangle {
            Layout.preferredWidth: 190
            Layout.fillHeight: true
            color: Theme.sunken(palette)
            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.hairline(palette)
            }
            ListView {
                id: pages
                objectName: "bench-settings-pages"
                anchors.fill: parent
                anchors.margins: Theme.gap
                anchors.topMargin: Theme.margin
                spacing: 2
                clip: true
                model: settingsWindow.options.pages
                currentIndex: settingsWindow.page
                delegate: ItemDelegate {
                    id: pageItem
                    required property string modelData
                    required property int index
                    width: pages.width
                    text: modelData
                    highlighted: ListView.isCurrentItem
                    onClicked: settingsWindow.page = index
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            StackLayout {
                objectName: "bench-settings-stack"
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: settingsWindow.page

                // --- General -----------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[0]
                    Form {
                        FieldLabel {
                            text: qsTr("Desktop:")
                        }
                        SettingCheck {
                            objectName: "bench-settings-notifications"
                            key: "desktop/notifications"
                            text: qsTr("Track-change notifications")
                            tip: qsTr("Show a notification when playback changes to another track.")
                        }
                        FieldLabel {}
                        SettingCheck {
                            objectName: "bench-settings-notifications-background"
                            key: "desktop/notifications-background-only"
                            text: qsTr("Only while the app is in the background")
                        }
                        FieldLabel {}
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Button {
                                objectName: "bench-settings-notification-test"
                                text: qsTr("Test notification")
                                onClicked: settingsWindow.settings.testNotification()
                            }
                            Label {
                                objectName: "bench-settings-notification-status"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.dim(palette)
                                text: settingsWindow.state.notificationStatus ?? ""
                            }
                        }
                        FieldLabel {
                            text: qsTr("Appearance:")
                        }
                        SettingCheck {
                            objectName: "bench-settings-panel-animations"
                            key: "appearance/panel-animations"
                            text: qsTr("Animate panel opening and closing")
                        }
                        FieldLabel {
                            text: qsTr("Show lists as:")
                        }
                        SettingCombo {
                            objectName: "bench-settings-lists-display"
                            key: "appearance/lists-display"
                            choices: settingsWindow.options.listsDisplays
                        }
                    }
                }

                // --- Playback ----------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[1]
                    Note {
                        intro: true
                        text: qsTr("The engine plays the music and keeps its queue: closing this window never interrupts it, and an engine that restarts restores its queue, paused.")
                    }
                    Form {
                        FieldLabel {
                            text: qsTr("Playback buffer:")
                        }
                        SettingCombo {
                            objectName: "bench-settings-buffer-profile"
                            key: "playback/buffer-profile"
                            choices: settingsWindow.options.bufferProfiles
                        }
                        FieldLabel {
                            text: qsTr("Capacity:")
                        }
                        SettingSpin {
                            id: bufferCapacity
                            objectName: "bench-settings-buffer-capacity"
                            key: "playback/buffer-capacity-ms"
                            from: 10
                            to: 10000
                            suffix: " ms"
                            enabled: settingsWindow.state.bufferCustom ?? false
                        }
                        FieldLabel {
                            text: qsTr("Start playback at:")
                        }
                        SettingSpin {
                            objectName: "bench-settings-buffer-threshold"
                            key: "playback/buffer-start-threshold-ms"
                            from: 1
                            to: bufferCapacity.value
                            suffix: " ms"
                            enabled: settingsWindow.state.bufferCustom ?? false
                        }
                    }
                    Note {
                        text: qsTr("Responsive starts sooner; Resilient tolerates longer interruptions. Buffer changes take effect on the next track.")
                    }
                    Form {
                        FieldLabel {
                            text: qsTr("Preamp with ReplayGain data:")
                        }
                        PreampSpin {
                            id: preampWith
                            objectName: "bench-settings-preamp-with"
                            key: "playback/rg-preamp-with"
                        }
                        FieldLabel {
                            text: qsTr("Preamp without ReplayGain data:")
                        }
                        PreampSpin {
                            objectName: "bench-settings-preamp-without"
                            key: "playback/rg-preamp-without"
                        }
                    }
                    Note {
                        text: qsTr("Preamps apply when local ReplayGain is enabled. Choose Track, Album, or Automatic from the playback controls.")
                    }
                }

                // --- Library -----------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[2]
                    LibraryFolders {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 300
                        visible: Tk.localLibrary !== null
                        browser: Tk.localLibrary
                    }
                    Note {
                        intro: true
                        visible: Tk.localLibrary === null
                        text: qsTr("Library folders are available from the running workspace.")
                    }
                    SettingCheck {
                        objectName: "bench-settings-ratings-in-tags"
                        key: "library/ratings-in-tags"
                        text: qsTr("Also write track ratings into the files")
                        tip: qsTr("Ratings stay in each engine's library either way. With this on, each engine also writes a track's rating into its files as FMPS_RATING, which other players read. Album ratings are not written.")
                    }
                    Form {
                        FieldLabel {
                            text: qsTr("RATING tags from other players:")
                        }
                        SettingCombo {
                            objectName: "bench-settings-rating-tag-scale"
                            key: "library/rating-tag-scale"
                            choices: settingsWindow.options.ratingScales
                            tip: qsTr("Ratings other players left in your files are taken into the library. FMPS_RATING and MP3 POPM always are; a plain RATING tag has no agreed scale, so it is read only on the one chosen here.")
                        }
                    }
                }

                // --- Engine ------------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[3]
                    Note {
                        intro: true
                        text: qsTr("melodyd plays the music and keeps the library. Trackknife starts this computer's, and it plays on after the window closes.")
                    }
                    Form {
                        FieldLabel {
                            text: qsTr("Password:")
                        }
                        SettingText {
                            id: enginePassword
                            objectName: "bench-settings-engine-password"
                            key: "engine/password"
                            echoMode: TextInput.Password
                            placeholderText: qsTr("the same on every engine and agent")
                            tip: qsTr("Needed to share this computer's engine, and given to the remote engine unless it has a password of its own below. It travels unencrypted: across an untrusted network, use WireGuard or a TLS proxy.")
                        }
                    }
                    GroupBox {
                        Layout.fillWidth: true
                        title: qsTr("This computer's engine")
                        ColumnLayout {
                            anchors.fill: parent
                            SettingCheck {
                                objectName: "bench-settings-show-local-library"
                                key: "library/show-local"
                                text: qsTr("Show this computer's library")
                                tip: qsTr("Hide it when all your music is in the remote engine's library. Files on this computer still open and play here.")
                            }
                            SettingCheck {
                                id: engineShare
                                objectName: "bench-settings-engine-share"
                                key: "engine/share"
                                text: qsTr("Share on the network")
                                tip: qsTr("Lets output agents play this computer's music, and other Trackknife windows control it")
                            }
                            Form {
                                FieldLabel {
                                    text: qsTr("Address:")
                                }
                                SettingText {
                                    objectName: "bench-settings-engine-listen"
                                    key: "engine/listen"
                                    enabled: engineShare.checked
                                    tip: qsTr("host:port; 0.0.0.0 listens on every network this computer is on")
                                }
                                FieldLabel {
                                    text: qsTr("Stream port:")
                                }
                                SettingSpin {
                                    objectName: "bench-settings-engine-stream-port"
                                    key: "engine/stream-port"
                                    from: 1
                                    to: 65535
                                    enabled: engineShare.checked
                                    textFromValue: (number, locale) => String(number)
                                    tip: qsTr("Where agents without a copy of the music fetch it, on the same address")
                                }
                                FieldLabel {
                                    text: qsTr("Music root:")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    SettingText {
                                        objectName: "bench-settings-engine-music-root"
                                        key: "engine/music-root"
                                        placeholderText: qsTr("optional")
                                        tip: qsTr("Agents started with their own --music-root are sent paths relative to this folder; agents without one stream")
                                    }
                                    Button {
                                        text: qsTr("Browse…")
                                        onClicked: musicRootDialog.open()
                                    }
                                }
                            }
                            Label {
                                objectName: "bench-settings-engine-agent-command"
                                Layout.fillWidth: true
                                Layout.leftMargin: settingsWindow.fieldIndent
                                color: Theme.dim(palette)
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                text: settingsWindow.state.agentCommand ?? ""
                            }
                        }
                    }
                    GroupBox {
                        Layout.fillWidth: true
                        title: qsTr("Engines elsewhere")
                        ColumnLayout {
                            anchors.fill: parent
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.leftMargin: settingsWindow.fieldIndent
                                Layout.preferredHeight: 110
                                color: palette.base
                                border.color: Theme.hairline(palette)
                                radius: Theme.radius
                                ListView {
                                    id: enginesList
                                    objectName: "bench-settings-engines"
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    clip: true
                                    model: settingsWindow.state.engineLabels ?? []
                                    currentIndex: settingsWindow.state.currentEngine ?? -1
                                    Accessible.name: qsTr("Engines elsewhere")
                                    delegate: ItemDelegate {
                                        required property string modelData
                                        required property int index
                                        width: enginesList.width
                                        text: modelData
                                        highlighted: ListView.isCurrentItem
                                        onClicked: settingsWindow.settings.selectEngine(index)
                                    }
                                }
                            }
                            RowLayout {
                                Layout.leftMargin: settingsWindow.fieldIndent
                                Button {
                                    objectName: "bench-settings-engine-add"
                                    text: qsTr("Add")
                                    onClicked: {
                                        settingsWindow.settings.addEngine();
                                        engineAddress.forceActiveFocus();
                                    }
                                }
                                Button {
                                    objectName: "bench-settings-engine-remove"
                                    text: qsTr("Remove")
                                    onClicked: settingsWindow.settings.removeEngine()
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Button {
                                    id: foundButton
                                    objectName: "bench-settings-found-engines"
                                    text: qsTr("On the network")
                                    enabled: (settingsWindow.state.discoveryError ?? "") === ""
                                    onClicked: foundMenu.popup(foundButton, 0, foundButton.height)
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 800
                                    ToolTip.text: enabled ? qsTr("Engines that announce themselves on this network")
                                                          : settingsWindow.state.discoveryError
                                    Menu {
                                        id: foundMenu
                                        objectName: "bench-settings-found-engines-menu"
                                        MenuItem {
                                            text: qsTr("None found yet")
                                            enabled: false
                                            visible: (settingsWindow.state.found ?? []).length === 0
                                            height: visible ? implicitHeight : 0
                                        }
                                        Instantiator {
                                            model: settingsWindow.state.found ?? []
                                            delegate: MenuItem {
                                                required property var modelData
                                                text: modelData.label
                                                onTriggered: settingsWindow.settings.chooseFoundEngine(modelData.address)
                                            }
                                            onObjectAdded: (index, object) => foundMenu.insertItem(index + 1, object)
                                            onObjectRemoved: (index, object) => foundMenu.removeItem(object)
                                        }
                                    }
                                }
                            }
                            Form {
                                FieldLabel {
                                    text: qsTr("Address:")
                                }
                                TextField {
                                    id: engineAddress
                                    objectName: "bench-settings-engine-socket"
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 460
                                    placeholderText: qsTr("host:port or socket path")
                                    text: settingsWindow.state.engine.address
                                    onTextEdited: settingsWindow.settings.editEngine({address: text})
                                }
                                FieldLabel {
                                    text: qsTr("Password:")
                                }
                                TextField {
                                    objectName: "bench-settings-engine-token"
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 460
                                    echoMode: TextInput.Password
                                    placeholderText: qsTr("the password above")
                                    text: settingsWindow.state.engine.password
                                    onTextEdited: settingsWindow.settings.editEngine({password: text})
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 800
                                    ToolTip.text: qsTr("Only when that engine's password differs from yours")
                                }
                                FieldLabel {
                                    text: qsTr("Its music folder:")
                                }
                                TextField {
                                    objectName: "bench-settings-remote-folder"
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 460
                                    placeholderText: qsTr("e.g. /mnt/nas/Music, as that engine sees it")
                                    text: settingsWindow.state.engine.musicFolder
                                    onTextEdited: settingsWindow.settings.editEngine({musicFolder: text})
                                }
                                FieldLabel {
                                    text: qsTr("Also reachable here at:")
                                }
                                TextField {
                                    objectName: "bench-settings-remote-mount"
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 460
                                    placeholderText: qsTr("optional; empty: the same path, or not reachable here")
                                    text: settingsWindow.state.engine.reachableAt
                                    onTextEdited: settingsWindow.settings.editEngine({reachableAt: text})
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 800
                                    ToolTip.text: qsTr("Where that folder is on this computer, if you have mounted it (NFS, SMB, …). Trackknife mounts nothing itself.")
                                }
                                FieldLabel {
                                    text: qsTr("Streamed here:")
                                }
                                ComboBox {
                                    objectName: "bench-settings-remote-stream"
                                    Layout.preferredWidth: 260
                                    enabled: playForRemote.checked
                                    model: settingsWindow.state.engineRates ?? []
                                    textRole: "label"
                                    currentIndex: settingsWindow.choiceIndex(model, settingsWindow.state.engine.streamKbps)
                                    onActivated: index => settingsWindow.settings.editEngine({streamKbps: model[index].value})
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 800
                                    ToolTip.text: qsTr("What it streams to this computer's speakers when its music is not reachable here. Automatic: the rates below, by whether it is on this network or reached through a VPN or a router.")
                                }
                                FieldLabel {}
                                SettingCheck {
                                    id: playForRemote
                                    objectName: "bench-settings-play-for-remote"
                                    key: "engine/play-for-remote"
                                    text: qsTr("Let other engines play on this computer's speakers")
                                    tip: qsTr("This computer's engine appears among the outputs of the remote engine and of any engine found on the network, so no melody-agent is needed here. Whichever starts playing last has the speakers.")
                                }
                                FieldLabel {
                                    text: qsTr("Streamed on this network:")
                                }
                                SettingCombo {
                                    objectName: "bench-settings-stream-nearby"
                                    key: "engine/stream-nearby-kbps"
                                    choices: settingsWindow.state.nearbyRates ?? []
                                    enabled: playForRemote.checked
                                    tip: qsTr("From an engine on this computer's own network")
                                }
                                FieldLabel {
                                    text: qsTr("Through a VPN or router:")
                                }
                                SettingCombo {
                                    objectName: "bench-settings-stream-away"
                                    key: "engine/stream-away-kbps"
                                    choices: settingsWindow.state.awayRates ?? []
                                    enabled: playForRemote.checked
                                    tip: qsTr("From an engine reached through WireGuard or another VPN, or through a router")
                                }
                            }
                            Note {
                                text: qsTr("A melodyd on a NAS or server (started with --listen), beside this computer's: its library gets a tab and its tracks play there, and this computer's speakers are offered to each. Unencrypted: for a home network or WireGuard.")
                            }
                        }
                    }
                }

                // --- Naming ------------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[4]
                    TabBar {
                        id: namingSections
                        objectName: "bench-output-profile-sections"
                        Layout.fillWidth: true
                        TabButton {
                            text: qsTr("Naming layouts")
                        }
                        TabButton {
                            text: qsTr("Move destinations")
                        }
                    }
                    StackLayout {
                        Layout.fillWidth: true
                        currentIndex: namingSections.currentIndex
                        enabled: settingsWindow.profiles.available ?? false

                        ColumnLayout {
                            spacing: 12
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.gap + 4
                                FieldLabel {
                                    text: qsTr("Layout:")
                                }
                                ComboBox {
                                    objectName: "bench-output-layout-list"
                                    Layout.fillWidth: true
                                    model: settingsWindow.profiles.layouts ?? []
                                    currentIndex: settingsWindow.profiles.layoutRow ?? -1
                                    displayText: currentIndex < 0 ? qsTr("New naming layout") : currentText
                                    onActivated: index => settingsWindow.settings.selectLayout(index)
                                    Accessible.name: qsTr("Naming layout")
                                }
                            }
                            Form {
                                FieldLabel {
                                    text: qsTr("Name:")
                                }
                                TextField {
                                    id: layoutName
                                    objectName: "bench-output-layout-name"
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("For example: Album folders")
                                    text: settingsWindow.profiles.layoutName ?? ""
                                    onTextEdited: settingsWindow.settings.setLayoutName(text)
                                }
                                FieldLabel {
                                    text: qsTr("Folders:")
                                }
                                TextField {
                                    objectName: "bench-output-layout-directory-expression"
                                    Layout.fillWidth: true
                                    font.family: "monospace"
                                    placeholderText: qsTr("For example: %album artist%/%album%")
                                    text: settingsWindow.profiles.directoryExpression ?? ""
                                    onTextEdited: settingsWindow.settings.setDirectoryExpression(text)
                                }
                                FieldLabel {
                                    text: qsTr("Filename:")
                                }
                                TextField {
                                    objectName: "bench-output-layout-basename-expression"
                                    Layout.fillWidth: true
                                    font.family: "monospace"
                                    placeholderText: qsTr("For example: %tracknumber% - %title%")
                                    text: settingsWindow.profiles.basenameExpression ?? ""
                                    onTextEdited: settingsWindow.settings.setBasenameExpression(text)
                                }
                                FieldLabel {
                                    text: qsTr("Filename policy:")
                                }
                                ComboBox {
                                    objectName: "bench-output-layout-sanitization"
                                    Layout.preferredWidth: 260
                                    model: settingsWindow.options.sanitizationPolicies
                                    textRole: "label"
                                    currentIndex: settingsWindow.choiceIndex(model, settingsWindow.profiles.sanitization)
                                    onActivated: index => settingsWindow.settings.setSanitization(model[index].value)
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 800
                                    ToolTip.text: qsTr("Portable replaces Windows-forbidden characters, trailing dots/spaces, and reserved device names; Unicode spelling is preserved")
                                }
                            }
                            RowLayout {
                                Layout.leftMargin: settingsWindow.fieldIndent
                                spacing: Theme.gap
                                Button {
                                    objectName: "bench-output-layout-new"
                                    text: qsTr("New")
                                    enabled: settingsWindow.profiles.canEditLayouts ?? false
                                    onClicked: {
                                        settingsWindow.settings.newLayout();
                                        layoutName.forceActiveFocus();
                                    }
                                }
                                Button {
                                    objectName: "bench-output-layout-save"
                                    text: qsTr("Save layout")
                                    enabled: settingsWindow.profiles.canSaveLayout ?? false
                                    onClicked: settingsWindow.settings.saveLayout()
                                }
                                Button {
                                    objectName: "bench-output-layout-remove"
                                    text: qsTr("Remove")
                                    enabled: settingsWindow.profiles.canRemoveLayout ?? false
                                    onClicked: settingsWindow.settings.removeLayout()
                                }
                            }
                        }

                        ColumnLayout {
                            spacing: 12
                            // ADR-0237: a destination is a folder on one
                            // engine's machine; which one is always in sight.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.gap + 4
                                FieldLabel {
                                    text: qsTr("Destinations on:")
                                }
                                ComboBox {
                                    objectName: "bench-destination-engine"
                                    Layout.fillWidth: true
                                    model: settingsWindow.profiles.places ?? []
                                    currentIndex: settingsWindow.profiles.place ?? 0
                                    enabled: settingsWindow.profiles.canChoosePlace ?? false
                                    onActivated: index => settingsWindow.settings.selectPlace(index)
                                    Accessible.name: qsTr("Engine the move destinations are on")
                                }
                                Button {
                                    objectName: "bench-destination-copy"
                                    visible: (settingsWindow.profiles.copyable ?? 0) > 0
                                    text: qsTr("Copy %1 from this computer").arg(settingsWindow.profiles.copyable ?? 0)
                                    onClicked: settingsWindow.settings.copyDestinations()
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 800
                                    ToolTip.text: qsTr("Save this computer's destinations that lie under this engine's music folder here too, as the engine names them")
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.gap + 4
                                FieldLabel {
                                    text: qsTr("Destination:")
                                }
                                ComboBox {
                                    objectName: "bench-destination-list"
                                    Layout.fillWidth: true
                                    model: settingsWindow.profiles.destinations ?? []
                                    currentIndex: settingsWindow.profiles.destinationRow ?? -1
                                    displayText: currentIndex < 0 ? qsTr("New move destination") : currentText
                                    onActivated: index => settingsWindow.settings.selectDestination(index)
                                    Accessible.name: qsTr("Move destination")
                                }
                            }
                            Form {
                                FieldLabel {
                                    text: qsTr("Name:")
                                }
                                TextField {
                                    id: destinationName
                                    objectName: "bench-destination-name"
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("For example: Music library")
                                    text: settingsWindow.profiles.destinationName ?? ""
                                    onTextEdited: settingsWindow.settings.setDestinationName(text)
                                }
                                FieldLabel {
                                    text: qsTr("Root:")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    TextField {
                                        objectName: "bench-destination-root"
                                        Layout.fillWidth: true
                                        readOnly: true
                                        placeholderText: qsTr("Choose an absolute folder")
                                        text: settingsWindow.profiles.destinationRoot ?? ""
                                    }
                                    Button {
                                        objectName: "bench-destination-browse"
                                        text: qsTr("Browse…")
                                        enabled: settingsWindow.profiles.canEditDestinations ?? false
                                        onClicked: {
                                            const chooser = settingsWindow.settings.browseRoot();
                                            if (chooser)
                                                engineFolderComponent.createObject(settingsWindow, {folder: chooser});
                                            else
                                                destinationRootDialog.open();
                                        }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.leftMargin: settingsWindow.fieldIndent
                                spacing: Theme.gap
                                Button {
                                    objectName: "bench-destination-new"
                                    text: qsTr("New")
                                    enabled: settingsWindow.profiles.canEditDestinations ?? false
                                    onClicked: {
                                        settingsWindow.settings.newDestination();
                                        destinationName.forceActiveFocus();
                                    }
                                }
                                Button {
                                    objectName: "bench-destination-save"
                                    text: qsTr("Save destination")
                                    enabled: settingsWindow.profiles.canSaveDestination ?? false
                                    onClicked: settingsWindow.settings.saveDestination()
                                }
                                Button {
                                    objectName: "bench-destination-remove"
                                    text: qsTr("Remove")
                                    enabled: settingsWindow.profiles.canRemoveDestination ?? false
                                    onClicked: settingsWindow.settings.removeDestination()
                                }
                            }
                        }
                    }
                    Label {
                        objectName: "bench-output-profiles-status"
                        Layout.leftMargin: settingsWindow.fieldIndent
                        color: Theme.dim(palette)
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: settingsWindow.profiles.status ?? ""
                    }
                }

                // --- ReplayGain --------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[5]
                    SettingCheck {
                        objectName: "bench-replaygain-sidecar-only"
                        key: "replaygain/sidecar-only"
                        text: qsTr("Store scan results in sidecar only")
                        tip: qsTr("Keep ReplayGain values out of file tags; scans write the loudness sidecar instead")
                    }
                    SettingCheck {
                        objectName: "bench-replaygain-true-peak"
                        key: "replaygain/true-peak"
                        text: qsTr("True peak as ReplayGain peak")
                        tip: qsTr("Scan oversampled true peak instead of the plain sample peak")
                    }
                }

                // --- Covers ------------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[6]
                    SettingCheck {
                        objectName: "bench-artwork-embed"
                        key: "artwork/embed"
                        text: qsTr("Embed covers into the files")
                    }
                    SettingCheck {
                        objectName: "bench-artwork-folder-image"
                        key: "artwork/write-folder-image"
                        text: qsTr("Write a front-cover image next to the tracks")
                    }
                    Form {
                        FieldLabel {
                            text: qsTr("Folder image name:")
                        }
                        ComboBox {
                            objectName: "bench-artwork-folder-image-name"
                            Layout.preferredWidth: 260
                            editable: true
                            model: settingsWindow.options.folderImageNames
                            editText: settingsWindow.values["artwork/folder-image-name"] ?? ""
                            onEditTextChanged: {
                                if (editText !== (settingsWindow.values["artwork/folder-image-name"] ?? ""))
                                    settingsWindow.settings.setValue("artwork/folder-image-name", editText);
                            }
                        }
                        FieldLabel {
                            text: qsTr("Fetch covers from:")
                        }
                        SettingCombo {
                            objectName: "bench-artwork-fetch-source"
                            key: "artwork/fetch-source"
                            choices: settingsWindow.options.fetchSources
                        }
                        FieldLabel {
                            text: qsTr("Largest embedded cover:")
                        }
                        SettingSpin {
                            objectName: "bench-artwork-max-embedded-edge"
                            key: "artwork/max-embedded-edge"
                            from: 0
                            to: 10000
                            stepSize: 100
                            suffix: " px"
                            special: qsTr("No limit")
                        }
                        FieldLabel {
                            text: qsTr("Largest folder image:")
                        }
                        SettingSpin {
                            objectName: "bench-artwork-max-folder-edge"
                            key: "artwork/max-folder-edge"
                            from: 0
                            to: 10000
                            stepSize: 100
                            suffix: " px"
                            special: qsTr("No limit")
                        }
                    }
                    Note {
                        text: qsTr("Front covers use this storage policy on Apply. The filename extension follows the image format (.jpg or .png). Folder replacements are reviewed and retain recovery backups. A cover wider or taller than its limit is scaled down and saved as JPEG (PNG if it has transparency) when it is written; covers already in your files are left alone.")
                    }
                }

                // --- Metadata services -------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[7]
                    Note {
                        intro: true
                        text: qsTr("MusicBrainz text search works without an account or API key. Lookups start only when you request identification.")
                    }
                    Form {
                        FieldLabel {
                            text: qsTr("AcoustID client key:")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            SettingText {
                                objectName: "bench-settings-acoustid-key"
                                key: "musicbrainz/acoustid-client-key"
                                echoMode: revealKey.checked ? TextInput.Normal : TextInput.Password
                                placeholderText: qsTr("Client/application key for fingerprint lookup")
                            }
                            CheckBox {
                                id: revealKey
                                objectName: "bench-settings-acoustid-show"
                                text: qsTr("Show")
                                Accessible.name: qsTr("Show AcoustID client key")
                            }
                        }
                    }
                    Note {
                        text: qsTr("Audio fingerprint identification uses AcoustID and the fpcalc tool. Use an application/client key, not an AcoustID user key. The key is stored in your local application settings. Clear it to remove it.")
                    }
                    Note {
                        objectName: "bench-settings-acoustid-link"
                        opacity: 1
                        textFormat: Text.RichText
                        text: "<a href=\"https://acoustid.org/new-application\">" + qsTr("Register an application to get an AcoustID client key") + "</a>"
                    }
                    Form {
                        FieldLabel {
                            text: qsTr("Last.fm API key:")
                        }
                        SettingText {
                            objectName: "bench-settings-lastfm-key"
                            key: "lastfm/api-key"
                            echoMode: TextInput.Password
                        }
                    }
                    Note {
                        textFormat: Text.RichText
                        text: qsTr("Dynamic playlists can match Last.fm recommendations, loved tracks, top tracks, or tags to your library. Requests send the seed artist/track, username, or tag when you choose Refresh. The key is stored locally. Account authorization and scrobbling are separate under Last.fm settings. ")
                              + "<a href=\"https://www.last.fm/api/account/create\">" + qsTr("Get a Last.fm API key") + "</a>"
                    }
                }

                // --- Last.fm -----------------------------------------------
                SettingsPage {
                    objectName: "lastfm-settings"
                    heading: settingsWindow.options.pages[8]
                    Note {
                        intro: true
                        text: qsTr("Sign in here once. Hand the account to an engine below and it scrobbles what it plays itself, with Trackknife closed; until then, playback is scrobbled while Trackknife is open. Account actions take effect immediately.")
                    }
                    ColumnLayout {
                        objectName: "lastfm-credentials"
                        Layout.fillWidth: true
                        visible: !(settingsWindow.lastFm.credentialsSaved ?? false)
                        Note {
                            text: qsTr("1. Register a free Last.fm API application using the link below. Choose an application name such as Trackknife; no callback URL is needed for desktop authorization.\n2. Paste the API key and shared secret here once.\n3. Connect to enable scrobbling, then approve access in your browser. This page connects automatically once you approve.")
                        }
                        Form {
                            FieldLabel {
                                text: qsTr("API key:")
                            }
                            TextField {
                                objectName: "lastfm-account-key"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 460
                                echoMode: TextInput.Password
                                text: settingsWindow.lastFm.key ?? ""
                                onTextEdited: settingsWindow.settings.setLastFmKey(text)
                            }
                            FieldLabel {
                                text: qsTr("Shared secret:")
                            }
                            TextField {
                                objectName: "lastfm-account-secret"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 460
                                echoMode: TextInput.Password
                                text: settingsWindow.lastFm.secret ?? ""
                                onTextEdited: settingsWindow.settings.setLastFmSecret(text)
                            }
                        }
                        Note {
                            opacity: 1
                            textFormat: Text.RichText
                            text: "<a href=\"https://www.last.fm/api/account/create\">" + qsTr("Create a Last.fm API account") + "</a> · "
                                  + "<a href=\"https://www.last.fm/api/accounts\">" + qsTr("Find your key and shared secret") + "</a>"
                        }
                        CheckBox {
                            objectName: "lastfm-reuse-key"
                            Layout.leftMargin: settingsWindow.fieldIndent
                            text: qsTr("Use this API key for dynamic playlists too")
                            checked: settingsWindow.lastFm.reuseKey ?? true
                            onToggled: {
                                settingsWindow.settings.setLastFmReuseKey(checked);
                                checked = Qt.binding(() => settingsWindow.lastFm.reuseKey ?? true);
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: settingsWindow.fieldIndent
                        wrapMode: Text.WordWrap
                        color: Theme.dim(palette)
                        text: qsTr("Credentials are saved privately on this computer.")
                    }
                    RowLayout {
                        Layout.leftMargin: settingsWindow.fieldIndent
                        spacing: Theme.gap
                        Button {
                            objectName: "lastfm-authorize"
                            text: settingsWindow.lastFm.connectText ?? ""
                            enabled: settingsWindow.lastFm.canConnect ?? false
                            onClicked: settingsWindow.settings.connectLastFm()
                        }
                        Button {
                            objectName: "lastfm-cancel"
                            text: qsTr("Cancel")
                            visible: settingsWindow.lastFm.waiting ?? false
                            onClicked: settingsWindow.settings.stopWaitingForLastFm()
                        }
                        Button {
                            text: qsTr("Disconnect / clear pending")
                            onClicked: settingsWindow.settings.disconnectLastFm()
                        }
                    }
                    CheckBox {
                        objectName: "lastfm-enabled"
                        Layout.leftMargin: settingsWindow.fieldIndent
                        text: qsTr("Scrobble playback to Last.fm")
                        enabled: settingsWindow.lastFm.connected ?? false
                        checked: settingsWindow.lastFm.scrobbling ?? false
                        onToggled: {
                            settingsWindow.settings.setScrobbling(checked);
                            checked = Qt.binding(() => settingsWindow.lastFm.scrobbling ?? false);
                        }
                    }
                    Label {
                        objectName: "lastfm-status"
                        Layout.fillWidth: true
                        Layout.leftMargin: settingsWindow.fieldIndent
                        wrapMode: Text.WordWrap
                        text: settingsWindow.lastFm.status ?? ""
                    }
                    // ADR-0220: the engines scrobble what they play. One
                    // session serves them all; it is handed over, never read
                    // back.
                    GroupBox {
                        objectName: "lastfm-engines"
                        Layout.fillWidth: true
                        title: qsTr("Engines scrobble what they play")
                        ColumnLayout {
                            anchors.fill: parent
                            Repeater {
                                model: settingsWindow.lastFm.engines ?? []
                                RowLayout {
                                    id: engineRow
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    spacing: Theme.gap + 4
                                    FieldLabel {
                                        text: engineRow.modelData.name + ":"
                                    }
                                    Label {
                                        objectName: engineRow.modelData.id + "-state"
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: engineRow.modelData.state
                                    }
                                    Button {
                                        objectName: engineRow.modelData.id + "-use"
                                        text: engineRow.modelData.inUse ? qsTr("In use") : qsTr("Use this account")
                                        enabled: !engineRow.modelData.inUse
                                        onClicked: settingsWindow.settings.useLastFmAccount(engineRow.index)
                                    }
                                }
                            }
                            RowLayout {
                                Layout.leftMargin: settingsWindow.fieldIndent
                                Button {
                                    objectName: "lastfm-engine-another"
                                    text: qsTr("Another engine…")
                                    onClicked: anotherEngine.open()
                                }
                                Label {
                                    id: anotherError
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }

                // --- Shortcuts ---------------------------------------------
                SettingsPage {
                    heading: settingsWindow.options.pages[9]
                    Note {
                        intro: true
                        text: qsTr("Click a shortcut and press the new keys. Clear it to disable it. Changes take effect on Save. Shortcuts operate within Trackknife, not across the desktop.")
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 3
                        columnSpacing: 12
                        rowSpacing: 6
                        Repeater {
                            model: settingsWindow.settings.shortcuts.commands ?? []
                            delegate: FieldLabel {
                                required property var modelData
                                required property int index
                                Layout.row: index
                                Layout.column: 0
                                text: modelData.label
                            }
                        }
                        Repeater {
                            model: settingsWindow.settings.shortcuts.commands ?? []
                            delegate: KeyField {
                                required property var modelData
                                required property int index
                                objectName: "shortcut-" + modelData.id
                                Layout.row: index
                                Layout.column: 1
                                row: index
                                shown: modelData.shown
                                Accessible.name: modelData.label
                            }
                        }
                        Repeater {
                            model: settingsWindow.settings.shortcuts.commands ?? []
                            delegate: ToolButton {
                                required property var modelData
                                required property int index
                                Layout.row: index
                                Layout.column: 2
                                text: qsTr("Clear")
                                enabled: modelData.key !== ""
                                onClicked: settingsWindow.settings.setShortcut(index, "")
                            }
                        }
                    }
                    Label {
                        objectName: "shortcut-conflict"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "firebrick"
                        visible: text !== ""
                        text: settingsWindow.settings.shortcuts.error ?? ""
                    }
                    Button {
                        objectName: "shortcut-restore-defaults"
                        text: qsTr("Restore defaults")
                        onClicked: settingsWindow.settings.restoreShortcuts()
                    }
                }
            }

            // What Save does, and does not, beside the buttons.
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.hairline(palette)
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.margin
                Layout.topMargin: Theme.gap + 4
                Layout.bottomMargin: Theme.gap + 4
                spacing: Theme.gapLarge
                Label {
                    objectName: "bench-settings-save-note"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.dim(palette)
                    text: settingsWindow.settings.saveNote(settingsWindow.page)
                }
                DialogButtonBox {
                    objectName: "bench-settings-buttons"
                    padding: 0
                    standardButtons: DialogButtonBox.Save | DialogButtonBox.Cancel
                    onAccepted: {
                        const refused = settingsWindow.settings.save();
                        if (refused < 0) {
                            settingsWindow.close();
                            return;
                        }
                        // ADR-0223: sharing without a password is not a thing to save.
                        settingsWindow.page = refused;
                        if (refused === settingsWindow.enginePage)
                            enginePassword.forceActiveFocus();
                    }
                    onRejected: settingsWindow.close()
                }
            }
        }
    }

    FolderDialog {
        id: musicRootDialog
        title: qsTr("Music root")
        onAccepted: settingsWindow.settings.setValue("engine/music-root",
                                                     decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, "")))
    }
    FolderDialog {
        id: destinationRootDialog
        title: qsTr("Choose move destination")
        onAccepted: settingsWindow.settings.chooseLocalRoot(selectedFolder)
    }
    Component {
        id: engineFolderComponent
        EngineFolderDialog {}
    }
    Dialog {
        id: anotherEngine
        title: qsTr("Another engine")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        GridLayout {
            columns: 2
            Label {
                text: qsTr("Address of the engine (host:port):")
            }
            TextField {
                id: anotherAddress
                Layout.preferredWidth: 240
            }
            Label {
                text: qsTr("Its password:")
            }
            TextField {
                id: anotherPassword
                Layout.preferredWidth: 240
                echoMode: TextInput.Password
            }
        }
        onOpened: {
            anotherAddress.text = "";
            anotherPassword.text = "";
            anotherAddress.forceActiveFocus();
        }
        onAccepted: {
            if (anotherAddress.text.trim() === "")
                return;
            anotherError.text = settingsWindow.settings.addLastFmEngine(anotherAddress.text, anotherPassword.text);
        }
    }
}
