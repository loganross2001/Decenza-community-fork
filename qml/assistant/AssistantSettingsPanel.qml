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
    // [barista-fork] Feedback line for the diagnostics "Export log" button (shows the written path).
    property string _diagExportedPath: ""
    readonly property var _diag: (typeof Barista !== "undefined") ? Barista.diagnostics : null
    readonly property var _backup: (typeof Barista !== "undefined") ? Barista.backup : null
    readonly property var _voiceId: (typeof Barista !== "undefined") ? Barista.voiceId : null

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

        // [barista-fork] Build stamp (diagnostic): shows the REAL compiled-in version code so a stale
        // install/QML cache is obvious at a glance — if this number lags the APK you installed, it's stale.
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            text: (typeof AppVersionCode !== "undefined") ? ("build " + AppVersionCode) : ""
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.scaled(11)
            opacity: 0.7
            Accessible.ignored: true
        }

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
                        // A plain Item isn't keyboard-focusable by default, so the FocusIndicator below
                        // (bound to activeFocus) would never show. Opt into Tab-key focus for parity with the
                        // shared StyledTabButton the main Settings tabs use.
                        activeFocusOnTab: true

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

                        // Panel width — how much of the screen the expanded barista reserves on the right
                        // (the machine UI reflows into the remaining left area).
                        Tr {
                            key: "barista.settings.panelWidth"; fallback: "Panel width"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        ComboBox {
                            id: panelWidthBox
                            Layout.fillWidth: true
                            textRole: "label"; valueRole: "value"
                            model: [
                                { value: "narrow", label: TranslationManager.translate("barista.settings.panelWidth.narrow", "Narrow") },
                                { value: "medium", label: TranslationManager.translate("barista.settings.panelWidth.medium", "Medium") },
                                { value: "wide",   label: TranslationManager.translate("barista.settings.panelWidth.wide", "Wide") }
                            ]
                            Accessible.name: TranslationManager.translate("barista.settings.panelWidth", "Panel width")
                            Component.onCompleted: {
                                var i = indexOfValue(root._settings ? root._settings.panelWidthMode : "medium")
                                if (i >= 0) currentIndex = i
                            }
                            onActivated: if (root._settings) root._settings.panelWidthMode = currentValue
                        }
                    }

                    // [barista-fork] Diagnostics — the always-on voice/coaching timeline recorder. Lets the
                    // owner reproduce a glitch and hand back the exported log; stays on the device.
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.diagnostics", "Diagnostics")

                        Tr {
                            Layout.fillWidth: true
                            key: "barista.settings.diagnosticsDesc"
                            fallback: "Records a timeline of the barista's voice, coaching, tools and mic so a glitch can be pinned down. Stays on this device — nothing is sent anywhere."
                            color: Theme.textSecondaryColor; font: Theme.labelFont; wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Switch {
                                id: diagSwitch
                                checked: root._diag ? root._diag.enabled : false
                                onToggled: if (root._diag) root._diag.enabled = checked
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: trDiagOn.text
                                Accessible.checked: checked
                                Accessible.focusable: true
                                Accessible.onToggleAction: toggle()
                            }
                            Tr {
                                id: trDiagOn
                                Layout.fillWidth: true
                                key: "barista.settings.diagnosticsEnabled"
                                fallback: "Record diagnostics"
                                color: Theme.textColor; font: Theme.bodyFont; wrapMode: Text.WordWrap
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root._diag
                                  ? (root._diag.eventCount + " events · " + root._diag.logFilePath)
                                  : ""
                            color: Theme.textSecondaryColor; font: Theme.labelFont; wrapMode: Text.WrapAnywhere
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: root._diagExportedPath.length > 0
                            text: root._diagExportedPath
                            color: Theme.successColor; font: Theme.labelFont; wrapMode: Text.WrapAnywhere
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium
                            AccessibleButton {
                                text: TranslationManager.translate("barista.settings.diagnosticsExport", "Export log")
                                accessibleName: TranslationManager.translate("barista.settings.diagnosticsExport", "Export diagnostic log")
                                onClicked: {
                                    if (!root._diag) return
                                    var p = root._diag.exportSnapshot()
                                    root._diagExportedPath = (p && p.length > 0)
                                        ? TranslationManager.translate("barista.settings.diagnosticsSaved", "Saved to: ") + p
                                        : TranslationManager.translate("barista.settings.diagnosticsSaveFail", "Could not write the log file.")
                                }
                            }
                            AccessibleButton {
                                subtle: true
                                text: TranslationManager.translate("common.button.clear", "Clear")
                                accessibleName: TranslationManager.translate("barista.settings.diagnosticsClear", "Clear diagnostic log")
                                onClicked: { if (root._diag) root._diag.clearLog(); root._diagExportedPath = "" }
                            }
                        }
                    }

                    // [barista-fork] Knowledge-base backup — independent 10-day rolling backup of the private
                    // barista data (assistant.db: tasting notes, reminders, maintenance, dates + settings),
                    // separate from Decent's main backup.
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.kbBackup", "Knowledge-base backup")

                        Tr {
                            Layout.fillWidth: true
                            key: "barista.settings.kbBackupDesc"
                            fallback: "Automatically keeps a 10-day rolling backup of your barista knowledge base — tasting notes, reminders, maintenance history, personal dates — plus your settings. Separate from Decent's main backup, and stays on this device."
                            color: Theme.textSecondaryColor; font: Theme.labelFont; wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Switch {
                                id: kbBackupSwitch
                                checked: root._backup ? root._backup.enabled : false
                                onToggled: if (root._backup) root._backup.enabled = checked
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: trKbBackupOn.text
                                Accessible.checked: checked
                                Accessible.focusable: true
                                Accessible.onToggleAction: toggle()
                            }
                            Tr {
                                id: trKbBackupOn
                                Layout.fillWidth: true
                                key: "barista.settings.kbBackupEnabled"
                                fallback: "Keep an automatic 10-day backup"
                                color: Theme.textColor; font: Theme.bodyFont; wrapMode: Text.WordWrap
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: {
                                if (!root._backup) return ""
                                var last = root._backup.lastBackupAt > 0
                                    ? new Date(root._backup.lastBackupAt * 1000).toLocaleString(Qt.locale(), Locale.ShortFormat)
                                    : TranslationManager.translate("barista.settings.kbBackupNever", "never")
                                return TranslationManager.translate("barista.settings.kbBackupStatus", "%1 backups · last: %2")
                                       .arg(root._backup.backupCount).arg(last)
                            }
                            color: Theme.textSecondaryColor; font: Theme.labelFont; wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root._backup ? root._backup.backupDir : ""
                            color: Theme.textSecondaryColor; font: Theme.labelFont; wrapMode: Text.WrapAnywhere
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: root._backup && root._backup.lastError.length > 0
                            text: root._backup ? (TranslationManager.translate("barista.settings.kbBackupError", "Last backup failed: ") + root._backup.lastError) : ""
                            color: Theme.errorColor; font: Theme.labelFont; wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium
                            AccessibleButton {
                                text: TranslationManager.translate("barista.settings.kbBackupNow", "Back up now")
                                accessibleName: TranslationManager.translate("barista.settings.kbBackupNow", "Back up the knowledge base now")
                                onClicked: if (root._backup) root._backup.backupNow()
                            }
                            AccessibleButton {
                                text: TranslationManager.translate("barista.settings.kbBackupChangeFolder", "Change folder…")
                                accessibleName: TranslationManager.translate("barista.settings.kbBackupChangeFolder", "Change the backup folder")
                                onClicked: kbBackupFolderDialog.open()
                            }
                            AccessibleButton {
                                subtle: true
                                visible: root._backup && !root._backup.isDefaultDir()
                                text: TranslationManager.translate("barista.settings.kbBackupResetFolder", "Reset")
                                accessibleName: TranslationManager.translate("barista.settings.kbBackupResetFolder", "Reset backup folder to default")
                                onClicked: if (root._backup) root._backup.resetBackupDir()
                            }
                        }

                        FolderDialog {
                            id: kbBackupFolderDialog
                            title: TranslationManager.translate("barista.settings.kbBackupPickTitle", "Choose the knowledge-base backup folder")
                            onAccepted: if (root._backup) root._backup.setBackupDir(selectedFolder)
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

                    // ── VOICE MODEL ──
                    // [barista-fork] Own card (was buried under "Saved voices") — the speed↔quality/stutter
                    // tradeoff. Turbo is fastest but stutters more; Multilingual v2 reads steadier; Flash fastest.
                    BaristaSectionCard {
                        visible: root._provider === "elevenlabs"
                        caption: TranslationManager.translate("barista.settings.elModelSection", "Voice model")

                        ComboBox {
                            id: elModelBox
                            Layout.fillWidth: true
                            textRole: "text"; valueRole: "value"
                            model: [
                                { value: "eleven_turbo_v2_5",    text: TranslationManager.translate("barista.settings.elModelTurbo", "Turbo — fastest, more stutter") },
                                { value: "eleven_multilingual_v2", text: TranslationManager.translate("barista.settings.elModelMulti", "Multilingual v2 — steadier, slower") },
                                { value: "eleven_flash_v2_5",    text: TranslationManager.translate("barista.settings.elModelFlash", "Flash — lowest latency") }
                            ]
                            Accessible.name: TranslationManager.translate("barista.settings.elModelSection", "Voice model")
                            Component.onCompleted: {
                                var want = root._settings ? root._settings.elevenlabsModel : "eleven_turbo_v2_5"
                                for (var i = 0; i < model.length; ++i)
                                    if (model[i].value === want) { currentIndex = i; break }
                            }
                            onActivated: {
                                var v = model[currentIndex].value
                                if (root._settings) root._settings.elevenlabsModel = v
                                if (root._voice) root._voice.preview()   // audition the model on the external speaker
                            }
                        }
                    }

                    // ── VOLUME & SPEED ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.sectionVolumeSpeed", "Volume & speed")

                        // Barista voice volume — linear 0..1 gain applied at playback (independent of coaching).
                        Tr {
                            key: "barista.settings.volume"; fallback: "Barista voice volume"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Slider {
                                id: baristaVolumeSlider
                                Layout.fillWidth: true
                                // [barista-fork] Fine step (0.02) + a perceptual curve applied at playback
                                // (effectiveVolume raw^2.5) so low-end steps aren't huge jumps.
                                from: 0.0; to: 1.0; stepSize: 0.02
                                value: root._settings ? root._settings.baristaVoiceVolume : 1.0
                                // [barista-fork] Save the setting AND push it to the live clip so the change is
                                // heard immediately (not just next utterance). onMoved during playback = instant.
                                onMoved: {
                                    if (root._settings) root._settings.baristaVoiceVolume = value
                                    if (root._voice) root._voice.applyLiveVolume()
                                }
                                Accessible.name: TranslationManager.translate("barista.settings.volume", "Barista voice volume")
                            }
                            Text {
                                text: Math.round(baristaVolumeSlider.value * 100) + "%"
                                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                            }
                        }
                        // [barista-fork] Make the Android relationship explicit — the app volume isn't the only knob.
                        Tr {
                            key: "barista.settings.volumeHint"
                            fallback: "This sets the app's level — it combines with the tablet's media volume and your speaker's own volume."
                            color: Theme.textSecondaryColor; font: Theme.captionFont
                            Layout.fillWidth: true; wrapMode: Text.WordWrap; Accessible.ignored: true
                        }

                        // Barista voice speed — rate multiplier (independent of coaching).
                        Tr {
                            key: "barista.settings.speed"; fallback: "Barista voice speed"
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
                                Accessible.name: TranslationManager.translate("barista.settings.speed", "Barista voice speed")
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

                        // [barista-fork] Thinking sound — a subtle earcon that loops while the barista is
                        // working, so the pause after you stop talking isn't dead silence. Auditioned on change.
                        Tr {
                            key: "barista.settings.thinkingSound"; fallback: "Thinking sound"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        ComboBox {
                            id: thinkingBox
                            Layout.fillWidth: true
                            // model rows carry the stored value + a translated label.
                            textRole: "text"; valueRole: "value"
                            model: [
                                { value: "off",    text: TranslationManager.translate("barista.settings.thinkingOff",    "Off") },
                                { value: "hum",    text: TranslationManager.translate("barista.settings.thinkingHum",    "Soft hum") },
                                { value: "breath", text: TranslationManager.translate("barista.settings.thinkingBreath", "Breath") },
                                { value: "pulse",  text: TranslationManager.translate("barista.settings.thinkingPulse",  "Gentle pulse") },
                                { value: "drone",  text: TranslationManager.translate("barista.settings.thinkingDrone",  "Warm drone") }
                            ]
                            Accessible.name: TranslationManager.translate("barista.settings.thinkingSound", "Thinking sound")
                            Component.onCompleted: {
                                var want = root._settings ? root._settings.thinkingSound : "hum"
                                for (var i = 0; i < model.length; ++i)
                                    if (model[i].value === want) { currentIndex = i; break }
                            }
                            onActivated: {
                                var v = model[currentIndex].value
                                if (root._settings) root._settings.thinkingSound = v
                                if (v !== "off" && root._voice && typeof root._voice.previewThinkingSound === "function")
                                    root._voice.previewThinkingSound()   // audition on the external speaker
                            }
                        }

                        // [barista-fork] Pause button behavior — flexibility in the voice-only UX.
                        Tr {
                            key: "barista.settings.pauseMode"; fallback: "Pause button"
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }
                        ComboBox {
                            id: pauseModeBox
                            Layout.fillWidth: true
                            textRole: "text"; valueRole: "value"
                            model: [
                                { value: "hold",   text: TranslationManager.translate("barista.settings.pauseHold",   "Hold — just pauses the mic, barista stays ready") },
                                { value: "freeze", text: TranslationManager.translate("barista.settings.pauseFreeze", "Freeze — stops everything until you resume") }
                            ]
                            Accessible.name: TranslationManager.translate("barista.settings.pauseMode", "Pause button")
                            Component.onCompleted: {
                                var want = root._settings ? root._settings.pauseMode : "hold"
                                for (var i = 0; i < model.length; ++i)
                                    if (model[i].value === want) { currentIndex = i; break }
                            }
                            onActivated: if (root._settings) root._settings.pauseMode = model[currentIndex].value
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

                    // ── VOICE RECOGNITION (experimental, Increment 1: enroll + capture test) ──
                    // [barista-fork] On-device speaker enrollment. Records ~10s of the ACTIVE user's voice and
                    // stores a voiceprint (kept in voiceprints.db on THIS device only — never backed up).
                    BaristaSectionCard {
                        visible: root._voiceId !== null
                        caption: TranslationManager.translate("barista.settings.voiceIdSection", "Voice recognition (experimental)")

                        Tr {
                            key: "barista.settings.voiceIdHint"
                            fallback: "Record your voice so I can tell who's talking. Say \"I'm <your name>\" first, then enroll. Stays on this device — never backed up."
                            Layout.fillWidth: true; wrapMode: Text.WordWrap
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }

                        // Enroll / recording indicator for the current active user.
                        AccessibleButton {
                            Layout.fillWidth: true
                            primary: !(root._voiceId && root._voiceId.recording)
                            enabled: root._voiceId && !root._voiceId.recording
                                     && root._voiceId.activeUser.length > 0
                            text: (root._voiceId && root._voiceId.activeUser.length > 0)
                                  ? TranslationManager.translate("barista.settings.voiceIdEnroll", "Enroll %1's voice").arg(root._voiceId.activeUser)
                                  : TranslationManager.translate("barista.settings.voiceIdNoUser", "Say who you are first, then enroll")
                            accessibleName: text
                            onClicked: if (root._voiceId) root._voiceId.enrollActiveUser()
                        }
                        AccessibleButton {
                            Layout.fillWidth: true
                            visible: root._voiceId && root._voiceId.recording
                            subtle: true
                            text: TranslationManager.translate("barista.settings.voiceIdCancel", "Stop recording")
                            accessibleName: text
                            onClicked: if (root._voiceId) root._voiceId.cancelEnroll()
                        }
                        // Status line (recording / saved / errors).
                        Text {
                            Layout.fillWidth: true; wrapMode: Text.WordWrap
                            visible: root._voiceId && root._voiceId.status.length > 0
                            text: root._voiceId ? root._voiceId.status : ""
                            color: (root._voiceId && root._voiceId.recording) ? Theme.primaryColor : Theme.textSecondaryColor
                            font: Theme.labelFont; Accessible.ignored: true
                        }

                        // Enrolled people, each deletable (per-person, opt-in — the definition of done for storage).
                        Repeater {
                            model: root._voiceId ? root._voiceId.enrolledNames : []
                            delegate: RowLayout {
                                required property string modelData
                                Layout.fillWidth: true
                                spacing: Theme.spacingSmall
                                Text {
                                    Layout.fillWidth: true
                                    text: parent.modelData
                                    color: Theme.textColor; font: Theme.bodyFont; Accessible.ignored: true
                                }
                                AccessibleButton {
                                    subtle: true
                                    text: TranslationManager.translate("common.button.delete", "Delete")
                                    accessibleName: TranslationManager.translate("barista.settings.voiceIdDelete", "Delete %1's voiceprint").arg(parent.modelData)
                                    onClicked: if (root._voiceId) root._voiceId.deleteVoiceprint(parent.modelData)
                                }
                            }
                        }

                        // [barista-fork] Increment 2 — recognize the speaker from their voice and set the active
                        // user automatically. Only meaningful once someone is enrolled (guarded in C++ too).
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Switch {
                                id: voiceIdEnabledSwitch
                                enabled: root._voiceId && root._voiceId.enrolledNames.length > 0
                                checked: root._settings ? root._settings.voiceIdEnabled : false
                                onToggled: if (root._settings) root._settings.voiceIdEnabled = checked
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: trVoiceIdEnabled.text
                                Accessible.checked: checked
                                Accessible.focusable: true
                                Accessible.onToggleAction: toggle()
                            }
                            Tr {
                                id: trVoiceIdEnabled
                                key: "barista.settings.voiceIdEnabled"
                                fallback: "Recognize who's speaking (needs an enrolled voice)"
                                Layout.fillWidth: true; wrapMode: Text.WordWrap
                                color: Theme.textColor; font: Theme.bodyFont
                            }
                        }

                        // [barista-fork] Match tuning — MFCC cosine bands can't be predicted up front, so the
                        // owner tunes them live from the `match` logs. Lower = more eager to switch; higher = more
                        // cautious. Only relevant once recognition is on.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            visible: root._settings && root._settings.voiceIdEnabled

                            Tr {
                                key: "barista.settings.voiceIdTuneHint"
                                fallback: "Tuning: lower = switches more eagerly, higher = more cautious."
                                Layout.fillWidth: true; wrapMode: Text.WordWrap
                                color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                            }

                            // Confident-match threshold.
                            Tr { key: "barista.settings.voiceIdConfidence"; fallback: "Confident match"
                                 color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
                            RowLayout {
                                Layout.fillWidth: true; spacing: Theme.spacingSmall
                                Slider {
                                    id: vidConfSlider
                                    Layout.fillWidth: true
                                    from: 0.5; to: 0.95; stepSize: 0.01
                                    value: root._settings ? root._settings.voiceIdConfidence : 0.72
                                    onMoved: if (root._settings) root._settings.voiceIdConfidence = value
                                    Accessible.name: TranslationManager.translate("barista.settings.voiceIdConfidence", "Confident match")
                                }
                                Text { text: vidConfSlider.value.toFixed(2); color: Theme.textSecondaryColor
                                       font: Theme.labelFont; Accessible.ignored: true }
                            }

                            // Confidence margin (best must beat 2nd-best by this).
                            Tr { key: "barista.settings.voiceIdMargin"; fallback: "Confidence margin"
                                 color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
                            RowLayout {
                                Layout.fillWidth: true; spacing: Theme.spacingSmall
                                Slider {
                                    id: vidMarginSlider
                                    Layout.fillWidth: true
                                    from: 0.0; to: 0.3; stepSize: 0.01
                                    value: root._settings ? root._settings.voiceIdMargin : 0.06
                                    onMoved: if (root._settings) root._settings.voiceIdMargin = value
                                    Accessible.name: TranslationManager.translate("barista.settings.voiceIdMargin", "Confidence margin")
                                }
                                Text { text: vidMarginSlider.value.toFixed(2); color: Theme.textSecondaryColor
                                       font: Theme.labelFont; Accessible.ignored: true }
                            }

                            // "Maybe, confirm" threshold.
                            Tr { key: "barista.settings.voiceIdMaybe"; fallback: "Maybe threshold"
                                 color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true }
                            RowLayout {
                                Layout.fillWidth: true; spacing: Theme.spacingSmall
                                Slider {
                                    id: vidMaybeSlider
                                    Layout.fillWidth: true
                                    from: 0.4; to: 0.9; stepSize: 0.01
                                    value: root._settings ? root._settings.voiceIdMaybe : 0.55
                                    onMoved: if (root._settings) root._settings.voiceIdMaybe = value
                                    Accessible.name: TranslationManager.translate("barista.settings.voiceIdMaybe", "Maybe threshold")
                                }
                                Text { text: vidMaybeSlider.value.toFixed(2); color: Theme.textSecondaryColor
                                       font: Theme.labelFont; Accessible.ignored: true }
                            }
                        }

                        // Opt-in concurrent-capture test (Increment-1 validation aid; off by default).
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Switch {
                                id: voiceIdProbeSwitch
                                checked: root._settings ? root._settings.voiceIdProbe : false
                                onToggled: if (root._settings) root._settings.voiceIdProbe = checked
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: trVoiceIdProbe.text
                                Accessible.checked: checked
                                Accessible.focusable: true
                                Accessible.onToggleAction: toggle()
                            }
                            Tr {
                                id: trVoiceIdProbe
                                key: "barista.settings.voiceIdProbe"; fallback: "Voice ID capture test (logs only)"
                                Layout.fillWidth: true
                                color: Theme.textColor; font: Theme.bodyFont
                                Accessible.ignored: true
                            }
                        }

                        // [barista-fork] V1 validator — a ~2s on-demand match test (no STT running here, so no
                        // mic contention). Logs the score/margin/decision + shows it above. For tuning before the
                        // engage-hail UX is built.
                        AccessibleButton {
                            subtle: true
                            visible: root._voiceId && root._voiceId.enrolledNames.length > 0
                            enabled: root._voiceId && !root._voiceId.recording
                            text: TranslationManager.translate("barista.settings.voiceIdTestShort", "Test short ID (2s)")
                            accessibleName: TranslationManager.translate("barista.settings.voiceIdTestShort", "Test short ID (2s)")
                            onClicked: if (root._voiceId) root._voiceId.testShortIdentify()
                        }
                        Tr {
                            key: "barista.settings.voiceIdTestShortHint"
                            fallback: "Records ~2s and logs the match score — for tuning."
                            visible: root._voiceId && root._voiceId.enrolledNames.length > 0
                            Layout.fillWidth: true; wrapMode: Text.WordWrap
                            color: Theme.textSecondaryColor; font: Theme.labelFont; Accessible.ignored: true
                        }

                        // [barista-fork] V2 validator — the anti-probe test: fire a short capture at ENGAGE, then
                        // open the STT mic (never concurrent). Speak a full sentence and check the transcript is
                        // complete. Off by default (adds a ~2.5s delay at engage when on).
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Switch {
                                id: voiceIdEngageTestSwitch
                                checked: root._settings ? root._settings.voiceIdEngageTest : false
                                onToggled: if (root._settings) root._settings.voiceIdEngageTest = checked
                                Accessible.role: Accessible.CheckBox
                                Accessible.name: trVoiceIdEngageTest.text
                                Accessible.checked: checked
                                Accessible.focusable: true
                                Accessible.onToggleAction: toggle()
                            }
                            Tr {
                                id: trVoiceIdEngageTest
                                key: "barista.settings.voiceIdEngageTest"; fallback: "Engage capture test (delays mic ~2.5s)"
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

                    // ── LIVE COACHING (spoken cues during the pull + steam, using the coaching voice below) ──
                    BaristaSectionCard {
                        caption: TranslationManager.translate("barista.settings.liveCoachingSection", "Live coaching")

                        Tr {
                            key: "barista.settings.liveCoachingHint"
                            fallback: "Spoken cues during your shot and steaming, in the coaching voice below. Default off."
                            color: Theme.textSecondaryColor; font: Theme.labelFont; wrapMode: Text.WordWrap
                            Layout.fillWidth: true; Accessible.ignored: true
                        }
                        // Coach the pull (espresso extraction cues).
                        RowLayout {
                            Layout.fillWidth: true; spacing: Theme.spacingSmall
                            Switch {
                                checked: (typeof Settings !== "undefined") ? Settings.app.espressoCoachAudioEnabled : false
                                onToggled: if (typeof Settings !== "undefined") Settings.app.espressoCoachAudioEnabled = checked
                                Accessible.role: Accessible.CheckBox; Accessible.checked: checked; Accessible.focusable: true
                                Accessible.name: trCoachPull.text; Accessible.onToggleAction: toggle()
                            }
                            Tr { id: trCoachPull; key: "barista.settings.coachPull"; fallback: "Coach the pull"
                                 color: Theme.textColor; font: Theme.bodyFont; Layout.fillWidth: true }
                        }
                        // Coach steaming (same setting the Steam page writes — one source of truth).
                        RowLayout {
                            Layout.fillWidth: true; spacing: Theme.spacingSmall
                            Switch {
                                checked: (typeof Settings !== "undefined") ? Settings.app.steamCoachAudioEnabled : false
                                onToggled: if (typeof Settings !== "undefined") Settings.app.steamCoachAudioEnabled = checked
                                Accessible.role: Accessible.CheckBox; Accessible.checked: checked; Accessible.focusable: true
                                Accessible.name: trCoachSteam.text; Accessible.onToggleAction: toggle()
                            }
                            Tr { id: trCoachSteam; key: "barista.settings.coachSteam"; fallback: "Coach steaming"
                                 color: Theme.textColor; font: Theme.bodyFont; Layout.fillWidth: true }
                        }
                        // Pre-shot game plan (bean-aware, spoken before you pull).
                        RowLayout {
                            Layout.fillWidth: true; spacing: Theme.spacingSmall
                            Switch {
                                checked: (typeof Settings !== "undefined") ? Settings.app.coachGameplanEnabled : false
                                onToggled: if (typeof Settings !== "undefined") Settings.app.coachGameplanEnabled = checked
                                Accessible.role: Accessible.CheckBox; Accessible.checked: checked; Accessible.focusable: true
                                Accessible.name: trCoachPlan.text; Accessible.onToggleAction: toggle()
                            }
                            Tr { id: trCoachPlan; key: "barista.settings.coachGameplan"; fallback: "Pre-shot game plan"
                                 color: Theme.textColor; font: Theme.bodyFont; Layout.fillWidth: true }
                        }
                    }

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
                                from: 0.0; to: 1.0; stepSize: 0.02   // [barista-fork] fine step + perceptual curve (effectiveVolume)
                                value: root._settings ? root._settings.coachingVoiceVolume : 1.0
                                onMoved: {
                                    if (root._settings) root._settings.coachingVoiceVolume = value
                                    if (root._coachingVoice) root._coachingVoice.applyLiveVolume()
                                }
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
