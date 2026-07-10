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
    // [barista-fork] The SEPARATE coaching voice (live steam + espresso coaches), chosen independently.
    readonly property var _coachingVoice: (typeof Barista !== "undefined") ? Barista.coachingVoice : null

    // Transparent root: this panel is embedded inside the AssistantOverlay's chromed
    // settingsCard, so it must NOT draw its own surface/border (that produced a
    // card-in-card double outline). The host card owns the chrome; the panel just lays
    // out + scrolls its content on it.
    color: "transparent"
    border.width: 0
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

            // Reveal state for the (masked) ElevenLabs API key field below.
            property bool _elKeyShown: false

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
            Component.onCompleted: text = root._settings ? root._settings.assistantName : ""
            placeholderText: TranslationManager.translate("barista.settings.namePlaceholder", "e.g. Gaggia")
            onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.assistantName = text }
        }

        // Your name — how the assistant addresses you (used in greetings)
        Tr {
            key: "barista.settings.yourName"; fallback: "Your name"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        StyledTextField {
            id: userField
            Layout.fillWidth: true
            Component.onCompleted: text = root._settings ? root._settings.userName : ""
            placeholderText: TranslationManager.translate("barista.settings.yourNamePlaceholder", "e.g. Chris")
            onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.userName = text }
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
            Component.onCompleted: text = root._settings ? root._settings.openaiApiKey : ""
            placeholderText: TranslationManager.translate("barista.settings.openaiKey",
                "OpenAI API key (or leave blank to use Settings → AI)")
            onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.openaiApiKey = text }
        }

        // ElevenLabs (only when provider = elevenlabs): masked API key + saved-voices manager.
        // The key is a SECRET — masked by default (password echo) with a Show/Hide toggle, matching the
        // convention in Settings → AI (SettingsAITab masks provider keys). There's no eye SVG in the icon
        // set and unicode-glyph icons are disallowed, so the reveal is a plain text toggle button.
        // The key is SHARED by the barista and coaching sections — show it whenever EITHER is on ElevenLabs
        // (a coaching-only ElevenLabs user still needs to enter it, and it lives only here).
        Tr {
            visible: col._provider === "elevenlabs" || col._coachingProvider === "elevenlabs"
            key: "barista.settings.elKeyLabel"; fallback: "ElevenLabs API key"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        RowLayout {
            visible: col._provider === "elevenlabs" || col._coachingProvider === "elevenlabs"
            Layout.fillWidth: true
            spacing: Theme.spacingSmall
            StyledTextField {
                id: elKeyField
                Layout.fillWidth: true
                echoMode: col._elKeyShown ? TextInput.Normal : TextInput.Password
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                Component.onCompleted: text = root._settings ? root._settings.elevenlabsApiKey : ""
                placeholderText: TranslationManager.translate("barista.settings.elKey", "ElevenLabs API key")
                accessibleName: TranslationManager.translate("barista.settings.elKeyLabel", "ElevenLabs API key")
                onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.elevenlabsApiKey = text }
            }
            AccessibleButton {
                subtle: true
                text: col._elKeyShown
                      ? TranslationManager.translate("common.button.hide", "Hide")
                      : TranslationManager.translate("common.button.show", "Show")
                accessibleName: col._elKeyShown
                      ? TranslationManager.translate("barista.settings.elKeyHide", "Hide API key")
                      : TranslationManager.translate("barista.settings.elKeyShow", "Show API key")
                onClicked: col._elKeyShown = !col._elKeyShown
            }
        }

        // Saved ElevenLabs voices — the reusable manager (select / add / edit / delete). The active barista
        // selection is elevenlabsVoiceId; tapping a row sets it via the voiceSelected signal.
        Tr {
            visible: col._provider === "elevenlabs"
            key: "barista.settings.elSavedVoices"; fallback: "Saved voices"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        BaristaSavedVoices {
            visible: col._provider === "elevenlabs"
            Layout.fillWidth: true
            settings: root._settings
            activeId: root._settings ? root._settings.elevenlabsVoiceId : ""
            onVoiceSelected: function(id) { if (root._settings) root._settings.elevenlabsVoiceId = id }
        }

        // Preview the barista's chosen voice (any provider).
        AccessibleButton {
            subtle: true
            text: TranslationManager.translate("barista.settings.preview", "Preview voice")
            accessibleName: TranslationManager.translate("barista.settings.preview", "Preview voice")
            onClicked: if (root._voice) root._voice.preview()
        }

        // ── Coaching voice ─────────────────────────────────────────────────
        // [barista-fork] A SEPARATE voice for the live steam + espresso coaches, picked independently of
        // the barista's conversational voice above. Reuses the same per-provider controls; the ElevenLabs
        // API key + saved-voices list are SHARED with the barista section (not duplicated here).
        Rectangle {   // subtle divider so the section reads as distinct
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingSmall
            implicitHeight: 1
            color: Theme.borderColor
        }
        Tr {
            key: "barista.settings.coachingVoiceSection"; fallback: "Coaching voice"
            color: Theme.textColor; font: Theme.subtitleFont; Accessible.ignored: true
        }
        Tr {
            key: "barista.settings.coachingVoiceHint"
            fallback: "The voice for live steam and espresso coaching (separate from the assistant's voice)."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor; font: Theme.bodyFont; Accessible.ignored: true
        }

        readonly property string _coachingProvider: root._settings ? root._settings.coachingTtsProvider : "native"
        ComboBox {
            id: coachingProviderBox
            Layout.fillWidth: true
            model: ["native", "openai", "elevenlabs"]
            Accessible.name: TranslationManager.translate("barista.settings.coachingVoiceSection", "Coaching voice")
            Component.onCompleted: {
                var i = root._settings ? model.indexOf(root._settings.coachingTtsProvider) : -1
                if (i >= 0) currentIndex = i
            }
            onActivated: if (root._settings) root._settings.coachingTtsProvider = currentText
        }

        // Native coaching voice picker (only when provider = native)
        ComboBox {
            id: coachingVoiceBox
            Layout.fillWidth: true
            visible: col._coachingProvider === "native"
            model: root._coachingVoice ? root._coachingVoice.availableVoices : []
            Accessible.name: TranslationManager.translate("barista.settings.nativeVoice", "Native voice")
            Component.onCompleted: _sync()
            function _sync() {
                if (!root._coachingVoice) return
                var i = model ? model.indexOf(root._coachingVoice.voiceName) : -1
                if (i >= 0) currentIndex = i
            }
            onActivated: if (root._coachingVoice && currentText.length > 0) root._coachingVoice.setVoiceByName(currentText)
            Connections {
                target: root._coachingVoice
                ignoreUnknownSignals: true
                function onAvailableVoicesChanged() { coachingVoiceBox._sync() }
            }
        }

        // OpenAI coaching voice (only when provider = openai) — reuses the shared OpenAI key
        ComboBox {
            Layout.fillWidth: true
            visible: col._coachingProvider === "openai"
            model: ["nova", "shimmer", "alloy", "echo", "fable", "onyx"]
            Accessible.name: TranslationManager.translate("barista.settings.openaiVoice", "OpenAI voice")
            Component.onCompleted: {
                var i = root._settings ? model.indexOf(root._settings.coachingOpenaiVoice) : -1
                if (i >= 0) currentIndex = i
            }
            onActivated: if (root._settings) root._settings.coachingOpenaiVoice = currentText
        }

        // ElevenLabs coaching voice (only when provider = elevenlabs): the SAME reusable saved-voices manager
        // as the barista section, bound to the coaching selection. The API key + add/edit/delete are shared
        // (one underlying list); tapping a row sets coachingElevenlabsVoiceId via voiceSelected.
        Tr {
            visible: col._coachingProvider === "elevenlabs"
            key: "barista.settings.elSavedVoices"; fallback: "Saved voices"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        BaristaSavedVoices {
            visible: col._coachingProvider === "elevenlabs"
            Layout.fillWidth: true
            settings: root._settings
            activeId: root._settings ? root._settings.coachingElevenlabsVoiceId : ""
            onVoiceSelected: function(id) { if (root._settings) root._settings.coachingElevenlabsVoiceId = id }
        }

        AccessibleButton {
            subtle: true
            text: TranslationManager.translate("barista.settings.previewCoaching", "Preview coaching voice")
            accessibleName: TranslationManager.translate("barista.settings.previewCoaching", "Preview coaching voice")
            onClicked: if (root._coachingVoice) root._coachingVoice.preview()
        }

        // Divider closing the coaching-voice section
        Rectangle {
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spacingSmall
            implicitHeight: 1
            color: Theme.borderColor
        }

        // Bell — the chime when the assistant greets you
        Tr {
            key: "barista.settings.bell"; fallback: "Bell"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        ComboBox {
            id: bellBox
            Layout.fillWidth: true
            // "custom" plays a file picked in Settings → AI (bellCustomPath); set it there first.
            model: ["poof", "ding", "off", "custom"]
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

        // Speak the opening greeting aloud (default off — otherwise the barista is present but quiet at the
        // start and only speaks its replies). Only meaningful when "Speak out loud" is on.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall
            enabled: root._settings ? root._settings.voiceEnabled : true
            Switch {
                id: greetAloudSwitch
                checked: root._settings ? root._settings.greetAloud : false
                onToggled: if (root._settings) root._settings.greetAloud = checked
                Accessible.role: Accessible.CheckBox
                Accessible.name: trGreetAloudLabel.text
                Accessible.checked: checked
                Accessible.focusable: true
                Accessible.onToggleAction: toggle()
            }
            Tr {
                id: trGreetAloudLabel
                key: "barista.settings.greetAloud"; fallback: "Speak greeting aloud"
                Layout.fillWidth: true
                color: Theme.textColor; font: Theme.bodyFont
                Accessible.ignored: true
            }
        }

        // ── Maintenance & reminders ────────────────────────────────────────
        // [barista-fork] Entry point to the maintenance schedule editor (intervals / enable / mark done).
        // The dialog is instantiated below and opened here — a provably-reachable path from the gear:
        // conversation-card gear → AssistantSettingsPanel (this file) → this button → MaintenanceSettingsDialog.
        Rectangle {   // subtle divider so the section reads as distinct
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingSmall
            implicitHeight: 1
            color: Theme.borderColor
        }
        AccessibleButton {
            Layout.fillWidth: true
            text: TranslationManager.translate("barista.settings.maintenance", "Maintenance & reminders…")
            accessibleName: TranslationManager.translate("barista.settings.maintenance", "Maintenance & reminders")
            onClicked: maintenanceDialog.open()
        }
        Tr {
            key: "barista.settings.maintenanceHint"
            fallback: "Editable default schedule for the DE1 — confirm intervals against Decent's published schedule."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        }
    }

    // The maintenance schedule editor. Modal, parented to the app Overlay (so it centres on screen,
    // not inside the scrolled panel). Reachable only via the button above.
    MaintenanceSettingsDialog {
        id: maintenanceDialog
    }
}
