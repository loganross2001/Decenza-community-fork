#include "speakergate.h"

#include "assistantvoice.h"

SpeakerGate::SpeakerGate(AssistantVoice* conv, AssistantVoice* coaching, QObject* parent)
    : QObject(parent), m_conv(conv), m_coaching(coaching)
{
    m_drain.setSingleShot(true);
    connect(&m_drain, &QTimer::timeout, this, [this]() {
        // The drain window elapsed with nothing speaking → the speaker is now acoustically quiet.
        if (!anySpeaking())
            setQuiet(true);
    });
    if (m_conv)
        connect(m_conv, &AssistantVoice::speakingChanged, this, &SpeakerGate::onSpeakingChanged);
    if (m_coaching)
        connect(m_coaching, &AssistantVoice::speakingChanged, this, &SpeakerGate::onSpeakingChanged);
    reevaluate();
}

bool SpeakerGate::anySpeaking() const
{
    return (m_conv && m_conv->speaking()) || (m_coaching && m_coaching->speaking());
}

void SpeakerGate::onSpeakingChanged()
{
    reevaluate();
}

void SpeakerGate::reevaluate()
{
    if (anySpeaking()) {
        // A source is speaking → not quiet, immediately; cancel any pending drain.
        m_drain.stop();
        setQuiet(false);
        return;
    }
    // Nothing speaking now. Don't declare quiet yet — wait out the acoustic tail. If we were already
    // quiet (idle), keep it; otherwise arm the drain window.
    if (!m_quiet && !m_drain.isActive())
        m_drain.start(m_drainMs);
}

void SpeakerGate::setDrainMs(int ms)
{
    m_drainMs = ms < 0 ? 0 : ms;
    if (m_drain.isActive())
        m_drain.start(m_drainMs);   // re-arm with the new window
}

void SpeakerGate::setQuiet(bool q)
{
    if (m_quiet == q)
        return;
    m_quiet = q;
    emit quietChanged();
}
