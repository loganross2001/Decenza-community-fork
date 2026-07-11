import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import QtMultimedia
import Decenza

// [barista-fork] Refined pop-up for browsing the account's ElevenLabs voices and picking one WITHOUT
// typing a cryptic voice id. On open it triggers a fetch (GET /v1/voices via Barista.voice — the voices
// list is account-wide, so the barista instance is always used regardless of which section opened us);
// shows a clean loading state, an error state with Retry, then a searchable/filterable list of voice
// cards. Each card previews the voice (preview_url mp3, one at a time) and selects it with a single tap:
// selecting IMPORTS the voice into the shared saved-voices list (addElevenlabsVoice, so it persists with
// its real name) AND emits voiceChosen(id), which the host (BaristaSavedVoices) re-emits as its own
// voiceSelected(id) so the correct section's active id is set (barista vs coaching). One tap = import +
// activate. "Save all to my list" imports every voice in one tap.
//
// Routing note: this component knows NOTHING about barista-vs-coaching. It reports voiceChosen(id); the
// host maps it to the right setter. The host passes in `activeId` so the currently-selected voice is
// highlighted with a check.
Dialog {
    id: root
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(Theme.scaled(560), parent ? parent.width * 0.95 : Theme.scaled(560))
    height: Math.min(Theme.scaled(680), parent ? parent.height * 0.9 : Theme.scaled(680))
    modal: true
    closePolicy: Dialog.CloseOnEscape | Dialog.CloseOnPressOutside
    padding: 0

    // The active voice id for the SECTION that opened us (bound by the host) — highlights the current pick.
    property string activeId: ""
    // Emitted when the user picks a voice. The host re-emits its own voiceSelected(id) to set the active id.
    signal voiceChosen(string id)

    // Fetch is ALWAYS via the barista instance: the voices list is account-wide, so a single fetch/state
    // serves both the barista and coaching sections.
    readonly property var _voice: (typeof Barista !== "undefined") ? Barista.voice : null
    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null

    // Fetched voices (list of maps: name, id, category, accent, gender, age, useCase, description, previewUrl) + error text.
    property var _voices: []
    property string _errorText: ""
    // Filter state.
    property string _query: ""
    property string _categoryFilter: "all"   // all | premade | cloned | professional
    // Which preview is currently playing (voice id), so the row shows a "playing" state; "" = none.
    property string _playingId: ""

    // Hidden Tr instances for strings used in property bindings / accessible-name concatenation.
    Tr { id: trSelect;  key: "barista.voices.select";  fallback: "Select voice"; visible: false }
    Tr { id: trActive;  key: "barista.voices.active";  fallback: "Selected";     visible: false }
    Tr { id: trSaved;   key: "barista.voices.saved";   fallback: "Saved";        visible: false }
    Tr { id: trPreview; key: "barista.voices.preview"; fallback: "Preview";      visible: false }
    Tr { id: trStop;    key: "barista.voices.stop";    fallback: "Stop preview";  visible: false }

    function _isSaved(id) {
        if (!root._settings)
            return false
        var list = root._settings.elevenlabsVoices
        for (var i = 0; i < list.length; ++i)
            if (list[i]["id"] === id)
                return true
        return false
    }

    function _matchesFilter(v) {
        if (root._categoryFilter !== "all" && (v["category"] || "") !== root._categoryFilter)
            return false
        if (root._query.length === 0)
            return true
        // Case-insensitive substring match across ALL metadata: a voice matches
        // if the query hits ANY of name / category / accent / gender / age /
        // useCase / description. Lets the owner search "narration", "british",
        // "young", etc. — not just the voice's display name.
        var q = root._query.toLowerCase()
        var fields = ["name", "category", "accent", "gender", "age", "useCase", "description"]
        for (var i = 0; i < fields.length; ++i) {
            var val = v[fields[i]]
            if (val && val.toLowerCase().indexOf(q) !== -1)
                return true
        }
        return false
    }

    function _visibleVoices() {
        var out = []
        for (var i = 0; i < root._voices.length; ++i)
            if (root._matchesFilter(root._voices[i]))
                out.push(root._voices[i])
        return out
    }

    // Preview: play the voice's preview_url mp3. Only one plays at a time — stop the previous first.
    function _preview(id, url) {
        previewPlayer.stop()
        if (root._playingId === id || !url || url.length === 0) {
            root._playingId = ""        // tapping the playing row toggles it off
            return
        }
        previewPlayer.source = url
        previewPlayer.play()
        root._playingId = id
    }

    // Build the persisted saved-voice record from a fetched voice: name + id + the searchable metadata
    // (accent / gender / age / useCase / description). The raw id is kept for selection but is never shown.
    function _savedRecord(v) {
        return {
            "name": v["name"] || v["id"],
            "id": v["id"],
            "accent": v["accent"] || "",
            "gender": v["gender"] || "",
            "age": v["age"] || "",
            "useCase": v["useCase"] || "",
            "description": v["description"] || ""
        }
    }

    // Select: import into the shared saved list (persists with the real name + metadata) + tell the host to
    // activate it. Metadata rides along so the saved list can display / search it later.
    function _select(v) {
        if (root._settings)
            root._settings.addElevenlabsVoiceWithMeta(root._savedRecord(v))
        root.voiceChosen(v["id"])
        previewPlayer.stop()
        root._playingId = ""
        root.close()
    }

    function _saveAll() {
        if (!root._settings)
            return
        for (var i = 0; i < root._voices.length; ++i)
            root._settings.addElevenlabsVoiceWithMeta(root._savedRecord(root._voices[i]))
    }

    onOpened: {
        root._voices = []
        root._errorText = ""
        root._query = ""
        searchField.text = ""   // the field keeps its text across reopen; clear it so it matches _query
        root._categoryFilter = "all"
        if (root._voice)
            root._voice.fetchElevenlabsVoices()
    }
    // Stop any preview when the dialog closes so there's no audio leak.
    onClosed: { previewPlayer.stop(); root._playingId = "" }
    Component.onDestruction: previewPlayer.stop()

    // One preview player, shared across rows (one plays at a time).
    MediaPlayer {
        id: previewPlayer
        audioOutput: AudioOutput {}
        onPlaybackStateChanged: {
            if (playbackState === MediaPlayer.StoppedState)
                root._playingId = ""
        }
    }

    Connections {
        target: root._voice
        ignoreUnknownSignals: true
        function onElevenlabsVoicesFetched(voices) { root._voices = voices; root._errorText = "" }
        function onVoicesFetchFailed(reason) { root._errorText = reason }
    }

    background: Rectangle {
        color: Theme.surfaceColor
        radius: Theme.cardRadius
        border.width: 1
        border.color: Theme.borderColor
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        // ── Header ────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            Layout.topMargin: Theme.spacingLarge
            Tr {
                key: "barista.voices.title"; fallback: "Browse ElevenLabs voices"
                Layout.fillWidth: true
                color: Theme.textColor; font: Theme.subtitleFont; Accessible.ignored: true
            }
            AccessibleButton {
                subtle: true
                text: "×"
                accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                onClicked: root.close()
            }
        }

        // ── Search + category filter (only when a list is present) ────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            spacing: Theme.spacingSmall
            visible: root._voices.length > 0 && !root._voice.fetchingVoices

            StyledTextField {
                id: searchField
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                placeholderText: TranslationManager.translate("barista.voices.searchPlaceholder", "Search by name, accent, use case…")
                accessibleName: TranslationManager.translate("barista.voices.search", "Search voices")
                onTextChanged: root._query = text
            }

            // Category filter chips. All / Premade / Cloned / Professional.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall
                Repeater {
                    model: [
                        { key: "all",          labelKey: "barista.voices.catAll",          labelFallback: "All" },
                        { key: "premade",      labelKey: "barista.voices.catPremade",      labelFallback: "Premade" },
                        { key: "cloned",       labelKey: "barista.voices.catCloned",       labelFallback: "Cloned" },
                        { key: "professional", labelKey: "barista.voices.catProfessional", labelFallback: "Professional" }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool selected: root._categoryFilter === modelData.key
                        implicitHeight: Theme.scaled(32)
                        implicitWidth: catLabel.implicitWidth + Theme.spacingMedium * 2
                        radius: height / 2
                        color: selected ? Theme.primaryColor : Theme.backgroundColor
                        border.width: 1
                        border.color: selected ? Theme.primaryColor : Theme.borderColor
                        Text {
                            id: catLabel
                            anchors.centerIn: parent
                            text: TranslationManager.translate(modelData.labelKey, modelData.labelFallback)
                            color: selected ? "white" : Theme.textSecondaryColor
                            font: Theme.labelFont
                            Accessible.ignored: true
                        }
                        AccessibleMouseArea {
                            anchors.fill: parent
                            accessibleName: catLabel.text
                            accessibleChecked: selected
                            onAccessibleClicked: root._categoryFilter = modelData.key
                        }
                    }
                }
            }
        }

        // ── Loading state ─────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingLarge
            visible: root._voice && root._voice.fetchingVoices
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: parent.visible
            }
            Tr {
                Layout.alignment: Qt.AlignHCenter
                key: "barista.voices.loading"; fallback: "Loading your ElevenLabs voices…"
                color: Theme.textSecondaryColor; font: Theme.bodyFont; Accessible.ignored: true
            }
            Item { Layout.fillHeight: true }
        }

        // ── Error state ───────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLarge
            spacing: Theme.spacingMedium
            visible: root._errorText.length > 0 && !(root._voice && root._voice.fetchingVoices)
            Image {
                Layout.alignment: Qt.AlignHCenter
                source: "qrc:/icons/warning.svg"
                sourceSize.width: Theme.scaled(32); sourceSize.height: Theme.scaled(32)
                Accessible.ignored: true
            }
            Text {
                Layout.fillWidth: true
                text: root._errorText
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true
            }
            AccessibleButton {
                Layout.alignment: Qt.AlignHCenter
                primary: true
                text: TranslationManager.translate("common.button.retry", "Retry")
                accessibleName: TranslationManager.translate("common.button.retry", "Retry")
                onClicked: {
                    root._errorText = ""
                    if (root._voice)
                        root._voice.fetchElevenlabsVoices()
                }
            }
        }

        // ── Empty-after-fetch state ───────────────────────────────────────
        Tr {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            visible: root._voices.length > 0 && root._visibleVoices().length === 0
                     && !(root._voice && root._voice.fetchingVoices)
            key: "barista.voices.noMatches"; fallback: "No voices match your search."
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor; font: Theme.bodyFont; Accessible.ignored: true
        }

        // ── Voice list ────────────────────────────────────────────────────
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            visible: root._voices.length > 0 && !(root._voice && root._voice.fetchingVoices)
            clip: true
            spacing: Theme.spacingSmall
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            model: root._visibleVoices()
            // Visible, draggable vertical scrollbar (Job 3 pattern).
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: voiceCard
                required property var modelData
                readonly property string vId: modelData["id"] || ""
                readonly property string vName: (modelData["name"] && modelData["name"].length > 0)
                                                ? modelData["name"] : vId
                readonly property bool isActive: root.activeId === vId
                readonly property bool isSaved: root._isSaved(vId)
                readonly property bool isPlaying: root._playingId === vId

                width: ListView.view ? ListView.view.width : implicitWidth
                implicitHeight: cardInner.implicitHeight + Theme.spacingMedium
                radius: Theme.cardRadius
                color: Theme.backgroundColor
                border.width: isActive ? 2 : 1
                border.color: isActive ? Theme.primaryColor : Theme.borderColor

                // Tap the card body to select (import + activate).
                AccessibleMouseArea {
                    anchors.fill: parent
                    accessibleName: trSelect.text + ": " + voiceCard.vName
                        + (voiceCard.isActive ? " (" + trActive.text + ")" : "")
                    accessibleChecked: voiceCard.isActive
                    onAccessibleClicked: root._select(voiceCard.modelData)
                }

                RowLayout {
                    id: cardInner
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingSmall
                    anchors.topMargin: Theme.spacingSmall
                    anchors.bottomMargin: Theme.spacingSmall
                    spacing: Theme.spacingSmall

                    // Active indicator (filled dot when active, hollow ring otherwise) — mirrors BaristaSavedVoices.
                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: Theme.scaled(16); implicitHeight: Theme.scaled(16)
                        radius: width / 2
                        color: voiceCard.isActive ? Theme.primaryColor : "transparent"
                        border.width: voiceCard.isActive ? 0 : 2
                        border.color: Theme.textSecondaryColor
                        Accessible.ignored: true
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.scaled(2)

                        // Name + "Saved" marker.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Text {
                                Layout.fillWidth: true
                                text: voiceCard.vName
                                color: Theme.textColor; font: Theme.bodyFont
                                elide: Text.ElideRight
                                Accessible.ignored: true
                            }
                            Text {
                                visible: voiceCard.isSaved
                                text: trSaved.text
                                color: Theme.successColor; font: Theme.captionFont
                                Accessible.ignored: true
                            }
                        }

                        // Subtle chips: category + accent + gender + age + use case.
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.scaled(6)
                            Repeater {
                                model: {
                                    var chips = []
                                    if (voiceCard.modelData["category"] && voiceCard.modelData["category"].length > 0)
                                        chips.push({ text: voiceCard.modelData["category"], accent: true })
                                    var labels = ["accent", "gender", "age", "useCase"]
                                    for (var i = 0; i < labels.length; ++i) {
                                        var val = voiceCard.modelData[labels[i]]
                                        if (val && val.length > 0)
                                            chips.push({ text: val, accent: false })
                                    }
                                    return chips
                                }
                                delegate: Rectangle {
                                    required property var modelData
                                    height: Theme.scaled(20)
                                    width: chipText.implicitWidth + Theme.spacingSmall * 2
                                    radius: height / 2
                                    color: "transparent"
                                    border.width: 1
                                    border.color: modelData.accent ? Theme.primaryColor : Theme.borderColor
                                    Text {
                                        id: chipText
                                        anchors.centerIn: parent
                                        text: modelData.text
                                        color: modelData.accent ? Theme.primaryColor : Theme.textSecondaryColor
                                        font: Theme.captionFont
                                        Accessible.ignored: true
                                    }
                                }
                            }
                        }
                    }

                    // Preview button — a play triangle / stop square drawn as a Shape (no font glyph, no new
                    // icon asset). Toggles this voice's preview_url; only one plays at a time.
                    AbstractButton {
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: Theme.scaled(40); implicitHeight: Theme.scaled(40)
                        enabled: (voiceCard.modelData["previewUrl"] || "").length > 0
                        opacity: enabled ? 1.0 : 0.4
                        Accessible.role: Accessible.Button
                        Accessible.name: (voiceCard.isPlaying ? trStop.text : trPreview.text) + ": " + voiceCard.vName
                        Accessible.focusable: true
                        Accessible.onPressAction: clicked()
                        onClicked: root._preview(voiceCard.vId, voiceCard.modelData["previewUrl"] || "")

                        background: Rectangle {
                            radius: width / 2
                            color: voiceCard.isPlaying ? Theme.primaryColor : "transparent"
                            border.width: 1
                            border.color: Theme.primaryColor
                        }
                        contentItem: Item {
                            // Play triangle (▶) drawn as a Shape when idle…
                            Shape {
                                anchors.centerIn: parent
                                visible: !voiceCard.isPlaying
                                width: Theme.scaled(14); height: Theme.scaled(14)
                                ShapePath {
                                    fillColor: Theme.primaryColor
                                    strokeWidth: 0
                                    startX: Theme.scaled(3);  startY: 0
                                    PathLine { x: Theme.scaled(14); y: Theme.scaled(7) }
                                    PathLine { x: Theme.scaled(3);  y: Theme.scaled(14) }
                                    PathLine { x: Theme.scaled(3);  y: 0 }
                                }
                            }
                            // …a stop square while playing.
                            Rectangle {
                                anchors.centerIn: parent
                                visible: voiceCard.isPlaying
                                width: Theme.scaled(10); height: Theme.scaled(10)
                                radius: Theme.scaled(2)
                                color: "white"
                            }
                        }
                    }
                }
            }
        }

        // ── Footer: Save all to my list ───────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            Layout.bottomMargin: Theme.spacingLarge
            visible: root._voices.length > 0 && !(root._voice && root._voice.fetchingVoices)
            Item { Layout.fillWidth: true }
            AccessibleButton {
                subtle: true
                text: TranslationManager.translate("barista.voices.saveAll", "Save all to my list")
                accessibleName: TranslationManager.translate("barista.voices.saveAll", "Save all to my list")
                onClicked: root._saveAll()
            }
        }
    }
}
