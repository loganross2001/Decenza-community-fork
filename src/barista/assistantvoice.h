#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

#ifdef Q_OS_ANDROID
#include <QJniObject>   // [barista-fork] native MediaPlayer handle (m_androidPlayer), value member
#endif

class QTextToSpeech;
class QSoundEffect;
class QMediaPlayer;
class QAudioOutput;
class QBuffer;
class QNetworkAccessManager;
class AssistantSettings;
class Settings;

// [barista-fork] The assistant's spoken voice — its OWN QTextToSpeech instance (deliberately not
// routed through AccessibilityManager, so the persona voice is a separate channel from the live-coach
// accessibility cues). On Android QTextToSpeech already fronts the native engine incl. neural voices,
// so no JNI is needed here (that's only for speech input, later).
class AssistantVoice : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList availableVoices READ availableVoices NOTIFY availableVoicesChanged)
    Q_PROPERTY(QString voiceName READ voiceName NOTIFY voiceNameChanged)
    Q_PROPERTY(bool speaking READ speaking NOTIFY speakingChanged)   // for muting the mic while it talks
    // [barista-fork] True ONLY while real audio is actually coming out (no pending-synth hold) — drives the
    // avatar's talking animation so the mouth doesn't move during the network→prepare gap before sound. Kept
    // SEPARATE from `speaking` (which also gates the mic across that gap); never wire mic logic to this.
    Q_PROPERTY(bool audible READ audible NOTIFY audibleChanged)
    // [barista-fork] True while an ElevenLabs voice-preview sample is playing (voice-picker highlight state).
    Q_PROPERTY(bool previewPlaying READ previewPlaying NOTIFY previewPlayingChanged)
    // [barista-fork] True on platforms where audio plays through the native player (Android). The voice picker
    // uses this to route previews to the external speaker instead of Qt's built-in-only MediaPlayer.
    Q_PROPERTY(bool usesNativeAudio READ usesNativeAudio CONSTANT)
    // [barista-fork] True while a GET /v1/voices fetch is in flight — drives a spinner in the picker pop-up.
    Q_PROPERTY(bool fetchingVoices READ fetchingVoices NOTIFY fetchingVoicesChanged)

public:
    // [barista-fork] Which voice profile this instance reads from AssistantSettings. Barista = the
    // conversational assistant voice; Coaching = the SEPARATE voice for the live steam + espresso coaches.
    // Both share the ElevenLabs API key + saved-voices list; the provider, the three per-provider voice ids,
    // the speaking speed, AND the playback volume are all per-role. Crucially, the Coaching role does NOT honor voiceEnabled() (the
    // barista's mute): the live coaches have their own upstream enable gates.
    enum class Role { Barista, Coaching };

    AssistantVoice(AssistantSettings* settings, Settings* appSettings,
                   Role role = Role::Barista, QObject* parent = nullptr);
    ~AssistantVoice() override;

    QStringList availableVoices() const;   // voice names, for the picker
    QString voiceName() const;             // the currently active voice's name
    bool speaking() const { return m_speaking; }
    bool audible() const { return m_audible; }
    bool previewPlaying() const { return m_previewPlaying; }
    bool usesNativeAudio() const {
#ifdef Q_OS_ANDROID
        return true;
#else
        return false;
#endif
    }
    bool fetchingVoices() const { return m_fetchingVoices; }

    // [barista-fork] Fetch the ACCOUNT's ElevenLabs voices (GET /v1/voices with the shared xi-api-key) so the
    // owner picks from a refined pop-up instead of typing cryptic voice ids. Async, non-blocking, re-entry
    // guarded. On success emits elevenlabsVoicesFetched(list) where each entry is a QVariantMap
    // {name, id, category, accent, gender, age, previewUrl}; on any failure emits voicesFetchFailed(reason).
    // The voices list is ACCOUNT-WIDE, so this is called on the barista instance (Barista.voice) and the
    // result is treated as shared by both the barista and coaching sections.
    Q_INVOKABLE void fetchElevenlabsVoices();

    Q_INVOKABLE void speak(const QString& rawText);       // no-op when voice is muted; normalizes units/ratios for TTS
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setVoiceByName(const QString& name);  // persists to settings + applies
    Q_INVOKABLE void preview();                            // speak a sample line to audition a voice
    Q_INVOKABLE void playBell();                           // play the configured bell (unless "off")
    Q_INVOKABLE void previewBell(const QString& name);     // audition a specific bell (qrc:/sounds/<name>.wav)
    // [barista-fork] "Don't leave the user in silence" NON-VERBAL cue: a soft, fixed thinking tick played when
    // a slow op has run ~5s with nothing spoken yet. Deliberately NOT the configurable bell (which can be "off"
    // or a loud "ding") — it plays a fixed quiet asset at low volume so it's gentle and always present. No-op
    // when the barista voice is muted (voiceEnabled() off), so a muted barista stays silent.
    Q_INVOKABLE void playThinkingCue();

    // [barista-fork] Subtle "thinking" earcon LOOP — starts the moment a model turn goes in flight and plays
    // continuously (through the native player, so it follows the external-speaker route on Android) until real
    // audio is audible or the turn ends. Honors the barista mute + the "off" setting. The loop IS the new
    // still-working heartbeat (replaces the delayed tick). Idempotent: start while looping is a no-op.
    Q_INVOKABLE void startThinkingLoop();
    Q_INVOKABLE void stopThinkingLoop();
    // Audition the currently-selected thinking sound for a moment (settings picker), via the same cue path so
    // it plays out the external speaker.
    Q_INVOKABLE void previewThinkingSound();
    // [barista-fork] Play a brief subtle tone through the external speaker to WAKE it from power-save, so the
    // first real utterance after the app loads / the tablet wakes isn't clipped by the speaker powering up.
    Q_INVOKABLE void playWakeTone();
    // [barista-fork] Apply this role's current volume to the CURRENTLY-playing clip in real time (a moved
    // slider takes effect immediately, not just next utterance). Reads effectiveVolume() fresh.
    Q_INVOKABLE void applyLiveVolume();

    // [barista-fork] Play / stop an ElevenLabs voice-preview sample. `url` is the ElevenLabs preview_url; on
    // Android it plays through the native player (external speaker), on desktop the picker keeps its own player.
    Q_INVOKABLE void playPreviewUrl(const QString& url);
    Q_INVOKABLE void stopPreviewUrl();

#ifdef Q_OS_ANDROID
    // [barista-fork] Called on the Qt main thread from the DecenzaAudioPlayer JNI callbacks when native Android
    // playback actually starts / finishes. `tag` identifies which player (0=voice, 1=cue, 2=preview) so a cue
    // or preview callback never touches the barista's speaking state. Public only so the free JNI trampolines
    // can reach them; not part of the QML/Q_INVOKABLE surface.
    void handleAndroidPlaybackStarted(int tag);
    void handleAndroidPlaybackFinished(int tag);
#endif

signals:
    void availableVoicesChanged();
    void voiceNameChanged();
    void speakingChanged();
    void audibleChanged();            // [barista-fork] real audio started/stopped (avatar talking-animation)
    void previewPlayingChanged();     // [barista-fork] voice-preview sample started/stopped
    void fetchingVoicesChanged();
    // [barista-fork] Emitted when GET /v1/voices succeeds: a list of QVariantMaps (see fetchElevenlabsVoices).
    void elevenlabsVoicesFetched(const QVariantList& voices);
    // [barista-fork] Emitted on any fetch failure (empty key / network error / non-200), with a human reason.
    void voicesFetchFailed(const QString& reason);

private:
    // [barista-fork] Role-effective settings reads — resolve to the barista OR the coaching getters
    // depending on m_role, so all the synth code below stays role-agnostic.
    QString effectiveProvider() const;
    QString effectiveVoiceName() const;
    QString effectiveOpenaiVoice() const;
    QString effectiveElevenlabsVoiceId() const;
    // [barista-fork] Per-role speed (rate multiplier) + volume (0..1). Speed + volume are now split per role,
    // so these resolve to the barista OR coaching getters depending on m_role. Read fresh at speak/playback
    // time so a moved slider takes effect on the NEXT utterance without any NOTIFY wiring.
    double effectiveSpeed() const;
    double effectiveVolume() const;
    // Apply THIS role's rate + volume to the native QTextToSpeech engine, immediately before every say().
    // Kept in one place so the native fallbacks (empty key / cloud error) also honor speed + volume.
    void applyNativeParams();

    void applyVoiceFromSettings();
    void updateSpeaking();
    void synthOpenAI(const QString& text);       // POST OpenAI TTS → play the returned mp3
    void synthElevenLabs(const QString& text);   // POST ElevenLabs TTS → play the returned mp3
    void playMp3(const QByteArray& audio);       // play compressed audio via QMediaPlayer
    QString openaiKey() const;                   // reuse the app's OpenAI key
    // [barista-fork] The selected thinking-earcon asset ("hum"|"breath"|"pulse"|"drone" → qrc path); empty when
    // "off". On Android the qrc asset is extracted to a temp file once (the native MediaPlayer can't read qrc:).
    QString thinkingSoundName() const;
#ifdef Q_OS_ANDROID
    QString extractThinkingAssetToFile(const QString& name);   // qrc:/sounds/think-<name>.wav → temp path (cached)
#endif

    QTextToSpeech* m_tts = nullptr;
    QSoundEffect* m_bell = nullptr;
    // [barista-fork] SEPARATE soft-tick effect for the silence-breaker cue — kept apart from m_bell so setting a
    // low cue volume never leaks into the configurable bell (and vice-versa). Created lazily on first cue.
    QSoundEffect* m_cue = nullptr;
    // [barista-fork] Desktop looping thinking earcon (QSoundEffect loops qrc: natively). On Android the loop
    // plays through m_androidCue instead. Created lazily.
    QSoundEffect* m_thinkingLoop = nullptr;
    // [barista-fork] Desktop speaker-wake tone (Android plays it through m_androidCue). Created lazily.
    QSoundEffect* m_wakeTone = nullptr;
    // [barista-fork] Desktop voice-preview player (Android uses m_androidPreview). Created lazily.
    QMediaPlayer* m_previewPlayer = nullptr;
    QAudioOutput* m_previewOut = nullptr;
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audioOut = nullptr;
    QBuffer* m_audioBuffer = nullptr;
    QNetworkAccessManager* m_net = nullptr;
    AssistantSettings* m_settings = nullptr;
    Settings* m_appSettings = nullptr;
    Role m_role = Role::Barista;
    bool m_speaking = false;
    bool m_audible = false;          // [barista-fork] mirrors real-audio-playing (== updateSpeaking's `active`)
    bool m_previewPlaying = false;   // [barista-fork] a voice-preview sample is playing
    bool m_thinkingLooping = false;  // [barista-fork] the thinking earcon loop is currently running
    bool m_fetchingVoices = false;   // re-entry guard for fetchElevenlabsVoices()
    // [barista-fork] cloud TTS (QMediaPlayer) only reports Playing once the network POST completes; this
    // holds `speaking` true across that gap so the mic stays paused. m_speakGen discards a stale/late reply.
    bool m_pendingSynth = false;
    int m_speakGen = 0;
    // [barista-fork] alternate the cloud-TTS temp file each utterance (decenza_tts_0/_1.mp3). Reusing ONE
    // path makes Android's media backend cache the prior clip's DURATION and stop the new (longer) audio
    // early — the "cut off mid-sentence" bug. A fresh path each time forces a clean reload.
    int m_ttsFileSeq = 0;
#ifdef Q_OS_ANDROID
    // [barista-fork] Native Android MediaPlayer (USAGE_MEDIA) — the cloud-TTS playback path on Android, because
    // Qt can't see/route to the external speaker. m_androidPlaying mirrors its state (set by the JNI callbacks)
    // and stands in for the m_player playback check inside updateSpeaking().
    QJniObject m_androidPlayer;    // tag 0 — the barista/coaching voice
    QJniObject m_androidCue;       // tag 1 — the thinking earcon loop
    QJniObject m_androidPreview;   // tag 2 — ElevenLabs voice-preview samples
    bool m_androidPlaying = false;
#endif
};
