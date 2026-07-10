#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

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
    // [barista-fork] True while a GET /v1/voices fetch is in flight — drives a spinner in the picker pop-up.
    Q_PROPERTY(bool fetchingVoices READ fetchingVoices NOTIFY fetchingVoicesChanged)

public:
    // [barista-fork] Which voice profile this instance reads from AssistantSettings. Barista = the
    // conversational assistant voice; Coaching = the SEPARATE voice for the live steam + espresso coaches.
    // Both share the ElevenLabs API key, saved-voices list, and voiceSpeed — only the provider + the three
    // per-provider voice ids differ. Crucially, the Coaching role does NOT honor voiceEnabled() (the
    // barista's mute): the live coaches have their own upstream enable gates.
    enum class Role { Barista, Coaching };

    AssistantVoice(AssistantSettings* settings, Settings* appSettings,
                   Role role = Role::Barista, QObject* parent = nullptr);

    QStringList availableVoices() const;   // voice names, for the picker
    QString voiceName() const;             // the currently active voice's name
    bool speaking() const { return m_speaking; }
    bool fetchingVoices() const { return m_fetchingVoices; }

    // [barista-fork] Fetch the ACCOUNT's ElevenLabs voices (GET /v1/voices with the shared xi-api-key) so the
    // owner picks from a refined pop-up instead of typing cryptic voice ids. Async, non-blocking, re-entry
    // guarded. On success emits elevenlabsVoicesFetched(list) where each entry is a QVariantMap
    // {name, id, category, accent, gender, age, previewUrl}; on any failure emits voicesFetchFailed(reason).
    // The voices list is ACCOUNT-WIDE, so this is called on the barista instance (Barista.voice) and the
    // result is treated as shared by both the barista and coaching sections.
    Q_INVOKABLE void fetchElevenlabsVoices();

    Q_INVOKABLE void speak(const QString& text);          // no-op when voice is muted
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setVoiceByName(const QString& name);  // persists to settings + applies
    Q_INVOKABLE void preview();                            // speak a sample line to audition a voice
    Q_INVOKABLE void playBell();                           // play the configured bell (unless "off")
    Q_INVOKABLE void previewBell(const QString& name);     // audition a specific bell (qrc:/sounds/<name>.wav)

signals:
    void availableVoicesChanged();
    void voiceNameChanged();
    void speakingChanged();
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

    void applyVoiceFromSettings();
    void updateSpeaking();
    void synthOpenAI(const QString& text);       // POST OpenAI TTS → play the returned mp3
    void synthElevenLabs(const QString& text);   // POST ElevenLabs TTS → play the returned mp3
    void playMp3(const QByteArray& audio);       // play compressed audio via QMediaPlayer
    QString openaiKey() const;                   // reuse the app's OpenAI key

    QTextToSpeech* m_tts = nullptr;
    QSoundEffect* m_bell = nullptr;
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audioOut = nullptr;
    QBuffer* m_audioBuffer = nullptr;
    QNetworkAccessManager* m_net = nullptr;
    AssistantSettings* m_settings = nullptr;
    Settings* m_appSettings = nullptr;
    Role m_role = Role::Barista;
    bool m_speaking = false;
    bool m_fetchingVoices = false;   // re-entry guard for fetchElevenlabsVoices()
    // [barista-fork] cloud TTS (QMediaPlayer) only reports Playing once the network POST completes; this
    // holds `speaking` true across that gap so the mic stays paused. m_speakGen discards a stale/late reply.
    bool m_pendingSynth = false;
    int m_speakGen = 0;
    // [barista-fork] alternate the cloud-TTS temp file each utterance (decenza_tts_0/_1.mp3). Reusing ONE
    // path makes Android's media backend cache the prior clip's DURATION and stop the new (longer) audio
    // early — the "cut off mid-sentence" bug. A fresh path each time forces a clean reload.
    int m_ttsFileSeq = 0;
};
