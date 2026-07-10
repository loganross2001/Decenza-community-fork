#pragma once

#include <QObject>
#include <QString>

class MainController;
class MachineState;
class AssistantSettings;

// [barista-fork] Lifecycle driver for the USER-INITIATED conversational assistant. The barista is a quiet
// persistent presence — it never speaks first. It only tracks WHETHER a conversation is live (`state`) and
// carries the CONTEXT the overlay's persona reads: recency of the last exchange (greeting cadence) and the
// last-pulled shot (so "that was sour" lands on a barista who already knows a shot finished). Machine
// events (Espresso selected, shot saved) are CONTEXT updates, never conversation triggers. The overlay runs
// the actual multi-turn Claude conversation; what the assistant says is AI-generated and adaptive.
class AssistantOrchestrator : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ stateString NOTIFY stateChanged)   // "present" | "conversing"
    Q_PROPERTY(qlonglong lastShotId READ lastShotId NOTIFY lastShotChanged)
    // The last-pulled shot the barista knows about but the user hasn't talked about yet — drives the P3
    // non-verbal "undiscussed shot" cue (a pulse on the tab), which REPLACES the old spoken close-out.
    Q_PROPERTY(qlonglong lastShotAtMs READ lastShotAtMs NOTIFY lastShotChanged)
    Q_PROPERTY(bool shotDiscussed READ shotDiscussed NOTIFY shotDiscussedChanged)

public:
    // Present = on screen (collapsed tab/avatar), no active session — never truly dormant while enabled.
    // Conversing = the user opened the mic / typed; a Claude session is live. Entered ONLY by engage().
    enum class State { Present, Conversing };
    Q_ENUM(State)

    AssistantOrchestrator(MainController* mainController, MachineState* machineState,
                          AssistantSettings* settings, QObject* parent = nullptr);

    QString stateString() const;
    qlonglong lastShotId() const { return m_lastShotId; }
    qlonglong lastShotAtMs() const { return m_lastShotAtMs; }
    bool shotDiscussed() const { return m_shotDiscussed; }

    // The user tapped the barista / started talking → open a conversation (the ONLY way into Conversing).
    Q_INVOKABLE void engage();
    // The user closed the panel → back to the quiet present state (nothing ever fully disappears).
    Q_INVOKABLE void dismiss();

    // [barista-fork] The end_conversation tool ran (the barista is dismissing itself on a spoken goodbye) →
    // emit dismissRequested() on the MAIN thread so the overlay can end the session AFTER the sign-off finishes
    // speaking. It does NOT change state itself (that's the overlay's job, timed to the TTS) — it merely signals
    // intent, so a background tool-loop thread never tears down the mic/TTS/UI directly. Distinct from dismiss(),
    // which is the immediate "user hit ×" path.
    Q_INVOKABLE void requestDismiss();

    // Espresso selected → pure CONTEXT update (current bean/profile). The barista does NOT speak here.
    Q_INVOKABLE void noteEspressoSelected();

    // The user talked about the pulled shot → clear the undiscussed-shot cue (both provider paths call this).
    Q_INVOKABLE void markShotDiscussed();

    // Recency of the RELATIONSHIP, computed at engage-time from the persisted lastExchangeAt (pure timestamp
    // compare — no timers-as-guards). "ongoing" | "earlierToday" | "firstOfDay" | "firstEver". The overlay's
    // persona reads this to decide whether a greeting is even warranted (folded into the first reply, if so).
    Q_INVOKABLE QString recencyBucket() const;
    Q_INVOKABLE qlonglong minutesSinceLastExchange() const;

    // One completed user↔barista exchange landed → stamp recency (single write path for lastExchangeAt).
    Q_INVOKABLE void markExchangeCompleted();

signals:
    void stateChanged();
    void lastShotChanged();
    void shotDiscussedChanged();
    // [barista-fork] The barista asked to end the conversation (end_conversation tool). The overlay listens and
    // ends the session AFTER the sign-off is spoken (never cuts it off). See requestDismiss().
    void dismissRequested();

private slots:
    void onShotSaved(qlonglong shotId);   // pure bookkeeping: record the shot, mark it undiscussed

private:
    void setState(State s);

    MainController* m_mainController = nullptr;
    MachineState* m_machineState = nullptr;
    AssistantSettings* m_settings = nullptr;
    State m_state = State::Present;
    qlonglong m_lastShotId = -1;
    qint64 m_lastShotAtMs = 0;       // epoch ms of the last saved shot (0 = none this session)
    bool m_shotDiscussed = true;     // true until a NEW shot arrives (nothing undiscussed at startup)
};
