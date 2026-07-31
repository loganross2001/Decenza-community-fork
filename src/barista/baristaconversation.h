#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class AssistantVoice;
class VoiceInput;
class SpeakerGate;

// [barista-fork] Two-way-comms redesign (Phase 1). THE single authoritative owner of the barista's live
// voice conversation lifecycle — replacing the ~16 QML latches + AssistantOrchestrator's Present/Conversing
// + VoiceInput's pause/echo bookkeeping with one explicit state machine. Full design + transition table in
// BARISTA_TwoWay_Comms_Redesign.md §13 (authoritative). Runs on the main thread; QML is a thin view.
//
// The mic obeys exactly one rule (the two-input arbiter):
//     micLive = (state == Listening) && speakerGate.quiet
// and micLive is the ONLY thing that ever starts/stops the recogniser.
//
// This class is built into a runtime-flagged parallel path (`useNewConversation`); until the flag is on it
// is inert (constructed, wired to signals, but the old QML path drives the session). That lets Phase 1 be
// flipped on-device and rolled back with a toggle rather than a redeploy.
class BaristaConversation : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Priming, Listening, Thinking, Speaking, Closing, NeedsTap };
    Q_ENUM(State)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString stateName READ stateName NOTIFY stateChanged)   // for QML view + diagnostics
    Q_PROPERTY(bool micLive READ micLive NOTIFY micLiveChanged)        // UI listening indicator + silence timer key off THIS, not state
    Q_PROPERTY(QString partialText READ partialText NOTIFY partialTextChanged)
    Q_PROPERTY(QString displayText READ displayText NOTIFY displayTextChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)           // convenience: state != Idle

    // Actuators are borrowed (owned by BaristaModule, which parents this). `coaching` may be null.
    BaristaConversation(AssistantVoice* voice, AssistantVoice* coaching, VoiceInput* voiceInput,
                        QObject* parent = nullptr);

    State state() const { return m_state; }
    QString stateName() const;
    bool micLive() const { return m_micLive; }
    bool active() const { return m_state != State::Idle; }
    QString partialText() const { return m_partial; }
    QString displayText() const { return m_display; }
    QString message() const { return m_message; }

    // The only two things QML calls. `tap()` is the single "engage / continue / tap-to-talk / skip" gesture,
    // interpreted by the current state; `dismiss()` is the × / always-close.
    Q_INVOKABLE void tap();
    Q_INVOKABLE void dismiss();

signals:
    void stateChanged();
    void micLiveChanged();
    void partialTextChanged();
    void displayTextChanged();
    void messageChanged();
    void activeChanged();
    // Emitted so BaristaModule can drive the turn actuators (AIConversation) without this class depending on
    // the AI layer directly — the module wires these to sendTurn / assemble-context, mirroring the existing
    // seam pattern. Phase-2 turn dispatch fills these in.
    void turnRequested(const QString& utterance);
    void contextRequested();
    void closingConfirmed();   // teardown complete → module clears the session / collapses the dock

public slots:
    // Actuator inputs (wired by BaristaModule). These are the events of the transition table.
    void onFinalText(const QString& text);        // VoiceInput.finalText  (C++→C++, never via QML)
    void onPartial(const QString& text);          // VoiceInput.partialChanged
    void onSttError(const QString& message);      // VoiceInput.error
    void onContextReady();                        // context/system prompt assembled
    void onModelSpeakable(const QString& text);   // filler / lead-in to speak now
    void onModelFinal(const QString& text, bool endConversation);  // final answer + forced close bit
    void onModelError(const QString& message);    // turn failed
    void onCloseRequested();                      // model called end_conversation → arm closing (deterministic teardown)

private:
    void setState(State s);
    void updateMicLive();                         // the arbiter — the ONLY caller of VoiceInput.setActive
    void onVoiceSpeakingChanged();                // AssistantVoice.speakingChanged → Speaking transitions
    void onGateQuietChanged();                    // SpeakerGate.quiet → re-evaluate the arbiter
    void updateThinkingTone();                    // play the thinking earcon while Thinking / the synth gap
    void setDisplay(const QString& t);
    void setPartial(const QString& t);
    void setMessage(const QString& t);
    void diag(const QString& event, const QString& detail = QString()) const;  // BaristaDiagnostics state records

    // Actuators (borrowed).
    AssistantVoice* m_voice = nullptr;
    AssistantVoice* m_coaching = nullptr;
    VoiceInput* m_voiceInput = nullptr;
    SpeakerGate* m_gate = nullptr;                // owned (child of this)

    // Authoritative state + the exactly-four extended fields (design §13).
    State m_state = State::Idle;
    bool m_closingArmed = false;
    bool m_turnInFlight = false;
    QString m_pendingAnswer;                      // answer that arrived while a filler was still playing ("" = none)
    int m_softErrors = 0;                         // NO_MATCH / SPEECH_TIMEOUT run
    int m_hardErrors = 0;                         // ERROR_CLIENT / BUSY run

    // View mirrors.
    bool m_micLive = false;
    QString m_partial;
    QString m_display;
    QString m_message;

    // Timers (single-shot unless noted) — the only ones the whole subsystem needs.
    QTimer m_primingTimeout;   // Priming → Listening safety (3s)
    QTimer m_turnTimeout;      // hung-turn guard, reset on model activity (20s)
    QTimer m_silence;          // Listening (micLive) → NeedsTap (8s)
    QTimer m_closingWatchdog;  // Closing → Idle if sign-off TTS never reports done (2.5s)
};
