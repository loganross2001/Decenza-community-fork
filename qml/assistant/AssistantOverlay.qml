import QtQuick
import QtQuick.Layouts
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
        function onFinalText(text) {   // spoken utterance → the AI; pause the mic until the turn is done
            root._resetSilence()
            if (root._voiceInput) root._voiceInput.pauseMic()
            root._send(text)
        }
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
    property bool _collapsed: false     // panel minimised to a thin edge tab (frees the whole screen)
    property var _pendingNext: null     // structuredNext recommendation awaiting apply/skip
    property bool _awaitConfirm: false  // a recommendation is armed for a voice "OK"
    property var _pendingGrind: null    // outstanding off-machine grind reminder (shown at greeting)
    property bool _awaitingContext: false   // waiting on the shot history before opening the conversation
    property bool _closeOutRated: false      // persist the close-out taste to the shot exactly once
    property bool _fellBack: false           // bean-filtered history was empty → fetched recent overall

    // Strip markdown/code so cloud voices don't read asterisks, hashes, or JSON aloud.
    function _speakSanitised(t) {
        if (!root._voice) return
        var clean = (t || "").replace(/```[\s\S]*?```/g, " ").replace(/[*_#`>]/g, "")
                              .replace(/^\s*[-•]\s+/gm, "").replace(/\s+/g, " ").trim()
        root._voice.speak(clean)
    }
    // Strip the trailing structuredNext fenced block from the DISPLAYED message (keep the prose).
    function _stripBlock(t) {
        return (t || "").replace(/```[\s\S]*?```/g, "").replace(/\n{3,}/g, "\n\n").trim()
    }

    // Apply a confirmed recommendation, then speak a short local confirmation (no model round-trip).
    function _applyPending() {
        if (!root._pendingNext || typeof Barista === "undefined" || !Barista.actions) {
            root._awaitConfirm = false; return
        }
        var res = Barista.actions.applyFromNext(root._pendingNext,
            (root._orch && root._orch.lastShotId) ? root._orch.lastShotId : 0)
        var msg
        if (res.blocked) {
            msg = TranslationManager.translate("barista.act.blocked", "I can't change that while a shot is running.")
        } else {
            var bits = (res.applied || []).concat(res.queued || [])
            msg = bits.length > 0
                ? TranslationManager.translate("barista.act.done", "Done — %1 for the next shot.").arg(bits.join(", "))
                : TranslationManager.translate("barista.act.nothing", "Nothing to change there.")
        }
        root._message = msg
        root._speakSanitised(msg)
        root._pendingNext = null
        root._awaitConfirm = false
    }
    function _skipPending() {
        root._pendingNext = null
        root._awaitConfirm = false
    }
    // Resolve the off-machine grind reminder: done → writes the grind; not done → leaves it.
    function _resolveGrind(done) {
        if (typeof Barista !== "undefined" && Barista.actions) Barista.actions.resolveGrind(done)
        var msg = done ? TranslationManager.translate("barista.act.grindOk", "Great — got it.")
                       : TranslationManager.translate("barista.act.grindNo", "No problem, I'll leave it.")
        root._message = msg
        root._speakSanitised(msg)
        root._pendingGrind = null
    }

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
        root._fellBack = false
        root._pendingNext = null
        root._awaitConfirm = false
        // At a greeting (before a shot), surface any off-machine grind the user agreed to but didn't confirm.
        root._pendingGrind = (root._state === "greeting" && typeof Barista !== "undefined" && Barista.actions)
                             ? Barista.actions.outstandingGrind() : null
        // (1) Load THIS bean's persisted conversation so the AI recalls its own prior guidance (pick up
        // where we left off, even after long gaps). (2) Assemble the FULL advisor-grade dialing context
        // (dial-in sessions, best shot, bean best, grinder context, closed-loop advice) → baristaContextReady.
        if (typeof MainController === "undefined" || !MainController.aiManager) {
            root._askWithContext("")
            return
        }
        var prof = (typeof ProfileManager !== "undefined") ? ProfileManager.currentProfileName : ""
        MainController.aiManager.switchConversation(Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType, prof)
        MainController.aiManager.requestBaristaContext(Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType, prof)
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

    // Open (or resume) the conversation with the data in the SYSTEM PROMPT (re-stamped each session,
    // never trimmed) so the AI ALWAYS has it; the kickoff message carries only intent. beginSession()
    // keeps the persisted thread (prior discussion) and can't wipe it.
    function _askWithContext(dataBlock, unused) {
        root._awaitingContext = false
        if (!root._conv)
            return
        var who = root._settings ? root._settings.assistantName : "Coach"
        var name = root._userName.length > 0 ? root._userName : ""

        var persona = "You are " + who + ", a warm, concise home espresso barista"
            + (name ? " talking to " + name : "") + ".\n"
            + "Speak 1-2 short, conversational sentences — no markdown, lists, or long number sequences in the spoken "
            + "part. The data block below is the app's LIVE DATABASE of this user's shots, dial-in history, best "
            + "recipes, and your own past advice: you DO have full access to it. NEVER say you lack their history, or "
            + "that this is their first shot, unless the block says 'recordedShots: 0'. Reference what you see and "
            + "suggest ONE concrete change for the next shot when it helps (fast/sour → finer; slow/bitter → coarser). "
            + "Recall your past advice and pick up where you left off. Adapt to their replies.\n"
            + "WHEN you recommend a concrete change, append EXACTLY ONE fenced block at the very END, with only the "
            + "field(s) you're changing:\n"
            + "```json\n{\"grinderSetting\":\"4.75\",\"doseG\":18.0,\"targetWeightG\":36.0,\"temperatureC\":92.0,\"expectation\":\"less sour\"}\n```\n"
            + "grinderSetting = grinder dial (off-machine), doseG = grams in, targetWeightG = grams out (yield/ratio), "
            + "temperatureC = brew temp. The app applies it when the user says OK, so give real values. Omit the block "
            + "entirely if you're not changing anything."

        // dataBlock is the pre-formatted advisor-grade context from AIManager.requestBaristaContext().
        var block = (dataBlock && dataBlock.length > 0) ? dataBlock : "recordedShots: 0"

        var kickoff = (root._state === "closeOut")
            ? "I just pulled a shot — how did it go?"
            : "It's the " + root._partOfDay() + " and I'm about to pull espresso. Greet me and suggest one thing."
        // Volatile bit goes in the kickoff (not the cached system prompt): the pending grind reminder.
        if (root._pendingGrind && root._pendingGrind.value)
            kickoff += " (I earlier agreed to set the grinder to " + root._pendingGrind.value
                     + " but haven't confirmed doing it — ask me early whether I actually set it.)"

        root._conv.beginSession(persona + "\n\n" + block, kickoff)
    }

    function _send(text) {
        var t = (text || "").trim()
        if (t.length === 0 || !root._conv || root._thinking)
            return
        var hasActions = (typeof Barista !== "undefined" && Barista.actions)
        // Off-machine grind reminder confirm takes priority (greeting).
        if (hasActions && root._pendingGrind && root._pendingGrind.value) {
            var g = Barista.actions.parseConfirmation(t)   // 1 yes / 0 no / -1 neither
            if (g === 1) { root._resolveGrind(true); return }
            if (g === 0) { root._resolveGrind(false); return }
        }
        // Apply-on-confirm for a pending recommendation ("OK" → apply; "no" → skip).
        if (hasActions && root._awaitConfirm && root._pendingNext) {
            var c = Barista.actions.parseConfirmation(t)
            if (c === 1) { root._applyPending(); return }
            if (c === 0) { root._skipPending(); return }
            root._awaitConfirm = false   // ambiguous → new topic, disarm and pass to the AI
        }
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
            root._collapsed = false   // a new greeting/close-out opens expanded
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
            root._message = root._stripBlock(response)   // hide the JSON action block from the display
            root._thinking = false
            root._speakSanitised(response)               // (also strips fenced blocks before TTS)
            root._resetSilence()   // keep the mic session alive while we're conversing
            // If this turn carried a concrete recommendation, arm apply-on-confirm + show the chip.
            var nx = root._conv ? root._conv.structuredNextForLastAssistantTurnMap() : null
            if (nx && Object.keys(nx).length > 0) {
                root._pendingNext = nx
                root._awaitConfirm = true
            }
            // Turn done. If speaking, speakingChanged(false) reopens the mic; if muted, reopen it now.
            if (root._voiceInput && root._voiceInput.listening
                    && root._settings && !root._settings.voiceEnabled)
                root._voiceInput.resumeMic()
        }
    }
    // The full advisor-grade context arrived → open the conversation grounded in it (so the AI KNOWS,
    // rather than asking). Gated on _awaitingContext so we only consume the request we fired.
    Connections {
        target: (typeof MainController !== "undefined") ? MainController.aiManager : null
        ignoreUnknownSignals: true
        function onBaristaContextReady(dataBlock) {
            if (root._awaitingContext)
                root._askWithContext(dataBlock, false)
        }
    }

    // ---- Conversation card (centered, idle page only) --------------------------
    Rectangle {
        id: card
        visible: (root._state === "greeting" || root._state === "closeOut") && !root._showSettings && !root._collapsed
        // Right-docked side panel: leaves the machine controls usable on the left, gives the
        // conversation room to grow, and is out of the way (recommended tablet-assistant UX).
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spacingMedium
        width: Math.min(Theme.scaled(440), parent.width * 0.42)
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
            anchors.bottom: parent.bottom
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
                AccessibleButton {
                    subtle: true
                    text: "→"   // collapse to a thin edge tab, freeing the whole screen
                    accessibleName: TranslationManager.translate("barista.collapse", "Collapse assistant")
                    onClicked: root._collapsed = true
                }
                AccessibleButton {
                    subtle: true
                    text: "×"
                    accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                    onClicked: if (root._orch) root._orch.dismiss()
                }
            }

            // The assistant's line (or a thinking indicator) — fills the panel so the input pins to the bottom
            Text {
                id: msgText
                Layout.fillWidth: true
                Layout.fillHeight: true
                verticalAlignment: Text.AlignTop
                wrapMode: Text.WordWrap
                color: Theme.textColor
                font: Theme.subtitleFont
                text: (root._thinking && root._message.length === 0)
                      ? TranslationManager.translate("barista.thinking", "…")
                      : root._message
            }

            // Apply-on-confirm chip: appears when a recommendation (or a grind reminder) is pending.
            ActionConfirmChip {
                id: actionChip
                Layout.fillWidth: true
                grindMode: root._pendingGrind && root._pendingGrind.value ? true : false
                grindValue: root._pendingGrind ? (root._pendingGrind.value || "") : ""
                next: root._pendingNext || ({})
                onApplied: actionChip.grindMode ? root._resolveGrind(true) : root._applyPending()
                onSkipped: actionChip.grindMode ? root._resolveGrind(false) : root._skipPending()
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

    // Collapsed state: a thin tab on the right edge. The whole screen is usable; tap to reopen.
    Rectangle {
        id: edgeTab
        visible: (root._state === "greeting" || root._state === "closeOut") && !root._showSettings && root._collapsed
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.scaled(34)
        height: Theme.scaled(96)
        radius: Theme.cardRadius
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor

        Accessible.role: Accessible.Button
        Accessible.name: TranslationManager.translate("barista.expand", "Open assistant")
        Accessible.focusable: true
        Accessible.onPressAction: root._collapsed = false

        Text {
            anchors.centerIn: parent
            text: "←"   // pull the panel back out
            color: Theme.textColor
            font: Theme.subtitleFont
            Accessible.ignored: true
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root._collapsed = false
        }
    }
}
