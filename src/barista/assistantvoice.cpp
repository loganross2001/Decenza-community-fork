#include "assistantvoice.h"

#include "assistantsettings.h"
#include "../core/settings.h"
#include "../core/settings_ai.h"

#include <QTextToSpeech>
#include <QVoice>
#include <QSoundEffect>
#include <QUrl>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>

AssistantVoice::AssistantVoice(AssistantSettings* settings, Settings* appSettings, QObject* parent)
    : QObject(parent)
    , m_tts(new QTextToSpeech(this))
    , m_bell(new QSoundEffect(this))
    , m_player(new QMediaPlayer(this))
    , m_audioOut(new QAudioOutput(this))
    , m_audioBuffer(new QBuffer(this))
    , m_net(new QNetworkAccessManager(this))
    , m_settings(settings)
    , m_appSettings(appSettings) {
    m_player->setAudioOutput(m_audioOut);
    applyVoiceFromSettings();
    if (m_settings) {
        connect(m_settings, &AssistantSettings::voiceNameChanged, this, [this] {
            applyVoiceFromSettings();
            emit voiceNameChanged();
        });
    }
    // Some engines (notably Android) populate the native voice list asynchronously.
    connect(m_tts, &QTextToSpeech::localeChanged, this, [this] {
        emit availableVoicesChanged();
    });
    // Track when we're actually speaking (native TTS or cloud playback) so the mic can mute itself.
    connect(m_tts, &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State s) {
        if (s == QTextToSpeech::Error)   // SF-2: native engine failed → release the hold or `speaking` wedges
            m_pendingSynth = false;
        updateSpeaking();
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { updateSpeaking(); });
    connect(m_player, &QMediaPlayer::errorOccurred, this,   // SF-2: decode/playback error → release the hold
            [this](QMediaPlayer::Error, const QString&) { m_pendingSynth = false; updateSpeaking(); });
}

void AssistantVoice::updateSpeaking() {
    const bool active = (m_tts && m_tts->state() == QTextToSpeech::Speaking)
                     || (m_player && m_player->playbackState() == QMediaPlayer::PlayingState);
    if (active)
        m_pendingSynth = false;       // real audio started — hand off from the pending flag
    const bool now = m_pendingSynth || active;
    if (now == m_speaking)
        return;
    m_speaking = now;
    emit speakingChanged();
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

QString AssistantVoice::openaiKey() const {
    // Prefer an explicit key from the assistant settings; else reuse the app's configured OpenAI key.
    if (m_settings && !m_settings->openaiApiKey().isEmpty())
        return m_settings->openaiApiKey();
    return (m_appSettings && m_appSettings->ai()) ? m_appSettings->ai()->openaiApiKey() : QString();
}

void AssistantVoice::speak(const QString& text) {
    if (!m_settings || !m_settings->voiceEnabled() || text.trimmed().isEmpty())
        return;
    // Mark speaking BEFORE dispatch so speakingChanged(true) fires synchronously — the mic pauses now,
    // not after the cloud-TTS POST finally starts playback (which is the "listening while talking" bug).
    ++m_speakGen;
    m_pendingSynth = true;
    updateSpeaking();
    const QString provider = m_settings->ttsProvider();
    if (provider == QLatin1String("openai"))
        synthOpenAI(text);
    else if (provider == QLatin1String("elevenlabs"))
        synthElevenLabs(text);
    else if (m_tts) {
        m_tts->setRate((m_settings->voiceSpeed() - 1.0) / 0.5);   // map ~0.7–1.3 → rate -0.6..0.6
        m_tts->say(text);   // native engine (stateChanged hands off from m_pendingSynth)
    } else {
        m_pendingSynth = false;   // no engine at all → nothing will speak
        updateSpeaking();
    }
}

void AssistantVoice::synthOpenAI(const QString& text) {
    const QString key = openaiKey();
    if (key.isEmpty()) {                 // no key → graceful fallback to the native voice
        if (m_tts) m_tts->say(text);
        else { m_pendingSynth = false; updateSpeaking(); }
        return;
    }
    QNetworkRequest req(QUrl(QStringLiteral("https://api.openai.com/v1/audio/speech")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    const QJsonObject body{
        {QStringLiteral("model"), QStringLiteral("tts-1")},
        {QStringLiteral("voice"), m_settings->openaiVoice()},
        {QStringLiteral("input"), text},
        {QStringLiteral("response_format"), QStringLiteral("mp3")},
        {QStringLiteral("speed"), m_settings->voiceSpeed()},    // user-adjustable pace
    };
    QNetworkReply* reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text, gen = m_speakGen] {
        if (gen == m_speakGen) {   // ignore a superseded / dismissed request's late reply
            if (reply->error() == QNetworkReply::NoError)
                playMp3(reply->readAll());
            else if (m_tts)
                m_tts->say(text);   // cloud call failed → speak natively rather than go silent
            else { m_pendingSynth = false; updateSpeaking(); }
        }
        reply->deleteLater();
    });
}

void AssistantVoice::synthElevenLabs(const QString& text) {
    const QString key = m_settings->elevenlabsApiKey();
    if (key.isEmpty()) {                 // no key → graceful fallback to the native voice
        if (m_tts) m_tts->say(text);
        else { m_pendingSynth = false; updateSpeaking(); }
        return;
    }
    QNetworkRequest req(QUrl(QStringLiteral("https://api.elevenlabs.io/v1/text-to-speech/%1")
                             .arg(m_settings->elevenlabsVoiceId())));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("xi-api-key", key.toUtf8());
    const QJsonObject body{
        {QStringLiteral("text"), text},
        {QStringLiteral("model_id"), QStringLiteral("eleven_turbo_v2_5")},
        {QStringLiteral("voice_settings"), QJsonObject{{QStringLiteral("speed"), m_settings->voiceSpeed()}}},
    };
    QNetworkReply* reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text, gen = m_speakGen] {
        if (gen == m_speakGen) {   // ignore a superseded / dismissed request's late reply
            if (reply->error() == QNetworkReply::NoError)
                playMp3(reply->readAll());
            else if (m_tts)
                m_tts->say(text);   // cloud call failed → speak natively rather than go silent
            else { m_pendingSynth = false; updateSpeaking(); }
        }
        reply->deleteLater();
    });
}

void AssistantVoice::playMp3(const QByteArray& audio) {
    if (!m_player || audio.isEmpty()) {
        m_pendingSynth = false; updateSpeaking();   // no audio will play → release the pending hold
        return;
    }
    m_player->stop();
    // Android's media backend truncates in-memory (QBuffer) sources after a fraction of a second —
    // write the mp3 to a temp file and play that; files play reliably and to completion.
    const QString path = QDir::tempPath() + QStringLiteral("/decenza_tts.mp3");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_pendingSynth = false; updateSpeaking();
        return;
    }
    f.write(audio);
    f.close();
    // S12: the temp path is reused every utterance; setSource with the SAME URL is a no-op in Qt, so
    // the backend can replay the previous clip / stale duration. Clear the source first to force a reload.
    m_player->setSource(QUrl());
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
}

void AssistantVoice::stop() {
    ++m_speakGen;              // discard any in-flight synth reply
    m_pendingSynth = false;
    if (m_tts)
        m_tts->stop();
    if (m_player)
        m_player->stop();
    updateSpeaking();
}

void AssistantVoice::setVoiceByName(const QString& name) {
    // Persist; the settings signal drives applyVoiceFromSettings() + voiceNameChanged().
    if (m_settings)
        m_settings->setVoiceName(name);
}

void AssistantVoice::preview() {
    const QString who = m_settings ? m_settings->assistantName() : QStringLiteral("Coach");
    const QString sample = QStringLiteral("Hi, I'm %1. Ready when you are.").arg(who);
    const QString provider = m_settings ? m_settings->ttsProvider() : QStringLiteral("native");
    if (provider == QLatin1String("openai"))
        synthOpenAI(sample);
    else if (provider == QLatin1String("elevenlabs"))
        synthElevenLabs(sample);
    else if (m_tts)
        m_tts->say(sample);   // audition — speaks even if muted
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
