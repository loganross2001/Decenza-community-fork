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
    connect(m_tts, &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State) { updateSpeaking(); });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { updateSpeaking(); });
}

void AssistantVoice::updateSpeaking() {
    const bool now = (m_tts && m_tts->state() == QTextToSpeech::Speaking)
                  || (m_player && m_player->playbackState() == QMediaPlayer::PlayingState);
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
    const QString provider = m_settings->ttsProvider();
    if (provider == QLatin1String("openai"))
        synthOpenAI(text);
    else if (provider == QLatin1String("elevenlabs"))
        synthElevenLabs(text);
    else if (m_tts)
        m_tts->say(text);   // native engine
}

void AssistantVoice::synthOpenAI(const QString& text) {
    const QString key = openaiKey();
    if (key.isEmpty()) {                 // no key → graceful fallback to the native voice
        if (m_tts) m_tts->say(text);
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
        {QStringLiteral("speed"), 1.15},                        // a touch faster than default
    };
    QNetworkReply* reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text] {
        if (reply->error() == QNetworkReply::NoError)
            playMp3(reply->readAll());
        else if (m_tts)
            m_tts->say(text);   // cloud call failed → speak natively rather than go silent
        reply->deleteLater();
    });
}

void AssistantVoice::synthElevenLabs(const QString& text) {
    const QString key = m_settings->elevenlabsApiKey();
    if (key.isEmpty()) {                 // no key → graceful fallback to the native voice
        if (m_tts) m_tts->say(text);
        return;
    }
    QNetworkRequest req(QUrl(QStringLiteral("https://api.elevenlabs.io/v1/text-to-speech/%1")
                             .arg(m_settings->elevenlabsVoiceId())));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("xi-api-key", key.toUtf8());
    const QJsonObject body{
        {QStringLiteral("text"), text},
        {QStringLiteral("model_id"), QStringLiteral("eleven_turbo_v2_5")},
        {QStringLiteral("voice_settings"), QJsonObject{{QStringLiteral("speed"), 1.15}}},   // faster pacing
    };
    QNetworkReply* reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text] {
        if (reply->error() == QNetworkReply::NoError)
            playMp3(reply->readAll());
        else if (m_tts)
            m_tts->say(text);   // cloud call failed → speak natively rather than go silent
        reply->deleteLater();
    });
}

void AssistantVoice::playMp3(const QByteArray& audio) {
    if (!m_player || audio.isEmpty())
        return;
    m_player->stop();
    // Android's media backend truncates in-memory (QBuffer) sources after a fraction of a second —
    // write the mp3 to a temp file and play that; files play reliably and to completion.
    const QString path = QDir::tempPath() + QStringLiteral("/decenza_tts.mp3");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(audio);
    f.close();
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
}

void AssistantVoice::stop() {
    if (m_tts)
        m_tts->stop();
    if (m_player)
        m_player->stop();
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
