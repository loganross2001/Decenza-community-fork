import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import QtQuick.Effects
import Decenza

// [barista-fork] The user-initiated barista assistant — a REAL conversation, not a script. The barista is
// a quiet persistent presence; it NEVER speaks first. When the user engages (taps/talks → orchestrator
// state "conversing") this PRIMES a multi-turn Claude conversation (system prompt with the user's name,
// bean, best recipe, and a sessionContext block carrying recency + any just-pulled shot) and WAITS. The
// user's first utterance begins the session; any greeting is folded into that first reply per recency.
// Machine events (Espresso selected, shot saved) are CONTEXT updates, never conversation triggers.
Item {
    id: root
    anchors.fill: parent

    readonly property var _orch: (typeof Barista !== "undefined") ? Barista.orchestrator : null
    readonly property var _voice: (typeof Barista !== "undefined") ? Barista.voice : null
    // [barista-fork] The SEPARATE coaching voice (live steam + espresso coaches) — used by the Select Barista
    // picker so a voice/volume/speed change can be scoped to coaching independently of the conversational voice.
    readonly property var _coachingVoice: (typeof Barista !== "undefined") ? Barista.coachingVoice : null
    readonly property var _voiceInput: (typeof Barista !== "undefined") ? Barista.voiceInput : null
    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null
    // [barista-fork] Select Barista picker (compact per-role voice/volume/speed chooser) open state.
    property bool _selectBaristaOpen: false

    // Voice-input session: 20s of silence auto-closes the mic (resets on any speech / reply / activity).
    Timer {
        id: silenceTimer
        // [barista-fork] Longer window (60s) while actively conversing off-screensaver so a pause for latte prep
        // doesn't silently kill the mic (that's what ate "that's it for now" — the mic auto-closed unheard). The
        // screensaver keeps the short 20s window for burn-in safety.
        interval: root._screensaverActive ? 20000 : 60000
        repeat: false
        onTriggered: {
            if (root._voiceInput) root._voiceInput.stop()
            root._diag("mic_autoclose_silence", { screensaver: root._screensaverActive })   // was invisible before
            // [barista-fork] Clear the grind-ask chip on auto-close so a panel re-opened much later doesn't lead
            // with a contextless "change the grind?" button (the owner's "came back later, stale button").
            if (root._pendingGrind) { root._pendingGrind = null; root._diag("grind_chip_cleared", { reason: "timeout" }) }
            // [barista-fork] Screensaver burn-in safety: a panel left open over the screensaver must not
            // sit lit if the user walked away. On silence-timeout during the screensaver, collapse back to
            // the faint drifting avatar (keep the session primed + recoverable via _tapDock — do NOT dismiss,
            // and do NOT wake the machine).
            if (root._screensaverActive && root._state === "conversing") {
                root._showSettings = false   // don't leave a stuck settings flag hiding everything when collapsed
                root._collapsed = true
            } else if (root._state === "conversing") {
                // [barista-fork] Off-screensaver: a silent mic-death was previously invisible, so a later
                // "that's it for now" hit a dead mic and looked like a hang. Make the close UNMISTAKABLE — a soft
                // non-verbal cue + a visible tap-to-resume (the Chat button reads "Chat" again now the mic stopped).
                if (root._voice && typeof root._voice.playThinkingCue === "function") root._voice.playThinkingCue()
                root._message = TranslationManager.translate("barista.mic.closed",
                    "Mic paused — tap Chat when you want to keep going.")
            }
        }
    }
    function _resetSilence() {
        // Only count silence while actually HEARING — a long thinking/searching/speaking turn must not
        // trip the 20s auto-off (paused = assistant busy). onPausedChanged restarts it when we reopen.
        if (root._voiceInput && root._voiceInput.listening && !root._voiceInput.paused)
            silenceTimer.restart()
        else
            silenceTimer.stop()
    }

    // [barista-fork] Part B — non-verbal silence-breaker. A single-shot UI-timeout timer (per CLAUDE.md, a
    // genuine UI timeout, not a polling loop): started the instant a real model turn is dispatched (_send),
    // cancelled the moment ANY speech starts (the lead-in or the answer) or the turn resolves/errs. If it
    // still fires at 5s with nothing spoken, we break the silence WITHOUT a canned phrase — a soft "thinking"
    // tick plus a more pronounced avatar thinking beat. Gentle, and never while the coaching voice is talking.
    Timer {
        id: slowOpTimer
        // [barista-fork] A REPEATING "still-here" heartbeat, not a one-shot. First beat at 3s (past ~3s of
        // silence a user starts wondering if it froze), then every 3s FOR AS LONG AS the turn is still working
        // silently — crucially this now keeps beating through the wait AFTER a spoken lead-in (a fast lead-in
        // used to cancel the watch, leaving dead air while the tool ran). It never plays over speech (the
        // guard skips while the barista or coach voice is talking), so it only fills genuine silence.
        interval: 3000
        repeat: true
        onTriggered: {
            if (!root._thinking || root._state !== "conversing")
                return
            // Never beat over the barista's own speech or a coaching cue (speech arbiter: no overlap).
            var coachBusy = (typeof Barista !== "undefined" && Barista.coachingVoice
                             && Barista.coachingVoice.speaking)
            if ((root._voice && root._voice.speaking) || coachBusy)
                return
            root._thinkingCue = true   // avatar shows a more pronounced thinking beat
            // [barista-fork] The AUDIO fill is now the continuous thinking-loop earcon (started immediately in
            // _beginSlowOpWatch, stopped when audio is audible) — NOT a delayed 3s tick. This timer keeps only
            // the visual "thinking beat" going; the loop covers the silence from the moment the user stops.
        }
    }
    // [barista-fork] Reactive control of the cue loop, which doubles as the SPEAKER KEEPALIVE: run it whenever a
    // turn is in flight OR a speak is pending-but-not-yet-audible (`speaking && !audible` = the synth/prepare gap
    // that was clipping the first words on a sleepy BT speaker), and stop the instant real audio is `audible`
    // (handled event-based in C++ handleAndroidPlaybackStarted). Called on turn start + whenever audible/speech
    // flips + on any speak dispatch. C++ startThinkingLoop honors mute + loops a sub-perceptible keepalive when
    // the earcon is "off" + is idempotent.
    function _updateThinkingLoop() {
        if (!root._voice)
            return
        var coachBusy = (typeof Barista !== "undefined" && Barista.coachingVoice && Barista.coachingVoice.speaking)
        var want = root._state === "conversing" && !root._voice.audible && !root._paused && !coachBusy
                   && (root._thinking || root._voice.speaking)
        if (want) root._voice.startThinkingLoop()
        else root._voice.stopThinkingLoop()
    }
    // Start the 5s silence-breaker for a freshly dispatched slow op (called from _send after the model turn
    // is actually sent). Clears the per-turn spoken flag + cue so each turn starts fresh.
    function _beginSlowOpWatch() {
        root._spokeThisTurn = false
        root._thinkingCue = false
        slowOpTimer.restart()
        root._updateThinkingLoop()   // [barista-fork] start the continuous thinking hum immediately
    }
    // Silence is broken (something was spoken) or the turn ended → stop watching + drop the cue.
    function _cancelSlowOpWatch() {
        slowOpTimer.stop()
        root._thinkingCue = false
        if (root._voice) root._voice.stopThinkingLoop()   // [barista-fork] turn resolved → stop the hum
    }
    // Mark that the barista has produced audible/visible words this turn.
    // [barista-fork] NOTE: this no longer cancels the slow-op heartbeat. A spoken LEAD-IN doesn't mean the
    // turn is done — the tool/response is still coming, and the heartbeat must keep filling that wait so it
    // doesn't feel frozen after "give me a second." The heartbeat is stopped instead when the real answer
    // lands (onResponseReceived → _thinking=false + _cancelSlowOpWatch) or the turn ends/teardown. The
    // heartbeat's own guard already keeps it from playing over the lead-in while it speaks.
    function _markSpokeThisTurn() {
        root._spokeThisTurn = true
    }
    Connections {
        target: root._voiceInput
        ignoreUnknownSignals: true
        function onFinalText(text) {   // spoken utterance → the AI; pause the mic until the turn is done
            // [barista-fork] NOTE: voice-ID must NEVER capture during a live STT turn — Android's mic-contention
            // policy silences the recognizer while a 2nd QAudioSource is open, so the recognizer missed the first
            // 3-6s of speech. The increment-2 concurrent capture was removed; ID now happens only off the STT
            // path (enrollment + the on-demand testers). See [[decenza-barista-voice-matching]].
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
        // [barista-fork] Real audio started/stopped → re-evaluate the thinking hum (stop it when the voice is
        // audible; resume it if a lead-in finished but the tool is still running). Avatar sync is a binding.
        function onAudibleChanged() {
            root._updateThinkingLoop()
            // [barista-fork] Text-on-speak: reveal the held reply text the instant the voice is actually audible,
            // so the words appear WITH the sound, not before it.
            if (root._voice && root._voice.audible && root._pendingDisplayText.length > 0) {
                root._message = root._pendingDisplayText
                root._pendingDisplayText = ""
                root._diag("display_reveal", { source: "audible" })
            }
            // [barista-fork] Read-along scroll: start pacing the text when the voice becomes audible (callLater so
            // the just-revealed text has laid out and implicitHeight is current); stop when it goes quiet.
            if (root._voice && root._voice.audible) Qt.callLater(root._startSpeechScroll)
            else root._stopSpeechScroll()
        }
        function onSpeakingChanged() {
            root._diag("speaking_changed", { speaking: root._voice ? root._voice.speaking : false, endAfter: root._endAfterReply })
            // [barista-fork] Part B: the barista actually started talking → the silence is broken, so kill the
            // 5s cue timer + clear the cue (belt-and-suspenders alongside _markSpokeThisTurn on the speak call).
            if (root._voice && root._voice.speaking)
                root._markSpokeThisTurn()
            // [barista-fork] Close-out safety net: the fallback timer is armed at goodbye DETECTION, so its 9s
            // must otherwise cover the whole model round-trip + TTS synthesis + speech. Re-arm it the moment the
            // sign-off actually STARTS speaking, so the budget is 9s of SPEECH (a verbose sign-off after slow
            // cloud latency isn't cut off), while a genuinely stuck speaking flag still force-closes 9s later.
            if (root._voice && root._voice.speaking && root._endAfterReply)
                dismissFallbackTimer.restart()
            // [barista-fork] The lead-in just finished and a post-tool answer was held back → speak it NOW,
            // so the answer never cut off the lead-in. Checked BEFORE the end-session/mic logic (a sign-off
            // answer must still speak). Speaking flips speaking→true again; the next speaking_off resumes
            // the normal end/mic handling once the real answer is done.
            if (root._voice && !root._voice.speaking && root._pendingSpeech.length > 0) {
                var deferred = root._pendingSpeech
                root._pendingSpeech = ""
                root._diag("speak_deferred_now", { chars: deferred.length })
                root._speakSanitised(deferred)
                return
            }
            // [barista-fork] Never-audible reveal fallback: this speak ended without ever becoming audible (a
            // barge-in stop, a cloud error with no native engine, or a reply that sanitised to empty). Reveal
            // the held text now instead of leaving it to ambush a LATER utterance's audible rise (the "old text
            // shows with a new response" bug). Sits after the deferral drain, before the end-session check.
            if (root._voice && !root._voice.speaking && !root._voice.audible && root._pendingDisplayText.length > 0) {
                root._message = root._pendingDisplayText
                root._pendingDisplayText = ""
                root._diag("display_reveal", { source: "speakEndFallback" })
            }
            // [barista-fork] The barista signed off and asked to end the session → collapse only NOW that the
            // sign-off finished speaking (never cut it off). Checked BEFORE the listening guard so a text-only
            // reply that still spoke a sign-off dismisses too. _endSessionNow clears the flag + ends the session.
            if (root._voice && !root._voice.speaking && root._endAfterReply) {
                root._endSessionNow()
                return
            }
            if (!root._voiceInput || !root._voiceInput.listening) return
            if (root._voice.speaking) root._voiceInput.pauseMic()
            // [barista-fork] Only reopen the mic when the TURN is actually done. A lead-in finishing while a
            // tool is still running (_thinking stays true until onResponseReceived) is NOT the end of the turn —
            // keep the mic paused so the barista doesn't "hang" with an open mic, and let the hum refill the gap
            // (below). Abnormal turn death is recovered separately by onErrorOccurred (clears _thinking, resumes).
            else if (!root._thinking && !root._paused) { root._voiceInput.resumeMic(); root._resetSilence() }
            // Re-evaluate the thinking hum on every speech transition: after a lead-in finishes it resumes to fill
            // the tool-running silence; once the real answer is audible onAudibleChanged stops it.
            root._updateThinkingLoop()
        }
    }
    // [barista-fork] Mic-echo guard for the COACHING voice (a separate AssistantVoice from the barista voice
    // above). If a live coach cue plays while a chat session's mic is open, pause the recognizer so it never
    // transcribes the coach; resume via the same safe helper the local-speak path uses (it re-checks the guards).
    Connections {
        target: (typeof Barista !== "undefined") ? Barista.coachingVoice : null
        ignoreUnknownSignals: true
        function onSpeakingChanged() {
            if (!root._voiceInput || !root._voiceInput.listening) return
            if (Barista.coachingVoice && Barista.coachingVoice.speaking) {
                root._voiceInput.pauseMic()
                root._diag("coach_mic_paused", {})
            } else {
                root._resumeMicAfterLocal()
            }
        }
    }
    readonly property string _state: _orch ? _orch.state : "present"   // "present" | "conversing"
    // [barista-fork] A shot the barista knows about but the user hasn't discussed yet — the old close-out,
    // now pure context (drives the justPulledShot injection + the P3 undiscussed-shot pulse).
    readonly property bool _hasUndiscussedShot: _orch && _orch.lastShotId > 0 && !_orch.shotDiscussed
    readonly property var _conv: (typeof MainController !== "undefined" && MainController.aiManager)
                                 ? MainController.aiManager.conversation : null

    // [barista-fork] Screensaver-safe presence. When the machine screensaver is up AND the dock is collapsed
    // (no active conversation), we swap the normal edge tab for a FAINT DRIFTING AVATAR: low opacity so it
    // barely lights the LCD (and it already rides the hardware backlight dim), repositioned periodically so no
    // pixel stays lit continuously (burn-in safety). Tapping it engages the barista OVER the screensaver
    // WITHOUT waking the DE1 (the overlay never calls ScreensaverPage.wake()/DE1Device.wakeUp()).
    readonly property bool _screensaverActive: (typeof ScreensaverManager !== "undefined")
                                               && ScreensaverManager.screensaverActive
    // True exactly when the collapsed dock should render as the drifting avatar (screensaver up + not expanded
    // + settings closed) — the same "collapsed dock" condition the edge tab uses, gated to the screensaver.
    readonly property bool _screensaverDock: root._screensaverActive && !root._showSettings
                                             && !(root._state === "conversing" && !root._collapsed)

    property string _message: ""       // the assistant's latest line
    property bool _thinking: false
    // [barista-fork] "Don't leave the user in silence" (Part A + B). _spokeThisTurn goes true the moment ANY
    // speech starts for the in-flight turn — the model's pre-tool lead-in ("let me pull that up", spoken via
    // onInterimReceived) OR the final answer. The 5s silence-breaker (Part B) only fires while this is still
    // false, so a natural verbal acknowledgment always suppresses the non-verbal cue. Reset at each turn start.
    property bool _spokeThisTurn: false
    // [barista-fork] Part B cue latch: a more pronounced avatar "thinking" beat while the slow op drags on with
    // nothing spoken. Set when the 5s timer fires; cleared as soon as anything is spoken or the turn resolves.
    property bool _thinkingCue: false
    // [barista-fork] Text-on-speak: the reply text is HELD here until the voice actually starts (onAudibleChanged
    // reveals it into _message), so text no longer appears before you hear anything. Muted replies show at once.
    property string _pendingDisplayText: ""
    // [barista-fork] Pause: the session is held (mic paused; speech also stopped in "freeze" mode). Only the
    // user's explicit Resume tap clears this — auto-resume paths are guarded on !_paused so they never reopen.
    property bool _paused: false
    property bool _showSettings: false
    property bool _collapsed: false     // panel minimised to a thin edge tab (frees the whole screen)

    // --- Right-side panel geometry (owner: barista is a reserved right panel, machine UI reflows left) ---
    // Width fraction of the whole screen, from the panelWidthMode setting (narrow/medium/wide).
    readonly property real _panelFraction: {
        var m = root._settings ? root._settings.panelWidthMode : "medium"
        if (m === "narrow") return 0.30
        if (m === "wide")   return 0.52
        return 0.42   // medium ≈ the previous floating-card proportion
    }
    // The panel's own width (capped so it never eats the whole screen on a wide display).
    readonly property real _panelWidth: Math.min(Theme.scaled(560), width * _panelFraction)
    // True while a full panel (conversation OR settings) is expanded on the right — NOT the edge tab.
    readonly property bool _panelExpanded: !_collapsed && (_state === "conversing" || _showSettings)
    // Space the main app UI (pageStack in main.qml) must leave clear on the right. The panel is flush to
    // the right edge; reserve its width plus the small gutter so content never slides under it. 0 = collapsed.
    readonly property real reservedWidth: _panelExpanded ? (_panelWidth + Theme.spacingMedium) : 0
    property var _pendingNext: null     // structuredNext recommendation awaiting apply/skip
    // [barista-fork] User-initiated model: the context is PRIMED (system prompt assembled) but the Claude
    // session is NOT begun until the user's first real utterance. No synthetic kickoff, no machine-first turn.
    property string _primedSystemPrompt: ""   // the assembled persona+data block, waiting for the first turn
    property bool _primed: false              // context ready → the first _send() begins the session
    property bool _sessionBegun: false        // beginSession() has fired for this activation (else followUp)
    property string _queuedFirstUtterance: "" // tap-chat-and-talk: words spoken BEFORE context finished
                                              // building are held here and replayed once primed (never dropped)
    property string _sessBrand: ""      // this activation's switch args (kept for thread restore on recreate)
    property string _sessType: ""
    property string _sessProf: ""
    property bool _awaitConfirm: false  // a recommendation is armed for a voice "OK"
    property var _pendingGrind: null    // outstanding off-machine grind reminder (shown at greeting)
    property bool _awaitingContext: false   // waiting on the shot history before opening the conversation
    property bool _closeOutRated: false      // persist the close-out taste to the shot exactly once
    property bool _fellBack: false           // bean-filtered history was empty → fetched recent overall
    // [barista-fork] The barista asked to end the conversation (end_conversation tool, or a non-Anthropic
    // standalone farewell). Don't dismiss instantly — let the sign-off speak first, THEN collapse to the tab.
    // Cleared on any new user utterance / new engage / teardown so a stuck flag can never dismiss a later turn.
    property bool _endAfterReply: false

    // [barista-fork] The post-tool answer, held when it arrives WHILE the pre-tool lead-in is still speaking.
    // Speaking it immediately would hard-cut the lead-in mid-sentence (the "trips over itself" bug — confirmed
    // in diagnostics: a long lead-in + a fast tool → speak_INTERRUPTS_previous). Instead we stash it here and
    // speak it from onSpeakingChanged once the lead-in finishes. Cleared on teardown so it never leaks a turn.
    property string _pendingSpeech: ""

    // [barista-fork] The pre-tool lead-in text spoken this turn (set in onInterimReceived, cleared each turn in
    // _send + on teardown). The post-tool answer is checked against it so the barista never says the same thing
    // twice ("repeats its entire part of the discussion"). See _answerDupOfLeadin.
    property string _leadinSpokenText: ""

    // [barista-fork] Safe passthrough to the diagnostic recorder (no-op if the module/recorder is absent).
    function _diag(event, detail) {
        if (typeof Barista !== "undefined" && Barista.diagnostics)
            Barista.diagnostics.mark("overlay", event, detail || ({}))
    }

    // [barista-fork] Repeat guard. Lowercase word-tokens; true when a SUBSTANTIAL lead-in (≥60 chars) already
    // contains ≥70% of the answer's tokens — i.e. the answer just restates what was already spoken. High
    // threshold so an answer that adds genuinely new info (e.g. weather numbers) still speaks.
    function _tokens(s) {
        return String(s || "").toLowerCase().replace(/[^a-z0-9 ]/g, " ")
                     .split(/\s+/).filter(function(w) { return w.length > 0 })
    }
    function _answerDupOfLeadin(answer) {
        var lead = root._leadinSpokenText
        if (!lead || lead.length < 60) return false
        var a = root._tokens(answer)
        if (a.length === 0) return false
        var lset = {}
        var l = root._tokens(lead)
        for (var i = 0; i < l.length; ++i) lset[l[i]] = true
        var hit = 0
        for (var j = 0; j < a.length; ++j) if (lset[a[j]]) hit++
        return (hit / a.length) >= 0.7
    }

    // Strip markdown/code so cloud voices don't read asterisks, hashes, or JSON aloud.
    function _speakSanitised(t) {
        if (!root._voice) return
        var clean = (t || "").replace(/```[\s\S]*?```/g, " ").replace(/[*_#`>]/g, "")
                              .replace(/^\s*[-•]\s+/gm, "").replace(/\s+/g, " ").trim()
        root._voice.speak(clean)
        // [barista-fork] Start the cue-loop keepalive so the BT speaker is awake before this speak becomes
        // audible (covers local confirmations + the deferred answer, which aren't in a _thinking window).
        root._updateThinkingLoop()
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
        root._pendingDisplayText = ""   // [barista-fork] local confirm shows immediately → no held text to reveal over it
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
    // [barista-fork] Reverse the last applied dial change (the ask→approve→apply safety net). Restores the
    // pre-apply dose/yield/temp override + the queued grind captured in applyFromNext, then speaks a short local
    // confirmation (no model round-trip, like _applyPending).
    function _undoLast() {
        if (typeof Barista === "undefined" || !Barista.actions) return
        var ok = Barista.actions.undoLastAutoApply()
        var msg = ok ? TranslationManager.translate("barista.act.undone", "Okay — I put it back.")
                     : TranslationManager.translate("barista.act.nothingUndo", "There's nothing to undo.")
        root._pendingDisplayText = ""   // [barista-fork] local confirm shows immediately
        root._message = msg
        root._speakSanitised(msg)
        root._resumeMicAfterLocal()
    }
    // S4: after a locally-spoken line, a MUTED engine won't fire speakingChanged to reopen the mic —
    // so reopen it here (mirrors the muted-resume path in onResponseReceived).
    function _resumeMicAfterLocal() {
        if (root._voiceInput && root._voiceInput.listening && !root._paused
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

    // [barista-fork] Arm the apply/skip chip ONLY when the recommendation is a REAL change from the
    // live recipe — the model often echoes the current dose/yield/grind in its structuredNext block,
    // which must NOT pop "apply or skip?" with nothing to actually change. Compares each proposed
    // field to the same current value applyFromNext would overwrite (baristaactions.cpp).
    function _nextDiffersFromCurrent(nx) {
        if (!nx) return false
        var eps = 0.05
        // Grinder dial: a genuinely different setting than the one on file. Numeric dials frequently
        // echo with cosmetic formatting differences ("2.5" vs "2.50", comma vs dot) that are NOT a real
        // change — when both sides parse as numbers, compare with a format-only tolerance so only a true
        // dial move (2.5 → 2.6) arms the chip; otherwise fall back to the exact trimmed-string compare.
        var g = String(nx.grinderSetting || "").trim()
        var curGrind = String(Settings.dye.dyeGrinderSetting || "").trim()
        if (g.length > 0 && g !== curGrind) {
            var gn = parseFloat(g.replace(",", "."))
            var cn = parseFloat(curGrind.replace(",", "."))
            if (!isNaN(gn) && !isNaN(cn)) {
                if (Math.abs(gn - cn) > 0.001) return true   // real dial change, not just formatting
            } else {
                return true   // non-numeric / newly-set dial → any text difference is a real change
            }
        }
        // Dose in.
        var dose = Number(nx.doseG)
        if (dose > 0 && Math.abs(dose - Number(Settings.dye.dyeBeanWeight)) > eps)
            return true
        // Explicit yield out.
        var yieldG = Number(nx.targetWeightG)
        if (yieldG > 0 && Math.abs(yieldG - Number(ProfileManager.targetWeight)) > eps)
            return true
        // Ratio → the yield it would imply (mirrors applyFromNext: only when no explicit yield).
        var ratio = Number(nx.ratio)
        if (ratio >= 1.0 && yieldG <= 0) {
            var baseDose = dose > 0 ? dose : Number(Settings.dye.dyeBeanWeight)
            if (baseDose > 0 && Math.abs(baseDose * ratio - Number(ProfileManager.targetWeight)) > eps)
                return true
        }
        // Temperature vs the current effective temp (override if set, else the profile's).
        var temp = Number(nx.temperatureC)
        if (temp > 0) {
            var curTemp = Settings.brew.hasTemperatureOverride
                          ? Number(Settings.brew.temperatureOverride)
                          : Number(ProfileManager.profileTargetTemperature)
            if (Math.abs(temp - curTemp) > 0.2)
                return true
        }
        return false
    }

    // Who the assistant is talking to: the user's chosen name, else the active barista.
    readonly property string _userName: {
        if (_settings && _settings.userName && _settings.userName.length > 0) return _settings.userName
        if (Settings.dye.dyeBarista && Settings.dye.dyeBarista.length > 0) return Settings.dye.dyeBarista
        return ""
    }
    // [barista-fork] Phase 1 identity: WHO is talking right NOW. The roster active user (dyeBarista — set by the
    // set_active_user tool when someone says who they are) wins over the owner's configured name, so the barista
    // addresses the current speaker. Falls back to the owner name, then generic. dyeBarista→userName→"".
    readonly property string _activeUserName: {
        if (Settings.dye.dyeBarista && Settings.dye.dyeBarista.length > 0) return Settings.dye.dyeBarista
        if (_settings && _settings.userName && _settings.userName.length > 0) return _settings.userName
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
    // [barista-fork] Recency of the RELATIONSHIP — the heart of the user-initiated model. Pure timestamp
    // compare (no timers-as-guards), read at engage/prime time from AssistantSettings.lastExchangeAt.
    //   ongoing (<60min) → no greeting, continue mid-conversation
    //   earlierToday (60min–8h, same day) → no hello; at most a light nod ONCE/day
    //   firstOfDay (new day or >8h) → warm brief hello folded into the first reply
    //   firstEver (never) → firstOfDay + "don't assume who it is" name caution
    // (In P2 this moves to orchestrator.recencyBucket(); the injection point below stays.)
    function _recencyBucket() {
        if (!root._settings || typeof root._settings.minutesSinceLastExchange !== "function")
            return "firstEver"
        var mins = root._settings.minutesSinceLastExchange()
        if (mins < 0) return "firstEver"
        if (mins < 60) return "ongoing"
        // Same calendar day? Compare the stored ISO date to today.
        var lastIso = root._settings.lastExchangeAt ? root._settings.lastExchangeAt() : ""
        var sameDay = false
        if (lastIso && lastIso.length >= 10) {
            var d = new Date(lastIso)
            var now = new Date()
            sameDay = d.getFullYear() === now.getFullYear() && d.getMonth() === now.getMonth()
                      && d.getDate() === now.getDate()
        }
        if (mins < 8 * 60 && sameDay) return "earlierToday"
        return "firstOfDay"
    }
    // Stamp the assistant's upcoming turn with the anchor shot id so its advice enters the recentAdvice
    // closed loop (its recommendation gets audited against the shot the user actually pulls next).
    function _stampTurn() {
        if (!root._conv || typeof MainController === "undefined" || !MainController.aiManager) return
        var id = MainController.aiManager.lastBaristaAnchorId()
        if (id > 0) root._conv.setShotIdForCurrentTurn(id)
    }
    // [barista-fork] Tap-chat-and-talk: engaging opens the mic in the same gesture. Barge-in — silence any
    // TTS still playing so the barista never talks over the user opening the mic. No-op if voice unavailable.
    function _openMic() {
        if (!root._voiceInput || !root._voiceInput.available)
            return
        if (root._voiceInput.listening)
            return
        if (root._voice) root._voice.stop()
        root._voiceInput.start()
        silenceTimer.restart()
    }
    // [barista-fork] Engage: if the V2 "engage capture test" is ON, run a short capture BEFORE opening the STT
    // mic (never concurrent — the anti-probe test), then open the mic when it signals done. Default off → open
    // the mic immediately. The done-signal always fires (even on capture failure), so the mic always opens.
    function _engageOpenMic() {
        if (root._settings && root._settings.voiceIdEngageTest
                && typeof Barista !== "undefined" && Barista.voiceId
                && Barista.voiceId.enrolledNames.length > 0) {
            Barista.voiceId.startEngageCaptureTest()   // → engageCaptureTestDone → _openMic
            return
        }
        root._openMic()
    }
    // Bridge the engage capture-test completion to opening the STT mic.
    Connections {
        target: (typeof Barista !== "undefined") ? Barista.voiceId : null
        ignoreUnknownSignals: true
        function onEngageCaptureTestDone() {
            if (root._state === "conversing")
                root._openMic()
        }
    }
    // B3: fully close the session — stop listening + speaking and clear any pending action, so a
    // dismissed (or destroyed) overlay never keeps transcribing/replying/talking in the background.
    function _closeSession() {
        if (root._voiceInput) root._voiceInput.stop()
        if (root._voice) root._voice.stop()
        silenceTimer.stop()
        root._cancelSlowOpWatch()   // [barista-fork] Part B: session torn down → stop the 5s cue timer + clear cue
        root._spokeThisTurn = false
        root._pendingNext = null
        root._awaitConfirm = false
        root._pendingGrind = null
        root._pendingDisplayText = ""   // [barista-fork] no held text survives into the next session's first audible
        root._awaitingContext = false   // BL-1: a late context build must not open a turn while dormant
        root._thinking = false
        root._primed = false
        root._sessionBegun = false
        root._primedSystemPrompt = ""
        root._queuedFirstUtterance = ""   // drop any un-replayed tap-and-talk utterance on teardown
        root._endAfterReply = false       // [barista-fork] stuck-flag guard: a torn-down session can't self-dismiss later
        dismissFallbackTimer.stop()       // [barista-fork] and cancel any pending close-out safety timer
        // SF-1: clear web search so it can't leak onto a later advisor turn on the same conversation key.
        if (root._conv) { root._conv.webSearchEnabled = false; root._conv.toolsEnabled = false; root._conv.verbatimPairs = 2 }
    }

    // [barista-fork] End the session on the barista's own sign-off: clear the flag FIRST (so nothing re-triggers),
    // then dismiss via the orchestrator — dismiss() flips state to "present", which fires onStateChanged →
    // _closeSession (stops the mic + TTS) and collapses back to the quiet edge tab. One dismiss path, timed to
    // land only after the sign-off finished speaking (or right after a muted/text reply renders).
    function _endSessionNow() {
        root._endAfterReply = false
        dismissFallbackTimer.stop()   // [barista-fork] we're closing now — cancel the safety-net timer
        root._pendingSpeech = ""   // [barista-fork] never let a held answer speak into a torn-down session
        root._leadinSpokenText = ""
        root._diag("session_end_now", {})   // [barista-fork] closure was previously inferable only by silence
        if (root._orch && typeof root._orch.dismiss === "function")
            root._orch.dismiss()
    }

    // [barista-fork] ROCK-SOLID CLOSE-OUT SAFETY NET. The normal goodbye path arms _endAfterReply and closes
    // once the sign-off finishes speaking (onSpeakingChanged). But if the sign-off never speaks or the
    // speaking→false transition never arrives (TTS error, empty/muted reply, a stuck speaking flag, a reply
    // that hangs mid-turn), _endAfterReply stays armed forever and the barista "won't close" (the reported
    // bug). This timer guarantees the session ends within a few seconds of the goodbye regardless. It is a UI
    // auto-dismiss timer (allowed), armed alongside _endAfterReply and cancelled the instant a real close
    // happens or a new utterance arrives.
    Timer {
        id: dismissFallbackTimer
        interval: 9000
        repeat: false
        onTriggered: {
            if (root._endAfterReply) {
                root._diag("dismiss_fallback_fired", {})
                root._endSessionNow()
            }
        }
    }

    // [barista-fork] Read-along auto-scroll: as the barista speaks a reply longer than the box, glide the text
    // from top to bottom so the user reads along instead of hand-scrolling. The native TTS exposes no word/
    // sentence progress, so this is PACED by an estimate of the speech length (≈ chars × ms/char), started when
    // the voice becomes audible and stopped when it stops or the user drags to scroll themselves. Approximate,
    // not word-synced (true sync needs the streaming-voice rework).
    NumberAnimation {
        id: speechScroll
        target: msgFlick
        property: "contentY"
        easing.type: Easing.Linear
    }
    function _startSpeechScroll() {
        if (typeof msgFlick === "undefined" || typeof msgText === "undefined") return
        var maxY = Math.max(0, msgText.implicitHeight - msgFlick.height)
        if (maxY <= 1) return   // fits in the box → nothing to scroll
        speechScroll.stop()
        speechScroll.from = msgFlick.contentY
        speechScroll.to = maxY
        // Prefer the native player's REAL clip duration so the scroll finishes exactly when the speech does
        // (Android). Fall back to a character-count estimate (~60ms/char ≈ natural pace) when it's unknown
        // (0 = non-native TTS / not yet reported); floor so very short overflow still eases rather than jumps.
        var realMs = (root._voice && root._voice.playbackDurationMs > 0) ? root._voice.playbackDurationMs : 0
        speechScroll.duration = realMs > 0 ? realMs : Math.max(1500, root._message.length * 60)
        speechScroll.start()
    }
    function _stopSpeechScroll() { speechScroll.stop() }

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
        root._message = ""
        root._pendingDisplayText = ""   // [barista-fork] fresh session → no held text from a prior one
        root._thinking = true
        root._awaitingContext = true
        root._closeOutRated = false
        root._fellBack = false
        root._pendingNext = null
        root._primed = false            // a new activation re-primes; the first user turn begins the session
        root._sessionBegun = false
        root._primedSystemPrompt = ""
        root._queuedFirstUtterance = ""
        root._awaitConfirm = false
        root._endAfterReply = false     // [barista-fork] a fresh engage never inherits a prior session's end-request
        dismissFallbackTimer.stop()     // [barista-fork] nor a stale close-out timer from a prior session
        root._pendingSpeech = ""        // [barista-fork] nor a held answer from a prior session
        // Grind is now DIRECT-SET on verbal approval (owner decision 2026-07-13 — "just set it, no button"),
        // so there is no off-machine grind to confirm and the approve chip never surfaces. Kept null here
        // (rather than deleting the chip) so the resolve/verbal-yes plumbing stays inert but intact.
        root._pendingGrind = null
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
        root._sessBrand = Settings.dye.dyeBeanBrand; root._sessType = Settings.dye.dyeBeanType; root._sessProf = prof
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

    // [barista-fork] PRIME the conversation: assemble the data + sessionContext into the SYSTEM PROMPT
    // (re-stamped each session, never trimmed) so the AI ALWAYS has it — then WAIT. beginSession() is
    // deferred to the user's first utterance in _send() (user-initiated model); it keeps the persisted
    // per-bean thread (prior discussion) and can't wipe it.
    function _askWithContext(dataBlock, unused) {
        root._awaitingContext = false
        if (root._state !== "conversing")   // BL-1: dismissed during context build → don't prime a dead session
            return
        if (!root._conv)
            return
        var who = root._settings ? root._settings.assistantName : "Coach"
        // [barista-fork] Phase 1 identity: address the ACTIVE user (dyeBarista wins — see _activeUserName), so a
        // guest who said "I'm Scott" is addressed as Scott, not the owner. The [Who] context block carries the
        // roster + honest-attribution rules.
        var name = root._activeUserName.length > 0 ? root._activeUserName : ""

        // [barista-fork] Ask→approve→apply flow. The barista PROPOSES a dial change and asks; only after the
        // user approves does it apply. On Anthropic it applies via the apply_dial_change tool; on other providers
        // there's no tool, so the app watches for the user's affirmative and applies the last proposal itself
        // (see the fenced-block fallback in _send). The persona differs so the model phrases things correctly.
        var _canApplyTool = typeof MainController !== "undefined" && MainController.aiManager
                            && MainController.aiManager.selectedProvider === "anthropic"
        var applyInstruction = _canApplyTool
            ? ("HOW CHANGES GET MADE — when you want to change the dial (grind, dose, ratio, or temp), or the "
               + "user asks for a specific value, FIRST propose it in your reply and ask for the go-ahead ('Want me to "
               + "take the grind to 4.4?'). Do NOT change anything yet. ONLY once the user clearly approves ('yes', "
               + "'do it', 'go ahead') do you call the apply_dial_change tool with just the field(s) that change. "
               + "THINK AND SET IN DOSE + RATIO, NOT YIELD — yield is just dose×ratio, so send doseG (grams IN) + ratio "
               + "and let the app compute the yield; name the shot type per SHOT TYPES above (the `ratio` field is still "
               + "the number). Fields: grinderSetting (off-machine grinder dial), doseG (grams in), ratio (e.g. 2.0 for "
               + "1:2.0), temperatureC; only send targetWeightG if the user gives an explicit grams-out. Never call it "
               + "unprompted, to acknowledge, or to restate unchanged settings. AFTER it runs, REPORT STRICTLY FROM THE "
               + "RESULT: confirm ONLY what's listed under 'applied' as done ('Done — grind's at 4.4 for the "
               + "next one.'); if anything is under 'failed' or 'rejected', tell them it did NOT take — NEVER claim a "
               + "change the result didn't confirm. A grind change is APPLIED right away like the rest — do NOT say it's "
               + "'queued' or tell them to go set it later; just confirm it's done (they'll dial their grinder as usual). "
               + "If the user says 'undo' or 'put it back', the app reverses the last change itself.\n")
            : ("HOW CHANGES GET MADE — when you want to change the dial (grind, dose, yield, ratio, or temp), or the "
               + "user asks for a specific value, FIRST propose it in your reply and ask for the go-ahead ('Want me to "
               + "take the grind to 4.4?'), and append EXACTLY ONE fenced block at the very END with ONLY the field(s) "
               + "that change:\n"
               + "```json\n{\"grinderSetting\":\"4.75\",\"doseG\":18.0,\"targetWeightG\":36.0,\"ratio\":2.0,\"temperatureC\":92.0,\"expectation\":\"less sour\"}\n```\n"
               + "grinderSetting = grinder dial (off-machine), doseG = grams IN, ratio = brew ratio e.g. 2.0 for 1:2.0 "
               + "(name the shot type per SHOT TYPES above; the ratio field is still the number), temperatureC = brew temp. "
               + "THINK IN DOSE + RATIO, NOT YIELD — yield is just dose×ratio, so send doseG + ratio and let the app "
               + "compute it; only send targetWeightG if the user gives an explicit grams-out. Give REAL numbers. The app applies the "
               + "proposed block ONLY after the user approves by voice ('yes'/'do it'); until then nothing changes. Do "
               + "NOT emit the block to acknowledge, to restate CURRENT settings unchanged, or in casual chat — only "
               + "when you're proposing a real change.\n")

        var persona = "You are " + who + ", " + (name.length ? name + "'s" : "the user's")
            + " friend behind the counter of their home espresso bar — a warm, curious person who happens to be a "
            + "great barista.\n"
            + "YOU ARE A GENUINELY HELPFUL GENERAL ASSISTANT who also happens to be an expert barista. Answer "
            + "WHATEVER the user asks — the weather, a general-knowledge question, help with something, a recipe, a "
            + "unit conversion — not only coffee. When the topic IS coffee, you're the coffee expert and you match "
            + "their depth; otherwise you just help, warmly and directly. Never deflect a non-coffee question back "
            + "to coffee.\n"
            + "Speak 1-2 short, conversational sentences — read aloud, so no markdown, lists, or long number sequences.\n"
            + "MATCH THE USER'S LANE. If they're being social — a guest, their morning, plans, 'my friend Scott is here "
            + "so I'm making two coffees' — respond like a friend: react genuinely, ask one natural follow-up, and remember "
            + "the people and occasions they mention so you can refer to them later ('how did Scott like his?'). Do NOT "
            + "steer casual talk back to dialing advice. Only coach when the topic is the coffee (they ask, they're about "
            + "to pull or just pulled a shot, or they report taste), or when something social makes coffee help genuinely "
            + "useful (two guests → offer to line up back-to-back shots) — and keep it light.\n"
            + "HOW A CONVERSATION STARTS — the USER always speaks first; you were quiet until they talked to you. "
            + "Your FIRST reply ANSWERS what they said. A greeting, if any, is a short RIDER folded into that reply, "
            + "never a turn of its own and never 'how can I help?'. Use the sessionContext.recency below to decide:\n"
            + "  • firstOfDay / firstEver → fold a short, warm hello into the front of your reply ('Morning! …'), "
            + "then answer. On firstEver you do NOT yet know who's at the machine (owner or guest) — stay name-free "
            + "until they tell you or it's obvious; then use "
            + (name.length ? name + "'s name" : "their name") + " naturally.\n"
            + "  • earlierToday → NO hello; at most a light one-clause nod ('Back for round two — ') IF "
            + "sessionContext.lightNodOk is true, then answer. Otherwise just answer.\n"
            + "  • ongoing → NO greeting at all; continue as if mid-conversation.\n"
            + "NEVER cold-greet, self-introduce, say your own name unprompted, re-introduce yourself, or open with "
            + "'how can I help'. You are a familiar presence, not a kiosk.\n"
            + "USING NAMES — the [Who] block below names the active user and who you already know. ALWAYS spell a person's "
               + "name the way it appears in [Who] (activeUser / known), NEVER the way the transcript spells it — speech "
               + "recognition can't tell 'Ana' from 'Anna' by sound and often picks the wrong one, so the [Who] roster "
               + "spelling is the authority (if [Who] says Ana but the transcript says Anna, use Ana). Use their name "
            + "naturally and warmly, but sparingly: a hello, a moment of agreement — never every sentence and never "
            + "robotically. When someone tells you who they are ('I'm Ana', 'this is Scott', 'Ana's making this "
            + "one'): if they're NEW to you (not in [Who] 'known'), warmly repeat the name back to confirm you heard "
            + "it right ('Ana — nice to meet you!') and THEN call set_active_user; if they're someone you already "
            + "know, just greet them by name ('Hey Ana!') and call set_active_user to switch. Speech mishears names, "
            + "so confirm a NEW one before you commit it. When the active user is a guest, not the owner, honor the "
            + "[Who] attribution rule: you know the machine's history but it's the OWNER's — never tell a guest they "
            + "pulled shots or have a history that isn't theirs.\n"
            + "VOICE RECOGNITION — a user message may begin with a line like '[voiceHint: heardVoice=Ana "
            + "confidence=confident]'. That is the app's ON-DEVICE voice recognition, NOT the user's words — NEVER "
            + "read it aloud, repeat it, or mention that you recognize voices. confidence=confident means Ana is "
            + "speaking now and the app has ALREADY made her the active user; if that's a change from who you were "
            + "addressing, just greet her by name naturally ('Hey Ana!') and use her history per the [Who] rules. "
            + "confidence=maybe means you're NOT sure — gently confirm before assuming ('Ana, is that you?') and only "
            + "call set_active_user once she says yes. A spoken correction ALWAYS wins over the voice guess: if "
            + "someone says 'no, I'm Chris', call set_active_user with Chris. Answer the user's actual words as normal.\n"
            + "TODAY'S OCCASION — if the context block has a \"todaysOccasion\" section (a US holiday and/or one of "
            + "the user's own saved dates), warmly acknowledge it ONCE when it's natural — folded into your greeting "
            + "on a firstOfDay/firstEver hello ('Morning — and happy Thanksgiving!'), or into your sign-off when the "
            + "chat wraps up. Keep it to a few words, genuine, never forced. It is part of the hello or goodbye, NOT "
            + "a separate proactive item and NOT a competing nudge (it never uses up the one-proactive-thing budget), "
            + "and NEVER raise it mid-conversation (recency 'ongoing') or more than once.\n"
            + "WHEN COACHING: the data block below is the app's LIVE DATABASE of this user's shots, dial-in history, best "
            + "recipes, and your own past advice: you DO have full access to it. NEVER say you lack their history, or "
            + "that this is their first shot, unless the block says 'recordedShots: 0'. The block's 'fullHistory' "
            + "field (total shots, the full earliest→latest date range, and per-bean counts) is the TRUE extent of "
            + "their history — treat it as authoritative; NEVER claim you only have recent shots or history back to "
            + "some recent date. Reference what you see, recall your past advice, and pick up where you left off.\n"
            + "DIALING FRAMEWORK — suggest ONE concrete change for the next shot when it helps. Read taste on two axes: "
            + "sour/sharp ↔ bitter/harsh, and weak/watery ↔ strong/intense. Sour+weak → grind finer (or lengthen the "
            + "ratio for more yield); bitter+strong → grind coarser (or shorten the ratio for less yield); nudge dose for strength. GUARD-RAIL: finer does "
            + "NOT always extract more — if a shot CHANNELS or CHOKES at a fine grind, going finer extracts LESS and less "
            + "evenly, so go COARSER (maybe a slightly lower dose), never chase it finer. Temperature is the LAST lever: "
            + "settle grind and ratio first, and don't fix sourness with temp before those — BUT if the profile was "
            + "designed around a temperature move, respect the profile's intent over this rule. Before recommending a "
            + "grind change, rule out: days off roast or a recent freeze→thaw (a bag under ~5 days runs fast and "
            + "unstable — don't chase it finer), dose consistency vs the last shots, and — if taste won't respond to "
            + "grind or ratio — water. Expect to grind finer as a bag ages. Adapt to their replies.\n"
            + "SHOT TYPES — talk about espresso by TYPE, not raw ratios. The types are RANGES, not points: "
            + "ristretto (short, ~1:1–1:1.5), normale (~1:2–1:2.5), lungo (~1:3 and longer). This user's own preset "
            + "dial-points: ristretto 1:" + Settings.brew.ratioPreset1.toFixed(1) + ", normale 1:" + Settings.brew.ratioPreset2.toFixed(1)
            + ", lungo 1:" + Settings.brew.ratioPreset3.toFixed(1) + ". LEAD with the type name whenever you describe, propose, "
            + "or confirm a shot ('let's make it a normale', 'that's pulling as a lungo'). Give a ratio NUMBER only when "
            + "fine-tuning WITHIN a type ('a normale, around 1:2.3') or when the user asks for the number. They're ranges "
            + "and bean-dependent, so never force a ratio into the wrong bucket or fake precision — between types, name the "
            + "nearest and qualify it ('a long normale, about 1:2.7'). When you APPLY a change, the tool still takes the numeric ratio.\n"
            + "TALKING ABOUT A SHOT — a shot is NOT a row of numbers, and you do NOT need to give every parameter "
            + "every time one comes up. Refer to a shot the way you'd say it out loud: 'that normale espresso on the "
            + "Kenya beans this morning', 'yesterday's lungo, ran a hair fast'. Lead with the drink TYPE plus a bean "
            + "or time cue, and bring in AT MOST the ONE number that actually matters to your point. NEVER recite dose, "
            + "yield, ratio, time and grind together — that's a spec sheet, not speech — even when the user asks about "
            + "several shots (describe the SHAPE: 'three normales this week, all pulling a touch long', not each one's "
            + "figures). The shot data gives you a 'descriptor' for each shot ('a lungo espresso on the Ethiopia beans') "
            + "— use it. The numeric fields are for YOUR reasoning and the apply/compare tools, NOT to read aloud; quote "
            + "a specific figure only when the user asks for it or when that figure IS the point ('grind's at 2.5 — let's "
            + "take it to 2.6'). CONTRAST — DON'T: 'that was a lungo, 18 grams in, 54 out, 1 to 3, 35 seconds, grind 2.7.' "
            + "DO: 'that lungo espresso on the Ethiopian beans — ran nice and long, right in the zone.'\n"
            + "JUST-PULLED SHOT: if sessionContext.justPulledShot is present, a shot finished a few minutes ago and "
            + "you already know it — do NOT announce it or ask 'how did it taste?' out of nowhere. Wait for the user. "
            + "When the user describes the taste ('that was sour', 'bit thin', 'perfect') OR gives a rating ('I'd "
            + "call that a 7', 'like an 80') THAT IS the tasting report — you do not need to ask for it, and they "
            + "never have to tap anything: speaking it is a complete path on its own. WHENEVER they give a taste "
            + "and/or a rating, "
            + "reliably call log_tasting_feedback (put their rating in overall_rating_0to100 when they give one, and "
            + "the taste axes you can infer) — the app records it on the shot itself for you. Do this WITHOUT "
            + "announcing the mechanics ('I've logged that', 'saved to your history'), but DO briefly confirm the "
            + "SUBSTANCE in your own words so they know you caught it ('Got it — a 7, touch sour'). Then PROPOSE ONE "
            + "concrete adjustment for the next shot, asking for the go-ahead — do NOT change anything until they "
            + "approve (see HOW CHANGES GET MADE below). Read taste on the two axes (sour/sharp ↔ bitter/harsh, "
            + "thin/watery ↔ strong/punchy) — that descriptor is the real dialing signal — but let them ramble; if "
            + "they just give a word or a number, take it and move on. Never nag, quiz, checklist, or recap. "
            + "Enjoyment (an optional 0–100 number) is SEPARATE from the taste and never required.\n"
            + "There is ALSO a tap way to log the very same thing, in case they ask how (\"how do I rate a shot / "
            + "log taste?\"): the post-shot review screen has a taste picker — tap Sour / Balanced / Bitter for "
            + "balance and Thin / Medium / Heavy for body — and the app offers that same quick taste tap before an "
            + "AI dial-in. Speaking it to you lands in the EXACT same place on the shot, so tell them either works "
            + "and they never NEED to tap. Only bring the tap UI up when they ask for it; never push it.\n"
            + applyInstruction
            + "VOICE — you are a person behind the counter, NOT a vending machine. Answer first (adjacency pairs: "
            + "respond to what they said before anything else). 1–2 short sentences. Minimal acknowledgment — 'got it', "
            + "not a restatement of what they told you. When you apply a change, confirm it ONCE, in your own natural "
            + "words ('Done — grind's at 4.4') — don't restate every field or repeat the confirmation. Use their name rarely. At most ONE question per "
            + "turn, and only if it's load-bearing — no checklists. Silence is fine: no filler, no 'let me know if you "
            + "need anything', no goodbye. Memory is politeness: follow up on last session's advice before offering new "
            + "advice. Keep matching their lane (social stays social).\n"
            + "EXAMPLE PHRASINGS (style anchors, not scripts):\n"
            + "  • firstOfDay: \"Morning! Two it is — you're at 4.5 on the Kenya, pulled sweet yesterday, I'd stay put. "
            + "Line them up back-to-back?\"\n"
            + "  • ongoing (within the hour): \"Same recipe? The last one was running just a hair fast.\" (no greeting, no name)\n"
            + "  • just-pulled, sour: \"Sour and thin — under-extracted. Want me to take the grind to 4.4? Milk could "
            + "use a couple more seconds of stretch too.\" (silent log; apply only once they say yes)\n"
            + "  • user approves: \"Done — grind's at 4.4 for the next one.\" (after apply_dial_change)\n"
            + "  • shot type (lead with the name): \"Want to stretch it to a lungo, around 1:2.9? Might open up that "
            + "florals.\" — not \"want to go to 1:2.9?\"\n"
            + "  • earlierToday: \"Back for round two — Ethiopian's a nice afternoon call. You were at 5.0 last time, "
            + "ran slow — want to go 5.2?\""

        // [barista-fork] RECIPES 2.0 (Fable design spec §7 — semantic rules; all user-facing phrasing stays yours).
        persona += "\nRECIPES: A recipe is a complete drink — a profile, a bean, grind, dose, yield, temperature, and "
            + "sometimes milk or hot water — activated as ONE unit; it is not a dial-in tweak. The context block's "
            + "[Recipes] section lists the active recipe and recent ones with their ids; only ever reference recipes by "
            + "an id you were given there or from list_recipes/get_active_recipe — never invent or guess one. To USE a "
            + "recipe (\"use my latte one\", \"switch to Morning Sun\"): resolve the reference to exactly ONE recipe — if "
            + "it could mean more than one, ask briefly which; if it clearly means one, don't interrogate. Then PROPOSE "
            + "it in one natural sentence — name it and say what changes: the profile loads (dropping any unsaved dial "
            + "tweaks), and ONLY if the recipe has milk, that the steam heater turns on and stays hot a few minutes. Call "
            + "activate_recipe ONLY after the user explicitly approves in this conversation; one approval covers one "
            + "activation. Activating reconfigures the machine; deactivating (\"go freestyle\") changes nothing physical, "
            + "so a direct request is approval enough. The machine's answer is the truth — report activation success or "
            + "failure from the tool result, and if it failed say plainly that nothing changed and why. If a dial change "
            + "you applied also updated the active recipe, mention it once, in passing. Don't lecture about how recipes "
            + "work — the user built them."

        // Proactivity level (user setting): what the assistant may VOLUNTEER (it always answers direct asks).
        var level = root._settings ? root._settings.proactivityLevel : "full"
        // Cooldown: on back-to-back shots of the SAME bean (within 6h) don't re-raise a proactive nudge.
        // An undiscussed just-pulled shot always allows a nudge — it's feedback on that shot, not a repeat.
        var mayNudge = (level !== "off") && root._settings
                       && (root._hasUndiscussedShot || root._settings.consumeProactiveNudge(root._bean, 6))
        if (level === "off")
            persona += "\nOnly answer what the user asks; do NOT volunteer suggestions unless asked."
        else if (mayNudge)
            persona += "\nBe proactive, but raise the SINGLE most useful thing — do NOT list multiple issues. "
                + "If the recent shots for this bean show 3+ attempts with no rating improvement, name the dialing "
                + "stall and propose a strategy change (a different variable, or a different profile) rather than "
                + "another micro-adjustment. Mention bean freshness/degassing only if clearly relevant."
                // [barista-fork] PROACTIVE RECIPE OFFER — ride the FIRST reply, never a cold greeting. The data
                // block's proactiveRecInputs (daysOffRoast + freshnessRead, storage/defrost state, recentShotNote),
                // alongside beanBestShot and recentTastingFeedbackOnThisBean, tell you when a recipe tweak is
                // genuinely worth raising (e.g. the bag has aged past its window → grind finer; a just-thawed
                // bag → hold a big move and re-dial gently). If — and ONLY if — one such tweak clearly helps AND
                // the moment fits (they've engaged you and coffee is fair game), fold ONE concrete offer into
                // your first reply, in plain coffee language, AFTER you answer whatever they said (answer first).
                + "\nPROACTIVE RECIPE OFFER: at most ONE tweak, phrased as a genuine offer with an easy, SPOKEN "
                + "way out — end it so 'no thanks' / 'leave it' is an obvious, no-pressure answer (e.g. \"Your "
                + "Kenya's out to twelve days now — want to nudge the grind a hair finer, or leave it where it "
                + "is?\"). It is an OFFER, not a plan: propose and ASK, change NOTHING until they clearly approve, "
                + "then apply per HOW CHANGES GET MADE. If they pass, drop it gracefully in a few words and do "
                + "not raise it again this session — silence is fine. Do NOT stack it on top of a just-pulled-shot "
                + "coaching turn (that turn already carries its own single suggestion), do NOT open with it before "
                + "answering them, and do NOT force it when they're being social — MATCH THE USER'S LANE wins. "
                + "Never invent dial numbers without an anchor in the data; if nothing clearly warrants a change, "
                + "offer nothing."
        else
            persona += "\nYou recently made a suggestion for this coffee, so don't re-raise it; only bring "
                + "something up if the user asks or the data has clearly changed."

        // [barista-fork] DUE REMINDERS & MAINTENANCE SURFACING. Deliberately gated ONLY on proactivityLevel
        // (off → never), NOT on the bean cooldown/consumeProactiveNudge above: a reminder the USER set up
        // ("remind me to flush the group head Saturday") shouldn't be suppressed just because they pulled the
        // same bean twice — the cooldown governs the barista's own recipe nudges, not the user's own reminders.
        // The context block carries a "Due now" section (reminders + maintenance) when anything is due.
        // ONE-PROACTIVE-THING-PER-TURN: reminders/maintenance and the recipe offer share a single budget — the
        // barista raises AT MOST ONE proactive thing per turn. Priority: a due reminder/maintenance item wins
        // over a recipe offer (the user explicitly asked to be reminded; a recipe tweak is the barista's idea).
        if (level !== "off")
            persona += "\nDUE REMINDERS & MAINTENANCE (PROACTIVE SURFACING): if the context block has a \"Due "
                + "now\" section, the user has a reminder or a maintenance task that's due. Raise the SINGLE most "
                + "pressing due item in your FIRST reply, AFTER you answer whatever they said — naturally, as a "
                + "friendly nudge, not an alarm ('Oh — you wanted to flush the group head today, by the way'). This "
                + "takes PRIORITY over the recipe offer above: raise a due item OR a recipe tweak, never both in "
                + "one turn, and never a pile — one proactive thing per turn, max. If they want to defer, that's "
                + "fine — don't nag, and don't re-raise the same item again this session. MAINTENANCE INTERVALS "
                + "follow Decent's DE1 cleaning guide but are user-adjustable defaults (see maintenanceNote): "
                + "mention a maintenance item as a gentle 'might be about time' the user can confirm or adjust in "
                + "settings — and for descaling, defer to their water (it's TDS-dependent), don't assert a fixed interval."

        // [barista-fork] MAINTENANCE-DOC CHANGE (PROACTIVE SURFACING). When the context block carries a
        // maintenanceDocChanged section, Decent's cleaning guide changed since it was last acknowledged. It is
        // rare and one-time, but it YIELDS to a due reminder/maintenance item — those can be overdue and
        // safety-relevant, and a one-off doc note must never starve them. It takes the single proactive slot
        // only when nothing is due this turn. Gated on proactivityLevel only (off → never surface it).
        if (level !== "off")
            persona += "\nMAINTENANCE-DOC CHANGE (PROACTIVE, LOWER priority than due items): if the context block "
                + "has a \"maintenanceDocChanged\" section, Decent updated their DE1 cleaning guide. Only raise it "
                + "when there is NO \"Due now\" reminder/maintenance item this turn (a due item always wins — defer "
                + "the doc change to a later turn). When you do raise it, in your FIRST reply, AFTER you answer "
                + "whatever the user said, briefly mention it and OFFER specific "
                + "default-interval updates you infer by comparing changedGuideText to currentDefaultSchedule "
                + "(e.g. 'Decent now suggests backflushing every 5 days instead of 7 — want me to update that?'). "
                + "Keep it to ONE proactive turn — raise the doc change OR a due item OR a recipe tweak, never a "
                + "pile. It is an OFFER with an easy spoken 'no thanks', NEVER auto-applied. On a yes, call "
                + "update_maintenance_default once per accepted task (it changes ONLY tasks still on their "
                + "default; anything the user customised is left untouched, and the tool will tell you if it "
                + "skipped one). On a 'no thanks' — or once you've applied everything they accepted — call "
                + "dismiss_maintenance_doc_change so it's not brought up again. For descaling, still defer to "
                + "their water rather than asserting a fixed interval."

        // Web search (Anthropic only) — keep the persona truthful about what it can/can't reach.
        var webOn = !!(root._settings && root._settings.webSearchEnabled)
                    && typeof MainController !== "undefined" && MainController.aiManager
                    && MainController.aiManager.selectedProvider === "anthropic"
        if (webOn)
            persona += "\nYou have live web search. AUTOMATICALLY use it for any real-time or factual question "
                + "you don't already know — current events, 'what is X', 'who is Y', a bean or roaster's tasting "
                + "notes, roast dates, brewing guides, gear — instead of guessing or saying you can't check. Do "
                + "NOT ask permission first ('want me to look that up?'); just search, then answer briefly from "
                + "what you find (at most once or twice per reply). BUT for three common questions you have FAST, "
                + "dedicated tools — use these INSTEAD of web search, they answer in about a second: get_weather "
                + "for the CURRENT WEATHER of a city (extract the city; omit it for 'weather around here' and it "
                + "uses the saved home location), get_stock_quote for a STOCK/ETF PRICE (you supply the ticker "
                + "symbol — map a company name to its symbol yourself), and get_local_news for NEWS / current "
                + "HEADLINES (pass a topic, or a city for local news; omit both for home-location local news). "
                + "Only fall back to web search for weather/stock/news if the fast tool returns an error. BE "
                + "ACCURATE about your reach: you can search public websites, but you can NOT log into the user's "
                + "accounts — Visualizer and Beanconqueror are apps, and their private uploads aren't something you "
                + "can query. The user's real shot history is the data block above, which IS live."
                // [barista-fork] Never VOLUNTEER a lookup. "Automatically use web search" above means when the
                // user's question actually needs it — it must NEVER trigger a lookup they didn't ask for. You are
                // a barista, not a general morning-briefing assistant: do NOT proactively look up weather, news,
                // stock prices, or search the web unprompted — ESPECIALLY on the greeting / first turn, where an
                // unrequested lookup adds a silent startup delay. Only reach for get_weather / get_stock_quote /
                // get_local_news / web search when the user EXPLICITLY asks for that information. When they do ask,
                // answer freely (this is about not volunteering lookups, never about refusing them)."
        else
            persona += "\nYou do NOT have web search in this session (it needs the Anthropic provider and the "
                + "web-search toggle on in settings). If asked about the weather, current events, or anything else "
                + "outside what you already know, say so briefly rather than guessing — for coffee questions, work "
                + "from the data block above."

        // query_shots client tool (Anthropic only) — the barista can pull ANY shot from the user's FULL local
        // history on demand, so it's never limited to the recent summary in the data block.
        var toolsOn = typeof MainController !== "undefined" && MainController.aiManager
                      && MainController.aiManager.selectedProvider === "anthropic"
        if (toolsOn)
            persona += "\nYou can look up the user's espresso shots from their FULL history at any time using the "
                + "query_shots tool — well beyond the recent summary in the data block. Use it whenever they ask "
                + "about a specific shot, a total count, or a bean/date range (e.g. \"my best shot on this bean\", "
                + "\"how many shots did I pull in June\", \"my very first shot\"). Reach for real data instead of "
                + "guessing, and never claim your history only goes back a few days — you can see all of it. "
                + "Each shot in the results has a shotId; when you want to actually diagnose or coach on one, call "
                + "get_shot_detail with that shotId to see the full dial-in and quality analysis (channeling, short "
                + "pour, grind or temperature issues, TDS, notes) before you give feedback on it. "
                + "Use compare_shots (2–5 shotIds) to see what changed between shots — the ratio/grind deltas and which "
                + "quality verdicts flipped — e.g. \"why is today worse than last week\" or \"did going coarser fix the "
                + "channeling\". Use get_bean_profile to pull ANY bean's freshness (days off roast / days since thaw) and "
                + "history, or ANY profile's design intent, when it's not the one already in your context. Use "
                + "detect_grind_drift when they ask why the same grind setting isn't pulling like it used to — it checks "
                + "whether shots at a fixed setting have drifted faster/slower over time (grinder wear or aging beans). "
                + "WHENEVER the user tells you how a shot TASTED or FELT — at close-out, mid-conversation, or an "
                + "unprompted 'that last one was sour' — call log_tasting_feedback with what you can extract (their words "
                + "in raw_text, plus any taste axes and a suggested adjustment you can infer). It saves to their private "
                + "feedback history so you can reason over it across sessions; for the CURRENT / just-pulled shot the "
                + "shot and dial are recorded automatically, so you don't supply a shot id. Don't announce that you're "
                + "logging it — just do it and keep the conversation natural. RATING A PAST SHOT: the user can also "
                + "rate or adjust an EARLIER shot by voice ('update my rating for this morning's shot', 'that one from "
                + "last Tuesday was actually bitter') — for that, FIRST find the shot with query_shots (its result has a "
                + "shotId), then call log_tasting_feedback with that shotId passed as shot_id, so the rating/taste lands "
                + "on THAT shot instead of the latest one. Still never ASK how a shot tasted unprompted — only log what "
                + "the user volunteers or what they explicitly ask you to record. The current bean's recent feedback is "
                + "already in your data block (recentTastingFeedbackOnThisBean); reach for search_tasting_feedback only "
                + "for a filtered lookup (e.g. 'when did I last call this sour') or feedback on a DIFFERENT bean."
                + "\nENDING THE CHAT — when the user clearly signals the conversation is over (a genuine wrap-up or "
                + "farewell — 'that'll be all', 'thanks, I'm good', 'I'm done', 'that's it for now', 'bye', 'see "
                + "you later', 'ok got it thanks'), give a SHORT warm sign-off ('Anytime — enjoy!') and call the "
                + "end_conversation tool in the SAME reply. The app speaks your sign-off first and only then closes "
                + "the panel, so the user never has to hit stop. Judge intent: do NOT end on a mid-conversation "
                + "'thanks' that's clearly followed by more (a question, another request) — only on a real close-out. "
                + "NEVER call it unprompted or to end a chat the user hasn't wrapped up."
                // [barista-fork] Reminder + maintenance TOOL usage — gated on tool availability (Anthropic client
                // tools), NOT on proactivityLevel: creating/clearing a reminder is a DIRECT ask the barista always
                // honors even when proactivity is off. (The proactive SURFACING of due items is gated separately
                // above on proactivityLevel.)
                + "\nREMINDERS & MAINTENANCE TOOLS — when the user asks you to remind them of something (\"remind "
                + "me to flush the group head on Saturday\", \"remind me to descale next month\"), call "
                + "create_reminder: resolve their timing to an ISO 8601 due using sessionContext.today (pick a "
                + "sensible morning hour if they don't give a time), pass their exact wording in userPhrasing, set "
                + "recurrence only if they clearly want it to repeat, then confirm briefly. When the user says a "
                + "reminder or maintenance task is handled (\"done\", \"already flushed it\", \"backflushed this "
                + "morning\"), clear it: complete_reminder with the reminderId, or log_maintenance with the taskKey "
                + "(both from the \"Due now\" block or a list_due_reminders result). Use list_due_reminders for an "
                + "explicit \"what do I need to do\" question. Only log_maintenance for a task that appears in the "
                + "Due now block — never invent a task or claim an authoritative interval."
                + "\nPERSONAL DATES — when the user tells you a personal day worth remembering (\"remember my "
                + "anniversary is June 3\", \"my birthday is October 12\"), call add_personal_date: resolve their "
                + "words to a month (1-12) and day (1-31), add a year only if they pinned a specific one (otherwise "
                + "leave it out so it recurs yearly), give a short label ('anniversary', 'Mom's birthday'), and "
                + "confirm briefly. On the day itself it'll show up in todaysOccasion so you can wish them well."
                // [barista-fork] "Don't leave the user in silence" — a real lookup (weather, stocks, news, web
                // search, or pulling shots from history) takes a moment, and the app SPEAKS what you write BEFORE
                // the tool runs. So when — and ONLY when — you're about to call a tool or look something up,
                // FIRST say a brief, natural acknowledgment in your OWN words, then call the tool in the SAME
                // turn. Keep it to a few words and VARY it every time so it never sounds canned ('let me pull
                // that up', 'one sec, checking', 'hmm, let me find out', 'give me a moment'). Make it a STATEMENT,
                // never a question — do NOT ask permission ('want me to look that up?'), just acknowledge and go.
                // For anything you can answer instantly from what you already know, skip it and just answer — the
                // acknowledgment is only for a genuine lookup, so there's never dead air while a tool runs.
                + "\nBEFORE A LOOKUP (no dead air) — a real lookup takes a moment (get_weather, get_stock_quote, "
                + "get_local_news, web search, query_shots / get_shot_detail and the other history tools). The app "
                + "speaks whatever you write BEFORE the tool runs, so whenever you're about to call one of these "
                + "tools, FIRST say a SHORT, natural acknowledgment in your OWN words — then call the tool in the "
                + "same reply. Vary it every time so it never sounds scripted ('let me pull that up', 'one sec, "
                + "checking', 'hmm, let me find out', 'give me a moment on that'). Make it a brief STATEMENT, not a "
                + "question, and never ask permission. Do this ONLY when a real lookup is actually coming — for "
                + "anything you already know, just answer straight away with no lead-in. "
                + "CRITICAL — NO DOUBLE-TALK: that pre-tool acknowledgment is spoken aloud to the user the instant "
                + "you write it, so it must be ONLY a few words of acknowledgment — never the answer, never data, "
                + "numbers, names, or findings. AFTER the tool result comes back, do NOT repeat or rephrase your "
                + "acknowledgment — the user already heard it; continue with ONLY the new information (if the "
                + "lookup failed, say just that, briefly). And don't silently re-run a lookup that already failed "
                + "unless the user asks again."

        // dataBlock is the pre-formatted, combined context (dial-in + bean profile + profile guidance).
        var block = (dataBlock && dataBlock.length > 0) ? dataBlock : "recordedShots: 0"

        // [barista-fork] sessionContext — the runtime facts the persona's greeting/close-out rules read.
        // recency drives whether a greeting is even warranted; justPulledShot lets "that was sour" land on
        // a barista who already knows a shot finished. This is CONTEXT, never an instruction to speak first.
        // Recency is computed in the orchestrator (C++, single source of truth); QML delegates.
        var bucket = (root._orch && typeof root._orch.recencyBucket === "function")
                     ? root._orch.recencyBucket() : root._recencyBucket()
        var minsSince = (root._orch && typeof root._orch.minutesSinceLastExchange === "function")
                        ? root._orch.minutesSinceLastExchange() : -1
        // The light "back for round two" nod is at most once per calendar day (earlierToday only).
        var lightNodOk = (bucket === "earlierToday" && root._settings
                          && typeof root._settings.consumeLightGreetForToday === "function")
                         ? root._settings.consumeLightGreetForToday() : false
        // [barista-fork] Absolute "now" so the model can resolve natural-language reminder due dates
        // ("Saturday", "tomorrow 8am") into an ISO datetime for create_reminder. Local wall-clock.
        var _now = new Date()
        var _pad = function(n) { return (n < 10 ? "0" : "") + n }
        var _todayIso = _now.getFullYear() + "-" + _pad(_now.getMonth() + 1) + "-" + _pad(_now.getDate())
        var _weekdayNames = ["Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"]
        var sessionCtx = "sessionContext:\n"
            + "  recency: " + bucket + "   (ongoing<60min | earlierToday | firstOfDay | firstEver)\n"
            + "  minutesSinceLastChat: " + (minsSince < 0 ? "never" : String(minsSince)) + "\n"
            + "  partOfDay: " + root._partOfDay() + "\n"
            + "  today: " + _todayIso + " (" + _weekdayNames[_now.getDay()] + ") "
            + _pad(_now.getHours()) + ":" + _pad(_now.getMinutes())
            + "   (use this to resolve reminder timing like \"Saturday\" or \"tomorrow\" into an ISO due)\n"
            + "  lightNodOk: " + (lightNodOk ? "true" : "false") + "\n"
        // A shot the barista already knows about (the old close-out, now pure context). shotDiscussed is
        // false until the user talks taste; the model must NOT proactively ask about it.
        if (root._hasUndiscussedShot) {
            var agoMin = root._orch.lastShotAtMs > 0
                         ? Math.max(0, Math.round((Date.now() - root._orch.lastShotAtMs) / 60000)) : 0
            sessionCtx += "  justPulledShot: { minutesAgo: " + agoMin + ", discussed: false }\n"
        }

        // New-bean note: no history for this bean → the bean profile is the starting point (not "first shot ever").
        var firstOnBean = (block.indexOf("recordedShots: 0") >= 0)
        if (firstOnBean)
            sessionCtx += "  firstShotOnThisBean: true   (use the bean profile to suggest a starting point)\n"
        // Outstanding off-machine grind the user agreed to but never confirmed — fold into context so the
        // barista can raise it naturally (no synthetic kickoff exists to carry it anymore).
        if (root._pendingGrind && root._pendingGrind.value)
            sessionCtx += "  unconfirmedGrindSetting: " + root._pendingGrind.value
                        + "   (user agreed to set this but hasn't confirmed — ask early if it came up)\n"

        root._conv.webSearchEnabled = webOn   // barista session only; reset by ask()/resetInMemory()
        root._conv.toolsEnabled = toolsOn     // barista session only; the query_shots opt-in (reset the same way)
        root._conv.verbatimPairs = 8          // keep more of the chat verbatim so casual context survives the session
        // PRIME AND WAIT: assemble the full system prompt but DO NOT begin the Claude session. The user
        // speaks first — the first _send() calls beginSession(primed, userText). No synthetic kickoff, no
        // machine-first turn. The barista is present-but-quiet until talked to.
        root._primedSystemPrompt = persona + "\n\n" + sessionCtx + "\n" + block
        root._primed = true
        root._sessionBegun = false
        root._thinking = false   // nothing is thinking — we're waiting on the user, not the model
        // [barista-fork] If the user already spoke while we were building context (tap-and-talk), replay that
        // first utterance now that we're primed — it begins the session with their real words (never dropped).
        if (root._queuedFirstUtterance.length > 0) {
            var q = root._queuedFirstUtterance
            root._queuedFirstUtterance = ""
            root._send(q)
        }
    }

    function _send(text) {
        var t = (text || "").trim()
        if (t.length === 0 || !root._conv)
            return
        if (root._state !== "conversing")   // B3: dismissed → don't keep sending (e.g. late voice finals)
            return
        // [barista-fork] tap-chat-and-talk: the user may speak DURING the context build (before the session
        // is primed). Don't drop those words — hold the first utterance and replay it once primed. (Only the
        // FIRST is queued; a second while still building is ignored, same as any mid-thinking utterance.)
        if (root._awaitingContext) {
            if (root._queuedFirstUtterance.length === 0)
                root._queuedFirstUtterance = t
            return
        }
        if (root._thinking)   // a turn is already in flight → drop (mirrors the prior behavior)
            return
        // [barista-fork] Stuck-flag guard: a new user utterance means the conversation is continuing, so any
        // pending self-dismiss from a prior turn is void — clear it so it can never collapse this later turn.
        root._endAfterReply = false
        dismissFallbackTimer.stop()   // [barista-fork] a new turn cancels any pending close-out safety timer
        root._leadinSpokenText = ""   // [barista-fork] fresh turn → no stale lead-in for the repeat guard
        // [barista-fork] New turn supersedes any held display text (a prior turn whose audio never started must
        // not linger or ambush this turn's reveal). _message is cleared on the model path below (shows "…").
        root._pendingDisplayText = ""
        var hasActions = (typeof Barista !== "undefined" && Barista.actions)
        // [barista-fork] CLEAR whole-string farewell ("that's it for now", "bye", …). Conservative exact match
        // (like _isUndo) so a mid-chat "thanks" that's followed by more never trips it.
        var _sendIsAnthropic = typeof MainController !== "undefined" && MainController.aiManager
                               && MainController.aiManager.selectedProvider === "anthropic"
        var _f = t.toLowerCase().replace(/[^a-z0-9'\s]/g, " ").replace(/\s+/g, " ").trim()
        var _isFarewell = _f === "bye" || _f === "bye bye" || _f === "goodbye"
                          || _f === "see you" || _f === "see ya" || _f === "see you later"
                          || _f === "that's all" || _f === "thats all"
                          || _f === "that'll be all" || _f === "thatll be all"
                          || _f === "that's it" || _f === "thats it"
                          || _f === "that's it for now" || _f === "thats it for now"
                          || _f === "i'm done" || _f === "im done" || _f === "all done"
                          || _f === "ok thanks" || _f === "okay thanks"
                          || _f === "thanks that's all" || _f === "thanks thats all"
                          || _f === "got it thanks" || _f === "ok got it thanks" || _f === "okay got it thanks"
        if (_isFarewell) {
            if (_sendIsAnthropic) {
                // [barista-fork] The "that's it for now" HANG fix: let the model speak its OWN natural sign-off
                // (owner's no-canned-strings rule), but ARM the session close so it ends after that reply even if
                // the model forgets to call end_conversation. Armed after the clear above; onResponseReceived /
                // onSpeakingChanged close the session once the sign-off finishes. Fall through to the model turn.
                root._endAfterReply = true
                dismissFallbackTimer.restart()   // [barista-fork] safety net: close even if the sign-off never speaks
            } else {
                // Non-Anthropic providers can't self-dismiss via a tool → handle the goodbye locally (no round-trip).
                var byeMsg = TranslationManager.translate("barista.bye", "Anytime — enjoy!")
                root._message = byeMsg
                if (root._orch && typeof root._orch.markExchangeCompleted === "function")
                    root._orch.markExchangeCompleted()
                root._speakSanitised(byeMsg)
                if (root._voice && root._voice.speaking) { root._endAfterReply = true; dismissFallbackTimer.restart() }
                else root._endSessionNow()
                return
            }
        }
        // [barista-fork] Spoken "undo" — the safety net for the ask→approve→apply flow. A SHORT undo phrase
        // ("undo", "undo that", "put it back") reverses the last applied change locally, gated on canUndo() so
        // "undo" with nothing to undo passes through to the model. Handled BEFORE the model turn (like the grind
        // confirm), so it never round-trips. One level (the last change) is enough.
        if (hasActions && Barista.actions.canUndo()) {
            var _u = t.toLowerCase().replace(/[^a-z0-9'\s]/g, " ").trim()
            var _isUndo = _u === "undo" || _u === "undo that" || _u === "undo it"
                          || _u === "put it back" || _u === "revert" || _u === "never mind that"
            if (_isUndo) { root._undoLast(); return }
        }
        // Off-machine grind reminder confirm — ONLY intercept yes/no when the assistant's last line names the
        // pending grind VALUE as a WHOLE number (SF-6/SF-R3-3). A raw substring of a short value like "8" would
        // match "18 grams" and falsely record a grind on a "yes" to a dose question. The chip is the reliable path.
        var _gv = root._pendingGrind ? String(root._pendingGrind.value || "") : ""
        var _grindAsked = _gv.length > 0 && root._message
                && new RegExp("(^|[^0-9.])" + _gv.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "([^0-9.]|\\.(?![0-9])|$)").test(root._message)
        if (hasActions && _grindAsked) {
            var g = Barista.actions.parseConfirmation(t)   // 1 yes / 0 no / -1 neither
            if (g === 1) { root._resolveGrind(true); return }
            if (g === 0) { root._resolveGrind(false); return }
            // [barista-fork] Neither yes nor no — the user moved on. Drop the CHIP so it can't linger contextlessly
            // (the queue record + unconfirmedGrindSetting context stay, so the model can re-raise in its own words).
            if (root._pendingGrind) { root._pendingGrind = null; root._diag("grind_chip_cleared", { reason: "newUtterance" }) }
        }
        // Apply-on-confirm for a pending recommendation ("OK" → apply; "no" → skip).
        if (hasActions && root._awaitConfirm && root._pendingNext) {
            var c = Barista.actions.parseConfirmation(t)
            if (c === 1) { root._applyPending(); return }
            if (c === 0) { root._skipPending(); return }
            root._awaitConfirm = false; root._pendingNext = null   // ambiguous → disarm + drop the stale chip (S11)
        }
        // PERSISTENCE + CUE-CLEAR: when there's an undiscussed just-pulled shot and the user's reply
        // DESCRIBES the taste, that's the report. Detect it once here so we can (a) clear the P3
        // undiscussed-shot cue on BOTH provider paths and (b) on the NON-Anthropic path, write the taste
        // onto the shot record. On the Anthropic path the model calls log_tasting_feedback itself (the
        // feedback→KB tool), so we must NOT double-write the shot note — only clear the cue.
        var _isAnthropic = typeof MainController !== "undefined" && MainController.aiManager
                           && MainController.aiManager.selectedProvider === "anthropic"
        if (root._hasUndiscussedShot && !root._closeOutRated && root._orch && root._orch.lastShotId > 0
                && typeof MainController !== "undefined" && MainController.shotHistory) {
            // Only capture from a SUBSTANTIVE reply (not "ok"/"hang on") and match whole words, so "no good"
            // isn't scored as good and a one-word confirmation doesn't overwrite the notes (S9).
            var w = t.toLowerCase().replace(/[^a-z0-9'\s]/g, " ").split(/\s+/)
            var isConfirmation = hasActions && Barista.actions.parseConfirmation(t) >= 0
            // Only treat the reply as taste feedback if it actually describes the shot — otherwise a social
            // remark ("Scott's here, two coffees") would burn the one-shot capture on non-taste text.
            var TASTE = ["sour", "bitter", "burnt", "balanced", "good", "great", "perfect", "nice", "delicious",
                "love", "lovely", "bad", "thin", "watery", "harsh", "weak", "strong", "rich", "smooth", "sweet",
                "sweeter", "acidic", "fruity", "chocolate", "chocolatey", "nutty", "bright", "muddy", "astringent",
                "sharp", "punchy", "intense",
                "tasty", "taste", "tasted", "tastes", "flavor", "flavour", "crema", "balance", "shot"]
            var hasTaste = w.some(function(x) { return TASTE.indexOf(x) >= 0 })
            if (!isConfirmation && w.length >= 2 && hasTaste) {
                root._closeOutRated = true
                // The shot has been discussed → clear the undiscussed-shot cue (both provider paths).
                if (root._orch && typeof root._orch.markShotDiscussed === "function")
                    root._orch.markShotDiscussed()
                if (!_isAnthropic) {   // non-Anthropic has no log_tasting_feedback tool → write the shot note here
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
        }
        root._message = ""   // [barista-fork] model turn dispatched → drop the prior reply so "…" shows, never stale text
        root._thinking = true
        if (root._voiceInput && root._voiceInput.listening) root._voiceInput.pauseMic()
        root._stampTurn()
        // [barista-fork] Voice-ID Increment 2: prepend a compact voice hint to the MODEL text only (never to
        // `t`, which the intercepts above match on, and never displayed). The identify already ran synchronously
        // at onFinalText, so heardName is fresh. A CONFIDENT match already switched the active user app-side; the
        // hint tells the model to greet them / attribute honestly. MAYBE → the model gently confirms first.
        // One-shot: consumed here so it never bleeds into a later turn.
        var _modelText = t
        if (root._settings && root._settings.voiceIdEnabled && typeof Barista !== "undefined" && Barista.voiceId
                && Barista.voiceId.heardConfidence && Barista.voiceId.heardConfidence !== "none") {
            _modelText = "[voiceHint: heardVoice=" + Barista.voiceId.heardName
                       + " confidence=" + Barista.voiceId.heardConfidence + "]\n" + t
            Barista.voiceId.consumeHeard()
        }
        // [barista-fork] User-initiated: the FIRST utterance of a primed session BEGINS the Claude
        // conversation with the user's real words as the first turn (no synthetic kickoff). Subsequent
        // turns follow up. The persona's greeting rules fold any hello into this first reply.
        if (root._primed && !root._sessionBegun) {
            root._sessionBegun = true
            root._primed = false
            if (!root._conv.beginSession(root._primedSystemPrompt, _modelText)) {   // busy (rare) → recover, don't wedge
                root._sessionBegun = false
                root._primed = true
                root._thinking = false
                root._message = TranslationManager.translate("barista.err",
                    "Something went wrong — tap Chat or type to try again.")
            } else {
                root._beginSlowOpWatch()   // [barista-fork] Part B: model turn dispatched → arm the 5s cue
            }
        } else {
            root._conv.followUp(_modelText)
            root._beginSlowOpWatch()       // [barista-fork] Part B: model turn dispatched → arm the 5s cue
        }
    }

    // [barista-fork] A NEW shot arrived (the orchestrator flips shotDiscussed→false) → re-arm the one-shot
    // taste capture. The overlay now persists across pages, so _closeOutRated can't rely on _startConversation
    // to reset it between back-to-back shots in one session (else the 2nd shot's taste is never captured and
    // the cue never clears).
    Connections {
        target: root._orch
        function onShotDiscussedChanged() {
            if (root._orch && !root._orch.shotDiscussed)
                root._closeOutRated = false
        }
        // [barista-fork] The barista called end_conversation (Anthropic self-dismiss on a spoken goodbye). Don't
        // collapse instantly — arm _endAfterReply and let the sign-off (already in this same reply) speak first.
        // onResponseReceived / onSpeakingChanged then ends the session once the sign-off finishes (never cut off).
        // If we're not actually in a live conversation, ignore it (a stray late emit must not dismiss nothing).
        function onDismissRequested() {
            if (root._state === "conversing") {
                root._endAfterReply = true
                dismissFallbackTimer.restart()   // [barista-fork] guarantee the close even if the sign-off never speaks
            }
        }
    }

    // [barista-fork] engage (→ Conversing) → PRIME the context and open the mic (user-initiated: the user
    // tapped/started talking). dismiss (→ Present) → close the session, back to the quiet tab. The barista
    // never speaks first — priming assembles the system prompt and waits for the first utterance.
    Connections {
        target: root._orch
        function onStateChanged() {
            if (!root._orch) return
            if (root._orch.state === "conversing") {
                root._collapsed = false     // expand the panel
                root._startConversation()   // warms context + primes the system prompt; waits for first utterance
                root._engageOpenMic()       // tap-chat-and-talk: the mic opens with the panel (after the engage test, if on)
            } else {   // "present"
                root._showSettings = false
                root._closeSession()
            }
        }
    }
    // [barista-fork] On (re)creation, catch up to the orchestrator's state (the overlay may be recreated on
    // page navigation). If a conversation is live, prime-and-wait is cheap+idempotent (no model call), so we
    // discriminate on whether the persisted per-bean thread already has a reply: none → re-prime; some →
    // restore the last line. (No SF-3 latch needed anymore — priming is safe to repeat.)
    Component.onCompleted: {
        if (root._state !== "conversing")
            return
        // [barista-fork] Barista engaged → nudge a sleeping BT/USB speaker awake with a subtle tone so the first
        // utterance isn't clipped while it powers up. Honors mute; no-op on the built-in speaker.
        if (root._voice && typeof root._voice.playWakeTone === "function")
            root._voice.playWakeTone()
        // Restore onto THIS bean's thread (an advisor visit on another page may have switched the key).
        var prof2 = (typeof ProfileManager !== "undefined") ? ProfileManager.currentProfileName : ""
        prof2 = prof2.replace(/^\*/, "").replace(/ \(modified\)$/, "")
        if (typeof MainController !== "undefined" && MainController.aiManager && root._conv && !root._conv.busy)
            MainController.aiManager.switchConversation(Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType, prof2)
        var lastLine = root._conv ? root._stripBlock(root._conv.lastResponse || "") : ""
        if (lastLine.length > 0) {   // thread already has history → restore the last line, keep the session
            root._message = lastLine
            root._sessionBegun = true
            root._thinking = !!(root._conv && root._conv.busy)
        } else {   // nothing said yet → (re)prime and wait for the first utterance
            root._startConversation()
        }
    }
    // B3: if the user navigates away mid-chat, the Loader destroys us — close the mic/TTS session first.
    Component.onDestruction: root._closeSession()
    // [barista-fork] Tablet woke from the screensaver → a BT/USB speaker may have slept during it. If a barista
    // session is live, nudge it awake so the next utterance isn't clipped. (Engaging fresh is covered by
    // Component.onCompleted; this covers a session that survived a sleep.)
    Connections {
        target: (typeof ScreensaverManager !== "undefined") ? ScreensaverManager : null
        ignoreUnknownSignals: true
        function onScreensaverActiveChanged() {
            if (!ScreensaverManager.screensaverActive && root._state === "conversing"
                && root._voice && typeof root._voice.playWakeTone === "function")
                root._voice.playWakeTone()
        }
    }
    // Claude's replies → show + speak (once each).
    Connections {
        target: root._conv
        ignoreUnknownSignals: true
        // [barista-fork] Part A — the model's short pre-tool lead-in ("let me pull that up"), emitted BEFORE
        // the tool/search runs. Speak it IMMEDIATELY so there's no dead air, and show it as the working line.
        // This is NOT the turn's answer — _thinking stays true, the mic stays paused, and the real reply still
        // arrives via onResponseReceived (which replaces this line). Marking _spokeThisTurn cancels the Part B
        // 5s cue (a natural verbal acknowledgment always wins over the non-verbal fallback). Model-generated
        // and varied — no hard-coded string anywhere.
        function onInterimReceived(text) {
            if (root._state !== "conversing")
                return
            if (root._awaitingContext)
                return
            var clean = root._stripBlock(text)
            if (clean.length === 0)
                return
            // [barista-fork] Text-on-speak: hold the lead-in text until the voice is audible (onAudibleChanged
            // reveals it). If muted (nothing will be spoken), show it now so a muted reply still displays.
            if (!root._voice || !root._settings || !root._settings.voiceEnabled) root._message = clean
            else root._pendingDisplayText = clean
            root._leadinSpokenText = clean  // [barista-fork] remember it — the post-tool answer must not repeat it
            root._markSpokeThisTurn()      // suppress the Part B cue + stop the 5s timer
            if (root._voiceInput && root._voiceInput.listening) root._voiceInput.pauseMic()
            root._diag("interim_leadin", { chars: clean.length, speakingNow: root._voice ? root._voice.speaking : false })
            root._speakSanitised(text)     // speak the model's OWN words now, before the tool result lands
        }
        function onResponseReceived(response) {
            if (root._state !== "conversing")   // BL-1: reply landed after dismiss (or it's an advisor turn) → ignore
                return
            if (root._awaitingContext)   // N-R3-2: a preempted turn's reply during our context build → not ours
                return
            // [barista-fork] Text-on-speak: hold the answer text until the voice is audible (revealed in
            // onAudibleChanged); show immediately only if muted (nothing will be spoken).
            var _answerText = root._stripBlock(response)   // hide the JSON action block from the display
            // [barista-fork] Repeat guard: if this post-tool answer just restates the already-spoken lead-in,
            // don't speak it twice. Show the (fuller) text directly and skip the TTS below.
            var _dupLeadin = root._answerDupOfLeadin(_answerText)
            if (_dupLeadin) {
                root._message = _answerText
                root._pendingDisplayText = ""
                root._diag("answer_skipped_duplicate_of_leadin", { chars: _answerText.length, leadinChars: root._leadinSpokenText.length })
            } else if (!root._voice || !root._settings || !root._settings.voiceEnabled) {
                root._message = _answerText
            } else {
                root._pendingDisplayText = _answerText
            }
            root._thinking = false
            root._markSpokeThisTurn()
            root._cancelSlowOpWatch()   // [barista-fork] the real answer is here → stop the still-working heartbeat
            // [barista-fork] Recency stamp: this reply completes a real user↔barista EXCHANGE (the session
            // only ever begins on the user's first utterance now), so mark it — the single source of truth
            // for greeting cadence next time. Routed through the orchestrator (one write path for lastExchangeAt).
            if (root._orch && typeof root._orch.markExchangeCompleted === "function")
                root._orch.markExchangeCompleted()
            // [barista-fork] Every reply is now a reply TO THE USER (no unprompted machine-first greeting),
            // so speak it per voiceEnabled — the old greetAloud-suppression of an opener no longer applies.
            if (root._voiceInput && root._voiceInput.listening) root._voiceInput.pauseMic()
            root._diag("response_received", { chars: (response || "").length, speakingNow: root._voice ? root._voice.speaking : false, endAfter: root._endAfterReply })
            // [barista-fork] Don't cut off a still-playing lead-in: if the barista is mid-sentence on its
            // pre-tool lead-in, hold the answer and speak it when that finishes (onSpeakingChanged drains it).
            // Skip the TTS entirely if the answer just duplicates the lead-in (already spoken + shown above).
            if (_dupLeadin) {
                // no-op: the lead-in already said this; text is displayed, nothing more to speak.
            } else if (root._voice && root._voice.speaking) {
                root._pendingSpeech = response
                root._diag("response_deferred_until_leadin_done", { chars: (response || "").length })
            } else {
                root._speakSanitised(response)           // (also strips fenced blocks before TTS)
            }
            root._resetSilence()   // keep the mic session alive while we're conversing
            // [barista-fork] Ask→approve→apply. On Anthropic the model applies via the apply_dial_change tool
            // itself once the user approves — so we must NOT also arm the app-side voice-confirm (that would
            // double-apply on "yes"). On NON-tool providers there's no tool, so we keep the app-side fallback:
            // hold the proposed structuredNext as pending; if the user's next utterance is a clear affirmative,
            // _send applies it. Require an ACTIONABLE field (not a bare "expectation" / an echo of current dial).
            var _anthropic = typeof MainController !== "undefined" && MainController.aiManager
                             && MainController.aiManager.selectedProvider === "anthropic"
            if (!_anthropic) {
                var nx = root._conv ? root._conv.structuredNextForLastAssistantTurnMap() : null
                if (root._nextDiffersFromCurrent(nx)) {
                    root._pendingNext = nx        // a real proposal → arm voice-approve ("yes" → apply)
                    root._awaitConfirm = true
                } else {
                    root._pendingNext = null      // no actionable proposal this turn → disarm
                    root._awaitConfirm = false
                }
            } else {
                root._pendingNext = null          // tool path: the model owns applying; never arm app-side
                root._awaitConfirm = false
            }
            // Turn done. If it will speak, `speaking` is already true → skip; onSpeakingChanged(false)
            // reopens the mic when playback truly ends. If nothing will speak, reopen now — UNLESS paused
            // (a muted reply landing during a hold-pause must not silently reopen the held mic).
            if (root._voiceInput && root._voiceInput.listening && !root._paused
                    && (!root._voice || !root._voice.speaking))
                root._voiceInput.resumeMic()
            // [barista-fork] The barista called end_conversation this turn (dismissRequested armed _endAfterReply,
            // and its sign-off is in THIS reply). If the reply is speaking, onSpeakingChanged(false) will end the
            // session when playback finishes (never cutting off the sign-off). If voice is muted/off (nothing to
            // speak), the sign-off is already rendered → end now. Mirrors the muted-resume idiom just above.
            if (root._endAfterReply && (!root._voice || !root._voice.speaking))
                root._endSessionNow()
        }
        function onErrorOccurred(error) {   // B2: never hang on "…" — surface it and recover the UI
            if (root._state !== "conversing")
                return
            if (root._awaitingContext)   // N-R3-2: a preempted turn's error during our context build → not ours
                return
            root._thinking = false
            root._cancelSlowOpWatch()   // [barista-fork] Part B: the turn failed → stop the 5s cue timer
            root._message = (error && error.length > 0)
                ? error
                : TranslationManager.translate("barista.err", "Something went wrong — tap Chat or type to try again.")
            // [barista-fork] The user asked to end (e.g. "that's it for now") but the closing turn errored/timed
            // out → honor the intent and close instead of hanging open with the flag armed.
            if (root._endAfterReply) {
                root._endSessionNow()
                return
            }
            // Reopen the mic if a session is live (the turn failed, not the session) — unless the user paused.
            if (root._voiceInput && root._voiceInput.listening && !root._paused
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
        visible: root._state === "conversing" && !root._showSettings && !root._collapsed   // P3 makes this the expanded dock
        // Right-docked side panel: leaves the machine controls usable on the left, gives the
        // conversation room to grow, and is out of the way (recommended tablet-assistant UX).
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spacingMedium
        // Nearly full-height panel (small top/bottom margin) — more presence, and it covers the top bar's
        // right edge so nothing (e.g. the Sleep button) pokes out above or below the card.
        anchors.topMargin: Theme.spacingSmall
        anchors.bottomMargin: Theme.spacingSmall
        width: root._panelWidth   // driven by the panelWidthMode setting (narrow/medium/wide)
        radius: Theme.cardRadius
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor

        Accessible.role: Accessible.StaticText
        Accessible.name: msgText.text

        // [barista-fork] Panel backdrop for the screensaver: when the barista is EXPANDED over the screensaver,
        // taps on EMPTY areas within the panel must NOT fall through to the screensaver's full-screen wake
        // MouseArea underneath (which would wake the DE1). This eats taps within the PANEL bounds only. It's the
        // first child (below cardCol), so buttons/fields/flickable above it still receive their events normally.
        // Taps OUTSIDE the panel are unaffected → they still hit the screensaver and wake as the owner wants.
        // Gated to the screensaver so normal (non-screensaver) panel behavior is completely unchanged.
        MouseArea {
            anchors.fill: parent
            visible: root._screensaverActive
            enabled: root._screensaverActive
            onClicked: {}   // eat the tap (accepted by default) so it never reaches the screensaver wake below
            onPressed: {}
        }

        ColumnLayout {
            id: cardCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: Theme.spacingLarge
            spacing: Theme.spacingMedium

            // Header — the NAME gets its own line (kept at full size), and the controls (gear / collapse /
            // dismiss) sit on a SEPARATE right-aligned line below it, so the tiny icons never overlap the name.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.scaled(2)
                Text {
                    text: root._settings ? root._settings.assistantName
                                         : TranslationManager.translate("barista.title", "Coach")
                    Layout.fillWidth: true
                    elide: Text.ElideRight   // safety for an unusually long name; normal names show in full
                    color: Theme.textColor
                    font: Theme.subtitleFont
                    Accessible.ignored: true
                }
                // Controls on their own line. `icon.color` pins them to full-contrast textColor: the `subtle`
                // default is primaryContrastColor, invisible on a LIGHT surface card (and 40% alpha when
                // disabled). Gear opens settings; → collapses to the edge tab; × dismisses.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.scaled(6)
                    Item { Layout.fillWidth: true }   // push the controls to the right
                    AccessibleButton {
                        subtle: true
                        icon.source: "qrc:/icons/settings.svg"
                        icon.color: Theme.textColor
                        accessibleName: TranslationManager.translate("barista.settings.open", "Assistant settings")
                        onClicked: root._showSettings = true
                    }
                    AccessibleButton {
                        subtle: true
                        text: "→"   // collapse to a thin edge tab, freeing the whole screen
                        icon.color: Theme.textColor
                        accessibleName: TranslationManager.translate("barista.collapse", "Collapse assistant")
                        onClicked: { root._showSettings = false; root._collapsed = true }
                    }
                    AccessibleButton {
                        subtle: true
                        text: "×"
                        icon.color: Theme.textColor
                        accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                        onClicked: if (root._orch) root._orch.dismiss()
                    }
                }
            }

            // [barista-fork] The character face — so the user can watch + listen instead of reading. Driven by
            // the voice/thinking/listening state; greet() fires on activation (see onStateChanged).
            BaristaAvatar {
                id: avatar
                visible: root._settings && root._settings.avatarEnabled
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 0
                // Sits up top under the header, sized for presence WITHOUT dominating the card — the message
                // text is the focus and must keep at least ~3 lines (msgFlick.Layout.minimumHeight). Capped to
                // the panel width; floored so it can't compute negative. maximumHeight = the size so a vertical
                // layout never stretches it, and lets the layout yield this space to the text on a short card.
                Layout.preferredWidth: Math.max(Theme.scaled(48),
                                                Math.min(Theme.scaled(132), root._panelWidth - Theme.spacingLarge * 2))
                Layout.preferredHeight: Layout.preferredWidth
                Layout.maximumHeight: Layout.preferredWidth
                // [barista-fork] Drive the mouth off `audible` (real audio out), NOT `speaking` — otherwise the
                // avatar starts talking during the network→prepare gap before any sound (the "avatar talks
                // before voices are heard" complaint). `speaking` still gates the mic; only the VISUAL syncs here.
                mode: (root._voice && root._voice.audible) ? "speaking"
                    : root._thinking ? "thinking"
                    : (root._voiceInput && root._voiceInput.listening && !root._voiceInput.paused) ? "listening"
                    : "idle"
                // [barista-fork] Part B non-verbal cue: a more pronounced thinking beat once a slow op has run
                // ~5s with nothing spoken (set by slowOpTimer). Only meaningful while actually thinking.
                thinkingCue: root._thinkingCue && root._thinking
            }

            // The assistant's line (or a thinking indicator) — fills the panel so the input pins to the bottom.
            // Secondary when the character is shown (the user doesn't need to read everything), but still visible
            // so errors, the action chip, and the listening partial stay readable.
            // [barista-fork] Long answers must SCROLL within the panel — a bare Text with fillHeight
            // overflowed its box and rendered ON TOP OF the chip/input below it (the "text writing over
            // itself"). Bound + clip it in a Flickable; reset to the top on each new answer so it reads
            // top-first, and the user can drag to scroll a long reply.
            // [barista-fork] Keep at least ~3 lines of the barista's message visible while the read-along
            // scroll runs — a 1.5-line window scrolling is distracting. FontMetrics tracks whichever font
            // msgText uses (body when the avatar shows, subtitle otherwise). This is a Layout MINIMUM, so the
            // text box grows to fill spare height and only floors at 3 lines when the card is tight (the layout
            // then yields the avatar's space, which is why the avatar caps its own maximumHeight).
            FontMetrics { id: msgMetrics; font: msgText.font }
            Flickable {
                id: msgFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: Math.ceil(msgMetrics.height * 3) + Theme.scaled(2)
                clip: true
                contentWidth: width
                contentHeight: msgText.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.VerticalFlick
                // [barista-fork] User grabbed the text to scroll themselves → cancel the read-along auto-scroll
                // so it doesn't fight them.
                onDraggingChanged: if (dragging) root._stopSpeechScroll()
                // Visible, draggable vertical scrollbar (Job 3 consistent-scrolling pattern).
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Text {
                    id: msgText
                    width: msgFlick.width
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.WordWrap
                    // Always full-contrast primary text (was dimmed to textSecondaryColor whenever the
                    // avatar showed — i.e. almost always — which made the barista's own words hard to read).
                    // Font sizing is left as it was (avatar-hidden path keeps its larger subtitle font).
                    color: Theme.textColor
                    font: avatar.visible ? Theme.bodyFont : Theme.subtitleFont
                    text: (root._thinking && root._message.length === 0)
                          ? TranslationManager.translate("barista.thinking", "…")
                          : root._message
                    onTextChanged: msgFlick.contentY = 0   // a new answer → show it from the top
                }
            }

            // [barista-fork] Off-machine grind reminder chip ONLY. The recommendation Apply/Skip modal path was
            // DELETED — dial changes now go ask→approve→apply (the barista proposes verbally, the user approves
            // by voice/text, then it applies via the apply_dial_change tool or the app-side affirmative fallback).
            // The grinder is physical, so its "did you set it?" Yes/No confirm stays a chip.
            ActionConfirmChip {
                id: actionChip
                Layout.fillWidth: true
                visible: root._pendingGrind && root._pendingGrind.value ? true : false
                grindMode: true
                grindValue: root._pendingGrind ? (root._pendingGrind.value || "") : ""
                onApplied: root._resolveGrind(true)
                onSkipped: root._resolveGrind(false)
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

            // [barista-fork] Voice-first control bar: Chat/Stop · Pause/Resume · keyboard toggle. The typed
            // input (field + Send) is hidden behind the keyboard icon — voice is the primary path, typing is a
            // fallback for noisy rooms / accessibility (kept, not removed).
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
                        else {
                            // Barge-in: an explicit tap-to-speak takes priority — silence any greeting/TTS
                            // still playing so the assistant never talks over the user opening the mic.
                            if (root._voice) root._voice.stop()
                            root._paused = false   // an explicit Chat tap ends any pause
                            root._voiceInput.start(); silenceTimer.restart()
                        }
                    }
                }

                // [barista-fork] Pause / Resume — holds the conversation without ending it. Behavior follows the
                // pauseMode setting: "hold" just pauses the mic; "freeze" also stops any speech + the hum. Only
                // an explicit Resume tap clears _paused (auto-resume paths are guarded on !_paused).
                AccessibleButton {
                    visible: (root._voiceInput && root._voiceInput.listening) || root._paused
                    subtle: true
                    primary: root._paused
                    text: root._paused ? TranslationManager.translate("barista.mic.resume", "Resume")
                                       : TranslationManager.translate("barista.mic.pause", "Pause")
                    accessibleName: root._paused
                          ? TranslationManager.translate("barista.mic.resumeAccessible", "Resume the conversation")
                          : TranslationManager.translate("barista.mic.pauseAccessible", "Pause the conversation")
                    onClicked: {
                        if (!root._voiceInput) return
                        if (!root._paused) {
                            root._paused = true
                            root._voiceInput.pauseMic()
                            // "freeze" mode also silences in-progress speech + the thinking hum; "hold" leaves
                            // any playing reply alone and just holds the mic.
                            if (root._settings && root._settings.pauseMode === "freeze" && root._voice) {
                                root._voice.stop()
                                root._voice.stopThinkingLoop()
                            }
                        } else {
                            root._paused = false
                            if (root._voiceInput.listening) root._voiceInput.resumeMic()
                            else root._voiceInput.start()
                            root._resetSilence()
                        }
                    }
                }

                // [barista-fork] Voice-only: no typed input. The barista is driven entirely by voice (Chat +
                // Pause); the text field + Send + keyboard fallback were removed at the owner's request. A
                // trailing spacer keeps Chat/Pause left-aligned.
                Item { Layout.fillWidth: true }
            }

            // [barista-fork] Card footer — fully decluttered: NO volume/speed knobs on the card face. A single
            // "Barista options" button opens the per-role popup, where a General/Coaching dot-selector scopes
            // the voice dropdown + volume + speed for each voice independently. Keeps the card face to just the
            // conversation + Chat/Pause; every voice knob lives one tap away in Barista options (or gear → Voice).
            AccessibleButton {
                Layout.fillWidth: true
                text: TranslationManager.translate("barista.baristaOptions", "Barista options")
                accessibleName: text
                onClicked: root._selectBaristaOpen = true
            }
        }
    }

    // ---- Assistant settings card (right-docked, mirrors the conversation card) -------------------
    // [barista-fork] The gear in the conversation-card header opens this. It hosts AssistantSettingsPanel
    // (voice PROVIDER native/openai/elevenlabs, ElevenLabs key + saved-voices list + add-voice, names,
    // bell, mute). Same right-dock geometry as `card`; a Back button returns to the conversation.
    Rectangle {
        id: settingsCard
        visible: root._showSettings && !root._collapsed
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spacingMedium
        // Same width as the conversation panel (driven by panelWidthMode) so the reserved right strip is
        // consistent whether chatting or in settings; the tabbed settings content scrolls within it.
        width: root._panelWidth
        radius: Theme.cardRadius
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor

        Accessible.role: Accessible.Grouping
        Accessible.name: TranslationManager.translate("barista.settings.title", "Assistant")

        // [barista-fork] Screensaver tap-eater (mirrors the conversation card's backdrop): while the barista
        // is expanded over the screensaver, taps on empty settings area must NOT fall through to the
        // full-screen screensaver wake beneath (which would wake the DE1). First child, below settingsCol.
        MouseArea {
            anchors.fill: parent
            visible: root._screensaverActive
            enabled: root._screensaverActive
            onClicked: {}
            onPressed: {}
        }

        ColumnLayout {
            id: settingsCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: Theme.spacingLarge
            spacing: Theme.spacingMedium

            // Header: back → conversation
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.scaled(6)
                AccessibleButton {
                    subtle: true
                    icon.source: "qrc:/icons/back.svg"   // no unicode-glyph icons (CLAUDE.md); ← isn't a safe glyph
                    accessibleName: TranslationManager.translate("common.button.back", "Back")
                    onClicked: root._showSettings = false
                }
                Text {
                    text: TranslationManager.translate("barista.settings.title", "Assistant")
                    Layout.fillWidth: true
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                    Accessible.ignored: true
                }
            }

            // [barista-fork] AssistantSettingsPanel is now a TABBED, transparent Rectangle: a tab bar over a
            // StackLayout of three pages, each its own Flickable. It no longer self-caps its height (the old
            // single-scroll design did) — its internal ColumnLayout anchors.fill and the active tab's Flickable
            // fills the remaining space and scrolls. So it MUST be given a bounded, fill-height box: fill this
            // container. Its own "×" emits closed() → back to the conversation, like Back.
            AssistantSettingsPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                onClosed: root._showSettings = false
            }
        }
    }

    // [barista-fork] BARISTA OPTIONS picker — a compact per-role voice/volume/speed chooser opened from the
    // card footer's "Barista options" button. A General/Coaching dot-selector scopes every control to the
    // conversational OR the coaching voice (both fully independent in settings — this surfaces them one tap
    // away, and is now the ONLY on-card path to the voice knobs since the card-face sliders were removed). The
    // voice dropdown REFLECTS the selected role's current provider (elevenlabs saved / openai / native) — it
    // does not switch providers (that stays in full Settings). Volume applies live; speed on the next utterance.
    Item {
        id: selectBaristaLayer
        anchors.fill: parent
        visible: root._selectBaristaOpen
        z: 100   // above the conversation + settings cards
        // On open, re-establish the role-scoped slider bindings + resync the voice dropdown (a prior drag may
        // have broken a binding, and the picker may re-open on the other role than it closed on).
        onVisibleChanged: if (visible) { selectBaristaCard._rebindSliders(); voiceBox._sync() }

        // Scrim — a tap outside the card dismisses the picker (the "click-away to close" a Popup would give).
        MouseArea {
            anchors.fill: parent
            onClicked: root._selectBaristaOpen = false
        }

        Rectangle {
            id: selectBaristaCard
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: Theme.spacingMedium
            width: root._panelWidth
            implicitHeight: pickerCol.implicitHeight + Theme.spacingLarge * 2
            height: implicitHeight
            radius: Theme.cardRadius
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.borderColor

            Accessible.role: Accessible.Grouping
            Accessible.name: TranslationManager.translate("barista.baristaOptions", "Barista options")

            // Which voice these controls adjust: "barista" (conversational) or "coaching" (live cues).
            property string pickerRole: "barista"
            readonly property bool _isBarista: pickerRole === "barista"
            readonly property var _roleVoice: _isBarista ? root._voice : root._coachingVoice
            // The role's active provider — drives the voice dropdown's model + selection.
            readonly property string _prov: root._settings
                ? (_isBarista ? root._settings.ttsProvider : root._settings.coachingTtsProvider)
                : "native"

            // Re-establish the slider bindings (a drag breaks a value: binding, and a role flip needs the
            // OTHER role's value) — called on open + whenever the role changes.
            function _rebindSliders() {
                pickVolume.value = Qt.binding(function() {
                    return root._settings ? (_isBarista ? root._settings.baristaVoiceVolume
                                                        : root._settings.coachingVoiceVolume) : 1.0 })
                pickSpeed.value = Qt.binding(function() {
                    return root._settings ? (_isBarista ? root._settings.baristaVoiceSpeed
                                                        : root._settings.coachingVoiceSpeed) : 1.0 })
            }
            onPickerRoleChanged: { _rebindSliders(); voiceBox._sync() }
            Component.onCompleted: _rebindSliders()

            // Absorb taps on empty card area so they don't fall through to the dismiss-scrim below. First
            // child (below pickerCol) so the interactive controls still receive their events.
            MouseArea { anchors.fill: parent; onClicked: {} }

            ColumnLayout {
                id: pickerCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.spacingLarge
                spacing: Theme.spacingMedium

                // Header: title + close
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: TranslationManager.translate("barista.baristaOptions", "Barista options")
                        Layout.fillWidth: true
                        color: Theme.textColor
                        font: Theme.subtitleFont
                        Accessible.ignored: true
                    }
                    AccessibleButton {
                        subtle: true
                        text: "×"   // × is a CLAUDE.md-safe glyph
                        accessibleName: TranslationManager.translate("common.button.close", "Close")
                        onClicked: root._selectBaristaOpen = false
                    }
                }

                // Role dot-selector — scopes every control below to the barista OR coaching voice.
                Text {
                    text: TranslationManager.translate("barista.adjusting", "Adjusting:")
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                    Accessible.ignored: true
                }
                Repeater {
                    model: [
                        { role: "barista",  label: TranslationManager.translate("barista.roleGeneral", "General") },
                        { role: "coaching", label: TranslationManager.translate("barista.roleCoaching", "Coaching") }
                    ]
                    delegate: Item {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.scaled(34)
                        readonly property bool _sel: selectBaristaCard.pickerRole === modelData.role
                        RowLayout {
                            anchors.fill: parent
                            spacing: Theme.spacingSmall
                            // The "dot"
                            Rectangle {
                                Layout.preferredWidth: Theme.scaled(18)
                                Layout.preferredHeight: Theme.scaled(18)
                                radius: width / 2
                                color: "transparent"
                                border.width: 2
                                border.color: _sel ? Theme.primaryColor : Theme.borderColor
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width * 0.5
                                    height: width
                                    radius: width / 2
                                    color: Theme.primaryColor
                                    visible: _sel
                                }
                            }
                            Text {
                                text: modelData.label
                                Layout.fillWidth: true
                                color: Theme.textColor
                                font: Theme.bodyFont
                                Accessible.ignored: true
                            }
                        }
                        AccessibleMouseArea {
                            anchors.fill: parent
                            accessibleRole: Accessible.RadioButton
                            accessibleChecked: _sel
                            accessibleName: modelData.label
                            onAccessibleClicked: selectBaristaCard.pickerRole = modelData.role
                        }
                    }
                }

                // Voice dropdown — provider-aware (elevenlabs saved / openai / native voices for THIS role).
                Text {
                    text: TranslationManager.translate("barista.voice", "Voice")
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                    Accessible.ignored: true
                }
                ComboBox {
                    id: voiceBox
                    Layout.fillWidth: true
                    Accessible.name: TranslationManager.translate("barista.voice", "Voice")
                    model: {
                        if (!root._settings) return []
                        if (selectBaristaCard._prov === "elevenlabs") {
                            var out = []
                            var vs = root._settings.elevenlabsVoices
                            for (var i = 0; i < vs.length; i++)
                                out.push(vs[i].name && String(vs[i].name).length > 0
                                         ? String(vs[i].name)
                                         : TranslationManager.translate("barista.voiceFav.slot", "Voice %1").arg(i + 1))
                            return out
                        }
                        if (selectBaristaCard._prov === "openai")
                            return ["nova", "shimmer", "alloy", "echo", "fable", "onyx"]
                        return selectBaristaCard._roleVoice ? selectBaristaCard._roleVoice.availableVoices : []
                    }
                    // Set currentIndex from the role's saved selection — the model+selection both swap on a role
                    // flip, and a ComboBox won't self-correct its integer index, so recompute explicitly.
                    function _sync() {
                        if (!root._settings) { currentIndex = -1; return }
                        if (selectBaristaCard._prov === "elevenlabs") {
                            var id = selectBaristaCard._isBarista ? root._settings.elevenlabsVoiceId
                                                                  : root._settings.coachingElevenlabsVoiceId
                            var vs = root._settings.elevenlabsVoices
                            var idx = -1
                            for (var i = 0; i < vs.length; i++) if (vs[i].id === id) { idx = i; break }
                            currentIndex = idx
                        } else if (selectBaristaCard._prov === "openai") {
                            var v = selectBaristaCard._isBarista ? root._settings.openaiVoice
                                                                 : root._settings.coachingOpenaiVoice
                            currentIndex = model.indexOf(v)
                        } else {
                            var name = selectBaristaCard._roleVoice ? selectBaristaCard._roleVoice.voiceName : ""
                            currentIndex = model.indexOf(name)
                        }
                    }
                    onActivated: {
                        if (!root._settings || currentIndex < 0) return
                        if (selectBaristaCard._roleVoice) selectBaristaCard._roleVoice.stop()   // barge-in
                        if (selectBaristaCard._prov === "elevenlabs") {
                            var vs = root._settings.elevenlabsVoices
                            var id = vs[currentIndex] ? vs[currentIndex].id : ""
                            if (selectBaristaCard._isBarista) root._settings.elevenlabsVoiceId = id
                            else root._settings.coachingElevenlabsVoiceId = id
                        } else if (selectBaristaCard._prov === "openai") {
                            if (selectBaristaCard._isBarista) root._settings.openaiVoice = currentText
                            else root._settings.coachingOpenaiVoice = currentText
                        } else if (selectBaristaCard._roleVoice) {
                            selectBaristaCard._roleVoice.setVoiceByName(currentText)
                        }
                        if (selectBaristaCard._roleVoice) selectBaristaCard._roleVoice.preview()   // audition
                    }
                    onModelChanged: _sync()
                    Component.onCompleted: _sync()
                    Connections {
                        target: selectBaristaCard._roleVoice
                        function onAvailableVoicesChanged() { voiceBox._sync() }
                    }
                }

                // Volume — role-scoped, applied live (a drag is heard immediately on the right voice).
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingSmall
                    Text {
                        text: TranslationManager.translate("barista.volume", "Volume")
                        color: Theme.textSecondaryColor
                        font: Theme.labelFont
                        Layout.preferredWidth: Theme.scaled(56)
                        Accessible.ignored: true
                    }
                    Slider {
                        id: pickVolume
                        Layout.fillWidth: true
                        from: 0.0; to: 1.0; stepSize: 0.02
                        Accessible.name: TranslationManager.translate("barista.volume", "Volume")
                        onMoved: {
                            if (!root._settings) return
                            if (selectBaristaCard._isBarista) root._settings.baristaVoiceVolume = value
                            else root._settings.coachingVoiceVolume = value
                            if (selectBaristaCard._roleVoice) selectBaristaCard._roleVoice.applyLiveVolume()
                        }
                    }
                    Text {
                        text: Math.round(pickVolume.value * 100) + "%"
                        color: Theme.textSecondaryColor
                        font: Theme.labelFont
                        Layout.preferredWidth: Theme.scaled(38)
                        horizontalAlignment: Text.AlignRight
                    }
                }

                // Speed — role-scoped, applied on the next utterance (matches the settings speed slider).
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingSmall
                    Text {
                        text: TranslationManager.translate("barista.speed", "Speed")
                        color: Theme.textSecondaryColor
                        font: Theme.labelFont
                        Layout.preferredWidth: Theme.scaled(56)
                        Accessible.ignored: true
                    }
                    Slider {
                        id: pickSpeed
                        Layout.fillWidth: true
                        from: 0.7; to: 1.3; stepSize: 0.05
                        Accessible.name: TranslationManager.translate("barista.speed", "Speed")
                        onMoved: {
                            if (!root._settings) return
                            if (selectBaristaCard._isBarista) root._settings.baristaVoiceSpeed = value
                            else root._settings.coachingVoiceSpeed = value
                        }
                    }
                    Text {
                        text: pickSpeed.value.toFixed(2) + "×"
                        color: Theme.textSecondaryColor
                        font: Theme.labelFont
                        Layout.preferredWidth: Theme.scaled(38)
                        horizontalAlignment: Text.AlignRight
                    }
                }

                // Preview — audition the selected role's voice at its current volume/speed.
                AccessibleButton {
                    Layout.fillWidth: true
                    text: TranslationManager.translate("barista.preview", "Preview")
                    accessibleName: text
                    onClicked: {
                        if (selectBaristaCard._roleVoice) {
                            selectBaristaCard._roleVoice.stop()
                            selectBaristaCard._roleVoice.preview()
                        }
                    }
                }
            }
        }
    }

    // [barista-fork] ALWAYS-PRESENT collapsed dock — the quiet persistent presence. Shows whenever the panel
    // isn't expanded (i.e. present, OR conversing-but-collapsed) and settings aren't open. Tap = the whole
    // "tap chat and talk" gesture: engage the barista (opens the conversation + mic) or, if already
    // conversing, just re-expand. A subtle pulse bids for attention when a just-pulled shot is undiscussed
    // (replaces the old spoken close-out) — gated on proactivityLevel so "off" never nags.
    Item {
        id: edgeTab
        // [barista-fork] Hidden during the screensaver-collapse state — the faint drifting avatar takes over
        // there (burn-in safety). Otherwise the normal collapsed-dock condition (present, or conversing-collapsed).
        visible: !root._showSettings && !(root._state === "conversing" && !root._collapsed)
                 && !root._screensaverDock
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        // [barista-fork] A polished PULL-TAB, not a box: hugs the avatar, but its inner (left) side is
        // rounded while the screen-edge (right) side is flush/square, and it carries a soft drop shadow so
        // it reads as lifted off the edge rather than rammed against it. Drawn as a Shape (below) since a
        // Rectangle can't round only two corners. The avatar size is owner-adjustable (avatarTabSize:
        // small/medium/large → 96/120/156 px); the tab HUGS it (avatar + an even ~16px margin). This is a
        // LIVE binding so the tab resizes the moment the setting changes. Default "medium" (120) — smaller
        // than the old hard-coded 156 "large", per the owner.
        readonly property real _avatarPx: {
            var s = root._settings ? root._settings.avatarTabSize : "medium"
            if (s === "small") return Theme.scaled(96)
            if (s === "large") return Theme.scaled(156)
            return Theme.scaled(120)   // "medium" (default)
        }
        width: edgeTab._avatarPx + Theme.scaled(16)
        height: edgeTab._avatarPx + Theme.scaled(16)

        // Tab background: rounded-left / flush-right path + a soft shadow for depth. strokeColor carries the
        // attention pulse (primary when a shot is undiscussed). Verified shape/winding via a render mock.
        Shape {
            id: tabBg
            anchors.fill: parent
            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.30)
                shadowBlur: 0.9
                shadowHorizontalOffset: -Theme.scaled(4)
                shadowVerticalOffset: Theme.scaled(3)
                autoPaddingEnabled: true
            }
            ShapePath {
                fillColor: Theme.surfaceColor
                strokeColor: edgeTab._pulseCue ? Theme.primaryColor : Theme.borderColor
                strokeWidth: 1
                startX: tabBg.width; startY: 0
                PathLine { x: Theme.cardRadius; y: 0 }
                PathArc  { x: 0; y: Theme.cardRadius; radiusX: Theme.cardRadius; radiusY: Theme.cardRadius; direction: PathArc.Counterclockwise }
                PathLine { x: 0; y: tabBg.height - Theme.cardRadius }
                PathArc  { x: Theme.cardRadius; y: tabBg.height; radiusX: Theme.cardRadius; radiusY: Theme.cardRadius; direction: PathArc.Counterclockwise }
                PathLine { x: tabBg.width; y: tabBg.height }
                PathLine { x: tabBg.width; y: 0 }
            }
        }

        // Non-verbal bid for attention: pulse the tab border/dot when a shot is undiscussed and the user's
        // proactivity level allows a cue at all ("off" = never). This is the ONLY thing that "speaks" for a
        // finished shot now — no unprompted TTS.
        readonly property bool _pulseCue: root._hasUndiscussedShot && root._settings
                                          && root._settings.proactivityLevel !== "off"

        Accessible.role: Accessible.Button
        Accessible.name: root._pulseCue
            ? TranslationManager.translate("barista.expand.shot", "Talk to the assistant about your last shot")
            : TranslationManager.translate("barista.expand", "Talk to the assistant")
        Accessible.focusable: true
        Accessible.onPressAction: root._tapDock()

        // The avatar as the tab face — a familiar presence, not a chevron. Falls back to the app icon glyph.
        Loader {
            id: tabAvatar
            anchors.centerIn: parent
            // Nudge up slightly: the avatar art sits low within its box, so a pure center reads
            // as "too low" in the tab. A small negative offset visually centers it.
            anchors.verticalCenterOffset: -Theme.scaled(6)
            width: edgeTab._avatarPx; height: edgeTab._avatarPx   // owner-sized; the tab hugs it, no dead space
            active: root._settings && root._settings.avatarEnabled
            source: "qrc:/qml/assistant/BaristaAvatar.qml"
            onLoaded: if (item) item.mode = "idle"
        }
        Image {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -Theme.scaled(6)
            visible: !(root._settings && root._settings.avatarEnabled)
            source: "qrc:/icons/barista.svg"
            sourceSize.height: edgeTab._avatarPx * 0.58   // fallback glyph tracks the avatar size (~90 at 156)
            fillMode: Image.PreserveAspectFit
            Accessible.ignored: true
        }

        // The attention pulse — a soft breathing highlight on the tab while a shot is undiscussed.
        SequentialAnimation on opacity {
            running: edgeTab._pulseCue
            loops: Animation.Infinite
            NumberAnimation { from: 1.0; to: 0.55; duration: 900; easing.type: Easing.InOutSine }
            NumberAnimation { from: 0.55; to: 1.0; duration: 900; easing.type: Easing.InOutSine }
            onRunningChanged: if (!running) edgeTab.opacity = 1.0
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root._tapDock()
        }
    }

    // [barista-fork] SCREENSAVER PRESENCE — the faint drifting avatar. Replaces the edge tab while the
    // screensaver is up and the dock is collapsed. Minimal chrome (just the avatar, or a small dot if avatars
    // are off), low opacity (~0.4) so it barely lights the LCD and rides the hardware backlight dim. It's
    // repositioned every ~30s (fade out → hop to the next safe position → fade in) so no pixel stays lit
    // continuously — burn-in safety. It's STATIONARY between hops so it stays easy to tap. Tapping it engages
    // the barista OVER the screensaver; the tap is consumed by this MouseArea (accepted by default), so it does
    // NOT reach the screensaver's full-screen wake MouseArea below → the DE1 stays asleep.
    Item {
        id: driftingAvatar
        visible: root._screensaverDock
        // Sized for a FORGIVING tap target on the screensaver: the item (and its fill MouseArea) is the
        // touch area, deliberately large (~2.5× the old 44px) so tapping near the icon opens the barista
        // instead of missing and hitting the screensaver's wake — waking the machine by accident.
        width: Theme.scaled(224)
        height: Theme.scaled(224)
        opacity: 0.4

        // Cycle through a fixed set of scattered positions inside a safe inset (Math.random may be unavailable
        // in some QML contexts, so we drive position from a rotating index — deterministic and always spread).
        // Fractions are kept well inside the edges (~15–80%) and avoid the dead center where screensaver content
        // usually sits. Computed from the LIVE overlay width/height, not hardcoded px.
        property int _driftIndex: 0
        readonly property var _driftXFracs: [0.18, 0.72, 0.30, 0.80, 0.22, 0.65]
        readonly property var _driftYFracs: [0.20, 0.28, 0.75, 0.68, 0.55, 0.18]
        function _placeAt(i) {
            if (root.width <= 0 || root.height <= 0) {
                // Overlay not laid out yet (e.g. created directly into screensaver-dock state):
                // retry once it has a size so the avatar never sits stuck at 0,0. Converges as
                // soon as layout gives a non-zero width.
                Qt.callLater(function() { driftingAvatar._placeAt(i) })
                return
            }
            var n = _driftXFracs.length
            var k = ((i % n) + n) % n
            driftingAvatar.x = Math.round(root.width * _driftXFracs[k] - driftingAvatar.width / 2)
            driftingAvatar.y = Math.round(root.height * _driftYFracs[k] - driftingAvatar.height / 2)
        }

        // Smooth fade for the drift hop (and for appear/disappear).
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.InOutQuad } }

        // When the screensaver-collapse state turns on, drop the avatar at a known-good spot immediately
        // (so it never sits stuck at 0,0 or mid-fade) and reset opacity to the resting faint level.
        onVisibleChanged: {
            if (visible) {
                driftingAvatar._placeAt(driftingAvatar._driftIndex)
                driftingAvatar.opacity = 0.4
            }
        }
        Component.onCompleted: if (visible) driftingAvatar._placeAt(driftingAvatar._driftIndex)

        // The avatar face (falls back to a small dot when avatars are disabled).
        Loader {
            id: driftAvatarFace
            anchors.centerIn: parent
            width: Theme.scaled(200); height: Theme.scaled(200)   // 2× (owner request) — bigger screensaver presence
            active: root._settings && root._settings.avatarEnabled
            source: "qrc:/qml/assistant/BaristaAvatar.qml"
            onLoaded: if (item) item.mode = "idle"
        }
        Rectangle {   // dot fallback (no avatar) — kept tiny and dim
            anchors.centerIn: parent
            visible: !(root._settings && root._settings.avatarEnabled)
            width: Theme.scaled(72); height: Theme.scaled(72)   // 2× with the avatar
            radius: width / 2
            color: Theme.primaryColor
        }

        Accessible.role: Accessible.Button
        Accessible.name: TranslationManager.translate("barista.expand", "Talk to the assistant")
        Accessible.focusable: true
        Accessible.onPressAction: root._tapDock()

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root._tapDock()   // accepted → does NOT reach the screensaver wake beneath
        }
    }

    // [barista-fork] Drift Timer — a legitimate periodic UI timer. Runs ONLY in the screensaver-collapse state
    // (stopped when a conversation is open or the screensaver is gone). Each tick fades the avatar out, advances
    // to the next scattered safe position, and fades it back in, so no pixel stays continuously lit.
    Timer {
        id: driftTimer
        interval: 30000
        repeat: true
        running: root._screensaverDock
        onTriggered: {
            driftingAvatar.opacity = 0.0                 // fade out
            driftingAvatar._driftIndex = driftingAvatar._driftIndex + 1
            fadeInDelay.restart()
        }
    }
    // Move + fade back in after the fade-out completes (Behavior duration = 300ms). Keeping the reposition on
    // this short one-shot (not inside the drift interval) means the avatar never visibly "jumps" while lit.
    Timer {
        id: fadeInDelay
        interval: 320
        repeat: false
        onTriggered: {
            if (!root._screensaverDock) return
            driftingAvatar._placeAt(driftingAvatar._driftIndex)   // hop to the new safe position while faded out
            driftingAvatar.opacity = 0.4                          // fade back in
        }
    }

    // [barista-fork] The dock tap: if present → engage() (onStateChanged expands + opens the mic); if
    // already conversing → just re-expand the panel. One gesture, "tap chat and talk".
    function _tapDock() {
        root._collapsed = false
        if (root._state !== "conversing" && root._orch)
            root._orch.engage()
    }
}
