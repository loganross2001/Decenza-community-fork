import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] Barista-assistant hook in the main Settings → AI tab. Reduced to the ENABLE toggle + a
// pointer to the barista's own settings panel (opened by the gear on the assistant), which now owns all the
// other controls (voice, character, proactivity, web search, names, tab size, coaching, maintenance). The
// Knowledge-store backup block is kept here — it has no home in that panel. Loaded by URL, so it imports the
// Decenza module for its types.
ColumnLayout {
    id: root
    Layout.fillWidth: true
    spacing: Theme.spacingMedium

    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null
    readonly property var _knowledge: (typeof Barista !== "undefined") ? Barista.knowledge : null

    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.borderColor }

    Tr {
        key: "barista.settings.title"; fallback: "Barista assistant"
        color: Theme.textColor; font: Theme.subtitleFont; Accessible.ignored: true
    }

    // Enable the proactive assistant. This is the ONLY barista control that remains in the main Settings → AI
    // tab — everything else (voice, character, proactivity, web search, names, tab size, coaching, maintenance)
    // now lives in the barista's own gear-opened settings panel (AssistantSettingsPanel.qml), so this section
    // stays to a single on/off switch plus a pointer. The Knowledge-store backup block below is kept here
    // because it has no home in that panel.
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

    // Pointer to the barista's own settings panel (opened by the assistant's gear), where the rest lives.
    Tr {
        key: "barista.settings.moreInPanel"
        fallback: "Voice, character, proactivity and more are in the barista's own settings (open it with the gear on the assistant)."
        Layout.fillWidth: true; wrapMode: Text.WordWrap
        color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
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
