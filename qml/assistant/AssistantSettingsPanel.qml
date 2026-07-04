import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] Compact settings for the assistant: name it, pick a voice, audition it, and mute.
// Shown inside the AssistantOverlay (idle page). Reads/writes Barista.settings + Barista.voice.
Rectangle {
    id: root
    signal closed()

    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null
    readonly property var _voice: (typeof Barista !== "undefined") ? Barista.voice : null

    radius: Theme.cardRadius
    color: Theme.surfaceColor
    border.width: 1
    border.color: Theme.borderColor
    // Cap to the screen and scroll — the settings list is taller than a phone screen.
    height: Math.min(col.implicitHeight + Theme.spacingLarge * 2,
                     (parent ? parent.height : Theme.scaled(600)) - Theme.spacingLarge * 2)

    Flickable {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: col
            width: parent.width
            spacing: Theme.spacingMedium

        // Header
        RowLayout {
            Layout.fillWidth: true
            Tr {
                key: "barista.settings.title"; fallback: "Assistant"
                Layout.fillWidth: true
                color: Theme.textColor
                font: Theme.subtitleFont
                Accessible.ignored: true
            }
            AccessibleButton {
                subtle: true
                text: "×"
                accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                onClicked: root.closed()
            }
        }

        // Name
        Tr {
            key: "barista.settings.name"; fallback: "Name"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        StyledTextField {
            id: nameField
            Layout.fillWidth: true
            text: root._settings ? root._settings.assistantName : ""
            placeholderText: TranslationManager.translate("barista.settings.namePlaceholder", "e.g. Gaggia")
            onEditingFinished: if (root._settings) root._settings.assistantName = text
        }

        // Your name — how the assistant addresses you (used in greetings)
        Tr {
            key: "barista.settings.yourName"; fallback: "Your name"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        StyledTextField {
            id: userField
            Layout.fillWidth: true
            text: root._settings ? root._settings.userName : ""
            placeholderText: TranslationManager.translate("barista.settings.yourNamePlaceholder", "e.g. Chris")
            onEditingFinished: if (root._settings) root._settings.userName = text
        }

        // Voice source: native (free/robotic) · OpenAI · ElevenLabs (human, cloud)
        Tr {
            key: "barista.settings.voice"; fallback: "Voice"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        readonly property string _provider: root._settings ? root._settings.ttsProvider : "native"
        ComboBox {
            id: providerBox
            Layout.fillWidth: true
            model: ["native", "openai", "elevenlabs"]
            Accessible.name: TranslationManager.translate("barista.settings.voice", "Voice")
            Component.onCompleted: {
                var i = root._settings ? model.indexOf(root._settings.ttsProvider) : -1
                if (i >= 0) currentIndex = i
            }
            onActivated: if (root._settings) root._settings.ttsProvider = currentText
        }

        // Native voice picker (only when provider = native)
        ComboBox {
            id: voiceBox
            Layout.fillWidth: true
            visible: col._provider === "native"
            model: root._voice ? root._voice.availableVoices : []
            Accessible.name: TranslationManager.translate("barista.settings.nativeVoice", "Native voice")
            Component.onCompleted: _sync()
            function _sync() {
                if (!root._voice) return
                var i = model ? model.indexOf(root._voice.voiceName) : -1
                if (i >= 0) currentIndex = i
            }
            onActivated: if (root._voice && currentText.length > 0) root._voice.setVoiceByName(currentText)
            Connections {
                target: root._voice
                ignoreUnknownSignals: true
                function onAvailableVoicesChanged() { voiceBox._sync() }
            }
        }

        // OpenAI voice (only when provider = openai) — reuses your app's OpenAI key
        ComboBox {
            Layout.fillWidth: true
            visible: col._provider === "openai"
            model: ["nova", "shimmer", "alloy", "echo", "fable", "onyx"]
            Accessible.name: TranslationManager.translate("barista.settings.openaiVoice", "OpenAI voice")
            Component.onCompleted: {
                var i = root._settings ? model.indexOf(root._settings.openaiVoice) : -1
                if (i >= 0) currentIndex = i
            }
            onActivated: if (root._settings) root._settings.openaiVoice = currentText
        }
        StyledTextField {
            Layout.fillWidth: true
            visible: col._provider === "openai"
            text: root._settings ? root._settings.openaiApiKey : ""
            placeholderText: TranslationManager.translate("barista.settings.openaiKey",
                "OpenAI API key (or leave blank to use Settings → AI)")
            echoMode: TextInput.PasswordEchoOnEdit
            onEditingFinished: if (root._settings) root._settings.openaiApiKey = text
        }

        // ElevenLabs (only when provider = elevenlabs): API key + voice id
        StyledTextField {
            Layout.fillWidth: true
            visible: col._provider === "elevenlabs"
            text: root._settings ? root._settings.elevenlabsApiKey : ""
            placeholderText: TranslationManager.translate("barista.settings.elKey", "ElevenLabs API key")
            echoMode: TextInput.PasswordEchoOnEdit
            onEditingFinished: if (root._settings) root._settings.elevenlabsApiKey = text
        }
        StyledTextField {
            Layout.fillWidth: true
            visible: col._provider === "elevenlabs"
            text: root._settings ? root._settings.elevenlabsVoiceId : ""
            placeholderText: TranslationManager.translate("barista.settings.elVoice", "ElevenLabs voice ID")
            onEditingFinished: if (root._settings) root._settings.elevenlabsVoiceId = text
        }

        AccessibleButton {
            subtle: true
            text: TranslationManager.translate("barista.settings.preview", "Preview voice")
            accessibleName: TranslationManager.translate("barista.settings.preview", "Preview voice")
            onClicked: if (root._voice) root._voice.preview()
        }

        // Bell — the chime when the assistant greets you
        Tr {
            key: "barista.settings.bell"; fallback: "Bell"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        ComboBox {
            id: bellBox
            Layout.fillWidth: true
            model: ["ding", "tick", "frameclick1", "frameclick2", "frameclick3", "off"]
            Accessible.name: TranslationManager.translate("barista.settings.bell", "Bell")
            Component.onCompleted: {
                var i = root._settings ? model.indexOf(root._settings.bellSound) : -1
                if (i >= 0) currentIndex = i
            }
            onActivated: {
                if (root._settings) root._settings.bellSound = currentText
                if (root._voice) root._voice.previewBell(currentText)   // audition the choice
            }
        }

        // Mute / speak toggle
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall
            Switch {
                id: voiceSwitch
                checked: root._settings ? root._settings.voiceEnabled : true
                onToggled: if (root._settings) root._settings.voiceEnabled = checked
                Accessible.role: Accessible.CheckBox
                Accessible.name: trVoiceLabel.text
                Accessible.checked: checked
                Accessible.focusable: true
                Accessible.onToggleAction: toggle()
            }
            Tr {
                id: trVoiceLabel
                key: "barista.settings.speak"; fallback: "Speak out loud"
                Layout.fillWidth: true
                color: Theme.textColor; font: Theme.bodyFont
                Accessible.ignored: true
            }
        }
        }
    }
}
