#include "baristaconversation.h"

#include "assistantvoice.h"
#include "voiceinput.h"
#include "speakergate.h"
#include "baristadiagnostics.h"
#include "closeintent.h"   // barista::looksLikeClose / barista::looksLikeStall (unit-tested in tst_closeintent)

#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>

namespace {
// [barista-fork] Timers (design §13). Single, named, in one place — replacing the scattered QML timers.
constexpr int kPrimingTimeoutMs = 3000;
// [barista-fork] A HUNG-turn guard, not an SLA. It is RESET on every model activity (each interim/lead-in and on
// re-entering Thinking), so a slow-but-progressing tool turn (lead-in → tool → 2nd round-trip → answer, easily
// 15-25s total) never trips it — only genuine dead air does. Was 10s, which wrongly abandoned tool turns to
// Listening mid-answer. Raised 20s→40s (owner report 2026-07-31: "long wait then straight back to listening,
// no tone"): a SILENT tool turn (no interim speech to cycle Speaking→Thinking) does NOT reset this timer, and
// the multi-tool coaching turns — recommend_next_shot → create_related_profile (file I/O + profile upload) —
// routinely exceed the old 20s, so the guard was firing on a working turn and dropping it (killing the thinking
// tone with it). 40s keeps a genuinely-working turn alive; true dead air still ends within a reasonable wait.
constexpr int kTurnTimeoutMs     = 40000;
// [barista-fork] "Walked away" guard, not a turn-taking limit. 8s (the review's value) would drop the mic on a
// normal think-pause and force a tap; 30s only fires when the user has genuinely stopped. VoiceInput handles the
// sub-second no-match churn itself, so this only needs to catch true abandonment.
constexpr int kSilenceMs         = 30000;
// [barista-fork] Walk-away backstop: once we've dropped to NeedsTap (30s of Listening silence already elapsed)
// and the user never taps, silently close the dock rather than leave it open forever — the "it just stays open"
// complaint's last line of defence, on top of the (now prefix-aware) close-intent matcher and end_conversation
// tool. Trimmed 45s→25s (owner report 2026-07-31: the dock "stays listening when it shouldn't" — when STT churn
// eats a spoken close, this backstop is what actually ends the session, and 75s total felt like it wouldn't
// close). 25s here only fires AFTER 30s of silence AND an ignored "Tap when you're ready" prompt (55s total
// idle), so it still can't cut off a live conversation — a returning user taps, which cancels it (setState
// NeedsTap→Listening). Silent teardown (no sign-off) — the user has already gone. UI auto-dismiss, the one
// timer use the design rules allow.
constexpr int kNeedsTapIdleMs    = 25000;
constexpr int kClosingWatchdogMs = 2500;
// [barista-fork] Max stall→continuation retries per user turn. The model sometimes ends its turn with only a
// promise ("let me check on that") and no tool call, so no answer follows; we auto-send a continuation to make
// it actually answer. Bounded so a model that keeps stalling can't loop — after this many, deliver what it said
// and return to Listening (today's behaviour). Raised 1→2 (owner report 2026-07-31: "gave me a second, then
// went right back to listening"): with one retry, a model that stalls TWICE spends its budget and the 2nd stall
// falls through as if it were the answer → straight to Listening with nothing said. Two retries lets a real
// answer land in the common case while still bounding the loop.
constexpr int kMaxAutoContinues  = 2;

// [barista-fork] looksLikeClose() / looksLikeStall() moved to closeintent.{h,cpp} so they can be unit-tested
// in isolation (pure QtCore, no voice/mic/AI deps) — see tests/tst_closeintent.cpp. Called below as
// barista::looksLikeClose / barista::looksLikeStall.

// [barista-fork] Self-echo backstop: is `heard` (a first STT result right after the mic opened) mostly a repeat
// of what the barista just said (`spoken`)? ≥70% of the heard words appearing in the spoken text ⇒ it's the
// speaker's acoustic tail, not the user. Short + generous so "yes, a flat white" (echoing a couple of words) is
// still safe (few words, low overlap ratio against a long sign-off).
bool isSelfEcho(const QString& heard, const QString& spoken)
{
    if (heard.isEmpty() || spoken.isEmpty())
        return false;
    const QStringList words = heard.toLower().split(QRegularExpression(QStringLiteral("\\W+")), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return false;
    const QString sp = QStringLiteral(" ") + spoken.toLower() + QStringLiteral(" ");
    int hits = 0;
    for (const QString& w : words)
        if (sp.contains(QStringLiteral(" ") + w + QStringLiteral(" ")))
            ++hits;
    return (static_cast<double>(hits) / words.size()) >= 0.7;
}
}  // namespace

BaristaConversation::BaristaConversation(AssistantVoice* voice, AssistantVoice* coaching,
                                         VoiceInput* voiceInput, QObject* parent)
    : QObject(parent), m_voice(voice), m_coaching(coaching), m_voiceInput(voiceInput),
      m_gate(new SpeakerGate(voice, coaching, this))
{
    connect(m_gate, &SpeakerGate::quietChanged, this, &BaristaConversation::onGateQuietChanged);
    if (m_voice) {
        connect(m_voice, &AssistantVoice::speakingChanged, this, &BaristaConversation::onVoiceSpeakingChanged);
        // Real audio starting/stopping toggles the thinking earcon (it must not play OVER audible speech, and
        // must resume in the silent gaps — same rule the old overlay used).
        connect(m_voice, &AssistantVoice::audibleChanged, this, &BaristaConversation::updateThinkingTone);
    }

    const auto oneShot = [](QTimer& t, int ms) { t.setSingleShot(true); t.setInterval(ms); };
    oneShot(m_primingTimeout, kPrimingTimeoutMs);
    oneShot(m_turnTimeout, kTurnTimeoutMs);
    oneShot(m_silence, kSilenceMs);
    oneShot(m_needsTapIdle, kNeedsTapIdleMs);
    oneShot(m_closingWatchdog, kClosingWatchdogMs);

    connect(&m_primingTimeout, &QTimer::timeout, this, [this]() {
        if (m_state == State::Priming) { diag(QStringLiteral("priming_timeout")); setState(State::Listening); }
    });
    connect(&m_turnTimeout, &QTimer::timeout, this, [this]() {
        if (m_state != State::Thinking) return;
        diag(QStringLiteral("turn_timeout"));
        m_turnInFlight = false;
        if (m_closingArmed) { setState(State::Speaking); if (m_voice) m_voice->speak(QStringLiteral("Alright, talk soon.")); }
        else { setMessage(QStringLiteral("That took too long — tap or try again.")); setState(State::Listening); }
    });
    connect(&m_silence, &QTimer::timeout, this, [this]() {
        if (m_state == State::Listening) {
            diag(QStringLiteral("silence_timeout"));
            setMessage(QStringLiteral("Tap when you're ready."));
            setState(State::NeedsTap);
        }
    });
    // [barista-fork] No canned "One sec" filler: the model emits its own lead-in ("let me check") via
    // onModelSpeakable, and the thinking earcon fills the silent gaps — a canned line would talk over the
    // model's lead-in. (A model-generated quick-filler can return in a later increment.)
    connect(&m_needsTapIdle, &QTimer::timeout, this, [this]() {
        if (m_state != State::NeedsTap)
            return;
        // Walk-away: silently tear down and tell the view to collapse the dock. closingConfirmed() is the
        // view's collapse signal (wired to the dock dismiss), same as a completed sign-off — but here there is
        // nothing to speak, so we emit it directly and go Idle rather than routing through Closing.
        diag(QStringLiteral("needstap_idle_autoclose"));
        emit closingConfirmed();
        setState(State::Idle);
    });
    connect(&m_closingWatchdog, &QTimer::timeout, this, [this]() {
        if (m_state == State::Closing) { diag(QStringLiteral("closing_watchdog")); setState(State::Idle); }
    });
}

QString BaristaConversation::stateName() const
{
    switch (m_state) {
    case State::Idle: return QStringLiteral("idle");
    case State::Priming: return QStringLiteral("priming");
    case State::Listening: return QStringLiteral("listening");
    case State::Thinking: return QStringLiteral("thinking");
    case State::Speaking: return QStringLiteral("speaking");
    case State::Closing: return QStringLiteral("closing");
    case State::NeedsTap: return QStringLiteral("needsTap");
    }
    return QStringLiteral("idle");
}

void BaristaConversation::setState(State s)
{
    if (m_state == s)
        return;
    const bool wasActive = active();
    m_state = s;
    diag(QStringLiteral("state"), stateName());

    // Per-state entry actions.
    switch (s) {
    case State::Idle:
        m_primingTimeout.stop(); m_turnTimeout.stop(); m_silence.stop();
        m_needsTapIdle.stop(); m_closingWatchdog.stop();
        m_closingArmed = false; m_turnInFlight = false; m_pendingAnswer.clear(); m_autoContinues = 0;
        if (m_voice) m_voice->stop();
        break;
    case State::Priming:
        m_primingTimeout.start();
        emit contextRequested();
        break;
    case State::Listening:
        m_needsTapIdle.stop();   // left NeedsTap by tapping — cancel the walk-away timer
        m_silence.start();   // note: the arbiter (updateMicLive) actually opens the mic only when the gate is quiet
        break;
    case State::Thinking:
        m_turnInFlight = true;
        m_turnTimeout.start();   // (re)started on every Thinking entry → resets on model activity
        break;
    case State::Speaking:
        m_turnTimeout.stop();
        break;
    case State::Closing:
        m_closingWatchdog.start();
        break;
    case State::NeedsTap:
        m_silence.stop();
        m_needsTapIdle.start();   // walk-away backstop: silent close if never tapped
        break;
    }

    emit stateChanged();
    if (active() != wasActive)
        emit activeChanged();
    updateMicLive();
    updateThinkingTone();
    if (s == State::Closing) {
        // Sign-off has already been spoken by the turn that armed closing; this is pure teardown.
        emit closingConfirmed();
    }
}

void BaristaConversation::updateThinkingTone()
{
    if (!m_voice)
        return;
    // The thinking earcon fills the SILENT waits: while Thinking (waiting on the model, incl. after a "let me
    // check" lead-in), and during the synth/network gap of Speaking before real audio is out. Never over
    // audible speech. Mirrors the old overlay's _updateThinkingLoop, minus the scattered flags.
    const bool want = (m_state == State::Thinking)
                      || (m_state == State::Speaking && !m_voice->audible());
    if (want)
        m_voice->startThinkingLoop();
    else
        m_voice->stopThinkingLoop();
}

void BaristaConversation::updateMicLive()
{
    // [barista-fork] m_micSuppressed is a third arbiter input on top of the two-input rule: while the bag camera
    // pauses the mic (design (b) / contention mitigation), the recogniser stays off regardless of state or gate.
    const bool want = (m_state == State::Listening) && m_gate && m_gate->quiet() && !m_micSuppressed;
    if (want == m_micLive)
        return;
    m_micLive = want;
    if (want)
        m_micHotSinceMs = QDateTime::currentMSecsSinceEpoch();   // start of the self-echo window
    diag(QStringLiteral("micLive"), want ? QStringLiteral("on") : QStringLiteral("off"));
    if (m_voiceInput)
        m_voiceInput->setActive(want);   // THE only mic control
    // Silence timer keys off micLive, not state: restart it when the mic actually goes hot.
    if (want && m_state == State::Listening)
        m_silence.start();
    emit micLiveChanged();
}

void BaristaConversation::onGateQuietChanged()
{
    updateMicLive();
}

// ---- QML gestures --------------------------------------------------------

void BaristaConversation::tap()
{
    switch (m_state) {
    case State::Idle:      setState(State::Priming); break;
    case State::NeedsTap:  m_softErrors = 0; m_hardErrors = 0; setMessage(QString()); setState(State::Listening); break;
    // [barista-fork] The button reads "Stop" (Stop listening) while Listening — a tap there must ACTUALLY stop the
    // mic, not no-op. Go to NeedsTap (mic off, session alive); the button flips back to "Chat" to resume. This was
    // the reported "Stop button doesn't work": the new-path tap() fell through to the default no-op (the old
    // direct-VoiceInput path used to call stop() here). Confirmed by `tap_ignored detail=listening` in the logs.
    case State::Listening: setState(State::NeedsTap); break;
    case State::Speaking:  if (m_voice) m_voice->stop(); onVoiceSpeakingChanged(); break;  // tap-to-skip barge-in
    default: diag(QStringLiteral("tap_ignored"), stateName()); break;  // Priming/Thinking/Closing
    }
}

void BaristaConversation::dismiss()
{
    diag(QStringLiteral("dismiss"));
    setState(State::Idle);   // Idle entry does the full teardown
}

// [barista-fork] Arm/disarm the voice-driven bag-photo shutter. QML sets it true when the camera opens and false
// when it closes; onFinalText reads it to decide whether an affirmative fires the shutter or is a normal turn.
void BaristaConversation::setAwaitingBagCapture(bool on)
{
    if (m_awaitingBagCapture == on)
        return;
    m_awaitingBagCapture = on;
    diag(QStringLiteral("awaiting_bag_capture"), on ? QStringLiteral("on") : QStringLiteral("off"));
}

// [barista-fork] Force the mic off (bag-camera pause toggle). Re-runs the arbiter so the change takes effect now.
void BaristaConversation::setMicSuppressed(bool on)
{
    if (m_micSuppressed == on)
        return;
    m_micSuppressed = on;
    diag(QStringLiteral("mic_suppressed"), on ? QStringLiteral("on") : QStringLiteral("off"));
    updateMicLive();
}

// ---- Actuator inputs (transition table) ----------------------------------

void BaristaConversation::onContextReady()
{
    if (m_state == State::Priming)
        setState(State::Listening);
}

void BaristaConversation::onFinalText(const QString& text)
{
    if (m_state != State::Listening)
        return;   // logged-and-ignored elsewhere; the mic shouldn't be hot outside Listening anyway
    // [barista-fork] Voice-driven bag-photo capture: while the camera is open awaiting a shot, a spoken
    // affirmative ("ready" / "go" / "take it") is a deterministic SHUTTER signal, not a question for the model.
    // Fire the capture locally and stay in Listening — behaviourally identical to a manual shutter tap: the
    // shutter → captured() → followUpWithImage path then dispatches the vision turn that reads the label and
    // calls add_bag. This bypasses the model, so it neither consumes a turn nor touches the one-turn-image window.
    //
    // Checked BEFORE the self-echo backstop ON PURPOSE. The camera prompt asks the user to "say ready", so the
    // barista's own last line CONTAINS the trigger word — a one-word "ready" scores 1.0 against it and the echo
    // guard would drop the user's genuine "ready", making the feature look dead (indistinguishable from an STT
    // failure). SpeakerGate.quiet() already holds the mic off while that prompt is spoken, so the live word is
    // never heard; the only residual is a brief acoustic tail, and an over-eager snap there is recoverable (retake)
    // whereas a dropped "ready" is not. So within the awaiting-capture window the affirmative wins over self-echo.
    if (m_awaitingBagCapture && barista::looksLikeAffirmative(text)) {
        diag(QStringLiteral("bag_capture_affirm"), text.left(40));
        m_awaitingBagCapture = false;
        emit captureBagPhotoRequested();
        return;   // stay Listening; the capture pipeline (followUpWithImage) drives the next model turn
    }
    // Self-echo backstop (belt-and-suspenders to the acoustic gate): a first result within 2s of the mic going
    // hot that mostly repeats the barista's last line is its own acoustic tail — discard it, keep listening.
    if (m_micHotSinceMs != 0
        && (QDateTime::currentMSecsSinceEpoch() - m_micHotSinceMs) < 2000
        && isSelfEcho(text, m_lastSpokenText)) {
        diag(QStringLiteral("self_echo_drop"), text.left(40));
        return;
    }
    m_softErrors = 0; m_hardErrors = 0;
    m_autoContinues = 0;   // fresh stall-retry budget for this user turn
    if (barista::looksLikeClose(text)) {
        m_closingArmed = true;
        diag(QStringLiteral("close_intent_local"));
    }
    setState(State::Thinking);
    emit turnRequested(text);
}

void BaristaConversation::onPartial(const QString& text)
{
    if (m_state == State::Listening)
        setPartial(text);
}

void BaristaConversation::onSttError(const QString& message)
{
    // VoiceInput currently only surfaces the FATAL error (it retries transient ones internally). The
    // soft/hard recovery ladder lands here once VoiceInput exposes the code split (recovery increment).
    if (m_state != State::Listening)
        return;
    diag(QStringLiteral("stt_error"), message);
    setMessage(message.isEmpty() ? QStringLiteral("Didn't catch that — tap to talk.") : message);
    setState(State::NeedsTap);
}

void BaristaConversation::onModelSpeakable(const QString& text)
{
    if (m_state != State::Thinking && m_state != State::Speaking)
        return;
    setState(State::Speaking);   // turnInFlight stays true
    m_lastSpokenText = text;     // for the self-echo backstop
    if (m_voice) m_voice->speak(text);
}

void BaristaConversation::onModelFinal(const QString& text, bool endConversation)
{
    m_closingArmed = m_closingArmed || endConversation;

    // [barista-fork] Stall guard: the model sometimes ENDS its turn with only a promise-to-continue
    // ("let me check on that") and NO tool call, so no answer ever follows and the machine would fall
    // straight back to Listening having said nothing useful (the reported "it goes to listening instead of
    // playing the tone until it answers" bug). Treat a bare stall as a lead-in: speak it, keep the turn IN
    // FLIGHT (so when it finishes the machine goes Thinking → the tone plays, not Listening), and send ONE
    // continuation to make the model actually produce the answer. Bounded by kMaxAutoContinues so a model that
    // keeps stalling can't loop — after the budget, it falls through to normal delivery below. Never on a
    // close turn (a sign-off is not a stall).
    if (!endConversation && !m_closingArmed && m_autoContinues < kMaxAutoContinues && barista::looksLikeStall(text)) {
        ++m_autoContinues;
        diag(QStringLiteral("stall_autocontinue"), text.left(40));
        emit continuationRequested();   // module sends a follow-up turn → the real answer arrives via onModelFinal
        onModelSpeakable(text);         // speak the stall as a lead-in (Speaking; m_turnInFlight left TRUE)
        return;
    }

    m_turnInFlight = false;
    setDisplay(text);
    m_lastSpokenText = text;   // for the self-echo backstop (covers both the queued + direct speak paths)
    if (m_state == State::Speaking && m_voice && m_voice->speaking()) {
        // A filler/lead-in is still playing — queue the answer; onVoiceSpeakingChanged drains it.
        m_pendingAnswer = text;
        diag(QStringLiteral("answer_pending_behind_filler"));
        return;
    }
    setState(State::Speaking);
    if (m_voice) m_voice->speak(text);
}

void BaristaConversation::onCloseRequested()
{
    // The model called end_conversation (its sign-off is in the same reply). Arm closing; the deterministic
    // teardown fires when that reply finishes speaking (Speaking→Closing), or the closing watchdog backstops it.
    // This is a SECOND close signal on top of the local looksLikeClose() accelerator — either is sufficient, so a
    // farewell the local matcher misses still closes when the model calls the tool, and vice-versa. (The forced
    // per-turn respond(text,end_conversation) protocol — the structural can't-miss version — is a later hardening
    // that needs on-device validation because it changes the turn protocol per provider.)
    if (m_state == State::Idle)
        return;
    m_closingArmed = true;
    diag(QStringLiteral("close_requested_tool"));
    // If nothing is left to speak (muted/text reply already done and we're idling in Listening), close now.
    if (m_state == State::Listening)
        setState(State::Closing);
}

// [barista-fork] Turn a raw provider error into a SHORT, actionable message the user can act on. The raw text
// (e.g. Gemini's "Your prepayment credits are depleted. Please go to AI Studio…") is logged verbatim in the
// diagnostic; what the user SEES should name the problem and the fix ("out of credits — add billing, or switch
// models"), not read as a vague "service error". Returns "" when nothing matches, so the caller falls back to the
// raw detail (still better than a generic string).
static QString friendlyModelError(const QString& raw)
{
    const QString low = raw.toLower();
    const QString who = low.contains(QLatin1String("gemini")) ? QStringLiteral("Gemini")
                      : (low.contains(QLatin1String("claude")) || low.contains(QLatin1String("anthropic")))
                            ? QStringLiteral("Claude")
                      : (low.contains(QLatin1String("openai")) || low.contains(QLatin1String("gpt")))
                            ? QStringLiteral("ChatGPT")
                      : QStringLiteral("The AI service");
    // Billing / out of credits / quota — the most common "service error".
    if (low.contains(QLatin1String("credit")) || low.contains(QLatin1String("deplet"))
        || low.contains(QLatin1String("billing")) || low.contains(QLatin1String("quota"))
        || low.contains(QLatin1String("resource_exhausted")) || low.contains(QLatin1String("insufficient"))
        || low.contains(QLatin1String("payment")) || low.contains(QLatin1String("balance")))
        return who + QStringLiteral(" is out of credits. Add billing/credits for it, or switch to a different AI "
                                    "model in the assistant Settings (Claude and Gemini both work here).");
    // Auth / key.
    if (low.contains(QLatin1String("api key")) || low.contains(QLatin1String("unauthorized"))
        || low.contains(QLatin1String(" 401")) || low.contains(QLatin1String("authentication"))
        || (low.contains(QLatin1String("invalid")) && low.contains(QLatin1String("key")))
        || low.contains(QLatin1String("not configured")))
        return who + QStringLiteral("'s API key is missing or invalid. Check it in the assistant Settings, or pick "
                                    "a different AI model.");
    // Temporary: rate limit / overloaded / unavailable.
    if (low.contains(QLatin1String("rate")) || low.contains(QLatin1String(" 429"))
        || low.contains(QLatin1String("overload")) || low.contains(QLatin1String(" 503"))
        || low.contains(QLatin1String("unavailable")) || low.contains(QLatin1String("try again")))
        return who + QStringLiteral(" is busy right now. Wait a moment and try again, or switch AI models in the "
                                    "assistant Settings.");
    return QString();
}

void BaristaConversation::onModelError(const QString& message)
{
    if (m_state != State::Thinking && m_state != State::Speaking)
        return;
    diag(QStringLiteral("model_error"), message);   // raw detail preserved in the log
    m_turnInFlight = false;
    if (m_closingArmed) { setState(State::Closing); return; }
    const QString friendly = friendlyModelError(message);
    setMessage(!friendly.isEmpty() ? friendly
               : (message.isEmpty() ? QStringLiteral("Something went wrong — tap or type to try again.")
                                    : message));
    setState(State::Listening);
}

void BaristaConversation::onVoiceSpeakingChanged()
{
    // Re-evaluate the arbiter regardless (SpeakerGate also watches, but keep this direct too).
    updateMicLive();
    if (m_state != State::Speaking)
        return;
    if (m_voice && m_voice->speaking())
        return;   // speech just STARTED, not ended
    // speechEnded dispatch — order is load-bearing (§13): pendingAnswer → turnInFlight → closingArmed → Listening.
    if (!m_pendingAnswer.isEmpty()) {
        const QString a = m_pendingAnswer;
        m_pendingAnswer.clear();
        if (m_voice) m_voice->speak(a);   // stays in Speaking; the next speechEnded re-dispatches
        return;
    }
    if (m_turnInFlight) { setState(State::Thinking); return; }
    if (m_closingArmed) { setState(State::Closing); return; }
    setState(State::Listening);   // the gate handles the acoustic drain before the mic actually opens
}

// ---- View mirrors + diagnostics ------------------------------------------

void BaristaConversation::setDisplay(const QString& t) { if (m_display != t) { m_display = t; emit displayTextChanged(); } }
void BaristaConversation::setPartial(const QString& t) { if (m_partial != t) { m_partial = t; emit partialTextChanged(); } }
void BaristaConversation::setMessage(const QString& t) { if (m_message != t) { m_message = t; emit messageChanged(); } }

void BaristaConversation::diag(const QString& event, const QString& detail) const
{
    if (detail.isEmpty())
        BaristaDiagnostics::record(QStringLiteral("conv"), event);
    else
        BaristaDiagnostics::record(QStringLiteral("conv"), event, {{QStringLiteral("detail"), detail}});
}
