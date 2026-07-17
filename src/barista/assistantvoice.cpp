#include "assistantvoice.h"

#include <cmath>   // [barista-fork] std::pow for the perceptual volume curve
#include "assistantsettings.h"
#include "baristadiagnostics.h"  // [barista-fork] voice/coaching timeline recorder
#include "speechnormalize.h"   // [barista-fork] rewrite grams/ratios/units for reliable TTS pronunciation
#include "../core/settings.h"
#include "../core/settings_ai.h"

#include <QTextToSpeech>
#include <QVoice>
#include <QSoundEffect>
#include <QUrl>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJniEnvironment>
#include <QCoreApplication>
#include <QHash>
#include <QMutex>

namespace {
// [barista-fork] Route the DecenzaAudioPlayer JNI callbacks back to the right AssistantVoice instance. Java
// calls the static natives with the C++ `handle` (the AssistantVoice*); we validate it against this registry
// before ever dereferencing it — a raw handle is never trusted blind. Register/unregister run in the
// AssistantVoice ctor/dtor (main thread); the lookup runs inside a main-thread queued lambda, so a destroyed
// instance is already gone from the map by the time any late callback lands (dtor + lambda are the same thread,
// they can't interleave). The mutex guards the map against the JNI thread that queues the lambda.
QMutex g_androidVoiceMutex;
QHash<jlong, AssistantVoice*> g_androidVoiceRegistry;

void androidVoiceRegister(jlong handle, AssistantVoice* v) {
    QMutexLocker lock(&g_androidVoiceMutex);
    g_androidVoiceRegistry.insert(handle, v);
}
void androidVoiceUnregister(jlong handle) {
    QMutexLocker lock(&g_androidVoiceMutex);
    g_androidVoiceRegistry.remove(handle);
}
AssistantVoice* androidVoiceLookup(jlong handle) {
    QMutexLocker lock(&g_androidVoiceMutex);
    return g_androidVoiceRegistry.value(handle, nullptr);
}

// Static natives — invoked by Java on the Android main Looper. Hop to the Qt main thread, then look the
// instance up (safe: the dtor removed it on that same thread) and drive its state. `tag` (0=voice, 1=cue,
// 2=preview) selects which player fired so a cue/preview callback never touches the barista's speaking state.
void jniOnStarted(JNIEnv*, jclass, jlong handle, jint tag, jint durationMs) {
    QMetaObject::invokeMethod(qApp, [handle, tag, durationMs]() {
        if (AssistantVoice* v = androidVoiceLookup(handle))
            v->handleAndroidPlaybackStarted(static_cast<int>(tag), static_cast<int>(durationMs));
    }, Qt::QueuedConnection);
}
void jniOnFinished(JNIEnv*, jclass, jlong handle, jint tag) {
    QMetaObject::invokeMethod(qApp, [handle, tag]() {
        if (AssistantVoice* v = androidVoiceLookup(handle))
            v->handleAndroidPlaybackFinished(static_cast<int>(tag));
    }, Qt::QueuedConnection);
}

// Bind the two natives to the Java class. Once, on the first AssistantVoice — mirrors registerVoiceNatives()
// in voiceinput.cpp. Only latch `done` on success so a transient failure can be retried by the next instance.
void registerAndroidAudioPlayerNatives() {
    static bool done = false;
    if (done)
        return;
    QJniEnvironment env;
    JNINativeMethod methods[] = {
        {"nativeOnStarted",  "(JII)V", reinterpret_cast<void*>(jniOnStarted)},
        {"nativeOnFinished", "(JI)V", reinterpret_cast<void*>(jniOnFinished)},
    };
    if (env.registerNativeMethods("io/github/kulitorum/decenza_de1/DecenzaAudioPlayer", methods, 2))
        done = true;
}

// [barista-fork] Player tags — must match the ints passed to the DecenzaAudioPlayer Java constructor.
constexpr int kTagVoice = 0;
constexpr int kTagCue = 1;
constexpr int kTagPreview = 2;
}  // namespace
#endif  // Q_OS_ANDROID

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
#ifdef Q_OS_ANDROID
    // [barista-fork] On this tablet Qt's QMediaDevices only ever enumerates "Built in speaker", so
    // QMediaPlayer/QAudioOutput can't follow the system route to an external USB-C/Bluetooth speaker — the
    // barista kept playing out of the tablet no matter what. Play cloud TTS through Android's own MediaPlayer
    // (tagged USAGE_MEDIA) instead — it DOES follow the route. One native player per instance, keyed by `this`
    // so its callbacks come back to the right AssistantVoice (barista vs coaching). The QAudioOutput
    // device-follow below is dead here, so it's compiled out.
    registerAndroidAudioPlayerNatives();
    const jlong androidHandle = static_cast<jlong>(reinterpret_cast<quintptr>(this));
    androidVoiceRegister(androidHandle, this);
    // Three native players, same handle=this, distinct tags — so the voice, the thinking-loop cue, and the
    // voice-preview each play/stop independently and their callbacks route to the right handler.
    m_androidPlayer  = QJniObject("io/github/kulitorum/decenza_de1/DecenzaAudioPlayer", "(JI)V", androidHandle, static_cast<jint>(kTagVoice));
    m_androidCue     = QJniObject("io/github/kulitorum/decenza_de1/DecenzaAudioPlayer", "(JI)V", androidHandle, static_cast<jint>(kTagCue));
    m_androidPreview = QJniObject("io/github/kulitorum/decenza_de1/DecenzaAudioPlayer", "(JI)V", androidHandle, static_cast<jint>(kTagPreview));
#else
    // [barista-fork] Follow the CURRENT default output. A QAudioOutput pins to whatever device was default
    // when it was constructed (the tablet's own speaker) and does NOT switch when an external USB-C or
    // Bluetooth speaker connects later — so the barista kept playing out of the tablet. Bind to the live
    // default now, and re-bind whenever the set of audio outputs changes (a speaker connecting/disconnecting).
    m_audioOut->setDevice(QMediaDevices::defaultAudioOutput());
    {
        auto* mediaDevices = new QMediaDevices(this);
        connect(mediaDevices, &QMediaDevices::audioOutputsChanged, this, [this]() {
            m_audioOut->setDevice(QMediaDevices::defaultAudioOutput());
        });
    }
#endif
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

AssistantVoice::~AssistantVoice() {
#ifdef Q_OS_ANDROID
    // Stop native playback and drop out of the callback registry BEFORE the object dies, so a late JNI callback
    // can never find a dangling `this`.
    if (m_androidPlayer.isValid())
        m_androidPlayer.callMethod<void>("stop");
    if (m_androidCue.isValid())
        m_androidCue.callMethod<void>("stop");
    if (m_androidPreview.isValid())
        m_androidPreview.callMethod<void>("stop");
    androidVoiceUnregister(static_cast<jlong>(reinterpret_cast<quintptr>(this)));
#endif
}

void AssistantVoice::updateSpeaking() {
#ifdef Q_OS_ANDROID
    // On Android the cloud mp3 plays through the native MediaPlayer (m_androidPlaying), not m_player.
    const bool active = (m_tts && m_tts->state() == QTextToSpeech::Speaking) || m_androidPlaying;
#else
    const bool active = (m_tts && m_tts->state() == QTextToSpeech::Speaking)
                     || (m_player && m_player->playbackState() == QMediaPlayer::PlayingState);
#endif
    if (active)
        m_pendingSynth = false;       // real audio started — hand off from the pending flag
    // [barista-fork] `audible` = real audio out RIGHT NOW (== `active`, no pending hold). Purely additive: it
    // drives the avatar's mouth so it doesn't move during the network→prepare gap. Never gates the mic.
    if (active != m_audible) {
        m_audible = active;
        emit audibleChanged();
    }
    const bool now = m_pendingSynth || active;
    if (now == m_speaking)
        return;
    m_speaking = now;
    BaristaDiagnostics::record(QStringLiteral("voice"),
        now ? QStringLiteral("speaking_on") : QStringLiteral("speaking_off"),
        {{QStringLiteral("role"), m_role == Role::Barista ? QStringLiteral("barista") : QStringLiteral("coaching")}});
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

// [barista-fork] Per-role speed + volume. Read fresh on every utterance so a moved slider applies to the
// NEXT thing spoken with no signal wiring.
double AssistantVoice::effectiveSpeed() const {
    if (!m_settings)
        return 1.0;
    return m_role == Role::Coaching ? m_settings->coachingVoiceSpeed() : m_settings->baristaVoiceSpeed();
}

double AssistantVoice::effectiveVolume() const {
    if (!m_settings)
        return 1.0;
    const double raw = m_role == Role::Coaching ? m_settings->coachingVoiceVolume()
                                                : m_settings->baristaVoiceVolume();
    // [barista-fork] PERCEPTUAL curve: loudness is ~logarithmic, so a LINEAR gain made low-end slider steps
    // feel like huge jumps and high-end steps inaudible. Map slider position → gain as raw^2 so equal slider
    // movement ≈ equal PERCEIVED loudness change. (raw^2.5 was too aggressive — 0.21 → ~0.02 = ~2% actual, too
    // quiet at the low end; raw^2 gives 0.21 → ~0.044, a usable low end while still smoother than linear.)
    const double v = raw < 0.0 ? 0.0 : (raw > 1.0 ? 1.0 : raw);
    return v * v;
}

// Apply this role's rate + volume to the native engine right before say(). QTextToSpeech::setRate takes
// [-1, 1]; map the ~0.7–1.3 speed multiplier to that range (1.0 → 0). setVolume takes [0, 1] directly.
void AssistantVoice::applyNativeParams() {
    if (!m_tts)
        return;
    m_tts->setRate((effectiveSpeed() - 1.0) / 0.5);   // map ~0.7–1.3 → rate -0.6..0.6
    m_tts->setVolume(effectiveVolume());              // 0..1 linear gain
}

QString AssistantVoice::openaiKey() const {
    // Prefer an explicit key from the assistant settings; else reuse the app's configured OpenAI key.
    if (m_settings && !m_settings->openaiApiKey().isEmpty())
        return m_settings->openaiApiKey();
    return (m_appSettings && m_appSettings->ai()) ? m_appSettings->ai()->openaiApiKey() : QString();
}

void AssistantVoice::speak(const QString& rawText) {
    if (!m_settings || rawText.trimmed().isEmpty())
        return;
    // Rewrite compact espresso notation ("18.8g", "1:2.4", "93°C") to words the
    // TTS engines pronounce reliably. Done ONCE here, before dispatch, so it
    // covers ALL providers (native / OpenAI / ElevenLabs) AND both roles — the
    // native fallbacks inside synthOpenAI/synthElevenLabs reuse their `text`
    // arg, so they inherit the normalized string for free. Display text is a
    // separate string (built in QML) and is untouched.
    const QString text = speechnormalize::normalizeForSpeech(rawText);
    // The barista voice honors voiceEnabled() (the barista mute). The COACHING voice deliberately does NOT
    // — the live coaches have their own upstream enable gates (extractionAnnouncements for the shot coach,
    // steamCoachAudioEnabled for the steam coach), so muting the barista must not silence coaching.
    if (m_role == Role::Barista && !m_settings->voiceEnabled()) {
        BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("speak_suppressed_muted"),
            {{QStringLiteral("chars"), rawText.size()}, {QStringLiteral("say"), rawText.left(80)}});
        return;
    }
    // [barista-fork][diag] A new speak() while still talking is the "trips over itself" cut-off — record it.
    const bool wasSpeaking = m_speaking;
    // Mark speaking BEFORE dispatch so speakingChanged(true) fires synchronously — the mic pauses now,
    // not after the cloud-TTS POST finally starts playback (which is the "listening while talking" bug).
    ++m_speakGen;
    m_pendingSynth = true;
    updateSpeaking();
    const QString provider = effectiveProvider();
    // [barista-fork][audio-diag] Capture the ROUTE so we can see why the barista plays on the tablet while
    // system audio uses the external speaker: which synth path (cloud mp3 vs native TTS), what Qt thinks the
    // default output is, what device our QAudioOutput is bound to, and the full list Qt enumerates.
    QStringList outNames;
    for (const QAudioDevice& d : QMediaDevices::audioOutputs())
        outNames << d.description();
    BaristaDiagnostics::record(QStringLiteral("voice"),
        wasSpeaking ? QStringLiteral("speak_INTERRUPTS_previous") : QStringLiteral("speak_start"),
        {{QStringLiteral("role"), m_role == Role::Barista ? QStringLiteral("barista") : QStringLiteral("coaching")},
         {QStringLiteral("provider"), provider},
         {QStringLiteral("chars"), rawText.size()},
         {QStringLiteral("gen"), static_cast<int>(m_speakGen)},
         {QStringLiteral("defaultOut"), QMediaDevices::defaultAudioOutput().description()},
         {QStringLiteral("boundOut"), m_audioOut ? m_audioOut->device().description() : QStringLiteral("none")},
         {QStringLiteral("outputs"), outNames.join(QStringLiteral(" | "))},
         {QStringLiteral("say"), rawText.left(60)}});
    if (provider == QLatin1String("openai"))
        synthOpenAI(text);
    else if (provider == QLatin1String("elevenlabs"))
        synthElevenLabs(text);
    else if (m_tts) {
        // [barista-fork][audio-diag] Native TTS bypasses m_audioOut entirely — Android TextToSpeech does its
        // own routing, so if THIS is the path, the QAudioOutput device fix can't affect it.
        BaristaDiagnostics::record(QStringLiteral("audio"), QStringLiteral("native_tts_path"),
            {{QStringLiteral("role"), m_role == Role::Barista ? QStringLiteral("barista") : QStringLiteral("coaching")}});
        applyNativeParams();   // this role's rate + volume
        m_tts->say(text);   // native engine (stateChanged hands off from m_pendingSynth)
    } else {
        m_pendingSynth = false;   // no engine at all → nothing will speak
        updateSpeaking();
    }
}

void AssistantVoice::synthOpenAI(const QString& text) {
    const QString key = openaiKey();
    if (key.isEmpty()) {                 // no key → graceful fallback to the native voice
        if (m_tts) { applyNativeParams(); m_tts->say(text); }
        else { m_pendingSynth = false; updateSpeaking(); }
        return;
    }
    QNetworkRequest req(QUrl(QStringLiteral("https://api.openai.com/v1/audio/speech")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    // OpenAI accepts speed in [0.25, 4.0]; our clamp keeps it well inside. Volume is applied at playback
    // (there's no request-side volume) via m_audioOut in playMp3().
    const QJsonObject body{
        {QStringLiteral("model"), QStringLiteral("tts-1")},
        {QStringLiteral("voice"), effectiveOpenaiVoice()},
        {QStringLiteral("input"), text},
        {QStringLiteral("response_format"), QStringLiteral("mp3")},
        {QStringLiteral("speed"), effectiveSpeed()},    // this role's user-adjustable pace
    };
    QNetworkReply* reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text, gen = m_speakGen] {
        if (gen == m_speakGen) {   // ignore a superseded / dismissed request's late reply
            if (reply->error() == QNetworkReply::NoError)
                playMp3(reply->readAll());
            else if (m_tts) { applyNativeParams(); m_tts->say(text); }   // cloud failed → speak natively, not silent
            else { m_pendingSynth = false; updateSpeaking(); }
        }
        reply->deleteLater();
    });
}

void AssistantVoice::synthElevenLabs(const QString& text) {
    const QString key = m_settings->elevenlabsApiKey();
    if (key.isEmpty()) {                 // no key → graceful fallback to the native voice
        if (m_tts) { applyNativeParams(); m_tts->say(text); }
        else { m_pendingSynth = false; updateSpeaking(); }
        return;
    }
    QNetworkRequest req(QUrl(QStringLiteral("https://api.elevenlabs.io/v1/text-to-speech/%1")
                             .arg(effectiveElevenlabsVoiceId())));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("xi-api-key", key.toUtf8());
    // [barista-fork] SPEED: ElevenLabs supports a per-request voice_settings.speed for this model, so we use
    // the API control (no media-player playbackRate fallback needed — that would also shift pitch). ElevenLabs'
    // accepted range is 0.7–1.2 ONLY; a value outside it 422s the whole request → a silent native fallback. Our
    // settings clamp is 0.7–1.3 (shared with native/OpenAI, which tolerate 1.3), so clamp the SENT value down to
    // 1.2 here independently. VOLUME has no request-side control on ElevenLabs → applied at playback in playMp3().
    double elSpeed = effectiveSpeed();
    if (elSpeed > 1.2) elSpeed = 1.2;
    if (elSpeed < 0.7) elSpeed = 0.7;
    // [barista-fork] ANTI-STUTTER: eleven_turbo_v2_5 with an unset/low stability tends to stutter and repeat
    // words mid-sentence (owner heard it "stutter in the middle of talking", esp. on numbers). Sending an
    // explicit stability + similarity_boost pins the model to a steadier read that doesn't wander into repeats.
    // 0.5 is the balanced value (not so high it goes monotone); similarity_boost keeps the chosen voice's timbre.
    // Model is user-selectable in settings (speed↔quality/stutter tradeoff); default eleven_turbo_v2_5.
    QString modelId = m_settings->elevenlabsModel();
    if (modelId.isEmpty())
        modelId = QStringLiteral("eleven_turbo_v2_5");
    const QJsonObject body{
        {QStringLiteral("text"), text},
        {QStringLiteral("model_id"), modelId},
        {QStringLiteral("voice_settings"), QJsonObject{
            {QStringLiteral("speed"), elSpeed},
            {QStringLiteral("stability"), 0.5},
            {QStringLiteral("similarity_boost"), 0.8},
        }},
    };
    QNetworkReply* reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text, gen = m_speakGen] {
        if (gen == m_speakGen) {   // ignore a superseded / dismissed request's late reply
            if (reply->error() == QNetworkReply::NoError)
                playMp3(reply->readAll());
            else if (m_tts) { applyNativeParams(); m_tts->say(text); }   // cloud failed → speak natively, not silent
            else { m_pendingSynth = false; updateSpeaking(); }
        }
        reply->deleteLater();
    });
}

void AssistantVoice::fetchElevenlabsVoices() {
    // [barista-fork] PRIVACY: this GET talks ONLY to api.elevenlabs.io with the user's OWN ElevenLabs key —
    // the exact same host + xi-api-key header already used for TTS above (synthElevenLabs). It reads the
    // account's voice list so the owner can pick from a pop-up instead of typing voice ids. No new network
    // destination, no new credential, no telemetry — no new privacy surface beyond the TTS calls we make.
    if (m_fetchingVoices)
        return;   // re-entry guard: a fetch is already in flight
    const QString key = m_settings ? m_settings->elevenlabsApiKey() : QString();
    if (key.isEmpty()) {
        emit voicesFetchFailed(tr("No ElevenLabs API key set."));
        return;
    }
    m_fetchingVoices = true;
    emit fetchingVoicesChanged();

    QNetworkRequest req(QUrl(QStringLiteral("https://api.elevenlabs.io/v1/voices")));
    req.setRawHeader("xi-api-key", key.toUtf8());   // same auth idiom as synthElevenLabs()
    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        // Clear the guard on EVERY exit path (success, non-200, network error) so the spinner never wedges.
        m_fetchingVoices = false;
        emit fetchingVoicesChanged();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            QString reason;
            if (status == 401 || status == 403)
                reason = tr("Invalid ElevenLabs API key.");
            else if (status != 0)
                reason = tr("ElevenLabs returned an error (%1).").arg(status);
            else
                reason = tr("Network error: %1").arg(reply->errorString());
            emit voicesFetchFailed(reason);
            reply->deleteLater();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        const QJsonArray arr = doc.object().value(QStringLiteral("voices")).toArray();
        QVariantList out;
        out.reserve(arr.size());
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            // [barista-fork] Show only the user's OWN voices ("My Voices"), matching what they see in the
            // ElevenLabs app. GET /v1/voices returns the account's WHOLE library — including ElevenLabs'
            // "premade" (stock) voices — which is the "way more voices than I selected" complaint AND the
            // source of the "garbled/incomplete" metadata (stock rows often carry sparse labels). The
            // reliable owner-vs-stock discriminator is the per-voice `category`: `premade` = ElevenLabs
            // stock; `cloned` / `professional` / `generated` = the user's own. Drop premade at the fetch so
            // BOTH the picker and the "Save all" import see the filtered set.
            const QString category = o.value(QStringLiteral("category")).toString();
            if (category == QLatin1String("premade"))
                continue;
            const QJsonObject labels = o.value(QStringLiteral("labels")).toObject();
            QVariantMap m;
            m.insert(QStringLiteral("name"), o.value(QStringLiteral("name")).toString());
            m.insert(QStringLiteral("id"), o.value(QStringLiteral("voice_id")).toString());
            m.insert(QStringLiteral("category"), o.value(QStringLiteral("category")).toString());
            m.insert(QStringLiteral("accent"), labels.value(QStringLiteral("accent")).toString());
            m.insert(QStringLiteral("gender"), labels.value(QStringLiteral("gender")).toString());
            m.insert(QStringLiteral("age"), labels.value(QStringLiteral("age")).toString());
            // [barista-fork] ElevenLabs labels also carry the intended use case (key "use case" in current
            // responses, "use_case" in older ones) and an optional free-text description. Capture both so the
            // picker's search can match on them and show the use case as a chip. Same host/key/response as above
            // — no new network surface.
            QString useCase = labels.value(QStringLiteral("use case")).toString();
            if (useCase.isEmpty())
                useCase = labels.value(QStringLiteral("use_case")).toString();
            m.insert(QStringLiteral("useCase"), useCase);
            m.insert(QStringLiteral("description"), o.value(QStringLiteral("description")).toString());
            m.insert(QStringLiteral("previewUrl"), o.value(QStringLiteral("preview_url")).toString());
            out.append(m);
        }
        emit elevenlabsVoicesFetched(out);
    });
}

void AssistantVoice::playMp3(const QByteArray& audio) {
    if (audio.isEmpty()) {
        m_pendingSynth = false; updateSpeaking();   // no audio will play → release the pending hold
        return;
    }
#ifndef Q_OS_ANDROID
    if (!m_player) {
        m_pendingSynth = false; updateSpeaking();
        return;
    }
    m_player->stop();
    // [barista-fork] Apply THIS role's playback volume to the cloud-TTS output. Cloud providers (OpenAI /
    // ElevenLabs) have no request-side volume, so gain is applied here on the QAudioOutput driving the mp3
    // player. Read fresh so a moved slider takes effect on the next utterance. (Desktop only — on Android the
    // native MediaPlayer below owns routing + volume, and Qt can't see the external device anyway.)
    if (m_audioOut) {
        // Re-bind to the live default output every utterance too (belt-and-suspenders on top of the
        // audioOutputsChanged signal, which is unreliable on some Android builds) — so a speaker connected
        // mid-session is used immediately instead of falling back to the tablet.
        m_audioOut->setDevice(QMediaDevices::defaultAudioOutput());
        m_audioOut->setVolume(effectiveVolume());
    }
#endif
    // Android's media backend truncates in-memory (QBuffer) sources after a fraction of a second —
    // write the mp3 to a temp file and play that; files play reliably and to completion.
    // ALTERNATE the filename each utterance: reusing one path makes the Android backend cache the prior
    // clip's DURATION and stop the new (longer) audio early (the cut-off-mid-sentence bug), and setSource
    // with the same URL is a no-op in Qt. A fresh path forces a clean reload with the correct duration.
    // Namespace the temp path by ROLE: the barista and coaching instances both cycle through the same
    // two-file rotation, so an un-namespaced path would let them clobber each other's clips — reviving the
    // Android "cut off mid-sentence" duration-cache bug across instances. A per-role prefix keeps them apart.
    // (The desktop QMediaPlayer path plays the same local file.)
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
#ifdef Q_OS_ANDROID
    // Hand the file to Android's MediaPlayer (USAGE_MEDIA) — it follows the system route to the external
    // speaker, unlike Qt. m_pendingSynth stays true (set in speak()) until the native onStarted callback flips
    // m_androidPlaying on, so `speaking` never drops during the network→prepare gap. Java releases the prior
    // clip + a playId guard suppresses its late callbacks, so this cleanly supersedes a barge-in.
    m_androidPlaying = false;
    if (m_androidPlayer.isValid()) {
        // [barista-fork] Log the volume actually sent to the native player — so a "volume slider does nothing"
        // report can be told apart from a Bluetooth absolute-volume no-op (value is right, BT ignored it).
        BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("native_play_volume"),
            {{QStringLiteral("vol"), QString::number(effectiveVolume(), 'f', 2)},
             {QStringLiteral("role"), m_role == Role::Barista ? QStringLiteral("barista") : QStringLiteral("coaching")}});
        m_androidPlayer.callMethod<void>("play", "(Ljava/lang/String;F)V",
            QJniObject::fromString(path).object<jstring>(),
            static_cast<jfloat>(effectiveVolume()));
    } else {
        m_pendingSynth = false; updateSpeaking();   // no native player → don't wedge `speaking`
    }
#else
    m_player->setSource(QUrl());
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
#endif
}

void AssistantVoice::stop() {
    ++m_speakGen;              // discard any in-flight synth reply
    m_pendingSynth = false;
    setPlaybackDurationMs(0);  // [barista-fork] clear so a barged-into clip's length can't scroll the next utterance
    if (m_tts)
        m_tts->stop();
#ifdef Q_OS_ANDROID
    if (m_androidPlayer.isValid())
        m_androidPlayer.callMethod<void>("stop");   // bumps Java's playId → its pending callbacks go quiet
    m_androidPlaying = false;
#else
    if (m_player)
        m_player->stop();
#endif
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
    // Same units/ratios normalization as speak() so an audition is faithful to
    // what real utterances sound like. (The fixed sample lines below have no
    // units/ratios, so this is a no-op today — kept for parity + future samples.)
    const QString sample = speechnormalize::normalizeForSpeech((m_role == Role::Coaching)
        ? QStringLiteral("On pace — about ten seconds to go.")   // a representative coaching cue
        : QStringLiteral("Hi, I'm %1. Ready when you are.")
              .arg(m_settings ? m_settings->assistantName() : QStringLiteral("Coach")));
    const QString provider = effectiveProvider();
    if (provider == QLatin1String("openai"))
        synthOpenAI(sample);
    else if (provider == QLatin1String("elevenlabs"))
        synthElevenLabs(sample);
    else if (m_tts) {
        applyNativeParams();     // audition with this role's rate + volume
        m_tts->say(sample);      // speaks even if muted
    }
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

void AssistantVoice::playThinkingCue() {
    // [barista-fork] Non-verbal silence-breaker. Honor the barista mute (a muted barista stays fully silent),
    // and use a FIXED soft asset at low volume — never the configurable bell (which may be "off" or loud). The
    // QML side already skips this when the coaching voice is speaking (the speech arbiter), so we don't re-check
    // here. A gentle "still working" tick, deliberately quiet and non-jarring.
    if (m_role == Role::Barista && m_settings && !m_settings->voiceEnabled())
        return;
    if (!m_cue) {
        m_cue = new QSoundEffect(this);
        m_cue->setSource(QUrl(QStringLiteral("qrc:/sounds/tick.wav")));
        m_cue->setVolume(0.25);   // low — a subtle presence, not an alert
    }
    m_cue->play();
}

// [barista-fork] The selected thinking-earcon short-name ("hum"|"breath"|"pulse"|"drone"), or empty for "off".
QString AssistantVoice::thinkingSoundName() const {
    const QString s = m_settings ? m_settings->thinkingSound() : QStringLiteral("hum");
    if (s == QLatin1String("off"))
        return QString();
    if (s == QLatin1String("breath") || s == QLatin1String("pulse") || s == QLatin1String("drone"))
        return s;
    return QStringLiteral("hum");   // default / unknown → the soft hum
}

#ifdef Q_OS_ANDROID
// Android's MediaPlayer can't read qrc: URLs, so copy the packaged sound asset (e.g. "think-hum.wav" or
// "keepalive.wav") to a stable temp file once and hand it the path. Returns empty on failure.
QString AssistantVoice::extractSoundAssetToFile(const QString& fileName) {
    const QString dest = QDir::tempPath() + QStringLiteral("/decenza_snd_") + fileName;
    if (QFile::exists(dest) && QFileInfo(dest).size() > 0)
        return dest;
    QFile src(QStringLiteral(":/sounds/") + fileName);   // qrc alias for qrc:/sounds/...
    if (!src.open(QIODevice::ReadOnly))
        return QString();
    QFile out(dest);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) { src.close(); return QString(); }
    out.write(src.readAll());
    out.close();
    src.close();
    return dest;
}
#endif

void AssistantVoice::startThinkingLoop() {
    // Honor the barista mute (a muted barista stays fully silent).
    if (m_role == Role::Barista && m_settings && !m_settings->voiceEnabled())
        return;
    if (m_thinkingLooping)         // idempotent — already looping
        return;
    // [barista-fork] The cue player also serves as a SPEAKER KEEPALIVE: it runs through the model-thinking AND
    // the synth/prepare gap before speech, so the BT speaker never gates off and clips the first words (stopped
    // in handleAndroidPlaybackStarted the instant real audio starts). When the earcon is "off", we still loop a
    // sub-perceptible keepalive.wav (true silence lets the A2DP sink sleep) so "off" users get the wake benefit.
    const QString name = thinkingSoundName();   // empty => "off"
    const bool keepaliveOnly = name.isEmpty();
    const QString soundLabel = keepaliveOnly ? QStringLiteral("keepalive") : name;
    const QString fileName   = keepaliveOnly ? QStringLiteral("keepalive.wav")
                                             : QStringLiteral("think-%1.wav").arg(name);
    m_thinkingLooping = true;
    // [barista-fork] User-configurable pulse gain (SettingsAITab / Coaching / Barista-options slider). keepalive.wav
    // stays near unity (authored sub-perceptible); the audible hums scale by thinkingVolume (default 0.5).
    const double humVol = m_settings ? m_settings->thinkingVolume() : 0.5;
    BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("thinking_loop_on"), {{QStringLiteral("sound"), soundLabel}});
#ifdef Q_OS_ANDROID
    const QString path = extractSoundAssetToFile(fileName);
    const float gain = keepaliveOnly ? 1.0f : static_cast<float>(humVol);
    if (!path.isEmpty() && m_androidCue.isValid()) {
        m_androidCue.callMethod<void>("playLooping", "(Ljava/lang/String;F)V",
            QJniObject::fromString(path).object<jstring>(), static_cast<jfloat>(gain));
    }
#else
    if (!m_thinkingLoop) {
        m_thinkingLoop = new QSoundEffect(this);
        m_thinkingLoop->setLoopCount(QSoundEffect::Infinite);
    }
    m_thinkingLoop->setVolume(keepaliveOnly ? 1.0 : humVol);
    m_thinkingLoop->setSource(QUrl(QStringLiteral("qrc:/sounds/") + fileName));
    m_thinkingLoop->play();
#endif
}

void AssistantVoice::stopThinkingLoop() {
    if (!m_thinkingLooping)
        return;
    m_thinkingLooping = false;
    BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("thinking_loop_off"), {});
#ifdef Q_OS_ANDROID
    if (m_androidCue.isValid())
        m_androidCue.callMethod<void>("stop");
#else
    if (m_thinkingLoop)
        m_thinkingLoop->stop();
#endif
}

// Audition the selected thinking sound briefly (settings picker) through the cue path so it plays out the
// external speaker. A short UI auto-stop (a genuine audition timeout, not a guard).
void AssistantVoice::previewThinkingSound() {
    if (thinkingSoundName().isEmpty())
        return;
    // Temporarily bypass the mute gate for an explicit audition? No — auditioning while muted is confusing;
    // startThinkingLoop already honors mute, matching previewBell's behavior of not fighting the config.
    m_thinkingLooping = false;   // force a fresh start even if a stale flag lingers
    startThinkingLoop();
    QTimer::singleShot(2200, this, [this]() { stopThinkingLoop(); });
}

// [barista-fork] Wake a sleeping BT/USB speaker with a brief subtle tone so the first real utterance after the
// app loads / the tablet wakes isn't clipped while the speaker powers up. Plays through the native cue player
// (external-speaker route) on Android; a QSoundEffect on desktop. Honors the barista mute (a muted barista has
// no upcoming utterance to protect, and must stay silent). Only the barista role wakes the speaker.
void AssistantVoice::playWakeTone() {
    if (m_role != Role::Barista)
        return;
    if (m_settings && !m_settings->voiceEnabled())
        return;
    if (m_thinkingLooping)   // [barista-fork] the cue loop already keeps the speaker awake → no wake tone needed
        return;
    BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("wake_tone"), {});
#ifdef Q_OS_ANDROID
    const QString dest = QDir::tempPath() + QStringLiteral("/decenza_wake.wav");
    if (!(QFile::exists(dest) && QFileInfo(dest).size() > 0)) {
        QFile src(QStringLiteral(":/sounds/wake-tone.wav"));   // qrc → temp (Android MediaPlayer can't read qrc:)
        if (src.open(QIODevice::ReadOnly)) {
            QFile out(dest);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) { out.write(src.readAll()); out.close(); }
            src.close();
        }
    }
    if (QFile::exists(dest) && m_androidCue.isValid())   // one-shot on the cue player (no hum is active at wake time)
        m_androidCue.callMethod<void>("play", "(Ljava/lang/String;F)V",
            QJniObject::fromString(dest).object<jstring>(), static_cast<jfloat>(0.5));
#else
    if (!m_wakeTone) {
        m_wakeTone = new QSoundEffect(this);
        m_wakeTone->setSource(QUrl(QStringLiteral("qrc:/sounds/wake-tone.wav")));
        m_wakeTone->setVolume(0.4);
    }
    m_wakeTone->play();
#endif
}

// [barista-fork] Push this role's current volume to the live clip so a moved slider is heard immediately.
void AssistantVoice::applyLiveVolume() {
    const double v = effectiveVolume();
#ifdef Q_OS_ANDROID
    if (m_androidPlayer.isValid())
        m_androidPlayer.callMethod<void>("setVolume", "(F)V", static_cast<jfloat>(v));
#else
    if (m_audioOut)
        m_audioOut->setVolume(v);
#endif
}

void AssistantVoice::playPreviewUrl(const QString& url) {
    if (url.isEmpty())
        return;
#ifdef Q_OS_ANDROID
    if (m_androidPreview.isValid()) {
        m_androidPreview.callMethod<void>("play", "(Ljava/lang/String;F)V",
            QJniObject::fromString(url).object<jstring>(), static_cast<jfloat>(1.0));
    }
#else
    // Desktop: the voice picker uses its own QML player (usesNativeAudio=false), so this is only a fallback.
    if (!m_previewPlayer) {
        m_previewOut = new QAudioOutput(this);
        m_previewPlayer = new QMediaPlayer(this);
        m_previewPlayer->setAudioOutput(m_previewOut);
        connect(m_previewPlayer, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState s) {
            const bool p = (s == QMediaPlayer::PlayingState);
            if (p != m_previewPlaying) { m_previewPlaying = p; emit previewPlayingChanged(); }
        });
    }
    m_previewPlayer->setSource(QUrl(url));
    m_previewPlayer->play();
#endif
}

void AssistantVoice::stopPreviewUrl() {
#ifdef Q_OS_ANDROID
    if (m_androidPreview.isValid())
        m_androidPreview.callMethod<void>("stop");
    if (m_previewPlaying) { m_previewPlaying = false; emit previewPlayingChanged(); }
#else
    if (m_previewPlayer)
        m_previewPlayer->stop();
#endif
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

#ifdef Q_OS_ANDROID
// [barista-fork] Driven (on the Qt main thread) by the DecenzaAudioPlayer JNI callbacks. `tag` selects which
// player fired: only the VOICE tag touches speaking; CUE + PREVIEW never do.
void AssistantVoice::handleAndroidPlaybackStarted(int tag, int durationMs) {
    const QString roleStr = m_role == Role::Barista ? QStringLiteral("barista") : QStringLiteral("coaching");
    if (tag == kTagVoice) {
        // Real voice audio is now out — kill any thinking loop immediately (no hum-over-voice overlap).
        stopThinkingLoop();
        m_androidPlaying = true;
        m_pendingSynth = false;   // real audio is now playing — hand off from the pending hold
        // [barista-fork] Publish the clip's real duration so the UI can time the read-along text scroll to the
        // actual speech length instead of a character-count estimate. -1 = looping / unknown → UI falls back.
        setPlaybackDurationMs(durationMs);
        BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("native_playback_started"),
            {{QStringLiteral("role"), roleStr}, {QStringLiteral("tag"), tag},
             {QStringLiteral("durationMs"), durationMs}});
        updateSpeaking();
    } else if (tag == kTagPreview) {
        if (!m_previewPlaying) { m_previewPlaying = true; emit previewPlayingChanged(); }
        BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("preview_on"), {{QStringLiteral("role"), roleStr}});
    }
    // tag == kTagCue: the loop's one start callback — nothing to drive (loop-on is logged from startThinkingLoop()).
}

void AssistantVoice::handleAndroidPlaybackFinished(int tag) {
    const QString roleStr = m_role == Role::Barista ? QStringLiteral("barista") : QStringLiteral("coaching");
    if (tag == kTagVoice) {
        m_androidPlaying = false;
        // [barista-fork] Release the pending-synth hold on EVERY voice finish. On a normal completion this is a
        // no-op (handleAndroidPlaybackStarted already cleared it), but when the native player ERRORS before it
        // ever starts (onError → nativeOnFinished with no nativeOnStarted), m_pendingSynth would otherwise stay
        // true → speaking stuck true → the thinking-hum's want-condition never clears and the pulse lingers
        // forever. Clearing it here is the definitive "this utterance is over" signal so speaking can settle.
        m_pendingSynth = false;
        setPlaybackDurationMs(0);   // clip done — clear so the next utterance's scroll doesn't reuse a stale length
        BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("native_playback_finished"),
            {{QStringLiteral("role"), roleStr}, {QStringLiteral("tag"), tag}});
        updateSpeaking();
    } else if (tag == kTagPreview) {
        if (m_previewPlaying) { m_previewPlaying = false; emit previewPlayingChanged(); }
        BaristaDiagnostics::record(QStringLiteral("voice"), QStringLiteral("preview_off"), {{QStringLiteral("role"), roleStr}});
    }
    // tag == kTagCue: a looping cue only "finishes" on error — treat as loop stopped, no state to unwind.
}
#endif
