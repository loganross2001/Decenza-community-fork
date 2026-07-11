#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariantList>

// [barista-fork] The barista module's OWN settings, stored under its own "barista/" QSettings
// group. Deliberately NOT a Settings-facade domain sub-object, so the upstream Settings classes
// are never edited (the whole point of the modular fork). QML reaches it as `Barista.settings.<prop>`.
class AssistantSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool voiceEnabled READ voiceEnabled WRITE setVoiceEnabled NOTIFY voiceEnabledChanged)
    Q_PROPERTY(bool greetAloud READ greetAloud WRITE setGreetAloud NOTIFY greetAloudChanged)
    Q_PROPERTY(QString assistantName READ assistantName WRITE setAssistantName NOTIFY assistantNameChanged)
    Q_PROPERTY(QString voiceName READ voiceName WRITE setVoiceName NOTIFY voiceNameChanged)
    Q_PROPERTY(QString userName READ userName WRITE setUserName NOTIFY userNameChanged)
    Q_PROPERTY(QString bellSound READ bellSound WRITE setBellSound NOTIFY bellSoundChanged)
    Q_PROPERTY(QString bellCustomPath READ bellCustomPath WRITE setBellCustomPath NOTIFY bellCustomPathChanged)
    Q_PROPERTY(QString ttsProvider READ ttsProvider WRITE setTtsProvider NOTIFY ttsProviderChanged)
    Q_PROPERTY(QString openaiVoice READ openaiVoice WRITE setOpenaiVoice NOTIFY openaiVoiceChanged)
    Q_PROPERTY(QString openaiApiKey READ openaiApiKey WRITE setOpenaiApiKey NOTIFY openaiApiKeyChanged)
    Q_PROPERTY(QString elevenlabsApiKey READ elevenlabsApiKey WRITE setElevenlabsApiKey NOTIFY elevenlabsApiKeyChanged)
    Q_PROPERTY(QString elevenlabsVoiceId READ elevenlabsVoiceId WRITE setElevenlabsVoiceId NOTIFY elevenlabsVoiceIdChanged)
    // [barista-fork] Coaching voice — a SEPARATE voice for the live steam + espresso coaches, chosen
    // independently of the barista's conversational voice. Mirrors the barista provider + per-provider
    // voice selection above, but deliberately REUSES the shared ElevenLabs API key + saved-voices list.
    // Speed AND volume are now split PER ROLE (baristaVoice*/coachingVoice* below) — only the ElevenLabs
    // key + saved-voices list stay shared; provider, the three voice ids, speed, and volume all differ.
    Q_PROPERTY(QString coachingTtsProvider READ coachingTtsProvider WRITE setCoachingTtsProvider NOTIFY coachingTtsProviderChanged)
    Q_PROPERTY(QString coachingVoiceName READ coachingVoiceName WRITE setCoachingVoiceName NOTIFY coachingVoiceNameChanged)
    Q_PROPERTY(QString coachingOpenaiVoice READ coachingOpenaiVoice WRITE setCoachingOpenaiVoice NOTIFY coachingOpenaiVoiceChanged)
    Q_PROPERTY(QString coachingElevenlabsVoiceId READ coachingElevenlabsVoiceId WRITE setCoachingElevenlabsVoiceId NOTIFY coachingElevenlabsVoiceIdChanged)
    // Saved ElevenLabs voices: a named list so the owner picks a voice instead of re-typing its cryptic id.
    Q_PROPERTY(QVariantList elevenlabsVoices READ elevenlabsVoices NOTIFY elevenlabsVoicesChanged)
    // [barista-fork] Per-role speaking rate + playback volume. Previously a single shared voiceSpeed with no
    // volume control; now the barista's conversational voice and the coaching voice each carry their OWN speed
    // and volume, set independently in the barista settings. Speed is a rate multiplier (1.0 = normal); volume
    // is a linear 0..1 gain applied at playback. The old shared barista/voiceSpeed migrates into BOTH speeds.
    Q_PROPERTY(double baristaVoiceSpeed READ baristaVoiceSpeed WRITE setBaristaVoiceSpeed NOTIFY baristaVoiceSpeedChanged)
    Q_PROPERTY(double coachingVoiceSpeed READ coachingVoiceSpeed WRITE setCoachingVoiceSpeed NOTIFY coachingVoiceSpeedChanged)
    Q_PROPERTY(double baristaVoiceVolume READ baristaVoiceVolume WRITE setBaristaVoiceVolume NOTIFY baristaVoiceVolumeChanged)
    Q_PROPERTY(double coachingVoiceVolume READ coachingVoiceVolume WRITE setCoachingVoiceVolume NOTIFY coachingVoiceVolumeChanged)
    Q_PROPERTY(bool webSearchEnabled READ webSearchEnabled WRITE setWebSearchEnabled NOTIFY webSearchEnabledChanged)
    Q_PROPERTY(bool avatarEnabled READ avatarEnabled WRITE setAvatarEnabled NOTIFY avatarEnabledChanged)
    Q_PROPERTY(QString avatarStyle READ avatarStyle WRITE setAvatarStyle NOTIFY avatarStyleChanged)
    // [barista-fork] Size of the avatar on the collapsed EDGE TAB (the pull-tab on the screen edge). Owner-
    // chosen; the tab hugs the avatar so this drives both. "small" | "medium" | "large" (default "medium").
    Q_PROPERTY(QString avatarTabSize READ avatarTabSize WRITE setAvatarTabSize NOTIFY avatarTabSizeChanged)
    Q_PROPERTY(QString proactivityLevel READ proactivityLevel WRITE setProactivityLevel NOTIFY proactivityLevelChanged)

public:
    explicit AssistantSettings(QObject* parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);

    bool voiceEnabled() const;                    // spoken output on/off (mute)
    void setVoiceEnabled(bool on);

    bool greetAloud() const;                      // also SPEAK the unprompted greeting opener (default off:
    void setGreetAloud(bool on);                  // the barista is present-but-quiet at the start, replies aloud)

    QString assistantName() const;                // the assistant's own name (default "Coach")
    void setAssistantName(const QString& name);

    QString voiceName() const;                    // chosen TTS voice ("" = engine default)
    void setVoiceName(const QString& name);

    QString userName() const;                     // what the assistant calls the user in greetings
    void setUserName(const QString& name);

    QString bellSound() const;                    // "poof" | "ding" | ... | "off" | "custom"
    void setBellCustomPath(const QString& path);
    QString bellCustomPath() const;               // user-picked sound file (absolute path), for "custom"
    void setBellSound(const QString& sound);

    QString ttsProvider() const;                  // "native" | "openai" | "elevenlabs"
    void setTtsProvider(const QString& p);

    QString openaiVoice() const;                  // alloy/echo/fable/onyx/nova/shimmer (default "nova")
    void setOpenaiVoice(const QString& v);

    QString openaiApiKey() const;                 // optional override; else the app's Settings.ai key is used
    void setOpenaiApiKey(const QString& k);

    QString elevenlabsApiKey() const;             // ElevenLabs key (the app has no such key)
    void setElevenlabsApiKey(const QString& k);

    QString elevenlabsVoiceId() const;            // ElevenLabs voice id (default a stock voice) — the ACTIVE selection
    void setElevenlabsVoiceId(const QString& id);

    // [barista-fork] Coaching-voice selection (parallel to the barista voice above). Shares the ElevenLabs
    // API key + saved-voices list; the provider, the three per-provider voice ids, speed, and volume are per-role.
    QString coachingTtsProvider() const;          // "native" | "openai" | "elevenlabs" (default "native")
    void setCoachingTtsProvider(const QString& p);
    QString coachingVoiceName() const;            // native TTS voice ("" = engine default)
    void setCoachingVoiceName(const QString& name);
    QString coachingOpenaiVoice() const;          // OpenAI voice (default "onyx" — distinct from barista's "nova")
    void setCoachingOpenaiVoice(const QString& v);
    QString coachingElevenlabsVoiceId() const;    // ElevenLabs voice id (default the stock "Rachel")
    void setCoachingElevenlabsVoiceId(const QString& id);

    // Saved ElevenLabs voices — a list of {name, id} maps, persisted as JSON under barista/elevenlabsVoices.
    // The active voice remains elevenlabsVoiceId; selecting a saved voice just calls setElevenlabsVoiceId(id).
    Q_INVOKABLE QVariantList elevenlabsVoices() const;              // parsed [{name, id}, ...]
    Q_INVOKABLE void addElevenlabsVoice(const QString& name, const QString& id);  // upsert by id (dedupe), then persist
    // Edit a saved voice in place: rename it and/or change its id. If the id changed and it was the active
    // barista and/or coaching selection, the active id follows the edit (both are re-pointed — the list is
    // shared between the two sections). Then persist + emit elevenlabsVoicesChanged.
    Q_INVOKABLE void updateElevenlabsVoice(const QString& oldId, const QString& name, const QString& newId);
    Q_INVOKABLE void removeElevenlabsVoice(const QString& id);     // remove by id, then persist

    // [barista-fork] Per-role speed + volume. Speed is a rate multiplier (default 1.0), clamped to a sane
    // spoken range; volume is a linear 0..1 gain (default 1.0). The barista* getters also carry the
    // migration of the legacy shared barista/voiceSpeed value (see the ctor).
    double baristaVoiceSpeed() const;             // barista voice speaking rate (default 1.0)
    void setBaristaVoiceSpeed(double s);
    double coachingVoiceSpeed() const;            // coaching voice speaking rate (default 1.0)
    void setCoachingVoiceSpeed(double s);
    double baristaVoiceVolume() const;            // barista voice playback volume 0..1 (default 1.0)
    void setBaristaVoiceVolume(double v);
    double coachingVoiceVolume() const;           // coaching voice playback volume 0..1 (default 1.0)
    void setCoachingVoiceVolume(double v);

    bool webSearchEnabled() const;                // let the barista search the web (Anthropic; default on)
    void setWebSearchEnabled(bool e);

    bool avatarEnabled() const;                   // show the animated character face (default on)
    void setAvatarEnabled(bool e);

    QString avatarStyle() const;                  // "face" | "cup" | "orb" | "bean" (default "face")
    void setAvatarStyle(const QString& s);

    QString avatarTabSize() const;                // edge-tab avatar size: "small" | "medium" | "large" (default "medium")
    void setAvatarTabSize(const QString& s);

    QString proactivityLevel() const;             // "off" | "greetings" | "full" (default "full")
    void setProactivityLevel(const QString& level);

    // Proactivity cooldown: returns true (and stamps "now") if the last proactive nudge for this bean was
    // more than cooldownHours ago — so it doesn't re-raise the same suggestion on back-to-back shots.
    Q_INVOKABLE bool consumeProactiveNudge(const QString& beanKey, int cooldownHours);

    // [barista-fork] Recency of the barista RELATIONSHIP (single source of truth for greeting cadence).
    // lastExchangeAt = wall-clock of the last COMPLETED user↔barista exchange (NOT taps/machine events),
    // persisted as an ISO-8601 string like consumeProactiveNudge. Global (the greeting is the relationship,
    // not the bean). Computed at engage-time as `now - lastExchangeAt` — no timers-as-guards.
    Q_INVOKABLE QString lastExchangeAt() const;              // ISO-8601, or "" if never
    Q_INVOKABLE void markExchangeCompleted();                // stamp "now" — one completed exchange
    Q_INVOKABLE qint64 minutesSinceLastExchange() const;     // -1 if never, else whole minutes elapsed

    // The light "back for round two" nod is at most ONCE per calendar day — persist the last date it fired.
    Q_INVOKABLE QString lastLightGreetDate() const;          // "yyyy-MM-dd", or "" if never
    Q_INVOKABLE bool consumeLightGreetForToday();            // true (and stamps today) if not yet used today

signals:
    void enabledChanged();
    void voiceEnabledChanged();
    void greetAloudChanged();
    void assistantNameChanged();
    void voiceNameChanged();
    void userNameChanged();
    void bellSoundChanged();
    void bellCustomPathChanged();
    void ttsProviderChanged();
    void openaiVoiceChanged();
    void openaiApiKeyChanged();
    void elevenlabsApiKeyChanged();
    void elevenlabsVoiceIdChanged();
    void coachingTtsProviderChanged();
    void coachingVoiceNameChanged();
    void coachingOpenaiVoiceChanged();
    void coachingElevenlabsVoiceIdChanged();
    void elevenlabsVoicesChanged();
    void baristaVoiceSpeedChanged();
    void coachingVoiceSpeedChanged();
    void baristaVoiceVolumeChanged();
    void coachingVoiceVolumeChanged();
    void webSearchEnabledChanged();
    void avatarEnabledChanged();
    void avatarStyleChanged();
    void avatarTabSizeChanged();
    void proactivityLevelChanged();

private:
    mutable QSettings m_settings;  // org/app default = DecentEspresso/DE1Qt (set in main)
};
