#include "baristaconversation.h"

#include "assistantvoice.h"
#include "voiceinput.h"
#include "speakergate.h"
#include "baristadiagnostics.h"

#include <QRegularExpression>

namespace {
// [barista-fork] Timers (design §13). Single, named, in one place — replacing the scattered QML timers.
constexpr int kPrimingTimeoutMs = 3000;
constexpr int kTurnTimeoutMs     = 10000;
constexpr int kSilenceMs         = 8000;
constexpr int kFillerMs          = 1200;   // no speakable within this of entering Thinking → one filler
constexpr int kClosingWatchdogMs = 2500;

// Generous local close-intent (a latency accelerator only — the forced per-turn close bit is the real
// mechanism, Phase 2). Short utterance + a normalised farewell phrase.
bool looksLikeClose(const QString& raw)
{
    const QString t = raw.trimmed().toLower();
    if (t.isEmpty() || t.length() > 40)
        return false;
    static const QRegularExpression re(
        QStringLiteral("^(that'?s (it|all|everything)( for now)?|that'?ll be all|we'?re (done|good)|"
                       "i'?m (done|good|all set)|no (that'?s all|thanks?)|goodnight|good night|goodbye|"
                       "bye( now)?|see ya|thanks,? (that'?s (it|all)|bye)|nothing else|all done)\\.?$"));
    return re.match(t).hasMatch();
}
}  // namespace

BaristaConversation::BaristaConversation(AssistantVoice* voice, AssistantVoice* coaching,
                                         VoiceInput* voiceInput, QObject* parent)
    : QObject(parent), m_voice(voice), m_coaching(coaching), m_voiceInput(voiceInput),
      m_gate(new SpeakerGate(voice, coaching, this))
{
    connect(m_gate, &SpeakerGate::quietChanged, this, &BaristaConversation::onGateQuietChanged);
    if (m_voice)
        connect(m_voice, &AssistantVoice::speakingChanged, this, &BaristaConversation::onVoiceSpeakingChanged);

    const auto oneShot = [](QTimer& t, int ms) { t.setSingleShot(true); t.setInterval(ms); };
    oneShot(m_primingTimeout, kPrimingTimeoutMs);
    oneShot(m_turnTimeout, kTurnTimeoutMs);
    oneShot(m_silence, kSilenceMs);
    oneShot(m_filler, kFillerMs);
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
    connect(&m_filler, &QTimer::timeout, this, [this]() {
        // Thinking is slow and nothing has spoken yet → one short filler. (Filler-content sourcing is
        // refined in the filler-move increment; a canned line keeps the everyday timeline honest for now.)
        if (m_state == State::Thinking && m_voice && !m_voice->speaking()) {
            diag(QStringLiteral("filler_spoken"));
            setState(State::Speaking);
            m_voice->speak(QStringLiteral("One sec."));
        }
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
        m_primingTimeout.stop(); m_turnTimeout.stop(); m_silence.stop(); m_filler.stop(); m_closingWatchdog.stop();
        m_closingArmed = false; m_turnInFlight = false; m_pendingAnswer.clear();
        if (m_voice) m_voice->stop();
        break;
    case State::Priming:
        m_primingTimeout.start();
        emit contextRequested();
        break;
    case State::Listening:
        m_silence.start();   // note: the arbiter (updateMicLive) actually opens the mic only when the gate is quiet
        break;
    case State::Thinking:
        m_turnInFlight = true;
        m_turnTimeout.start();
        m_filler.start();
        break;
    case State::Speaking:
        m_turnTimeout.stop();
        m_filler.stop();
        break;
    case State::Closing:
        m_closingWatchdog.start();
        break;
    case State::NeedsTap:
        m_silence.stop();
        break;
    }

    emit stateChanged();
    if (active() != wasActive)
        emit activeChanged();
    updateMicLive();
    if (s == State::Closing) {
        // Sign-off has already been spoken by the turn that armed closing; this is pure teardown.
        emit closingConfirmed();
    }
}

void BaristaConversation::updateMicLive()
{
    const bool want = (m_state == State::Listening) && m_gate && m_gate->quiet();
    if (want == m_micLive)
        return;
    m_micLive = want;
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
    // (Self-echo text filter is the Phase-3 backstop; the acoustic gate covers the common case.)
    m_softErrors = 0; m_hardErrors = 0;
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
    if (m_voice) m_voice->speak(text);
}

void BaristaConversation::onModelFinal(const QString& text, bool endConversation)
{
    m_closingArmed = m_closingArmed || endConversation;
    m_turnInFlight = false;
    setDisplay(text);
    if (m_state == State::Speaking && m_voice && m_voice->speaking()) {
        // A filler/lead-in is still playing — queue the answer; onVoiceSpeakingChanged drains it.
        m_pendingAnswer = text;
        diag(QStringLiteral("answer_pending_behind_filler"));
        return;
    }
    setState(State::Speaking);
    if (m_voice) m_voice->speak(text);
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
