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

AssistantVoice::AssistantVoice(AssistantSettings* settings, Settings* appSettings,
                               Role role, QObject* parent)
    : QObject(parent)
    , m_tts(new QTextToSpeech(this))
    , m_bell(new QSoundEffect(this))
    , m_player(new QMediaPlayer(this))
    , m_audioOut(new QAudioOutput(this))
    , m_audioBuffer(new QBuffer(this))
    , m_net(new QNetworkAccessManager(this))
    , m_settings(settings)
    , m_appSettings(appSettings)
    , m_role(role) {
    m_player->setAudioOutput(m_audioOut);
    applyVoiceFromSettings();
    if (m_settings) {
        // Re-apply the native voice when THIS role's voice-name setting changes.
        void (AssistantSettings::*nameSignal)() = (m_role == Role::Coaching)
                               ? &AssistantSettings::coachingVoiceNameChanged
                               : &AssistantSettings::voiceNameChanged;
        connect(m_settings, nameSignal, this, [this] {
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

// [barista-fork] Role-effective settings reads. The barista and coaching voices differ only in provider +
// the three per-provider voice ids; everything else (keys, saved-voices list, speed) is shared.
QString AssistantVoice::effectiveProvider() const {
    if (!m_settings)
        return QStringLiteral("native");
    return m_role == Role::Coaching ? m_settings->coachingTtsProvider() : m_settings->ttsProvider();
}

QString AssistantVoice::effectiveVoiceName() const {
    if (!m_settings)
        return QString();
    return m_role == Role::Coaching ? m_settings->coachingVoiceName() : m_settings->voiceName();
}

QString AssistantVoice::effectiveOpenaiVoice() const {
    if (!m_settings)
        return QStringLiteral("nova");
    return m_role == Role::Coaching ? m_settings->coachingOpenaiVoice() : m_settings->openaiVoice();
}

QString AssistantVoice::effectiveElevenlabsVoiceId() const {
    if (!m_settings)
        return QString();
    return m_role == Role::Coaching ? m_settings->coachingElevenlabsVoiceId()
                                    : m_settings->elevenlabsVoiceId();
}

QString AssistantVoice::openaiKey() const {
    // Prefer an explicit key from the assistant settings; else reuse the app's configured OpenAI key.
    if (m_settings && !m_settings->openaiApiKey().isEmpty())
        return m_settings->openaiApiKey();
    return (m_appSettings && m_appSettings->ai()) ? m_appSettings->ai()->openaiApiKey() : QString();
}

void AssistantVoice::speak(const QString& text) {
    if (!m_settings || text.trimmed().isEmpty())
        return;
    // The barista voice honors voiceEnabled() (the barista mute). The COACHING voice deliberately does NOT
    // — the live coaches have their own upstream enable gates (extractionAnnouncements for the shot coach,
    // steamCoachAudioEnabled for the steam coach), so muting the barista must not silence coaching.
    if (m_role == Role::Barista && !m_settings->voiceEnabled())
        return;
    // Mark speaking BEFORE dispatch so speakingChanged(true) fires synchronously — the mic pauses now,
    // not after the cloud-TTS POST finally starts playback (which is the "listening while talking" bug).
    ++m_speakGen;
    m_pendingSynth = true;
    updateSpeaking();
    const QString provider = effectiveProvider();
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
        {QStringLiteral("voice"), effectiveOpenaiVoice()},
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
                             .arg(effectiveElevenlabsVoiceId())));
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
    // ALTERNATE the filename each utterance: reusing one path makes the Android backend cache the prior
    // clip's DURATION and stop the new (longer) audio early (the cut-off-mid-sentence bug), and setSource
    // with the same URL is a no-op in Qt. A fresh path forces a clean reload with the correct duration.
    // Namespace the temp path by ROLE: the barista and coaching instances both cycle through the same
    // two-file rotation, so an un-namespaced path would let them clobber each other's clips — reviving the
    // Android "cut off mid-sentence" duration-cache bug across instances. A per-role prefix keeps them apart.
    const QString rolePrefix = (m_role == Role::Coaching) ? QStringLiteral("c") : QStringLiteral("b");
    const QString path = QDir::tempPath()
                       + QStringLiteral("/decenza_tts_%1%2.mp3").arg(rolePrefix).arg(m_ttsFileSeq++ % 2);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_pendingSynth = false; updateSpeaking();
        return;
    }
    f.write(audio);
    f.close();
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
    // Persist THIS role's native voice name; the settings signal drives applyVoiceFromSettings() + voiceNameChanged().
    if (!m_settings)
        return;
    if (m_role == Role::Coaching)
        m_settings->setCoachingVoiceName(name);
    else
        m_settings->setVoiceName(name);
}

void AssistantVoice::preview() {
    const QString sample = (m_role == Role::Coaching)
        ? QStringLiteral("On pace — about ten seconds to go.")   // a representative coaching cue
        : QStringLiteral("Hi, I'm %1. Ready when you are.")
              .arg(m_settings ? m_settings->assistantName() : QStringLiteral("Coach"));
    const QString provider = effectiveProvider();
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
    if (name == QLatin1String("custom")) {
        // The user's own sound file picked from the tablet (Settings → AI → Bell → Custom…). Stored as a
        // URL by the file picker (file:// on desktop, content:// on Android) — play it directly.
        const QString u = m_settings ? m_settings->bellCustomPath() : QString();
        if (u.isEmpty())
            return;
        m_bell->setSource(QUrl(u));
    } else {
        m_bell->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(name)));
    }
    m_bell->play();
}

void AssistantVoice::applyVoiceFromSettings() {
    if (!m_tts || !m_settings)
        return;
    const QString want = effectiveVoiceName();
    if (want.isEmpty())
        return;   // keep the engine default
    for (const QVoice& v : m_tts->availableVoices()) {
        if (v.name() == want) {
            m_tts->setVoice(v);
            break;
        }
    }
}
