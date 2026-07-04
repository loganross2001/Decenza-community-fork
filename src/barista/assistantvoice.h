#pragma once

#include <QObject>
#include <QStringList>

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

public:
    AssistantVoice(AssistantSettings* settings, Settings* appSettings, QObject* parent = nullptr);

    QStringList availableVoices() const;   // voice names, for the picker
    QString voiceName() const;             // the currently active voice's name
    bool speaking() const { return m_speaking; }

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

private:
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
    bool m_speaking = false;
};
