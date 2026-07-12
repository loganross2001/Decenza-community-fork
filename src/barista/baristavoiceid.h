#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "mfccembedder.h"
#include "voiceprintstore.h"

class VoiceCapture;

// [barista-fork] Voice-ID Increment 1 — the QML-facing coordinator (Barista.voiceId).
//
// Ties together capture (VoiceCapture) → embed (SpeakerEmbedder, MFCC baseline) → store (VoiceprintStore).
// Increment 1 does ENROLLMENT + a concurrent-capture PROBE only — no live matching / auto-switch yet (that is
// Increment 2, which will feed a confident match into the existing set_active_user path — voice-ID is an INPUT
// to Phase-1 identity, not a new identity system). Enrollment keys a voiceprint to the ACTIVE user's name
// (Settings.dye.dyeBarista → userName), supplied via an injected provider so this stays decoupled.
class BaristaVoiceId : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList enrolledNames READ enrolledNames NOTIFY enrolledNamesChanged)
    Q_PROPERTY(QString activeUser READ activeUser NOTIFY activeUserChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // [barista-fork] Increment 2 — the last identification result, consumed once per turn by the overlay to
    // build the per-turn voice hint for the model. heardConfidence = "confident" | "maybe" | "none".
    Q_PROPERTY(QString heardName READ heardName NOTIFY heardChanged)
    Q_PROPERTY(QString heardConfidence READ heardConfidence NOTIFY heardChanged)
public:
    explicit BaristaVoiceId(QObject* parent = nullptr);

    void initialize(const QString& voiceprintsDbPath);
    // Injected: returns the current active-user name (dyeBarista → userName → "").
    void setActiveUserProvider(std::function<QString()> provider);
    // [barista-fork] Increment 2: injected seam to SET the active user on a confident match (→ dyeBarista,
    // the same path the set_active_user tool uses — voice-ID is an INPUT to Phase-1 identity).
    void setActiveUserSeam(std::function<void(const QString&)> seam);
    // [barista-fork] Owner-tunable match thresholds (from AssistantSettings; wired in baristamodule). Clamped.
    void setThresholds(double confident, double margin, double maybe);

    QString heardName() const { return m_heardName; }
    QString heardConfidence() const { return m_heardConfidence; }
    // Read + clear the last result (one-shot per turn).
    Q_INVOKABLE void consumeHeard();

    QStringList enrolledNames() const { return m_enrolledNames; }
    QString activeUser() const;
    bool recording() const { return m_recording; }
    QString status() const { return m_status; }

    // Record ~10s and store the active user's voiceprint (opt-in; refuses with a clear status if no active
    // user name is set yet).
    Q_INVOKABLE void enrollActiveUser();
    Q_INVOKABLE void cancelEnroll();
    Q_INVOKABLE void deleteVoiceprint(const QString& name);
    Q_INVOKABLE void refreshEnrolled();

    // Concurrent-capture probe: a short parallel capture during a live conversation turn to validate (via the
    // device log) whether QAudioSource works while Android SpeechRecognizer holds the mic. Discards the audio.
    Q_INVOKABLE void runProbe();

    // [barista-fork] Increment 2 — live identification: capture the ACTUAL utterance (start when the mic opens,
    // stop at the final STT result) then match it against the enrolled voiceprints. RETIRED: capturing while the
    // Android SpeechRecognizer is listening makes the OS silence the recognizer (it missed the first 3-6s of
    // speech). Kept only so no external caller breaks; the overlay no longer calls these. Voice-ID now happens
    // OFF the STT path — see the on-demand validators below and [[decenza-barista-voice-matching]].
    Q_INVOKABLE void startUtteranceCapture();
    Q_INVOKABLE void stopUtteranceCaptureAndIdentify();

    // [barista-fork] V1 validator — a ~2s on-demand capture from the settings panel (NO SpeechRecognizer active,
    // so zero mic contention) that logs the match score/margin/decision WITHOUT switching the active user. Proves
    // whether a short clip still matches well enough for the planned engage-time "voice hail".
    Q_INVOKABLE void testShortIdentify();

    // [barista-fork] V2 validator — a ~2.5s capture fired at barista ENGAGE, BEFORE the STT mic opens, to prove
    // that a capture-then-recognizer sequence leaves the mic healthy (the anti-probe test: no concurrency).
    // Records `engage_capture_done`, then ALWAYS emits engageCaptureTestDone() so the overlay opens the mic
    // (even on failure — never wedge the session).
    Q_INVOKABLE void startEngageCaptureTest();

signals:
    void enrolledNamesChanged();
    void activeUserChanged();
    void recordingChanged();
    void statusChanged();
    void heardChanged();
    void engageCaptureTestDone();

private:
    void onCaptured(const QByteArray& pcm, int sampleRate, const QString& label);
    void setStatus(const QString& s);
    void setRecording(bool r);

    // Increment 2: embed + match + log. testOnly=true logs the match but does NOT expose the hint or switch
    // the active user (used by the V1 short-clip validator).
    void identify(const QByteArray& pcm, int sampleRate, bool testOnly = false);

    VoiceCapture* m_capture = nullptr;
    VoiceprintStore* m_store = nullptr;
    MfccEmbedder m_embedder;
    std::function<QString()> m_activeUserProvider;
    std::function<void(const QString&)> m_setActiveUserSeam;   // Increment 2: confident-match → set active user

    QStringList m_enrolledNames;
    QVector<VoiceprintStore::Voiceprint> m_prints;   // cached vectors, for synchronous matching on capture-stop
    bool m_recording = false;
    bool m_probeInFlight = false;
    bool m_identifyInFlight = false;
    QString m_status;
    QString m_enrollingName;   // name captured at enroll-start (active user could change mid-record)
    QString m_heardName;
    QString m_heardConfidence = QStringLiteral("none");

    // [barista-fork] Owner-tunable match thresholds (defaults = the baseline constants). Set from AssistantSettings.
    float m_confidentScore = 0.72f;
    float m_confidentMargin = 0.06f;
    float m_maybeScore = 0.55f;
};
