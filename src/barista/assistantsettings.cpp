#include "assistantsettings.h"

#include <QDateTime>
#include <QRegularExpression>

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

QString AssistantSettings::ttsProvider() const {
    return m_settings.value(QStringLiteral("barista/ttsProvider"), QStringLiteral("native")).toString();
}

void AssistantSettings::setTtsProvider(const QString& p) {
    if (ttsProvider() == p)
        return;
    m_settings.setValue(QStringLiteral("barista/ttsProvider"), p);
    emit ttsProviderChanged();
}

QString AssistantSettings::openaiVoice() const {
    return m_settings.value(QStringLiteral("barista/openaiVoice"), QStringLiteral("nova")).toString();
}

void AssistantSettings::setOpenaiVoice(const QString& v) {
    if (openaiVoice() == v)
        return;
    m_settings.setValue(QStringLiteral("barista/openaiVoice"), v);
    emit openaiVoiceChanged();
}

QString AssistantSettings::openaiApiKey() const {
    return m_settings.value(QStringLiteral("barista/openaiApiKey"), QString()).toString();
}

void AssistantSettings::setOpenaiApiKey(const QString& k) {
    const QString trimmed = k.trimmed();
    if (openaiApiKey() == trimmed)
        return;
    m_settings.setValue(QStringLiteral("barista/openaiApiKey"), trimmed);
    emit openaiApiKeyChanged();
}

QString AssistantSettings::elevenlabsApiKey() const {
    return m_settings.value(QStringLiteral("barista/elevenlabsApiKey"), QString()).toString();
}

void AssistantSettings::setElevenlabsApiKey(const QString& k) {
    if (elevenlabsApiKey() == k)
        return;
    m_settings.setValue(QStringLiteral("barista/elevenlabsApiKey"), k);
    emit elevenlabsApiKeyChanged();
}

QString AssistantSettings::elevenlabsVoiceId() const {
    // Default: "Rachel", a stock ElevenLabs voice, so it works before the user customises.
    return m_settings.value(QStringLiteral("barista/elevenlabsVoiceId"),
                            QStringLiteral("21m00Tcm4TlvDq8ikWAM")).toString();
}

void AssistantSettings::setElevenlabsVoiceId(const QString& id) {
    if (elevenlabsVoiceId() == id)
        return;
    m_settings.setValue(QStringLiteral("barista/elevenlabsVoiceId"), id);
    emit elevenlabsVoiceIdChanged();
}

double AssistantSettings::voiceSpeed() const {
    return m_settings.value(QStringLiteral("barista/voiceSpeed"), 1.0).toDouble();
}

void AssistantSettings::setVoiceSpeed(double s) {
    // Clamp to a sane spoken range (both OpenAI and ElevenLabs accept ~0.7–1.3 comfortably).
    if (s < 0.7) s = 0.7;
    if (s > 1.3) s = 1.3;
    if (qFuzzyCompare(voiceSpeed(), s))
        return;
    m_settings.setValue(QStringLiteral("barista/voiceSpeed"), s);
    emit voiceSpeedChanged();
}

QString AssistantSettings::proactivityLevel() const {
    return m_settings.value(QStringLiteral("barista/proactivityLevel"), QStringLiteral("full")).toString();
}

void AssistantSettings::setProactivityLevel(const QString& level) {
    if (proactivityLevel() == level)
        return;
    m_settings.setValue(QStringLiteral("barista/proactivityLevel"), level);
    emit proactivityLevelChanged();
}

bool AssistantSettings::consumeProactiveNudge(const QString& beanKey, int cooldownHours) {
    QString safe = beanKey;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")), QStringLiteral("_"));
    if (safe.isEmpty())
        safe = QStringLiteral("default");
    const QString key = QStringLiteral("barista/nudge/") + safe;
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime last = QDateTime::fromString(m_settings.value(key).toString(), Qt::ISODate);
    if (last.isValid() && last.secsTo(now) < static_cast<qint64>(cooldownHours) * 3600)
        return false;   // still cooling down — don't re-raise the same nudge on a back-to-back shot
    m_settings.setValue(key, now.toString(Qt::ISODate));
    return true;
}
