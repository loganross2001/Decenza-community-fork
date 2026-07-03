#pragma once

#include <QObject>
#include <QStringList>

class QTextToSpeech;
class AssistantSettings;

// [barista-fork] The assistant's spoken voice — its OWN QTextToSpeech instance (deliberately not
// routed through AccessibilityManager, so the persona voice is a separate channel from the live-coach
// accessibility cues). On Android QTextToSpeech already fronts the native engine incl. neural voices,
// so no JNI is needed here (that's only for speech input, later).
class AssistantVoice : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList availableVoices READ availableVoices NOTIFY availableVoicesChanged)
    Q_PROPERTY(QString voiceName READ voiceName NOTIFY voiceNameChanged)

public:
    AssistantVoice(AssistantSettings* settings, QObject* parent = nullptr);

    QStringList availableVoices() const;   // voice names, for the picker
    QString voiceName() const;             // the currently active voice's name

    Q_INVOKABLE void speak(const QString& text);          // no-op when voice is muted
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setVoiceByName(const QString& name);  // persists to settings + applies
    Q_INVOKABLE void preview();                            // speak a sample line to audition a voice

signals:
    void availableVoicesChanged();
    void voiceNameChanged();

private:
    void applyVoiceFromSettings();

    QTextToSpeech* m_tts = nullptr;
    AssistantSettings* m_settings = nullptr;
};
