import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] Barista-assistant settings, hosted in the main Settings → AI tab via a Loader hook
// there (so the settings live where the user expects, not in a card gear). Reads/writes Barista.settings
// + Barista.voice. Loaded by URL, so it imports the Decenza module for its types.
ColumnLayout {
    id: root
    Layout.fillWidth: true
    spacing: Theme.spacingMedium

    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null
    readonly property var _voice: (typeof Barista !== "undefined") ? Barista.voice : null
    readonly property string _provider: root._settings ? root._settings.ttsProvider : "native"

    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.borderColor }

    Tr {
        key: "barista.settings.title"; fallback: "Barista assistant"
        color: Theme.textColor; font: Theme.subtitleFont; Accessible.ignored: true
    }

    // Enable the proactive assistant
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        Switch {
            checked: root._settings ? root._settings.enabled : true
            onToggled: if (root._settings) root._settings.enabled = checked
            Accessible.role: Accessible.CheckBox; Accessible.name: trEnable.text
            Accessible.checked: checked; Accessible.focusable: true; Accessible.onToggleAction: toggle()
        }
        Tr { id: trEnable; key: "barista.settings.enable"; fallback: "Proactive assistant"
             Layout.fillWidth: true; color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true }
    }

    // Assistant name
    Tr { key: "barista.settings.name"; fallback: "Assistant name"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    StyledTextField {
        Layout.fillWidth: true
        Component.onCompleted: text = root._settings ? root._settings.assistantName : ""
        placeholderText: TranslationManager.translate("barista.settings.namePlaceholder", "e.g. Gaggia")
        onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.assistantName = text }
    }

    // Your name
    Tr { key: "barista.settings.yourName"; fallback: "Your name"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    StyledTextField {
        Layout.fillWidth: true
        Component.onCompleted: text = root._settings ? root._settings.userName : ""
        placeholderText: TranslationManager.translate("barista.settings.yourNamePlaceholder", "e.g. Chris")
        onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.userName = text }
    }

    // Voice source
    Tr { key: "barista.settings.voice"; fallback: "Voice"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    ComboBox {
        id: providerBox
        Layout.fillWidth: true
        model: ["native", "openai", "elevenlabs"]
        Component.onCompleted: { var i = root._settings ? model.indexOf(root._settings.ttsProvider) : -1; if (i >= 0) currentIndex = i }
        onActivated: if (root._settings) root._settings.ttsProvider = currentText
    }
    ComboBox {
        id: nativeVoiceBox
        Layout.fillWidth: true
        visible: root._provider === "native"
        model: root._voice ? root._voice.availableVoices : []
        Component.onCompleted: _sync()
        function _sync() { if (!root._voice) return; var i = model ? model.indexOf(root._voice.voiceName) : -1; if (i >= 0) currentIndex = i }
        onActivated: if (root._voice && currentText.length > 0) root._voice.setVoiceByName(currentText)
        Connections { target: root._voice; ignoreUnknownSignals: true; function onAvailableVoicesChanged() { nativeVoiceBox._sync() } }
    }
    ComboBox {
        Layout.fillWidth: true
        visible: root._provider === "openai"
        model: ["nova", "shimmer", "alloy", "echo", "fable", "onyx"]
        Component.onCompleted: { var i = root._settings ? model.indexOf(root._settings.openaiVoice) : -1; if (i >= 0) currentIndex = i }
        onActivated: if (root._settings) root._settings.openaiVoice = currentText
    }
    StyledTextField {
        Layout.fillWidth: true
        visible: root._provider === "openai"
        Component.onCompleted: text = root._settings ? root._settings.openaiApiKey : ""
        placeholderText: TranslationManager.translate("barista.settings.openaiKey", "OpenAI API key (or leave blank to use the AI key above)")
        onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.openaiApiKey = text }
    }
    StyledTextField {
        Layout.fillWidth: true
        visible: root._provider === "elevenlabs"
        Component.onCompleted: text = root._settings ? root._settings.elevenlabsApiKey : ""
        placeholderText: TranslationManager.translate("barista.settings.elKey", "ElevenLabs API key")
        onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.elevenlabsApiKey = text }
    }
    StyledTextField {
        Layout.fillWidth: true
        visible: root._provider === "elevenlabs"
        Component.onCompleted: text = root._settings ? root._settings.elevenlabsVoiceId : ""
        placeholderText: TranslationManager.translate("barista.settings.elVoice", "ElevenLabs voice ID")
        onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.elevenlabsVoiceId = text }
    }

    // Voice speed slider
    Tr { key: "barista.settings.speed"; fallback: "Voice speed"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        Slider {
            id: speedSlider
            Layout.fillWidth: true
            from: 0.7; to: 1.3; stepSize: 0.05
            value: root._settings ? root._settings.voiceSpeed : 1.0
            onMoved: if (root._settings) root._settings.voiceSpeed = value
            Accessible.name: TranslationManager.translate("barista.settings.speed", "Voice speed")
        }
        Text { text: speedSlider.value.toFixed(2) + "×"; color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
        AccessibleButton {
            subtle: true
            text: TranslationManager.translate("barista.settings.preview", "Preview")
            accessibleName: TranslationManager.translate("barista.settings.preview", "Preview voice")
            onClicked: if (root._voice) root._voice.preview()
        }
    }

    // Bell
    Tr { key: "barista.settings.bell"; fallback: "Bell"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    ComboBox {
        Layout.fillWidth: true
        model: ["ding", "tick", "frameclick1", "frameclick2", "frameclick3", "off"]
        Component.onCompleted: { var i = root._settings ? model.indexOf(root._settings.bellSound) : -1; if (i >= 0) currentIndex = i }
        onActivated: { if (root._settings) root._settings.bellSound = currentText; if (root._voice) root._voice.previewBell(currentText) }
    }

    // Speak / mute
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        Switch {
            checked: root._settings ? root._settings.voiceEnabled : true
            onToggled: if (root._settings) root._settings.voiceEnabled = checked
            Accessible.role: Accessible.CheckBox; Accessible.name: trSpeak.text
            Accessible.checked: checked; Accessible.focusable: true; Accessible.onToggleAction: toggle()
        }
        Tr { id: trSpeak; key: "barista.settings.speak"; fallback: "Speak out loud"
             Layout.fillWidth: true; color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true }
    }
}
