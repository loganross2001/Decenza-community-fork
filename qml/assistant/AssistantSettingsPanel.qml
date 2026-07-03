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
    implicitHeight: col.implicitHeight + Theme.spacingLarge * 2

    ColumnLayout {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spacingLarge
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

        // Voice picker
        Tr {
            key: "barista.settings.voice"; fallback: "Voice"
            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall
            ComboBox {
                id: voiceBox
                Layout.fillWidth: true
                model: root._voice ? root._voice.availableVoices : []
                Accessible.name: TranslationManager.translate("barista.settings.voice", "Voice")
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
            AccessibleButton {
                subtle: true
                text: TranslationManager.translate("barista.settings.preview", "Preview")
                accessibleName: TranslationManager.translate("barista.settings.preview", "Preview")
                onClicked: if (root._voice) root._voice.preview()
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
