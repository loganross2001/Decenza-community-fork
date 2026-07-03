#include "assistantsettings.h"

AssistantSettings::AssistantSettings(QObject* parent)
    : QObject(parent) {}

bool AssistantSettings::enabled() const {
    // Default ON — but the runtime toggle is the kill-switch for a kitchen appliance:
    // a bad assistant state must never be able to block making coffee.
    return m_settings.value(QStringLiteral("barista/enabled"), true).toBool();
}

void AssistantSettings::setEnabled(bool on) {
    if (enabled() == on)
        return;
    m_settings.setValue(QStringLiteral("barista/enabled"), on);
    emit enabledChanged();
}

bool AssistantSettings::voiceEnabled() const {
    return m_settings.value(QStringLiteral("barista/voiceEnabled"), true).toBool();
}

void AssistantSettings::setVoiceEnabled(bool on) {
    if (voiceEnabled() == on)
        return;
    m_settings.setValue(QStringLiteral("barista/voiceEnabled"), on);
    emit voiceEnabledChanged();
}

QString AssistantSettings::assistantName() const {
    return m_settings.value(QStringLiteral("barista/assistantName"), QStringLiteral("Coach")).toString();
}

void AssistantSettings::setAssistantName(const QString& name) {
    const QString trimmed = name.trimmed();
    if (assistantName() == trimmed)
        return;
    m_settings.setValue(QStringLiteral("barista/assistantName"), trimmed);
    emit assistantNameChanged();
}

QString AssistantSettings::voiceName() const {
    return m_settings.value(QStringLiteral("barista/voiceName"), QString()).toString();
}

void AssistantSettings::setVoiceName(const QString& name) {
    if (voiceName() == name)
        return;
    m_settings.setValue(QStringLiteral("barista/voiceName"), name);
    emit voiceNameChanged();
}

QString AssistantSettings::userName() const {
    return m_settings.value(QStringLiteral("barista/userName"), QString()).toString();
}

void AssistantSettings::setUserName(const QString& name) {
    const QString trimmed = name.trimmed();
    if (userName() == trimmed)
        return;
    m_settings.setValue(QStringLiteral("barista/userName"), trimmed);
    emit userNameChanged();
}

QString AssistantSettings::bellSound() const {
    return m_settings.value(QStringLiteral("barista/bellSound"), QStringLiteral("ding")).toString();
}

void AssistantSettings::setBellSound(const QString& sound) {
    if (bellSound() == sound)
        return;
    m_settings.setValue(QStringLiteral("barista/bellSound"), sound);
    emit bellSoundChanged();
}
