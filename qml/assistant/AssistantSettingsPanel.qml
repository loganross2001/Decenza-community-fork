import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Decenza

// [barista-fork] Tabbed settings for the assistant (General · Voice · Coaching · Maintenance). Shown inside
// the AssistantOverlay's settingsCard. Reads/writes Barista.settings + Barista.voice + Barista.coachingVoice.
//
// STRUCTURE — a custom underline tab ROW (accent-coloured active tab + a ~3px accent underline bar + a 1px
// divider under the whole bar) over a StackLayout of four pages. Each page is its OWN Flickable so a tall
// page scrolls independently on touch; related controls sit inside labelled BaristaSectionCard groups (not a
// flat list). Shared state (barista/coaching PROVIDER + the ElevenLabs key-reveal flag) lives on `root` so
// both tabs can see it — the ElevenLabs API key field is shared and must stay reachable from whichever tab
// currently uses ElevenLabs.
//
// SCROLLING (Job 3 pattern, applied to all four page Flickables): contentHeight = the inner column's real
// implicitHeight; the inner column width bound to the Flickable's OWN width (the viewport, not the panel);
// contentWidth = width + flickableDirection = VerticalFlick (kills horizontal entirely, so a horizontal
// slider drag is on an orthogonal axis and never contends with the vertical page flick); StopAtBounds;
// clip; and a visible, draggable ScrollBar.vertical.
Rectangle {
    id: root
    signal closed()

    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null
    readonly property var _voice: (typeof Barista !== "undefined") ? Barista.voice : null
    // [barista-fork] The SEPARATE coaching voice (live steam + espresso coaches), chosen independently.
    readonly property var _coachingVoice: (typeof Barista !== "undefined") ? Barista.coachingVoice : null

    // [barista-fork] Shared cross-tab state — HOISTED to root. The barista provider lives on the Voice tab
    // and the coaching provider on the Coaching tab (separate Flickable scopes), but the ElevenLabs API key
    // field is SHARED and shown whenever EITHER is on ElevenLabs. Keeping these on root lets the Voice tab's
    // key field see the coaching provider (a coaching-only ElevenLabs user still needs to enter the key).
    readonly property string _provider: root._settings ? root._settings.ttsProvider : "native"
    readonly property string _coachingProvider: root._settings ? root._settings.coachingTtsProvider : "native"
    property bool _elKeyShown: false   // reveal state for the (masked) ElevenLabs API key field

    // Which tab is showing (persisted in this local property only — a settings panel doesn't warrant a
    // stored preference). 0 = General, 1 = Voice, 2 = Coaching, 3 = Maintenance.
    property int _tab: 0

    // Transparent root: this panel is embedded inside the AssistantOverlay's chromed settingsCard, so it
    // must NOT draw its own surface/border (that produced a card-in-card double outline). The host card owns
    // the chrome; the panel fills its host container and lays out the tab bar + scrolling pages on it.
    color: "transparent"
    border.width: 0

    // The tab labels, in order. Drives both the custom tab row and the a11y names.
    readonly property var _tabLabels: [
        TranslationManager.translate("barista.settings.tabGeneral", "General"),
        TranslationManager.translate("barista.settings.tabVoice", "Voice"),
        TranslationManager.translate("barista.settings.tabCoaching", "Coaching"),
        TranslationManager.translate("barista.settings.tabMaintenance", "Maintenance")
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingMedium

        // [barista-fork] No internal title/× header here — the host settingsCard already draws a "Back +
        // Assistant" header, so a second "Assistant" + × stacked on top was redundant and ate vertical space.
        // The host Back button closes the panel; the closed() signal is kept for any external caller.

        // ── Tab row ────────────────────────────────────────────────────────
        // A custom underline tab bar (best-practice active indicator): the active tab is drawn in the accent
        // colour with a ~3px accent underline BAR beneath it; inactive tabs are muted; a 1px divider line
        // runs under the whole row (the underline bar sits on top of it). Built LOCAL to this panel — the
        // shared StyledTabButton draws a merged rounded box (wrong shape for an underline) and is used by the
        // main Settings tabs, so it must not be edited. A11y (PageTab role, focusable, selected-in-name,
        // FocusIndicator) is preserved on each tab item below.
        Item {
            id: tabBar
            Layout.fillWidth: true
            implicitHeight: Theme.scaled(40)

            // 1px divider under the whole bar.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.borderColor
            }

            RowLayout {
                anchors.fill: parent
                spacing: 0
                Repeater {
                    model: root._tabLabels
                    delegate: Item {
                        id: tabItem
                        required property int index
                        required property string modelData
                        readonly property bool active: root._tab === index

                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Text {
                            anchors.centerIn: parent
                            text: tabItem.modelData
                            font.pixelSize: Theme.scaled(14)
                            font.bold: tabItem.active
                            color: tabItem.active ? Theme.primaryColor : Theme.textSecondaryColor
                            Accessible.ignored: true
                        }

                        // ~3px accent underline bar under the active tab, on top of the divider.
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Theme.scaled(3)
                            radius: height / 2
                            color: Theme.primaryColor
                            visible: tabItem.active
                        }

                        Accessible.role: Accessible.PageTab
                        Accessible.name: tabItem.modelData + " "
                            + TranslationManager.translate("common.tab", "tab")
                            + (tabItem.active ? ", " + TranslationManager.translate("common.selected", "selected") : "")
                        Accessible.focusable: true
                        Accessible.onPressAction: root._tab = tabItem.index

                        FocusIndicator {
                            targetItem: tabItem
                            visible: tabItem.activeFocus
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root._tab = tabItem.index
                        }
                    }
                }
            }
        }

        // ── Tab pages ──────────────────────────────────────────────────────
        // Each page is its own Flickable so a tall page scrolls independently on touch. The inner content
        // column binds its width to the Flickable's VIEWPORT (its own width), not the whole panel — so
        // horizontal size is stable and vertical dragging actually flicks. contentWidth: width +
        // VerticalFlick kills horizontal scroll so sliders (a horizontal drag) never contend with the page.
        StackLayout {
            id: pages
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root._tab

            // ═══ GENERAL TAB ═════════════════════════════════════════════════
            // [barista-fork] Barista identity + behaviour that isn't voice-specific, grouped into IDENTITY,
            // BEHAVIOUR, and APPEARANCE cards.
            Flickable {
                id: generalFlick
                contentHeight: generalCol.implicitHeight
                contentWidth: width
                flickableDirection: Flickable.VerticalFlick
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                ColumnLayout {
                    id: generalCol
                    width: generalFlick.width
                    spacing: Theme.spacingMedium

                    // ── IDENTITY ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionIdentity", "Identity")

                        // Name — how the assistant refers to itself.
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

                        // Your name — how the assistant addresses you (used in greetings).
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

                        // [barista-fork] Home location — the default city the fast weather/news tools use when you
                        // ask about "the weather / news around here" without naming a place. Empty = the barista asks.
                        Tr {
                            key: "barista.settings.homeLocation"; fallback: "Home location"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        StyledTextField {
                            id: homeLocationField
                            Layout.fillWidth: true
                            Component.onCompleted: text = root._settings ? root._settings.homeLocation : ""
                            placeholderText: TranslationManager.translate("barista.settings.homeLocationPlaceholder", "e.g. Bellevue — for local weather & news")
                            onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.homeLocation = text }
                        }
                    }

                    // ── BEHAVIOUR ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionBehaviour", "Behaviour")

                        // Proactivity — "full" suggests freely; "greetings" greets + one item; "off" answers only.
                        Tr {
                            key: "barista.settings.proactivity"; fallback: "Proactivity"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        ComboBox {
                            id: proactivityBox
                            Layout.fillWidth: true
                            model: ["full", "greetings", "off"]
                            Accessible.name: TranslationManager.translate("barista.settings.proactivity", "Proactivity")
                            Component.onCompleted: {
                                var i = root._settings ? model.indexOf(root._settings.proactivityLevel) : -1
                                if (i >= 0) currentIndex = i
                            }
                            onActivated: if (root._settings) root._settings.proactivityLevel = currentText
                        }

                        // Web search (Anthropic provider only).
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Switch {
                                id: webSearchSwitch
                                checked: root._settings ? root._settings.webSearchEnabled : true
                                onToggled: if (root._settings) root._settings.webSearchEnabled = checked
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: trWebSearch.text
                                Accessible.checked: checked
                                Accessible.focusable: true
                                Accessible.onToggleAction: toggle()
                            }
                            Tr {
                                id: trWebSearch
                                key: "barista.settings.web"
                                fallback: "Web search — look up beans, roasters & brewing guides (Anthropic)"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true
                            }
                        }
                    }

                    // ── APPEARANCE ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionAppearance", "Appearance")

                        // Show the animated character face.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Switch {
                                id: avatarSwitch
                                checked: root._settings ? root._settings.avatarEnabled : true
                                onToggled: if (root._settings) root._settings.avatarEnabled = checked
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: trAvatarLabel.text
                                Accessible.checked: checked
                                Accessible.focusable: true
                                Accessible.onToggleAction: toggle()
                            }
                            Tr {
                                id: trAvatarLabel
                                key: "barista.settings.avatar"
                                fallback: "Show character — a face to watch while it talks"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true
                            }
                        }

                        // Which character face to show (only meaningful when the character is shown).
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            visible: root._settings ? root._settings.avatarEnabled : true
                            Tr {
                                key: "barista.settings.avatarStyle"; fallback: "Character style"
                                Layout.fillWidth: true
                                color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true
                            }
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
                                Component.onCompleted: {
                                    var i = indexOfValue(root._settings ? root._settings.avatarStyle : "face")
                                    if (i >= 0) currentIndex = i
                                }
                                onActivated: if (root._settings) root._settings.avatarStyle = currentValue
                                Accessible.name: TranslationManager.translate("barista.settings.avatarStyle", "Character style")
                            }
                        }

                        // Edge-tab avatar size — how big the collapsed pull-tab avatar on the screen edge is.
                        Tr {
                            key: "barista.settings.tabSize"; fallback: "Tab size"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        ComboBox {
                            id: tabSizeBox
                            Layout.fillWidth: true
                            textRole: "label"; valueRole: "value"
                            model: [
                                { value: "small",  label: TranslationManager.translate("barista.settings.tabSize.small", "Small") },
                                { value: "medium", label: TranslationManager.translate("barista.settings.tabSize.medium", "Medium") },
                                { value: "large",  label: TranslationManager.translate("barista.settings.tabSize.large", "Large") }
                            ]
                            Accessible.name: TranslationManager.translate("barista.settings.tabSize", "Tab size")
                            Component.onCompleted: {
                                var i = indexOfValue(root._settings ? root._settings.avatarTabSize : "medium")
                                if (i >= 0) currentIndex = i
                            }
                            onActivated: if (root._settings) root._settings.avatarTabSize = currentValue
                        }
                    }
                }
            }

            // ═══ VOICE TAB ═══════════════════════════════════════════════════
            // The barista's conversational voice, grouped into VOICE (provider + key + saved voices),
            // VOLUME & SPEED, and GREETING & OUTPUT cards.
            Flickable {
                id: voiceFlick
                contentHeight: voiceCol.implicitHeight
                contentWidth: width
                flickableDirection: Flickable.VerticalFlick
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                ColumnLayout {
                    id: voiceCol
                    // Bind to the Flickable's own width (the viewport), not the panel.
                    width: voiceFlick.width
                    spacing: Theme.spacingMedium

                    // ── VOICE (provider + per-provider pickers + shared ElevenLabs key) ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionVoice", "Voice")

                        // Voice source: native (free/robotic) · OpenAI · ElevenLabs (human, cloud).
                        Tr {
                            key: "barista.settings.voiceProvider"; fallback: "Provider"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        ComboBox {
                            id: providerBox
                            Layout.fillWidth: true
                            model: ["native", "openai", "elevenlabs"]
                            Accessible.name: TranslationManager.translate("barista.settings.voiceProvider", "Provider")
                            Component.onCompleted: {
                                var i = root._settings ? model.indexOf(root._settings.ttsProvider) : -1
                                if (i >= 0) currentIndex = i
                            }
                            onActivated: if (root._settings) root._settings.ttsProvider = currentText
                        }

                        // Native voice picker (only when provider = native).
                        ComboBox {
                            id: voiceBox
                            Layout.fillWidth: true
                            visible: root._provider === "native"
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

                        // OpenAI voice (only when provider = openai) — reuses your app's OpenAI key.
                        ComboBox {
                            Layout.fillWidth: true
                            visible: root._provider === "openai"
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
                            visible: root._provider === "openai"
                            Component.onCompleted: text = root._settings ? root._settings.openaiApiKey : ""
                            placeholderText: TranslationManager.translate("barista.settings.openaiKey",
                                "OpenAI API key (or leave blank to use Settings → AI)")
                            onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.openaiApiKey = text }
                        }

                        // ElevenLabs (only when EITHER voice uses it): masked API key.
                        // The key is a SECRET — masked by default (password echo) with a Show/Hide toggle, matching
                        // Settings → AI. No eye SVG exists and unicode-glyph icons are disallowed, so the reveal is a
                        // plain text toggle. The key is SHARED by the barista and coaching sections — show it whenever
                        // EITHER is on ElevenLabs (a coaching-only ElevenLabs user still needs it, and it lives here).
                        Tr {
                            visible: root._provider === "elevenlabs" || root._coachingProvider === "elevenlabs"
                            key: "barista.settings.elKeyLabel"; fallback: "ElevenLabs API key"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        RowLayout {
                            visible: root._provider === "elevenlabs" || root._coachingProvider === "elevenlabs"
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            StyledTextField {
                                id: elKeyField
                                Layout.fillWidth: true
                                echoMode: root._elKeyShown ? TextInput.Normal : TextInput.Password
                                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                                Component.onCompleted: text = root._settings ? root._settings.elevenlabsApiKey : ""
                                placeholderText: TranslationManager.translate("barista.settings.elKey", "ElevenLabs API key")
                                accessibleName: TranslationManager.translate("barista.settings.elKeyLabel", "ElevenLabs API key")
                                onEditingFinished: { Qt.inputMethod.commit(); if (root._settings) root._settings.elevenlabsApiKey = text }
                            }
                            AccessibleButton {
                                subtle: true
                                text: root._elKeyShown
                                      ? TranslationManager.translate("common.button.hide", "Hide")
                                      : TranslationManager.translate("common.button.show", "Show")
                                accessibleName: root._elKeyShown
                                      ? TranslationManager.translate("barista.settings.elKeyHide", "Hide API key")
                                      : TranslationManager.translate("barista.settings.elKeyShow", "Show API key")
                                onClicked: root._elKeyShown = !root._elKeyShown
                            }
                        }
                    }

                    // ── SAVED VOICES ──
                    // The reusable manager (select / add / edit / delete + Browse). The active barista selection is
                    // elevenlabsVoiceId; tapping a row sets it via the voiceSelected signal. Own card so it groups.
                    BaristaSectionCard {
                        visible: root._provider === "elevenlabs"
                        caption: TranslationManager.translate("barista.settings.elSavedVoices", "Saved voices")

                        BaristaSavedVoices {
                            Layout.fillWidth: true
                            settings: root._settings
                            activeId: root._settings ? root._settings.elevenlabsVoiceId : ""
                            onVoiceSelected: function(id) { if (root._settings) root._settings.elevenlabsVoiceId = id }
                        }
                    }

                    // ── VOLUME & SPEED ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionVolumeSpeed", "Volume & speed")

                        // Barista voice volume — linear 0..1 gain applied at playback (independent of coaching).
                        Tr {
                            key: "barista.settings.volume"; fallback: "Voice volume"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Slider {
                                id: baristaVolumeSlider
                                Layout.fillWidth: true
                                from: 0.0; to: 1.0; stepSize: 0.05
                                value: root._settings ? root._settings.baristaVoiceVolume : 1.0
                                onMoved: if (root._settings) root._settings.baristaVoiceVolume = value
                                Accessible.name: TranslationManager.translate("barista.settings.volume", "Voice volume")
                            }
                            Text {
                                text: Math.round(baristaVolumeSlider.value * 100) + "%"
                                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                            }
                        }

                        // Barista voice speed — rate multiplier (independent of coaching).
                        Tr {
                            key: "barista.settings.speed"; fallback: "Voice speed"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Slider {
                                id: baristaSpeedSlider
                                Layout.fillWidth: true
                                from: 0.7; to: 1.3; stepSize: 0.05
                                value: root._settings ? root._settings.baristaVoiceSpeed : 1.0
                                onMoved: if (root._settings) root._settings.baristaVoiceSpeed = value
                                Accessible.name: TranslationManager.translate("barista.settings.speed", "Voice speed")
                            }
                            Text {
                                text: baristaSpeedSlider.value.toFixed(2) + "×"
                                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                            }
                        }

                        // Preview the barista's chosen voice (any provider).
                        AccessibleButton {
                            subtle: true
                            text: TranslationManager.translate("barista.settings.preview", "Preview voice")
                            accessibleName: TranslationManager.translate("barista.settings.preview", "Preview voice")
                            onClicked: if (root._voice) root._voice.preview()
                        }
                    }

                    // ── GREETING & OUTPUT (bell + speak toggles) ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionGreeting", "Greeting & output")

                        // Bell — the chime when the assistant greets you.
                        Tr {
                            key: "barista.settings.bell"; fallback: "Bell"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        ComboBox {
                            id: bellBox
                            Layout.fillWidth: true
                            // "custom" plays your own sound file (chosen just below); "off" silences the chime.
                            model: ["poof", "ding", "off", "custom"]
                            Accessible.name: TranslationManager.translate("barista.settings.bell", "Bell")
                            Component.onCompleted: {
                                var i = root._settings ? model.indexOf(root._settings.bellSound) : -1
                                if (i >= 0) currentIndex = i
                            }
                            onActivated: {
                                if (root._settings) root._settings.bellSound = currentText
                                if (currentText === "custom") {
                                    // First time on "custom" with no file yet → prompt for one; otherwise audition it.
                                    if (root._settings && root._settings.bellCustomPath.length === 0) bellFileDialog.open()
                                    else if (root._voice) root._voice.previewBell("custom")
                                } else if (root._voice) {
                                    root._voice.previewBell(currentText)   // audition the choice
                                }
                            }
                        }

                        // Custom-sound chooser — only shown for "custom". Plays your own .wav/.mp3 from the tablet.
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
                                Accessible.ignored: true
                                text: {
                                    var p = (root._settings && root._settings.bellCustomPath) ? String(root._settings.bellCustomPath) : ""
                                    if (p.length === 0) return TranslationManager.translate("barista.settings.bellNoFile", "No file chosen")
                                    return decodeURIComponent(p.substring(p.lastIndexOf("/") + 1))
                                }
                            }
                        }

                        // Mute / speak toggle.
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

                        // Speak the opening greeting aloud (default off — otherwise the barista is present but quiet
                        // at the start and only speaks its replies). Only meaningful when "Speak out loud" is on.
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
                    }
                }
            }

            // ═══ COACHING TAB ════════════════════════════════════════════════
            // [barista-fork] A SEPARATE voice for the live steam + espresso coaches, picked independently of
            // the barista's conversational voice. Reuses the same per-provider controls; the ElevenLabs API
            // key + the underlying saved-voices list are SHARED with the Voice tab (not duplicated here).
            Flickable {
                id: coachingFlick
                contentHeight: coachingCol.implicitHeight
                contentWidth: width
                flickableDirection: Flickable.VerticalFlick
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                ColumnLayout {
                    id: coachingCol
                    width: coachingFlick.width
                    spacing: Theme.spacingMedium

                    // ── COACHING VOICE (provider + per-provider pickers + shared saved voices) ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.coachingVoiceSection", "Coaching voice")

                        Tr {
                            key: "barista.settings.coachingVoiceHint"
                            fallback: "The voice for live steam and espresso coaching (separate from the assistant's voice)."
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textSecondaryColor; font: Theme.bodyFont; Accessible.ignored: true
                        }

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

                        // Native coaching voice picker (only when provider = native).
                        ComboBox {
                            id: coachingVoiceBox
                            Layout.fillWidth: true
                            visible: root._coachingProvider === "native"
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

                        // OpenAI coaching voice (only when provider = openai) — reuses the shared OpenAI key.
                        ComboBox {
                            Layout.fillWidth: true
                            visible: root._coachingProvider === "openai"
                            model: ["nova", "shimmer", "alloy", "echo", "fable", "onyx"]
                            Accessible.name: TranslationManager.translate("barista.settings.openaiVoice", "OpenAI voice")
                            Component.onCompleted: {
                                var i = root._settings ? model.indexOf(root._settings.coachingOpenaiVoice) : -1
                                if (i >= 0) currentIndex = i
                            }
                            onActivated: if (root._settings) root._settings.coachingOpenaiVoice = currentText
                        }

                        // ElevenLabs coaching voice (only when provider = elevenlabs): the SAME reusable saved-voices
                        // manager as the Voice tab, bound to the coaching selection. The API key + add/edit/delete are
                        // shared (one underlying list); tapping a row sets coachingElevenlabsVoiceId via voiceSelected.
                        Tr {
                            visible: root._coachingProvider === "elevenlabs"
                            key: "barista.settings.elSavedVoices"; fallback: "Saved voices"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        BaristaSavedVoices {
                            visible: root._coachingProvider === "elevenlabs"
                            Layout.fillWidth: true
                            settings: root._settings
                            activeId: root._settings ? root._settings.coachingElevenlabsVoiceId : ""
                            onVoiceSelected: function(id) { if (root._settings) root._settings.coachingElevenlabsVoiceId = id }
                        }

                        // Reminder that the ElevenLabs key lives on the Voice tab — shown only when coaching uses
                        // ElevenLabs but the barista does NOT (otherwise the key field is right there on Voice).
                        Tr {
                            visible: root._coachingProvider === "elevenlabs" && root._provider !== "elevenlabs"
                            key: "barista.settings.coachingElKeyHint"
                            fallback: "The ElevenLabs API key is shared — set it on the Voice tab."
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                    }

                    // ── VOLUME & SPEED ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionVolumeSpeed", "Volume & speed")

                        // Coaching voice volume — independent of the barista voice's volume.
                        Tr {
                            key: "barista.settings.coachingVolume"; fallback: "Coaching volume"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Slider {
                                id: coachingVolumeSlider
                                Layout.fillWidth: true
                                from: 0.0; to: 1.0; stepSize: 0.05
                                value: root._settings ? root._settings.coachingVoiceVolume : 1.0
                                onMoved: if (root._settings) root._settings.coachingVoiceVolume = value
                                Accessible.name: TranslationManager.translate("barista.settings.coachingVolume", "Coaching volume")
                            }
                            Text {
                                text: Math.round(coachingVolumeSlider.value * 100) + "%"
                                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                            }
                        }

                        // Coaching voice speed — independent of the barista voice's speed.
                        Tr {
                            key: "barista.settings.coachingSpeed"; fallback: "Coaching speed"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Slider {
                                id: coachingSpeedSlider
                                Layout.fillWidth: true
                                from: 0.7; to: 1.3; stepSize: 0.05
                                value: root._settings ? root._settings.coachingVoiceSpeed : 1.0
                                onMoved: if (root._settings) root._settings.coachingVoiceSpeed = value
                                Accessible.name: TranslationManager.translate("barista.settings.coachingSpeed", "Coaching speed")
                            }
                            Text {
                                text: coachingSpeedSlider.value.toFixed(2) + "×"
                                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                            }
                        }

                        AccessibleButton {
                            subtle: true
                            text: TranslationManager.translate("barista.settings.previewCoaching", "Preview coaching voice")
                            accessibleName: TranslationManager.translate("barista.settings.previewCoaching", "Preview coaching voice")
                            onClicked: if (root._coachingVoice) root._coachingVoice.preview()
                        }
                    }
                }
            }

            // ═══ MAINTENANCE TAB ═════════════════════════════════════════════
            // [barista-fork] Entry point to the maintenance schedule editor (intervals / enable / mark done,
            // plus the Decent-docs-check toggle/last-checked/"Check now" — those live INSIDE the dialog). A
            // provably-reachable path: conversation-card gear → this panel → Maintenance tab → this button →
            // MaintenanceSettingsDialog.
            Flickable {
                id: maintFlick
                contentHeight: maintCol.implicitHeight
                contentWidth: width
                flickableDirection: Flickable.VerticalFlick
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                ColumnLayout {
                    id: maintCol
                    width: maintFlick.width
                    spacing: Theme.spacingMedium

                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.maintenanceSection", "Maintenance & reminders")

                        Tr {
                            key: "barista.settings.maintenanceHint"
                            fallback: "Editable default schedule for the DE1 — confirm intervals against Decent's published schedule."
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textSecondaryColor; font: Theme.bodyFont; Accessible.ignored: true
                        }
                        AccessibleButton {
                            Layout.fillWidth: true
                            primary: true
                            text: TranslationManager.translate("barista.settings.maintenance", "Maintenance & reminders…")
                            accessibleName: TranslationManager.translate("barista.settings.maintenance", "Maintenance & reminders")
                            onClicked: maintenanceDialog.open()
                        }
                    }
                }
            }
        }
    }

    // The maintenance schedule editor. Modal, parented to the app Overlay (so it centres on screen, not
    // inside the scrolled panel). Reachable only via the Maintenance tab's button above.
    MaintenanceSettingsDialog {
        id: maintenanceDialog
    }

    // Custom-bell file chooser (Voice tab). Writes bellCustomPath + auditions the pick — the only place in
    // the app that sets a custom bell sound now that the main-menu barista section is reduced to on/off.
    FileDialog {
        id: bellFileDialog
        title: TranslationManager.translate("barista.settings.bellChoose", "Choose sound file…")
        nameFilters: ["Sound files (*.wav *.mp3)", "All files (*)"]
        onAccepted: {
            if (root._settings) root._settings.bellCustomPath = String(selectedFile)
            if (root._voice) root._voice.previewBell("custom")
        }
    }
}
