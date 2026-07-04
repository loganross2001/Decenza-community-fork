import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import Decenza

// [barista-fork] The proactive barista assistant — a REAL conversation, not a script. When the
// orchestrator activates (Espresso selected → "greeting"; a shot finished → "closeOut") this seeds
// a multi-turn Claude conversation (MainController.aiManager.conversation.ask/followUp) with the
// user's name, bean and best recipe as context, then shows + speaks Claude's replies and sends the
// user's typed replies back. What it says is AI-generated and adapts to the user. Idle-page only.
Item {
    id: root
    anchors.fill: parent

    readonly property var _orch: (typeof Barista !== "undefined") ? Barista.orchestrator : null
    readonly property var _voice: (typeof Barista !== "undefined") ? Barista.voice : null
    readonly property var _voiceInput: (typeof Barista !== "undefined") ? Barista.voiceInput : null
    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null

    // Voice-input session: 20s of silence auto-closes the mic (resets on any speech / reply / activity).
    Timer {
        id: silenceTimer
        interval: 20000
        repeat: false
        onTriggered: if (root._voiceInput) root._voiceInput.stop()
    }
    function _resetSilence() {
        if (root._voiceInput && root._voiceInput.listening) silenceTimer.restart()
        else silenceTimer.stop()
    }
    Connections {
        target: root._voiceInput
        ignoreUnknownSignals: true
        function onFinalText(text) { root._resetSilence(); root._send(text) }   // spoken utterance → the AI
        function onPartialChanged() { root._resetSilence() }
        function onListeningChanged() { root._resetSilence() }
        function onError(message) {   // never fail silently — say what happened
            root._message = TranslationManager.translate("barista.mic.error",
                "I couldn't hear you clearly (%1). Tap Chat to try again, or just type.").arg(message)
        }
    }
    // Mute the mic while the assistant is speaking (no echo), resume when it finishes.
    Connections {
        target: root._voice
        ignoreUnknownSignals: true
        function onSpeakingChanged() {
            if (!root._voiceInput || !root._voiceInput.listening) return
            if (root._voice.speaking) root._voiceInput.pauseMic()
            else { root._voiceInput.resumeMic(); root._resetSilence() }
        }
    }
    readonly property string _state: _orch ? _orch.state : "dormant"
    readonly property var _conv: (typeof MainController !== "undefined" && MainController.aiManager)
                                 ? MainController.aiManager.conversation : null

    property string _message: ""       // the assistant's latest line
    property bool _thinking: false
    property bool _showSettings: false
    property bool _awaitingContext: false   // waiting on requestRecentShotContext before ask()
    property bool _closeOutRated: false      // persist the close-out taste to the shot exactly once

    // Who the assistant is talking to: the user's chosen name, else the active barista.
    readonly property string _userName: {
        if (_settings && _settings.userName && _settings.userName.length > 0) return _settings.userName
        if (Settings.dye.dyeBarista && Settings.dye.dyeBarista.length > 0) return Settings.dye.dyeBarista
        return ""
    }
    readonly property string _bean: {
        var s = ((Settings.dye.dyeBeanBrand || "") + " " + (Settings.dye.dyeBeanType || "")).trim()
        return s
    }
    function _partOfDay() {
        var h = new Date().getHours()
        return h < 12 ? "morning" : (h < 18 ? "afternoon" : "evening")
    }

    // Kick off a conversation. First pull the user's REAL dial-in history for this bean so the
    // assistant KNOWS it (and can suggest), instead of asking. ask() fires once the history arrives.
    function _startConversation() {
        if (!root._conv) {
            root._message = TranslationManager.translate("barista.noai",
                "Add an AI key in Settings → AI and I'll be able to chat.")
            return
        }
        root._message = ""
        root._thinking = true
        root._awaitingContext = true
        root._closeOutRated = false
        // (1) Load THIS bean's persisted conversation so the AI recalls its own prior guidance and we
        // pick up where we left off — even after a 2-month gap. (2) Pull the bean's FULL shot history.
        if (typeof MainController !== "undefined" && MainController.aiManager) {
            MainController.aiManager.switchConversation(Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType,
                (typeof ProfileManager !== "undefined") ? ProfileManager.currentProfileName : "")
        }
        if (typeof MainController !== "undefined" && MainController.shotHistory
                && (Settings.dye.dyeBeanBrand.length > 0 || Settings.dye.dyeBeanType.length > 0)) {
            MainController.shotHistory.requestShotsFiltered(
                { "beanBrand": Settings.dye.dyeBeanBrand, "beanType": Settings.dye.dyeBeanType }, 0, 50)
        } else if (typeof MainController !== "undefined" && MainController.shotHistory) {
            MainController.shotHistory.requestShotsFiltered({}, 0, 15)   // no bean set → recent overall
        } else {
            root._askWithContext("")
        }
    }

    // Turn recent shots into a compact history the AI can reason over.
    function _buildHistory(results) {
        if (!results || results.length === 0)
            return ""
        var lines = []
        for (var i = 0; i < results.length && i < 10; i++) {
            var s = results[i]
            var bean = (((s.beanBrand || "") + " " + (s.beanType || "")).trim()) || "?"
            var detail = []
            if (s.grinderSetting && String(s.grinderSetting).length > 0) detail.push("grind " + s.grinderSetting)
            if (Number(s.doseWeightG) > 0) detail.push("dose " + Number(s.doseWeightG).toFixed(1) + "g")
            if (Number(s.durationSec) > 0) detail.push(Math.round(s.durationSec) + "s")
            if (Number(s.enjoyment0to100) > 0) detail.push("rated " + s.enjoyment0to100 + "/100")
            if (s.grindIssueDetected) detail.push("grind issue")
            lines.push("- " + (s.dateTime || "") + "  " + bean + ": " + detail.join(", "))
        }
        return lines.join("\n")
    }

    // Open (or resume) the conversation. Persona is a STABLE system prompt; the data goes in the
    // message so followUp() carries fresh history each time while keeping the persisted thread.
    function _askWithContext(history) {
        root._awaitingContext = false
        if (!root._conv)
            return
        var who = root._settings ? root._settings.assistantName : "Coach"
        var name = root._userName.length > 0 ? root._userName : ""
        var bean = root._bean.length > 0 ? root._bean : "their coffee"
        var hist = (history && history.length > 0) ? history : "(no shots recorded yet)"

        var persona = "You are " + who + ", a warm, concise home espresso barista"
            + (name ? " talking to " + name : "") + ". You have LONG-TERM memory of this user's shots and your own "
            + "past advice for each coffee — recall it and pick up where you left off, even after long gaps. Treat any "
            + "shot history you are given as fact; NEVER claim it is their first shot if shots are listed. Keep every "
            + "reply to 1-2 short spoken sentences, conversational, and adapt. Suggest at most one concrete change for "
            + "the next shot when it helps (fast/sour → finer; slow/bitter → coarser)."

        var ctx
        if (root._state === "closeOut") {
            ctx = "I just pulled a shot of " + bean + ". My shots on this coffee (most recent first — dose, grind, time, "
                + "rating):\n" + hist + "\nComment briefly on how it likely went and offer one tweak, or ask one short question."
        } else {
            ctx = "It is the " + root._partOfDay() + " and I am about to pull espresso; my coffee is " + bean
                + ". My shots on this coffee (most recent first — dose, grind, time, rating):\n" + hist
                + "\nGreet me, reference what you see, and suggest one change for this shot."
        }

        // Returning to this bean (a saved thread exists) → continue it so prior advice is recalled;
        // otherwise start a fresh thread with the persona.
        var returning = root._conv.hasSavedConversation
        if (!(returning && root._conv.followUp(ctx)))
            root._conv.ask(persona, ctx)
    }

    function _send(text) {
        var t = (text || "").trim()
        if (t.length === 0 || !root._conv || root._thinking)
            return
        // PERSISTENCE: capture the close-out taste feedback onto the shot record, so it's available to
        // the AI on every future call — never starting over or guessing. (Recent-shot context includes it.)
        if (root._state === "closeOut" && !root._closeOutRated && root._orch && root._orch.lastShotId > 0
                && typeof MainController !== "undefined" && MainController.shotHistory) {
            root._closeOutRated = true
            var low = t.toLowerCase()
            var enj = (low.indexOf("sour") >= 0) ? 45
                    : (low.indexOf("bitter") >= 0) ? 55
                    : (low.indexOf("balanc") >= 0 || low.indexOf("good") >= 0 || low.indexOf("great") >= 0
                       || low.indexOf("perfect") >= 0 || low.indexOf("nice") >= 0 || low.indexOf("love") >= 0) ? 82
                    : 0
            var meta = { "espressoNotes": t }
            if (enj > 0)
                meta["enjoyment0to100"] = enj
            MainController.shotHistory.requestUpdateShotMetadata(root._orch.lastShotId, meta)
        }
        root._thinking = true
        root._conv.followUp(t)
    }

    // Activation → start talking (+ bell on the greeting).
    Connections {
        target: root._orch
        ignoreUnknownSignals: true
        function onStateChanged() {
            if (!root._orch) return
            if (root._orch.state === "greeting") {
                if (root._voice) root._voice.playBell()
                root._startConversation()
            } else if (root._orch.state === "closeOut") {
                root._startConversation()
            } else if (root._orch.state === "dormant") {
                root._showSettings = false
            }
        }
    }
    // Claude's replies → show + speak (once each).
    Connections {
        target: root._conv
        ignoreUnknownSignals: true
        function onResponseReceived(response) {
            root._message = response
            root._thinking = false
            if (root._voice) root._voice.speak(response)
            root._resetSilence()   // keep the mic session alive while we're conversing
        }
    }
    // Recent shots arrived → open the conversation grounded in the real history (so the AI KNOWS,
    // rather than asking). Gated on _awaitingContext so we only consume the request we fired.
    Connections {
        target: (typeof MainController !== "undefined") ? MainController.shotHistory : null
        ignoreUnknownSignals: true
        function onShotsFilteredReady(results, isAppend, totalCount) {
            if (root._awaitingContext)
                root._askWithContext(root._buildHistory(results))
        }
    }

    // ---- Conversation card (centered, idle page only) --------------------------
    Rectangle {
        id: card
        visible: (root._state === "greeting" || root._state === "closeOut") && !root._showSettings
        anchors.centerIn: parent
        width: Math.min(Theme.scaled(520), parent.width - Theme.spacingLarge * 2)
        height: cardCol.implicitHeight + Theme.spacingLarge * 2
        radius: Theme.cardRadius
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor

        Accessible.role: Accessible.StaticText
        Accessible.name: msgText.text

        ColumnLayout {
            id: cardCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spacingLarge
            spacing: Theme.spacingMedium

            // Header: name · gear · dismiss
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.scaled(6)
                Text {
                    text: root._settings ? root._settings.assistantName
                                         : TranslationManager.translate("barista.title", "Coach")
                    Layout.fillWidth: true
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                    Accessible.ignored: true
                }
                Item {
                    implicitWidth: Theme.scaled(32); implicitHeight: Theme.scaled(32)
                    Accessible.role: Accessible.Button
                    Accessible.name: TranslationManager.translate("barista.settings.open", "Assistant settings")
                    Accessible.focusable: true
                    Accessible.onPressAction: root._showSettings = true
                    Image {
                        anchors.centerIn: parent
                        source: "qrc:/icons/settings.svg"
                        width: Theme.scaled(20); height: Theme.scaled(20)
                        fillMode: Image.PreserveAspectFit
                        visible: status === Image.Ready
                        Accessible.ignored: true
                        // settings.svg is a white-stroke icon → tint to the theme colour so it's
                        // visible on the light card surface (same pattern as the coaching-card sparkle).
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            colorization: 1.0
                            colorizationColor: Theme.textColor
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._showSettings = true
                    }
                }
                AccessibleButton {
                    subtle: true
                    text: "×"
                    accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                    onClicked: if (root._orch) root._orch.dismiss()
                }
            }

            // The assistant's line (or a thinking indicator)
            Text {
                id: msgText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textColor
                font: Theme.subtitleFont
                text: (root._thinking && root._message.length === 0)
                      ? TranslationManager.translate("barista.thinking", "…")
                      : root._message
            }

            // Live listening indicator (partial transcription while the mic is open)
            Text {
                Layout.fillWidth: true
                visible: root._voiceInput && root._voiceInput.listening
                wrapMode: Text.WordWrap
                color: Theme.textSecondaryColor
                font: Theme.labelFont
                text: {
                    var p = (root._voiceInput && root._voiceInput.partial) ? root._voiceInput.partial : ""
                    return p.length > 0 ? p : TranslationManager.translate("barista.mic.listening", "Listening…")
                }
                Accessible.ignored: true
            }

            // Reply input — type, or use the Chat mic to talk hands-free
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                // Chat / mic toggle: opens the mic (Chat) or closes it (Stop)
                AccessibleButton {
                    visible: root._voiceInput && root._voiceInput.available
                    primary: root._voiceInput && root._voiceInput.listening
                    subtle: !(root._voiceInput && root._voiceInput.listening)
                    text: (root._voiceInput && root._voiceInput.listening)
                          ? TranslationManager.translate("barista.mic.stop", "Stop")
                          : TranslationManager.translate("barista.mic.chat", "Chat")
                    accessibleName: (root._voiceInput && root._voiceInput.listening)
                          ? TranslationManager.translate("barista.mic.stopAccessible", "Stop listening")
                          : TranslationManager.translate("barista.mic.chatAccessible", "Talk to the assistant")
                    onClicked: {
                        if (!root._voiceInput) return
                        if (root._voiceInput.listening) root._voiceInput.stop()
                        else { root._voiceInput.start(); silenceTimer.restart() }
                    }
                }

                StyledTextField {
                    id: replyField
                    Layout.fillWidth: true
                    enabled: !root._thinking
                    placeholderText: TranslationManager.translate("barista.reply", "Reply…")
                    onAccepted: { root._send(text); text = "" }
                }
                AccessibleButton {
                    subtle: true
                    enabled: !root._thinking
                    text: TranslationManager.translate("barista.chat.send", "Send")
                    accessibleName: TranslationManager.translate("barista.chat.send", "Send")
                    onClicked: { root._send(replyField.text); replyField.text = "" }
                }
            }
        }
    }

    // Settings panel (name / voice / bell / mute), toggled from the card's gear.
    AssistantSettingsPanel {
        visible: root._showSettings && (root._state === "greeting" || root._state === "closeOut")
        anchors.centerIn: parent
        width: Math.min(Theme.scaled(520), parent.width - Theme.spacingLarge * 2)
        onClosed: root._showSettings = false
    }
}
