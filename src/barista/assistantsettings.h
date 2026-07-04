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
    Q_PROPERTY(QString openaiApiKey READ openaiApiKey WRITE setOpenaiApiKey NOTIFY openaiApiKeyChanged)
    Q_PROPERTY(QString elevenlabsApiKey READ elevenlabsApiKey WRITE setElevenlabsApiKey NOTIFY elevenlabsApiKeyChanged)
    Q_PROPERTY(QString elevenlabsVoiceId READ elevenlabsVoiceId WRITE setElevenlabsVoiceId NOTIFY elevenlabsVoiceIdChanged)
    Q_PROPERTY(double voiceSpeed READ voiceSpeed WRITE setVoiceSpeed NOTIFY voiceSpeedChanged)
    Q_PROPERTY(bool webSearchEnabled READ webSearchEnabled WRITE setWebSearchEnabled NOTIFY webSearchEnabledChanged)
    Q_PROPERTY(bool avatarEnabled READ avatarEnabled WRITE setAvatarEnabled NOTIFY avatarEnabledChanged)
    Q_PROPERTY(QString proactivityLevel READ proactivityLevel WRITE setProactivityLevel NOTIFY proactivityLevelChanged)

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

    QString openaiApiKey() const;                 // optional override; else the app's Settings.ai key is used
    void setOpenaiApiKey(const QString& k);

    QString elevenlabsApiKey() const;             // ElevenLabs key (the app has no such key)
    void setElevenlabsApiKey(const QString& k);

    QString elevenlabsVoiceId() const;            // ElevenLabs voice id (default a stock voice)
    void setElevenlabsVoiceId(const QString& id);

    double voiceSpeed() const;                    // speaking rate multiplier (default 1.0)
    void setVoiceSpeed(double s);

    bool webSearchEnabled() const;                // let the barista search the web (Anthropic; default on)
    void setWebSearchEnabled(bool e);

    bool avatarEnabled() const;                   // show the animated character face (default on)
    void setAvatarEnabled(bool e);

    QString proactivityLevel() const;             // "off" | "greetings" | "full" (default "full")
    void setProactivityLevel(const QString& level);

    // Proactivity cooldown: returns true (and stamps "now") if the last proactive nudge for this bean was
    // more than cooldownHours ago — so it doesn't re-raise the same suggestion on back-to-back shots.
    Q_INVOKABLE bool consumeProactiveNudge(const QString& beanKey, int cooldownHours);

signals:
    void enabledChanged();
    void voiceEnabledChanged();
    void assistantNameChanged();
    void voiceNameChanged();
    void userNameChanged();
    void bellSoundChanged();
    void ttsProviderChanged();
    void openaiVoiceChanged();
    void openaiApiKeyChanged();
    void elevenlabsApiKeyChanged();
    void elevenlabsVoiceIdChanged();
    void voiceSpeedChanged();
    void webSearchEnabledChanged();
    void avatarEnabledChanged();
    void proactivityLevelChanged();

private:
    mutable QSettings m_settings;  // org/app default = DecentEspresso/DE1Qt (set in main)
};
