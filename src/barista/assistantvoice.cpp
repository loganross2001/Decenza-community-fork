#include "assistantvoice.h"

#include "assistantsettings.h"

#include <QTextToSpeech>
#include <QVoice>
#include <QSoundEffect>
#include <QUrl>

AssistantVoice::AssistantVoice(AssistantSettings* settings, QObject* parent)
    : QObject(parent)
    , m_tts(new QTextToSpeech(this))
    , m_bell(new QSoundEffect(this))
    , m_settings(settings) {
    applyVoiceFromSettings();
    if (m_settings) {
        connect(m_settings, &AssistantSettings::voiceNameChanged, this, [this] {
            applyVoiceFromSettings();
            emit voiceNameChanged();
        });
    }
    // Some engines (notably Android) populate the voice list asynchronously; surface changes so
    // a settings picker can refresh once voices are ready.
    connect(m_tts, &QTextToSpeech::localeChanged, this, [this] {
        emit availableVoicesChanged();
    });
}

QStringList AssistantVoice::availableVoices() const {
    QStringList names;
    if (!m_tts)
        return names;
    const auto voices = m_tts->availableVoices();
    names.reserve(voices.size());
    for (const QVoice& v : voices)
        names << v.name();
    return names;
}

QString AssistantVoice::voiceName() const {
    return m_tts ? m_tts->voice().name() : QString();
}

void AssistantVoice::speak(const QString& text) {
    if (!m_tts || !m_settings || !m_settings->voiceEnabled())
        return;
    if (text.trimmed().isEmpty())
        return;
    m_tts->say(text);   // enqueues; QTextToSpeech interrupts the previous utterance on a new say()
}

void AssistantVoice::stop() {
    if (m_tts)
        m_tts->stop();
}

void AssistantVoice::setVoiceByName(const QString& name) {
    // Persist; the settings signal drives applyVoiceFromSettings() + voiceNameChanged().
    if (m_settings)
        m_settings->setVoiceName(name);
}

void AssistantVoice::preview() {
    if (!m_tts)
        return;
    const QString who = m_settings ? m_settings->assistantName() : QStringLiteral("Coach");
    m_tts->say(QStringLiteral("Hi, I'm %1. Ready when you are.").arg(who));   // audition — speaks even if muted
}

void AssistantVoice::playBell() {
    if (m_settings)
        previewBell(m_settings->bellSound());
}

void AssistantVoice::previewBell(const QString& name) {
    if (!m_bell || name.isEmpty() || name == QLatin1String("off"))
        return;
    m_bell->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(name)));
    m_bell->play();
}

void AssistantVoice::applyVoiceFromSettings() {
    if (!m_tts || !m_settings)
        return;
    const QString want = m_settings->voiceName();
    if (want.isEmpty())
        return;   // keep the engine default
    for (const QVoice& v : m_tts->availableVoices()) {
        if (v.name() == want) {
            m_tts->setVoice(v);
            break;
        }
    }
}
