#include "speakergate.h"

#include "assistantvoice.h"
#include "baristadiagnostics.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

namespace {
// Per-route acoustic-tail windows (ms). Bluetooth A2DP buffers ~100-300ms + room decay → keep a real guard;
// USB-C / wired / built-in are low-latency (Phase-0 measured ~0 tail on built-in), so a short window is safe
// and keeps the mic snappy. Route is detected live (SpeakerGate::routeDrainMs).
constexpr int kBluetoothDrainMs = 400;
// [[maybe_unused]]: only referenced inside routeDrainMs()'s Q_OS_ANDROID branch, so a desktop
// build (where that block is #ifdef'd out) would otherwise trip -Werror=unused-const-variable.
[[maybe_unused]] constexpr int kWiredDrainMs = 150;
}  // namespace

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
    // quiet (idle), keep it; otherwise arm the drain window sized to the route we just spoke through.
    if (!m_quiet && !m_drain.isActive()) {
        const int prev = m_drainMs;
        m_drainMs = routeDrainMs();
        if (m_drainMs != prev)
            BaristaDiagnostics::record(QStringLiteral("gate"), QStringLiteral("drain_ms"),
                {{QStringLiteral("ms"), m_drainMs}});
        m_drain.start(m_drainMs);
    }
}

int SpeakerGate::routeDrainMs() const
{
#ifdef Q_OS_ANDROID
    const QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (ctx.isValid()) {
        const bool bt = QJniObject::callStaticMethod<jboolean>(
            "io/github/kulitorum/decenza_de1/DecenzaSpeech", "outputIsBluetooth",
            "(Landroid/content/Context;)Z", ctx.object());
        return bt ? kBluetoothDrainMs : kWiredDrainMs;
    }
#endif
    return kBluetoothDrainMs;   // desktop / unknown → conservative
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
