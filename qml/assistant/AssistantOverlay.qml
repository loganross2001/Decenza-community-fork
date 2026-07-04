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
        // Only count silence while actually HEARING — a long thinking/searching/speaking turn must not
        // trip the 20s auto-off (paused = assistant busy). onPausedChanged restarts it when we reopen.
        if (root._voiceInput && root._voiceInput.listening && !root._voiceInput.paused)
            silenceTimer.restart()
        else
            silenceTimer.stop()
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
        function onPausedChanged() { root._resetSilence() }   // restart the countdown when the mic reopens
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
    property var _pendingBegin: null    // SF-4: a greeting that preempted a still-busy close-out; retry when free
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
            root._awaitConfirm = false; root._pendingNext = null; return
        }
        var anchor = (typeof MainController !== "undefined" && MainController.aiManager)
                     ? MainController.aiManager.lastBaristaAnchorId() : 0
        var res = Barista.actions.applyFromNext(root._pendingNext, anchor > 0 ? anchor : 0)
        var msg
        if (res.blocked) {
            msg = TranslationManager.translate("barista.act.blocked", "I can't change that while a shot is running.")
        } else {
            var bits = (res.applied || []).concat(res.queued || [])
            var rej = res.rejected || []
            if (bits.length > 0 && rej.length === 0)
                msg = TranslationManager.translate("barista.act.done", "Done — %1 for the next shot.").arg(bits.join(", "))
            else if (bits.length > 0)
                msg = TranslationManager.translate("barista.act.donePart", "Done — %1. I skipped %2 (out of range).").arg(bits.join(", ")).arg(rej.join(", "))
            else if (rej.length > 0)
                msg = TranslationManager.translate("barista.act.rej", "That looked off (%1), so I left things as they are.").arg(rej.join(", "))
            else
                msg = TranslationManager.translate("barista.act.nothing", "Nothing to change there.")
        }
        root._message = msg
        root._speakSanitised(msg)
        root._pendingNext = null
        root._awaitConfirm = false
        root._resumeMicAfterLocal()
    }
    function _skipPending() {
        root._pendingNext = null
        root._awaitConfirm = false
        root._resumeMicAfterLocal()
    }
    // S4: after a locally-spoken line, a MUTED engine won't fire speakingChanged to reopen the mic —
    // so reopen it here (mirrors the muted-resume path in onResponseReceived).
    function _resumeMicAfterLocal() {
        if (root._voiceInput && root._voiceInput.listening
                && (!root._voice || !root._voice.speaking))
            root._voiceInput.resumeMic()
    }
    // Resolve the off-machine grind reminder: done → writes the grind; not done → leaves it.
    function _resolveGrind(done) {
        if (typeof Barista !== "undefined" && Barista.actions) Barista.actions.resolveGrind(done)
        var msg = done ? TranslationManager.translate("barista.act.grindOk", "Great — got it.")
                       : TranslationManager.translate("barista.act.grindNo", "No problem, I'll leave it.")
        root._message = msg
        root._speakSanitised(msg)
        root._pendingGrind = null
        root._resumeMicAfterLocal()
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
    // Stamp the assistant's upcoming turn with the anchor shot id so its advice enters the recentAdvice
    // closed loop (its recommendation gets audited against the shot the user actually pulls next).
    function _stampTurn() {
        if (!root._conv || typeof MainController === "undefined" || !MainController.aiManager) return
        var id = MainController.aiManager.lastBaristaAnchorId()
        if (id > 0) root._conv.setShotIdForCurrentTurn(id)
    }
    // B3: fully close the session — stop listening + speaking and clear any pending action, so a
    // dismissed (or destroyed) overlay never keeps transcribing/replying/talking in the background.
    function _closeSession() {
        if (root._voiceInput) root._voiceInput.stop()
        if (root._voice) root._voice.stop()
        silenceTimer.stop()
        root._pendingNext = null
        root._awaitConfirm = false
        root._pendingGrind = null
        root._awaitingContext = false   // BL-1: a late context build must not open a turn while dormant
        root._thinking = false
        root._pendingBegin = null
        // SF-1: clear web search so it can't leak onto a later advisor turn on the same conversation key.
        if (root._conv) root._conv.webSearchEnabled = false
    }
    // SF-4: the preempted greeting couldn't beginSession because a close-out reply was mid-flight. Its reply
    // has now landed (conversation is free), so open the greeting — deferred to avoid re-entering the AI stack.
    function _retryBegin() {
        var pb = root._pendingBegin
        root._pendingBegin = null
        Qt.callLater(function() {
            if (root._state === "dormant" || !root._conv || !pb) return
            root._thinking = true
            root._stampTurn()
            root._conv.beginSession(pb.sys, pb.kick)
        })
    }

    // Kick off a conversation. First pull the user's REAL dial-in history for this bean so the
    // assistant KNOWS it (and can suggest), instead of asking. ask() fires once the history arrives.
    function _startConversation() {
        // _conv (aiManager.conversation) always exists — the real "can I chat?" test is isConfigured,
        // else we'd hang on "…" forever with no key (B2).
        if (!root._conv || typeof MainController === "undefined" || !MainController.aiManager
                || !MainController.aiManager.isConfigured) {
            root._thinking = false
            root._message = TranslationManager.translate("barista.noai",
                "Add an AI key in Settings → AI and I'll be able to chat.")
            return
        }
        if (root._orch) root._orch.markSessionStarted()   // SF-3: don't re-kick on page re-entry
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
        // Use the CLEAN profile title — currentProfileName decorates a modified profile ("*Title" /
        // "Title (modified)"), which would give a tweaked profile a different conversation key and split
        // the barista's memory from the same profile untweaked (S7). Matches the KB-lookup stripping.
        var prof = (typeof ProfileManager !== "undefined") ? ProfileManager.currentProfileName : ""
        prof = prof.replace(/^\*/, "").replace(/ \(modified\)$/, "")
        MainController.aiManager.switchConversation(Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType, prof)
        // Assemble ALL sources into one block via the context builder — the user's dial-in history PLUS,
        // for an unlinked bean, the community bean profile, PLUS the profile's curated guidance. It emits
        // one contextReady(). Fall back to the core request alone if the builder isn't available.
        if (typeof Barista !== "undefined" && Barista.contextBuilder)
            Barista.contextBuilder.build(Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType, prof)
        else
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
        if (root._state === "dormant")   // BL-1: dismissed during context build → don't open a turn
            return
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

        // Proactivity level (user setting): what the assistant may VOLUNTEER (it always answers direct asks).
        var level = root._settings ? root._settings.proactivityLevel : "full"
        // Cooldown: on back-to-back shots of the SAME bean (within 6h) don't re-raise a proactive nudge.
        // Close-out always engages — it's feedback on the shot just pulled, not a repeated greeting nudge.
        var mayNudge = (level !== "off") && root._settings
                       && (root._state === "closeOut" || root._settings.consumeProactiveNudge(root._bean, 6))
        if (level === "off")
            persona += "\nOnly answer what the user asks; do NOT volunteer suggestions unless asked."
        else if (mayNudge)
            persona += "\nBe proactive, but raise the SINGLE most useful thing — do NOT list multiple issues. "
                + "If the recent shots for this bean show 3+ attempts with no rating improvement, name the dialing "
                + "stall and propose a strategy change (a different variable, or a different profile) rather than "
                + "another micro-adjustment. Mention bean freshness/degassing only if clearly relevant."
        else
            persona += "\nGreet warmly and briefly — you recently made a suggestion for this coffee, so don't "
                + "re-raise it; only bring something up if the user asks or the data has clearly changed."

        // Web search (Anthropic only) — keep the persona truthful about what it can/can't reach.
        var webOn = !!(root._settings && root._settings.webSearchEnabled)
                    && typeof MainController !== "undefined" && MainController.aiManager
                    && MainController.aiManager.selectedProvider === "anthropic"
        if (webOn)
            persona += "\nYou also have live web search. Use it when the user asks about things outside the "
                + "data block — a bean or roaster's tasting notes, roast dates, brewing guides, gear, or "
                + "anything current — instead of saying you can't check. Search at most once or twice per "
                + "reply and answer briefly from what you find. BE ACCURATE about your reach: you can search "
                + "public websites, but you can NOT log into the user's accounts — Visualizer and Beanconqueror "
                + "are apps, and their private uploads aren't something you can query. The user's real shot "
                + "history is the data block above, which IS live."
        else
            persona += "\nYou cannot browse the web in this session. If asked about outside info, say so "
                + "briefly and work from the data block."

        // dataBlock is the pre-formatted, combined context (dial-in + bean profile + profile guidance).
        var block = (dataBlock && dataBlock.length > 0) ? dataBlock : "recordedShots: 0"

        var suggest = (level === "off" || !mayNudge)
            ? "Greet me briefly and wait for me to ask before suggesting changes."
            : "Greet me and suggest one useful thing."
        var kickoff = (root._state === "closeOut")
            ? "I just pulled a shot — how did it go?"
            : "It's the " + root._partOfDay() + " and I'm about to pull espresso. " + suggest
        // New-bean opener: no history for this bean → open with a grounded start from the bean profile.
        if (root._state !== "closeOut" && block.indexOf("recordedShots: 0") >= 0)
            kickoff += " (This is my first shot on this coffee — use the bean profile to suggest a starting point.)"
        // Volatile bit goes in the kickoff (not the cached system prompt): the pending grind reminder.
        if (root._pendingGrind && root._pendingGrind.value)
            kickoff += " (I earlier agreed to set the grinder to " + root._pendingGrind.value
                     + " but haven't confirmed doing it — ask me early whether I actually set it.)"

        root._conv.webSearchEnabled = webOn   // barista session only; reset by ask()/resetInMemory()
        root._stampTurn()
        var _sys = persona + "\n\n" + block
        if (!root._conv.beginSession(_sys, kickoff))   // SF-4: busy (a prior turn in flight) → retry when free
            root._pendingBegin = { "sys": _sys, "kick": kickoff }
    }

    function _send(text) {
        var t = (text || "").trim()
        if (t.length === 0 || !root._conv || root._thinking)
            return
        if (root._state === "dormant")   // B3: dismissed → don't keep sending (e.g. late voice finals)
            return
        var hasActions = (typeof Barista !== "undefined" && Barista.actions)
        // Off-machine grind reminder confirm — ONLY intercept yes/no when the assistant's last line actually
        // names the pending grind VALUE (SF-6). "grind" alone is too loose (it talks about grind constantly),
        // so a "yes" to a taste question could falsely record a grind. The Yes/No chip is the reliable path.
        if (hasActions && root._pendingGrind && root._pendingGrind.value
                && root._message && root._message.indexOf(root._pendingGrind.value) >= 0) {
            var g = Barista.actions.parseConfirmation(t)   // 1 yes / 0 no / -1 neither
            if (g === 1) { root._resolveGrind(true); return }
            if (g === 0) { root._resolveGrind(false); return }
        }
        // Apply-on-confirm for a pending recommendation ("OK" → apply; "no" → skip).
        if (hasActions && root._awaitConfirm && root._pendingNext) {
            var c = Barista.actions.parseConfirmation(t)
            if (c === 1) { root._applyPending(); return }
            if (c === 0) { root._skipPending(); return }
            root._awaitConfirm = false; root._pendingNext = null   // ambiguous → disarm + drop the stale chip (S11)
        }
        // PERSISTENCE: capture the close-out taste feedback onto the shot record, so it's available to
        // the AI on every future call — never starting over or guessing. (Recent-shot context includes it.)
        if (root._state === "closeOut" && !root._closeOutRated && root._orch && root._orch.lastShotId > 0
                && typeof MainController !== "undefined" && MainController.shotHistory) {
            // Only capture from a SUBSTANTIVE reply (not "ok"/"hang on") and match whole words, so "no good"
            // isn't scored as good and a one-word confirmation doesn't overwrite the notes (S9).
            var w = t.toLowerCase().replace(/[^a-z0-9'\s]/g, " ").split(/\s+/)
            var isConfirmation = hasActions && Barista.actions.parseConfirmation(t) >= 0
            if (!isConfirmation && w.length >= 2) {
                root._closeOutRated = true
                var neg = w.indexOf("no") >= 0 || w.indexOf("not") >= 0 || w.indexOf("bad") >= 0
                var enj = (w.indexOf("sour") >= 0) ? 45
                        : (w.indexOf("bitter") >= 0 || w.indexOf("burnt") >= 0) ? 55
                        : (!neg && (w.indexOf("balanced") >= 0 || w.indexOf("good") >= 0 || w.indexOf("great") >= 0
                            || w.indexOf("perfect") >= 0 || w.indexOf("nice") >= 0 || w.indexOf("delicious") >= 0
                            || w.indexOf("love") >= 0 || w.indexOf("lovely") >= 0)) ? 82
                        : 0
                var meta = { "espressoNotes": t }
                if (enj > 0)
                    meta["enjoyment0to100"] = enj
                MainController.shotHistory.requestUpdateShotMetadata(root._orch.lastShotId, meta)
            }
        }
        root._thinking = true
        if (root._voiceInput && root._voiceInput.listening) root._voiceInput.pauseMic()
        root._stampTurn()
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
                root._closeSession()
            }
        }
    }
    // B1: the overlay only exists on the idle page, so a close-out/greeting whose state flipped while the
    // overlay was destroyed (during the shot) never fired onStateChanged. Catch up on (re)creation.
    Component.onCompleted: {
        // SF-3: catch up ONLY if this activation hasn't been started yet (the latch survives the overlay
        // being destroyed/recreated on page navigation), so returning to idle doesn't re-run the greeting.
        if ((root._state === "greeting" || root._state === "closeOut")
                && root._orch && !root._orch.sessionStarted)
            root._startConversation()
    }
    // B3: if the user navigates away mid-chat, the Loader destroys us — close the mic/TTS session first.
    Component.onDestruction: root._closeSession()
    // Claude's replies → show + speak (once each).
    Connections {
        target: root._conv
        ignoreUnknownSignals: true
        function onResponseReceived(response) {
            if (root._state === "dormant")   // BL-1: reply landed after dismiss (or it's an advisor turn) → ignore
                return
            if (root._pendingBegin) { root._retryBegin(); return }   // SF-4: this is the preempted turn's stale reply
            root._message = root._stripBlock(response)   // hide the JSON action block from the display
            root._thinking = false
            // Pause the mic BEFORE speaking (greeting path has no prior pause). speak() now flips
            // `speaking` synchronously, so this + the post-speak check below are race-free.
            if (root._voiceInput && root._voiceInput.listening) root._voiceInput.pauseMic()
            root._speakSanitised(response)               // (also strips fenced blocks before TTS)
            root._resetSilence()   // keep the mic session alive while we're conversing
            // If this turn carried a concrete recommendation, arm apply-on-confirm + show the chip.
            var nx = root._conv ? root._conv.structuredNextForLastAssistantTurnMap() : null
            if (nx && Object.keys(nx).length > 0) {
                root._pendingNext = nx
                root._awaitConfirm = true
            } else {
                root._pendingNext = null   // no recommendation this turn → drop any stale chip (S11)
                root._awaitConfirm = false
            }
            // Turn done. If it will speak, `speaking` is already true → skip; onSpeakingChanged(false)
            // reopens the mic when playback truly ends. If nothing will speak, reopen now.
            if (root._voiceInput && root._voiceInput.listening
                    && (!root._voice || !root._voice.speaking))
                root._voiceInput.resumeMic()
        }
        function onErrorOccurred(error) {   // B2: never hang on "…" — surface it and recover the UI
            if (root._state === "dormant")
                return
            if (root._pendingBegin) { root._retryBegin(); return }   // SF-4: the preempted turn failed → open the greeting
            root._thinking = false
            root._message = (error && error.length > 0)
                ? error
                : TranslationManager.translate("barista.err", "Something went wrong — tap Chat or type to try again.")
            // Reopen the mic if a session is live (the turn failed, not the session).
            if (root._voiceInput && root._voiceInput.listening
                    && (!root._voice || !root._voice.speaking))
                root._voiceInput.resumeMic()
            root._resetSilence()
        }
    }
    // The full advisor-grade context arrived → open the conversation grounded in it (so the AI KNOWS,
    // rather than asking). Gated on _awaitingContext so we only consume the request we fired.
    Connections {
        target: (typeof MainController !== "undefined") ? MainController.aiManager : null
        ignoreUnknownSignals: true
        function onBaristaContextReady(dataBlock) {
            // Only the fallback path (no context builder) consumes this directly; otherwise the builder
            // owns this signal and emits the fuller combined block via onContextReady below.
            if (root._awaitingContext && !(typeof Barista !== "undefined" && Barista.contextBuilder))
                root._askWithContext(dataBlock, false)
        }
    }
    // Combined context (dial-in + community bean profile + curated profile guidance) from the builder.
    Connections {
        target: (typeof Barista !== "undefined") ? Barista.contextBuilder : null
        ignoreUnknownSignals: true
        function onContextReady(fullBlock) {
            if (root._awaitingContext)
                root._askWithContext(fullBlock, false)
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

            // Live listening indicator — only when actually HEARING (not while thinking or speaking).
            Text {
                Layout.fillWidth: true
                visible: root._voiceInput && root._voiceInput.listening && !root._voiceInput.paused
                         && !root._thinking && (!root._voice || !root._voice.speaking)
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
