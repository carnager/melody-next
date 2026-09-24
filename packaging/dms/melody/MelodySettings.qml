import QtQuick
import qs.Common
import qs.Modules.Plugins
import qs.Widgets

PluginSettings {
    id: root

    pluginId: "melody"

    StyledText {
        width: parent.width
        text: "Melody"
        font.pixelSize: Theme.fontSizeLarge
        font.weight: Font.Bold
        color: Theme.surfaceText
    }

    StyledText {
        width: parent.width
        text: "What the engine plays, through melody-cli. The heart gives the track five stars and loves it on Last.fm; again, and it takes both back."
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.surfaceVariantText
        wrapMode: Text.WordWrap
    }

    StyledRect {
        width: parent.width
        height: 1
        color: Theme.surfaceVariant
    }

    StringSetting {
        settingKey: "engine"
        label: "Engine"
        description: "Its name on the network, or HOST:PORT. Empty: whichever engine, here or on the network, is playing."
        placeholder: "whichever plays"
        defaultValue: ""
    }

    StringSetting {
        settingKey: "password"
        label: "Password"
        description: "Only for an engine started with a password."
        placeholder: ""
        defaultValue: ""
    }

    StringSetting {
        settingKey: "maxWidth"
        label: "Maximum width"
        description: "In pixels; longer titles are cut short."
        placeholder: "320"
        defaultValue: "320"
    }

    StringSetting {
        settingKey: "cli"
        label: "melody-cli"
        description: "The command, if it isn't on your PATH."
        placeholder: "melody-cli"
        defaultValue: "melody-cli"
    }
}
