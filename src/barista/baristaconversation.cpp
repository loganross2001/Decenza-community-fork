#include "baristaconversation.h"

#include "assistantvoice.h"
#include "voiceinput.h"
#include "speakergate.h"
#include "baristadiagnostics.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>

namespace {
// [barista-fork] Timers (design §13). Single, named, in one place — replacing the scattered QML timers.
constexpr int kPrimingTimeoutMs = 3000;
// [barista-fork] A HUNG-turn guard, not an SLA. It is RESET on every model activity (each interim/lead-in and on
// re-entering Thinking), so a slow-but-progressing tool turn (lead-in → tool → 2nd round-trip → answer, easily
// 15-25s total) never trips it — only genuine dead air does. Was 10s, which wrongly abandoned tool turns to
// Listening mid-answer.
constexpr int kTurnTimeoutMs     = 20000;
// [barista-fork] "Walked away" guard, not a turn-taking limit. 8s (the review's value) would drop the mic on a
// normal think-pause and force a tap; 30s only fires when the user has genuinely stopped. VoiceInput handles the
// sub-second no-match churn itself, so this only needs to catch true abandonment.
constexpr int kSilenceMs         = 30000;
// [barista-fork] Walk-away backstop: once we've dropped to NeedsTap (30s of Listening silence already elapsed)
// and the user never taps, silently close the dock rather than leave it open forever — the "it just stays open"
// complaint's last line of defence, on top of the (now prefix-aware) close-intent matcher and end_conversation
// tool. Generous so it only fires on genuine abandonment (~75s total idle), and it is a silent teardown (no
// sign-off) — the user has already gone. This is UI auto-dismiss, the one timer use the design rules allow.
constexpr int kNeedsTapIdleMs    = 45000;
constexpr int kClosingWatchdogMs = 2500;
// [barista-fork] Max stall→continuation retries per user turn. The model sometimes ends its turn with only a
// promise ("let me check on that") and no tool call, so no answer follows; we auto-send a continuation to make
// it actually answer. Bounded so a model that keeps stalling can't loop — after this many, deliver what it said
// and return to Listening (today's behaviour).
constexpr int kMaxAutoContinues  = 1;

// Generous local close-intent (a latency accelerator + fallback — the forced per-turn respond(text,
// end_conversation) bit is the structural mechanism). Short utterance + a normalised farewell phrase.
//
// The old version anchored the farewell at ^…$ with NO prefix handling, so every POLITE goodbye missed:
// "thanks, that'll be all" didn't start with a listed farewell (the "thanks" prefix broke the anchor) and
// wasn't in the narrow `thanks,? (that's (it|all)|bye)` branch either — the reported can't-close bug. Fix:
// strip leading politeness/filler prefixes FIRST, then match the (broadened) farewell set on what remains.
bool looksLikeClose(const QString& raw)
{
    QString t = raw.trimmed().toLower();
    if (t.isEmpty() || t.length() > 60)
        return false;
    // Peel up to two stacked leading prefixes ("ok thanks, …", "alright cool, …") so the farewell that
    // follows anchors cleanly. Bare politeness ("thanks") strips to empty and does NOT close (too aggressive).
    static const QRegularExpression prefix(
        QStringLiteral("^(ok(ay)?|alright|all right|right|so|well|now|um+|uh+|yeah|yep|yes|no|nah|cool|nice|"
                       "great|perfect|awesome|lovely|thank you so much|thank you|thanks|cheers|"
                       "appreciate it|i appreciate it)[,.!\\s]+"));
    for (int i = 0; i < 2; ++i) {
        const qsizetype before = t.size();
        t.remove(prefix);
        t = t.trimmed();
        if (t.size() == before)
            break;   // nothing stripped this pass
    }
    if (t.isEmpty())
        return false;
    static const QRegularExpression re(
        QStringLiteral("^("
                       "that'?s (it|all|everything|me|us|enough)( for now| then| done)?|"
                       "that'?ll (be all|do( it)?)( for now| then)?|"
                       "that will (be all|do)|"
                       "we'?re (done|good|all set|all done|finished|set)|"
                       "i'?m (done|good|all set|all done|finished|fine|set)|"
                       "(that'?s all|no more|nothing else|nothing more|no more questions)|"
                       "good ?night|good ?bye|bye( now| bye)?|see ya|see you( later)?|catch you later|"
                       "all done|all set|we can stop|let'?s stop|stop( there)?|that'?s enough|"
                       "no (that'?s (it|all)|i'?m (good|done)|thank you|thanks?)"
                       ")[.!]?$"));
    return re.match(t).hasMatch();
}

// [barista-fork] Stall detector: is the model's WHOLE final reply just a promise to continue ("let me check on
// that", "one moment", "checking…") with no actual answer? Tight on purpose — anchored ^…$ so it matches only a
// bare stall, never a real answer that happens to open with "let me…". A match means: speak it as a lead-in,
// keep the turn alive, and fetch the real answer via one continuation (see onModelFinal).
bool looksLikeStall(const QString& raw)
{
    QString t = raw.trimmed().toLower();
    if (t.isEmpty() || t.length() > 60)
        return false;
    t.remove(QRegularExpression(QStringLiteral("[.!,?…]+$")));   // drop trailing punctuation
    // Peel one leading filler ("ok, …", "sure — …") so the stall stem anchors.
    t.remove(QRegularExpression(QStringLiteral("^(ok(ay)?|sure|alright|right|well|hmm+|so|yeah|yep)[,.!\\s]+")));
    t = t.trimmed();
    static const QRegularExpression re(QStringLiteral(
        "^(let me |i'?ll |i will |let me just |give me |just |gonna |going to )?"
        "(check|look|see|find|pull|dig|verify|confirm|find out|look into|check on|look that up|"
        "hold on|hang on|checking|looking|searching|"
        "(a|one) (sec|second|moment|minute)|just (a|one) (sec|second|moment|minute)|one moment)"
        "( that| it| on that| on it| into that| into it| up| that up| it up| for you| for a moment)*$"));
    return re.match(t).hasMatch();
}

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
    const bool want = (m_state == State::Listening) && m_gate && m_gate->quiet();
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
    case State::Speaking:  if (m_voice) m_voice->stop(); onVoiceSpeakingChanged(); break;  // tap-to-skip barge-in
    default: diag(QStringLiteral("tap_ignored"), stateName()); break;  // Priming/Listening/Thinking/Closing
    }
}

void BaristaConversation::dismiss()
{
    diag(QStringLiteral("dismiss"));
    setState(State::Idle);   // Idle entry does the full teardown
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
    if (looksLikeClose(text)) {
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
    if (!endConversation && !m_closingArmed && m_autoContinues < kMaxAutoContinues && looksLikeStall(text)) {
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

void BaristaConversation::onModelError(const QString& message)
{
    if (m_state != State::Thinking && m_state != State::Speaking)
        return;
    diag(QStringLiteral("model_error"), message);
    m_turnInFlight = false;
    if (m_closingArmed) { setState(State::Closing); return; }
    setMessage(message.isEmpty() ? QStringLiteral("Something went wrong — tap or type to try again.") : message);
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
