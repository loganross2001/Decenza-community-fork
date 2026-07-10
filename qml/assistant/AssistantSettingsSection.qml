import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
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
    readonly property var _knowledge: (typeof Barista !== "undefined") ? Barista.knowledge : null
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

    // How proactive it is
    Tr { key: "barista.settings.proactivity"; fallback: "Proactivity"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    ComboBox {
        Layout.fillWidth: true
        // "full" = suggests freely; "greetings" = greets + one item; "off" = answers only.
        model: ["full", "greetings", "off"]
        Component.onCompleted: { var i = root._settings ? model.indexOf(root._settings.proactivityLevel) : -1; if (i >= 0) currentIndex = i }
        onActivated: if (root._settings) root._settings.proactivityLevel = currentText
    }

    // Web search (Anthropic provider only)
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        Switch {
            checked: root._settings ? root._settings.webSearchEnabled : true
            onToggled: if (root._settings) root._settings.webSearchEnabled = checked
            Accessible.role: Accessible.CheckBox; Accessible.name: trWeb.text
            Accessible.checked: checked; Accessible.focusable: true; Accessible.onToggleAction: toggle()
        }
        Tr { id: trWeb; key: "barista.settings.web"
             fallback: "Web search — look up beans, roasters & brewing guides (Anthropic)"
             Layout.fillWidth: true; wrapMode: Text.WordWrap
             color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true }
    }

    // Show the animated character face
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        Switch {
            checked: root._settings ? root._settings.avatarEnabled : true
            onToggled: if (root._settings) root._settings.avatarEnabled = checked
            Accessible.role: Accessible.CheckBox; Accessible.name: trAvatar.text
            Accessible.checked: checked; Accessible.focusable: true; Accessible.onToggleAction: toggle()
        }
        Tr { id: trAvatar; key: "barista.settings.avatar"
             fallback: "Show character — a face to watch while it talks"
             Layout.fillWidth: true; wrapMode: Text.WordWrap
             color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true }
    }

    // Which character face to show
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        visible: root._settings ? root._settings.avatarEnabled : true
        Tr { key: "barista.settings.avatarStyle"; fallback: "Character style"
             Layout.fillWidth: true; color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true }
        ComboBox {
            id: avatarStyleBox
            Layout.preferredWidth: Theme.scaled(150)
            textRole: "label"; valueRole: "value"
            model: [
                { value: "face", label: TranslationManager.translate("barista.settings.avatar.face", "Face") },
                { value: "cup",  label: TranslationManager.translate("barista.settings.avatar.cup", "Coffee cup") },
                { value: "orb",  label: TranslationManager.translate("barista.settings.avatar.orb", "Voice orb") },
                { value: "bean", label: TranslationManager.translate("barista.settings.avatar.bean", "Coffee bean") }
            ]
            Component.onCompleted: { var i = indexOfValue(root._settings ? root._settings.avatarStyle : "face"); if (i >= 0) currentIndex = i }
            onActivated: if (root._settings) root._settings.avatarStyle = currentValue
            Accessible.name: TranslationManager.translate("barista.settings.avatarStyle", "Character style")
        }
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

    // Voice speed slider (barista voice — the coaching voice has its own speed in the barista settings panel)
    Tr { key: "barista.settings.speed"; fallback: "Voice speed"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        Slider {
            id: speedSlider
            Layout.fillWidth: true
            from: 0.7; to: 1.3; stepSize: 0.05
            value: root._settings ? root._settings.baristaVoiceSpeed : 1.0
            onMoved: if (root._settings) root._settings.baristaVoiceSpeed = value
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

    // Voice volume slider (barista voice)
    Tr { key: "barista.settings.volume"; fallback: "Voice volume"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        Slider {
            id: volumeSlider
            Layout.fillWidth: true
            from: 0.0; to: 1.0; stepSize: 0.05
            value: root._settings ? root._settings.baristaVoiceVolume : 1.0
            onMoved: if (root._settings) root._settings.baristaVoiceVolume = value
            Accessible.name: TranslationManager.translate("barista.settings.volume", "Voice volume")
        }
        Text { text: Math.round(volumeSlider.value * 100) + "%"; color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    }

    // Bell — the greeting chime. "poof" is the soft default; "off" silences it; "custom" plays your own file.
    Tr { key: "barista.settings.bell"; fallback: "Bell"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    ComboBox {
        id: bellCombo
        Layout.fillWidth: true
        model: ["poof", "ding", "off", "custom"]
        Component.onCompleted: { var i = root._settings ? model.indexOf(root._settings.bellSound) : -1; if (i >= 0) currentIndex = i }
        onActivated: {
            if (root._settings) root._settings.bellSound = currentText
            if (currentText === "custom") {
                if (root._settings && root._settings.bellCustomPath.length === 0) bellFileDialog.open()
                else if (root._voice) root._voice.previewBell("custom")
            } else if (root._voice) {
                root._voice.previewBell(currentText)
            }
        }
    }
    // Custom-sound chooser — only shown for "custom". Plays your own .wav from the tablet.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacingSmall
        visible: root._settings && root._settings.bellSound === "custom"
        Button {
            text: TranslationManager.translate("barista.settings.bellChoose", "Choose sound file…")
            onClicked: bellFileDialog.open()
        }
        Text {
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            color: Theme.textSecondaryColor
            font: Theme.labelFont
            text: {
                var p = (root._settings && root._settings.bellCustomPath) ? String(root._settings.bellCustomPath) : ""
                if (p.length === 0) return TranslationManager.translate("barista.settings.bellNoFile", "No file chosen")
                return decodeURIComponent(p.substring(p.lastIndexOf("/") + 1))
            }
        }
    }
    FileDialog {
        id: bellFileDialog
        title: TranslationManager.translate("barista.settings.bellChoose", "Choose sound file…")
        nameFilters: ["Sound files (*.wav *.mp3)", "All files (*)"]
        onAccepted: {
            if (root._settings) root._settings.bellCustomPath = String(selectedFile)
            if (root._voice) root._voice.previewBell("custom")
        }
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

    // ── Knowledge store: portable, relocatable, backed up ──────────────────────────
    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.borderColor }
    Tr { key: "barista.kb.title"; fallback: "Knowledge store"
         color: Theme.textColor; font: Theme.subtitleFont; Accessible.ignored: true }
    Tr {
        key: "barista.kb.blurb"
        fallback: "Your assistant's memory — every conversation and the advice it gave you. Back it up to a folder you own (e.g. a synced or cloud folder) so it's portable and safe."
        Layout.fillWidth: true; wrapMode: Text.WordWrap
        color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
    }

    Tr { key: "barista.kb.folder"; fallback: "Backup folder"
         color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
    StyledTextField {
        Layout.fillWidth: true
        Component.onCompleted: text = root._knowledge ? root._knowledge.location : ""
        placeholderText: TranslationManager.translate("barista.kb.folderPlaceholder", "Folder path")
        onEditingFinished: { Qt.inputMethod.commit(); if (root._knowledge) root._knowledge.location = text }
    }

    RowLayout {
        Layout.fillWidth: true; spacing: Theme.spacingSmall
        AccessibleButton {
            text: TranslationManager.translate("barista.kb.backup", "Back up now")
            accessibleName: text
            onClicked: if (root._knowledge) root._knowledge.backupNow()
        }
        // Restore from an existing backup (newest first)
        ComboBox {
            id: restoreBox
            Layout.fillWidth: true
            visible: root._knowledge && root._knowledge.backups.length > 0
            model: root._knowledge ? root._knowledge.backups : []
        }
        AccessibleButton {
            subtle: true
            visible: root._knowledge && root._knowledge.backups.length > 0
            text: TranslationManager.translate("barista.kb.restore", "Restore")
            accessibleName: TranslationManager.translate("barista.kb.restoreA", "Restore backup")
            onClicked: if (root._knowledge && restoreBox.currentText.length > 0) root._knowledge.restore(restoreBox.currentText)
        }
    }

    Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap
        color: Theme.textSecondaryColor; font: Theme.labelFont
        Accessible.ignored: true
        text: {
            if (!root._knowledge) return ""
            if (root._knowledge.status && root._knowledge.status.length > 0) return root._knowledge.status
            return root._knowledge.lastBackup && root._knowledge.lastBackup.length > 0
                   ? TranslationManager.translate("barista.kb.last", "Last backup: %1").arg(root._knowledge.lastBackup)
                   : ""
        }
        visible: text.length > 0
    }
}
