#include "assistantsettings.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

AssistantSettings::AssistantSettings(QObject* parent)
    : QObject(parent) {
    // Seed once: if there's no saved-voices list yet but the owner already has a non-default custom
    // ElevenLabs id, persist it as a named entry so the new picker doesn't orphan it. Guarded so we only
    // seed a genuinely empty (missing) list, and we persist so it survives adding OTHER voices later.
    if (!m_settings.contains(QStringLiteral("barista/elevenlabsVoices"))) {
        const QString current = elevenlabsVoiceId();
        const QString stockRachel = QStringLiteral("21m00Tcm4TlvDq8ikWAM");
        if (!current.isEmpty() && current != stockRachel)
            addElevenlabsVoice(QStringLiteral("Saved voice"), current);
    }
}

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

bool AssistantSettings::greetAloud() const {
    // Default OFF: on start-up the barista shows its greeting (and chimes/animates) but does NOT speak
    // unprompted — a voice interrupting the moment you step up reads as abrupt. It still speaks its REPLIES
    // once you talk to it (that's voiceEnabled). Turn this on to have it also say the opening line aloud.
    return m_settings.value(QStringLiteral("barista/greetAloud"), false).toBool();
}

void AssistantSettings::setGreetAloud(bool on) {
    if (greetAloud() == on)
        return;
    m_settings.setValue(QStringLiteral("barista/greetAloud"), on);
    emit greetAloudChanged();
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
    return m_settings.value(QStringLiteral("barista/bellSound"), QStringLiteral("poof")).toString();
}

void AssistantSettings::setBellSound(const QString& sound) {
    if (bellSound() == sound)
        return;
    m_settings.setValue(QStringLiteral("barista/bellSound"), sound);
    emit bellSoundChanged();
}

// The user's own bell sound file (an absolute path on the device), used when bellSound == "custom".
QString AssistantSettings::bellCustomPath() const {
    return m_settings.value(QStringLiteral("barista/bellCustomPath"), QString()).toString();
}

void AssistantSettings::setBellCustomPath(const QString& path) {
    if (bellCustomPath() == path)
        return;
    m_settings.setValue(QStringLiteral("barista/bellCustomPath"), path);
    emit bellCustomPathChanged();
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

// [barista-fork] Coaching voice — a parallel provider + per-provider voice selection, persisted under
// barista/coaching*. Shares the ElevenLabs API key + saved-voices list + voiceSpeed with the barista voice.
QString AssistantSettings::coachingTtsProvider() const {
    return m_settings.value(QStringLiteral("barista/coachingTtsProvider"), QStringLiteral("native")).toString();
}

void AssistantSettings::setCoachingTtsProvider(const QString& p) {
    if (coachingTtsProvider() == p)
        return;
    m_settings.setValue(QStringLiteral("barista/coachingTtsProvider"), p);
    emit coachingTtsProviderChanged();
}

QString AssistantSettings::coachingVoiceName() const {
    return m_settings.value(QStringLiteral("barista/coachingVoiceName"), QString()).toString();
}

void AssistantSettings::setCoachingVoiceName(const QString& name) {
    if (coachingVoiceName() == name)
        return;
    m_settings.setValue(QStringLiteral("barista/coachingVoiceName"), name);
    emit coachingVoiceNameChanged();
}

QString AssistantSettings::coachingOpenaiVoice() const {
    // Default "onyx" — a calm, distinct-able voice so the coach doesn't sound identical to the
    // barista's default ("nova") out of the box.
    return m_settings.value(QStringLiteral("barista/coachingOpenaiVoice"), QStringLiteral("onyx")).toString();
}

void AssistantSettings::setCoachingOpenaiVoice(const QString& v) {
    if (coachingOpenaiVoice() == v)
        return;
    m_settings.setValue(QStringLiteral("barista/coachingOpenaiVoice"), v);
    emit coachingOpenaiVoiceChanged();
}

QString AssistantSettings::coachingElevenlabsVoiceId() const {
    // Default: the stock "Rachel" voice (same fallback as the barista), so it works before customising.
    return m_settings.value(QStringLiteral("barista/coachingElevenlabsVoiceId"),
                            QStringLiteral("21m00Tcm4TlvDq8ikWAM")).toString();
}

void AssistantSettings::setCoachingElevenlabsVoiceId(const QString& id) {
    if (coachingElevenlabsVoiceId() == id)
        return;
    m_settings.setValue(QStringLiteral("barista/coachingElevenlabsVoiceId"), id);
    emit coachingElevenlabsVoiceIdChanged();
}

// Saved ElevenLabs voices — a JSON array of {name, id} objects under barista/elevenlabsVoices, mirroring
// the settings_brew steamPitcherPresets pattern (QJsonDocument round-trip → QVariantList of maps for QML).
QVariantList AssistantSettings::elevenlabsVoices() const {
    const QByteArray data = m_settings.value(QStringLiteral("barista/elevenlabsVoices")).toByteArray();
    const QJsonArray arr = QJsonDocument::fromJson(data).array();

    QVariantList result;
    for (const QJsonValue& v : arr)
        result.append(v.toObject().toVariantMap());
    return result;
}

void AssistantSettings::addElevenlabsVoice(const QString& name, const QString& id) {
    const QString trimmedId = id.trimmed();
    if (trimmedId.isEmpty())
        return;   // an entry with no id is meaningless — a blank name is fine (falls back to the id)
    const QString trimmedName = name.trimmed();

    const QByteArray data = m_settings.value(QStringLiteral("barista/elevenlabsVoices")).toByteArray();
    QJsonArray arr = QJsonDocument::fromJson(data).array();

    // Dedupe by id: if this id already exists, UPDATE its name in place; otherwise append a new entry.
    bool found = false;
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr[i].toObject();
        if (obj.value(QStringLiteral("id")).toString() == trimmedId) {
            obj[QStringLiteral("name")] = trimmedName;
            arr[i] = obj;
            found = true;
            break;
        }
    }
    if (!found) {
        QJsonObject obj;
        obj[QStringLiteral("name")] = trimmedName;
        obj[QStringLiteral("id")] = trimmedId;
        arr.append(obj);
    }

    m_settings.setValue(QStringLiteral("barista/elevenlabsVoices"), QJsonDocument(arr).toJson());
    emit elevenlabsVoicesChanged();
}

void AssistantSettings::removeElevenlabsVoice(const QString& id) {
    const QString trimmedId = id.trimmed();
    if (trimmedId.isEmpty())
        return;

    const QByteArray data = m_settings.value(QStringLiteral("barista/elevenlabsVoices")).toByteArray();
    QJsonArray arr = QJsonDocument::fromJson(data).array();

    bool removed = false;
    for (int i = 0; i < arr.size(); ++i) {
        if (arr[i].toObject().value(QStringLiteral("id")).toString() == trimmedId) {
            arr.removeAt(i);
            removed = true;
            break;
        }
    }
    if (removed) {
        m_settings.setValue(QStringLiteral("barista/elevenlabsVoices"), QJsonDocument(arr).toJson());
        // If the removed voice was the active one, fall the selection back to the first
        // remaining saved voice so it isn't left orphaned (active but absent from the list).
        // If none remain, leave the active id as-is (TTS still works from the raw id).
        if (elevenlabsVoiceId() == trimmedId && !arr.isEmpty())
            setElevenlabsVoiceId(arr.first().toObject().value(QStringLiteral("id")).toString());
        emit elevenlabsVoicesChanged();
    }
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

bool AssistantSettings::webSearchEnabled() const {
    return m_settings.value(QStringLiteral("barista/webSearchEnabled"), true).toBool();
}

void AssistantSettings::setWebSearchEnabled(bool e) {
    if (webSearchEnabled() == e)
        return;
    m_settings.setValue(QStringLiteral("barista/webSearchEnabled"), e);
    emit webSearchEnabledChanged();
}

bool AssistantSettings::avatarEnabled() const {
    return m_settings.value(QStringLiteral("barista/avatarEnabled"), true).toBool();
}

void AssistantSettings::setAvatarEnabled(bool e) {
    if (avatarEnabled() == e)
        return;
    m_settings.setValue(QStringLiteral("barista/avatarEnabled"), e);
    emit avatarEnabledChanged();
}

QString AssistantSettings::avatarStyle() const {
    return m_settings.value(QStringLiteral("barista/avatarStyle"), QStringLiteral("face")).toString();
}

void AssistantSettings::setAvatarStyle(const QString& s) {
    if (avatarStyle() == s)
        return;
    m_settings.setValue(QStringLiteral("barista/avatarStyle"), s);
    emit avatarStyleChanged();
}

// [barista-fork] Recency of the barista relationship — the heart of the user-initiated model. One ISO
// timestamp, stamped on every completed exchange; read (never as a timer) at engage-time to decide whether
// a greeting is even warranted. Mirrors the consumeProactiveNudge persistence pattern.
QString AssistantSettings::lastExchangeAt() const {
    return m_settings.value(QStringLiteral("barista/lastExchangeAt")).toString();
}

void AssistantSettings::markExchangeCompleted() {
    m_settings.setValue(QStringLiteral("barista/lastExchangeAt"),
                        QDateTime::currentDateTime().toString(Qt::ISODate));
}

qint64 AssistantSettings::minutesSinceLastExchange() const {
    const QDateTime last = QDateTime::fromString(lastExchangeAt(), Qt::ISODate);
    if (!last.isValid())
        return -1;   // never met
    const qint64 secs = last.secsTo(QDateTime::currentDateTime());
    return secs < 0 ? 0 : secs / 60;   // clock skew → treat as "just now", never negative
}

QString AssistantSettings::lastLightGreetDate() const {
    return m_settings.value(QStringLiteral("barista/lastLightGreetDate")).toString();
}

bool AssistantSettings::consumeLightGreetForToday() {
    const QString today = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
    if (lastLightGreetDate() == today)
        return false;   // already nodded once today — stay quiet
    m_settings.setValue(QStringLiteral("barista/lastLightGreetDate"), today);
    return true;
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
