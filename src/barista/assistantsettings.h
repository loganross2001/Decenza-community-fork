#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

// [barista-fork] The barista module's OWN settings, stored under its own "barista/" QSettings
// group. Deliberately NOT a Settings-facade domain sub-object, so the upstream Settings classes
// are never edited (the whole point of the modular fork). QML reaches it as `Barista.settings.<prop>`.
class AssistantSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool voiceEnabled READ voiceEnabled WRITE setVoiceEnabled NOTIFY voiceEnabledChanged)
    Q_PROPERTY(QString assistantName READ assistantName WRITE setAssistantName NOTIFY assistantNameChanged)
    Q_PROPERTY(QString voiceName READ voiceName WRITE setVoiceName NOTIFY voiceNameChanged)
    Q_PROPERTY(QString userName READ userName WRITE setUserName NOTIFY userNameChanged)
    Q_PROPERTY(QString bellSound READ bellSound WRITE setBellSound NOTIFY bellSoundChanged)
    Q_PROPERTY(QString ttsProvider READ ttsProvider WRITE setTtsProvider NOTIFY ttsProviderChanged)
    Q_PROPERTY(QString openaiVoice READ openaiVoice WRITE setOpenaiVoice NOTIFY openaiVoiceChanged)
    Q_PROPERTY(QString elevenlabsApiKey READ elevenlabsApiKey WRITE setElevenlabsApiKey NOTIFY elevenlabsApiKeyChanged)
    Q_PROPERTY(QString elevenlabsVoiceId READ elevenlabsVoiceId WRITE setElevenlabsVoiceId NOTIFY elevenlabsVoiceIdChanged)

public:
    explicit AssistantSettings(QObject* parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);

    bool voiceEnabled() const;                    // spoken output on/off (mute)
    void setVoiceEnabled(bool on);

    QString assistantName() const;                // the assistant's own name (default "Coach")
    void setAssistantName(const QString& name);

    QString voiceName() const;                    // chosen TTS voice ("" = engine default)
    void setVoiceName(const QString& name);

    QString userName() const;                     // what the assistant calls the user in greetings
    void setUserName(const QString& name);

    QString bellSound() const;                    // "ding" | "tick" | ... | "off"
    void setBellSound(const QString& sound);

    QString ttsProvider() const;                  // "native" | "openai" | "elevenlabs"
    void setTtsProvider(const QString& p);

    QString openaiVoice() const;                  // alloy/echo/fable/onyx/nova/shimmer (default "nova")
    void setOpenaiVoice(const QString& v);

    QString elevenlabsApiKey() const;             // ElevenLabs key (the app has no such key)
    void setElevenlabsApiKey(const QString& k);

    QString elevenlabsVoiceId() const;            // ElevenLabs voice id (default a stock voice)
    void setElevenlabsVoiceId(const QString& id);

signals:
    void enabledChanged();
    void voiceEnabledChanged();
    void assistantNameChanged();
    void voiceNameChanged();
    void userNameChanged();
    void bellSoundChanged();
    void ttsProviderChanged();
    void openaiVoiceChanged();
    void elevenlabsApiKeyChanged();
    void elevenlabsVoiceIdChanged();

private:
    mutable QSettings m_settings;  // org/app default = DecentEspresso/DE1Qt (set in main)
};
