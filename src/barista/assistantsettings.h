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

public:
    explicit AssistantSettings(QObject* parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);

    // Spoken output on/off (mute). Default on.
    bool voiceEnabled() const;
    void setVoiceEnabled(bool on);

    // The assistant's own name (the user names it). Default "Coach".
    QString assistantName() const;
    void setAssistantName(const QString& name);

    // Chosen TTS voice name (empty = engine default). Matched against QTextToSpeech voices.
    QString voiceName() const;
    void setVoiceName(const QString& name);

signals:
    void enabledChanged();
    void voiceEnabledChanged();
    void assistantNameChanged();
    void voiceNameChanged();

private:
    mutable QSettings m_settings;  // org/app default = DecentEspresso/DE1Qt (set in main)
};
