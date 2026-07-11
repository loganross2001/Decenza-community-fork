import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] Reusable ElevenLabs saved-voices manager. Renders the saved voices as tidy, tappable
// rows (name prominent, a metadata subtitle — accent · gender · use case, NOT the raw id — an active
// indicator, and inline Edit / Delete icon-buttons), a search field that filters the list on that metadata,
// plus a "+ Add voice" button that reveals an inline Name + Voice ID editor. The SAME editor is reused for
// Edit (prefilled). Used in BOTH the barista Voice section and the Coaching voice section — the underlying
// saved-voices list is a single shared model on AssistantSettings, so this component is section-agnostic:
//
//   • `activeId`  (in)  — the id currently selected for THIS section (barista or coaching).
//   • `voiceSelected(id)` (out) — emitted when a row is tapped; the parent wires it to the right setter
//                                 (elevenlabsVoiceId for barista, coachingElevenlabsVoiceId for coaching).
//
// Add / Edit / Delete call the shared AssistantSettings methods directly (add/update/removeElevenlabsVoice),
// so no section-knowledge is baked in here.
ColumnLayout {
    id: root

    // The barista settings object (Barista.settings). Owns the shared saved-voices list + its mutators.
    property var settings: null
    // The active voice id FOR THIS SECTION (bound by the parent from the barista/coaching selection).
    property string activeId: ""
    // Emitted when the user taps a row to select it. The parent applies it to the correct target.
    signal voiceSelected(string id)

    spacing: Theme.spacingSmall

    // Inline editor state. When _editorOpen is true the Name + Voice ID fields + Save/Cancel show; _editingId
    // is the id being edited ("" = adding a new voice, non-empty = editing that saved voice in place).
    property bool _editorOpen: false
    property string _editingId: ""

    // Saved-list search query (name + accent + gender + age + useCase + description, case-insensitive, ANY).
    property string _query: ""

    // The filtered saved-voices list. Recomputes whenever the query changes OR the underlying list changes
    // (elevenlabsVoicesChanged) — both are dependencies of this binding, so the Repeater below stays live.
    readonly property var _visibleVoices: {
        var all = root.settings ? root.settings.elevenlabsVoices : []
        // Only filter when the search field is actually shown (list > 2). Otherwise a stale query left over
        // from when the list was longer could hide every row with no visible way to clear it.
        if (root._query.length === 0 || all.length <= 2)
            return all
        var q = root._query.toLowerCase()
        var fields = ["name", "accent", "gender", "age", "useCase", "description"]
        var out = []
        for (var i = 0; i < all.length; ++i) {
            var v = all[i]
            for (var f = 0; f < fields.length; ++f) {
                var val = v[fields[f]]
                if (val && String(val).toLowerCase().indexOf(q) !== -1) {
                    out.push(v)
                    break
                }
            }
        }
        return out
    }

    // Format the visible metadata subtitle from a saved-voice record: "American · Female · Narration"
    // (accent · gender · useCase, empties omitted). Never includes the raw id.
    function _metaLine(v) {
        var parts = []
        var order = ["accent", "gender", "useCase"]
        for (var i = 0; i < order.length; ++i) {
            var val = v[order[i]]
            if (val && String(val).length > 0)
                parts.push(String(val))
        }
        return parts.join(" · ")
    }

    // Hidden Tr instances for strings used in property bindings / accessibleName concatenation.
    Tr { id: trSelect; key: "barista.settings.elSelectVoice"; fallback: "Select voice"; visible: false }
    Tr { id: trActive; key: "barista.settings.elActiveVoice"; fallback: "Active"; visible: false }
    Tr { id: trEdit; key: "barista.settings.elEditVoice"; fallback: "Edit voice"; visible: false }
    Tr { id: trDelete; key: "barista.settings.elRemoveVoice"; fallback: "Remove voice"; visible: false }

    function _openAdd() {
        root._editingId = ""
        elNameField.text = ""
        elIdField.text = ""
        root._editorOpen = true
        elNameField.forceActiveFocus()
    }
    function _openEdit(id, name) {
        root._editingId = id
        elNameField.text = name
        elIdField.text = id
        root._editorOpen = true
        elNameField.forceActiveFocus()
    }
    function _cancelEditor() {
        root._editorOpen = false
        root._editingId = ""
        elNameField.text = ""
        elIdField.text = ""
    }
    function _saveEditor() {
        Qt.inputMethod.commit()   // commit the IME so the in-progress word isn't dropped (mobile)
        if (!root.settings)
            return
        var newId = elIdField.text.trim()
        if (newId.length === 0)
            return   // an entry with no id is meaningless — keep the editor open
        if (root._editingId.length > 0) {
            root.settings.updateElevenlabsVoice(root._editingId, elNameField.text, newId)
            // If the user was editing the ACTIVE voice's id, follow it here too so the UI reflects it
            // immediately (the C++ also re-points the persisted selection).
            if (root.activeId === root._editingId && newId !== root._editingId)
                root.voiceSelected(newId)
        } else {
            root.settings.addElevenlabsVoice(elNameField.text, newId)
            // Make a freshly-added voice the active selection for this section, so the owner sees an
            // immediate effect without a second tap.
            root.voiceSelected(newId)
        }
        root._cancelEditor()
    }

    // Empty state.
    Tr {
        visible: !root.settings || root.settings.elevenlabsVoices.length === 0
        key: "barista.settings.elNoVoices"; fallback: "No saved voices yet — add one below."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textSecondaryColor; font: Theme.bodyFont; Accessible.ignored: true
    }

    // Search the saved list by name / accent / gender / age / use case / description. Shown once there are
    // enough rows to be worth filtering. Filters the rows below via root._query → root._visibleVoices.
    StyledTextField {
        id: savedSearchField
        visible: !!root.settings && root.settings.elevenlabsVoices.length > 2 && !root._editorOpen
        Layout.fillWidth: true
        inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
        placeholderText: TranslationManager.translate("barista.settings.elSearchPlaceholder",
            "Search by name, accent, use case…")
        accessibleName: TranslationManager.translate("barista.settings.elSearch", "Search saved voices")
        onTextChanged: root._query = text
    }

    // "No matches" — the list is non-empty but the query filtered everything out.
    Tr {
        visible: savedSearchField.visible && root._query.length > 0 && root._visibleVoices.length === 0
        key: "barista.settings.elNoMatches"; fallback: "No saved voices match your search."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
    }

    // Saved-voice rows. Bind to the FILTERED list (which itself depends on elevenlabsVoices, so it re-evaluates
    // on elevenlabsVoicesChanged AND on the search query). Row fill = Theme.backgroundColor so rows sit
    // distinctly INSIDE the card's surface (matching BagCard/EquipmentCard/MaintenanceSettingsDialog content
    // rows), not the same surfaceColor as the host card (which read flat/washed-out).
    Repeater {
        model: root._visibleVoices
        delegate: Rectangle {
            id: voiceRow
            required property var modelData
            readonly property string voiceId: modelData["id"] !== undefined ? modelData["id"] : ""
            readonly property string voiceName: (modelData["name"] !== undefined && modelData["name"].length > 0)
                                                ? modelData["name"] : voiceId
            // Metadata subtitle (accent · gender · useCase). Never the raw id.
            readonly property string metaLine: root._metaLine(modelData)
            readonly property bool isActive: root.activeId === voiceId

            Layout.fillWidth: true
            implicitHeight: rowInner.implicitHeight + Theme.spacingMedium
            radius: Theme.cardRadius
            color: Theme.backgroundColor
            // Active-voice affordance: a thicker primary-coloured border (mirrors EquipmentCard/BagCard).
            border.width: isActive ? 2 : 1
            border.color: isActive ? Theme.primaryColor : Theme.borderColor

            AccessibleMouseArea {
                anchors.fill: parent
                accessibleName: trSelect.text + ": " + voiceRow.voiceName
                    + (voiceRow.metaLine.length > 0 ? ", " + voiceRow.metaLine : "")
                    + (voiceRow.isActive ? " (" + trActive.text + ")" : "")
                onAccessibleClicked: root.voiceSelected(voiceRow.voiceId)
            }

            RowLayout {
                id: rowInner
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingMedium
                anchors.rightMargin: Theme.spacingSmall
                anchors.topMargin: Theme.spacingSmall
                anchors.bottomMargin: Theme.spacingSmall
                spacing: Theme.spacingSmall

                // Active indicator: a filled primary dot on the active row, a hollow ring otherwise, so the
                // selection is obvious at a glance (radio-style) without relying on a glyph font.
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: Theme.scaled(16); implicitHeight: Theme.scaled(16)
                    radius: width / 2
                    color: voiceRow.isActive ? Theme.primaryColor : "transparent"
                    border.width: voiceRow.isActive ? 0 : 2
                    border.color: Theme.textSecondaryColor
                    Accessible.ignored: true
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        text: voiceRow.voiceName
                        color: Theme.textColor; font: Theme.bodyFont
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }
                    Text {
                        Layout.fillWidth: true
                        // Metadata subtitle (accent · gender · use case) — NOT the raw id, which stays
                        // internal for selection only. A voice with no metadata (old manual add) shows just
                        // its name (this row hides).
                        text: voiceRow.metaLine
                        visible: voiceRow.metaLine.length > 0
                        color: Theme.textSecondaryColor; font: Theme.labelFont
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }
                }

                StyledIconButton {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: Theme.scaled(36); implicitHeight: Theme.scaled(36)
                    icon.source: "qrc:/icons/edit.svg"
                    accessibleName: trEdit.text + ": " + voiceRow.voiceName
                    onClicked: root._openEdit(voiceRow.voiceId, voiceRow.voiceName)
                }
                StyledIconButton {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: Theme.scaled(36); implicitHeight: Theme.scaled(36)
                    icon.source: "qrc:/icons/trash.svg"
                    activeColor: Theme.errorColor
                    inactiveColor: Theme.errorColor
                    accessibleName: trDelete.text + ": " + voiceRow.voiceName
                    onClicked: if (root.settings) root.settings.removeElevenlabsVoice(voiceRow.voiceId)
                }
            }
        }
    }

    // "Browse ElevenLabs voices" — the refined picker pop-up that fetches the account's voices (GET
    // /v1/voices via Barista.voice) so the owner selects from a list instead of typing ids. Shown only when
    // the ElevenLabs API key is set (the fetch needs it). It reuses THIS section's select-target: the picker
    // reports voiceChosen(id) and we re-emit our own voiceSelected(id), so the parent routes it to the right
    // active setter (barista → elevenlabsVoiceId, coaching → coachingElevenlabsVoiceId). The manual
    // "+ Add voice" editor below stays as a fallback.
    AccessibleButton {
        visible: !root._editorOpen && !!root.settings
                 && root.settings.elevenlabsApiKey && root.settings.elevenlabsApiKey.length > 0
        Layout.fillWidth: true
        icon.source: "qrc:/icons/search.svg"
        text: TranslationManager.translate("barista.voices.browse", "Browse ElevenLabs voices")
        accessibleName: TranslationManager.translate("barista.voices.browse", "Browse ElevenLabs voices")
        // First browse: load the Loader (onLoaded opens it). Subsequent browses: the item is already alive,
        // so re-open it directly — setting active=true again is a no-op and would NOT re-fire onLoaded.
        onClicked: {
            if (pickerLoader.item)
                pickerLoader.item.open()
            else
                pickerLoader.active = true
        }
    }

    // The picker is loaded on demand (it pulls in QtMultimedia); opened once created, and stays loaded so a
    // second browse is instant. voiceChosen re-emits this section's voiceSelected so the correct active id is set.
    // The voiceChosen.connect lives in onLoaded (fires once) — with item-reuse it must NOT be re-connected, or
    // the handler would fire multiple times per selection.
    Loader {
        id: pickerLoader
        active: false
        source: "qrc:/qml/assistant/ElevenLabsVoicePicker.qml"
        onLoaded: {
            item.activeId = Qt.binding(function() { return root.activeId })
            item.voiceChosen.connect(function(id) { root.voiceSelected(id) })
            item.open()
        }
    }

    // "+ Add voice" button — reveals the inline editor. Hidden while the editor is already open.
    AccessibleButton {
        visible: !root._editorOpen
        Layout.fillWidth: true
        icon.source: "qrc:/icons/plus.svg"
        text: TranslationManager.translate("barista.settings.elAddVoice", "Add voice")
        accessibleName: TranslationManager.translate("barista.settings.elAddVoice", "Add voice")
        onClicked: root._openAdd()
    }

    // Inline add / edit editor: Name + Voice ID + Save/Cancel. Sits inside a distinct inset panel so the
    // fields don't read cramped. Reused for both add (_editingId == "") and edit (prefilled).
    Rectangle {
        visible: root._editorOpen
        Layout.fillWidth: true
        radius: Theme.cardRadius
        color: Theme.backgroundColor
        border.width: 1
        border.color: Theme.borderColor
        implicitHeight: editorCol.implicitHeight + Theme.spacingMedium * 2

        ColumnLayout {
            id: editorCol
            anchors.fill: parent
            anchors.margins: Theme.spacingMedium
            spacing: Theme.spacingSmall

            Text {
                Layout.fillWidth: true
                text: root._editingId.length > 0
                      ? TranslationManager.translate("barista.settings.elEditVoice", "Edit voice")
                      : TranslationManager.translate("barista.settings.elAddVoice", "Add voice")
                color: Theme.textColor; font: Theme.labelFont
                Accessible.ignored: true
            }

            Tr {
                key: "barista.settings.elVoiceName"; fallback: "Name"
                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
            }
            StyledTextField {
                id: elNameField
                Layout.fillWidth: true
                placeholderText: TranslationManager.translate("barista.settings.elVoiceNamePlaceholder", "e.g. Mali")
                accessibleName: TranslationManager.translate("barista.settings.elVoiceName", "Name")
            }

            Tr {
                key: "barista.settings.elVoice"; fallback: "ElevenLabs voice ID"
                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
            }
            StyledTextField {
                id: elIdField
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                placeholderText: TranslationManager.translate("barista.settings.elVoice", "ElevenLabs voice ID")
                accessibleName: TranslationManager.translate("barista.settings.elVoice", "ElevenLabs voice ID")
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingSmall
                spacing: Theme.spacingSmall
                AccessibleButton {
                    subtle: true
                    text: TranslationManager.translate("common.button.cancel", "Cancel")
                    accessibleName: TranslationManager.translate("common.button.cancel", "Cancel")
                    onClicked: root._cancelEditor()
                }
                Item { Layout.fillWidth: true }
                AccessibleButton {
                    primary: true
                    text: TranslationManager.translate("common.button.save", "Save")
                    accessibleName: TranslationManager.translate("common.button.save", "Save")
                    enabled: elIdField.text.trim().length > 0
                    onClicked: root._saveEditor()
                }
            }
        }
    }
}
