#pragma once

#include <QObject>
#include <QTimer>

class AssistantVoice;

// [barista-fork] Two-way-comms redesign (Phase 1). The acoustic half of the mic arbiter.
//
// The mic must only hear when the SPEAKER is actually quiet — and "quiet" is a fact about the audio
// OUTPUT path, not about the conversation state, because the coaching voices (steam/espresso) can play
// while the user is mid-conversation (a shot pulling). So this gate watches ALL local TTS sources
// (the conversational voice AND the coaching voice) and publishes one boolean, `quiet`:
//   - it goes FALSE the instant any source starts speaking, and
//   - it returns TRUE only `drainMs` after the LAST source's speech ends — the acoustic tail window
//     (Bluetooth A2DP sink latency ~100-300ms + room echo outlast QTextToSpeech's "done" event, so a
//     bare `speaking==false` would let the recogniser transcribe the barista's own tail: the echo loop).
//
// The controller's rule is then just:  micLive = (state == Listening) && speakerGate.quiet.
// Per-route drain (BT ~400ms vs built-in ~150ms) and the self-echo text backstop are layered on later
// (Phase 3); this class owns the timing primitive and nothing else.
class SpeakerGate : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool quiet READ quiet NOTIFY quietChanged)

public:
    // `coaching` may be null (barista built without the coaching voice); the gate then watches only `conv`.
    SpeakerGate(AssistantVoice* conv, AssistantVoice* coaching, QObject* parent = nullptr);

    bool quiet() const { return m_quiet; }

    // Per-route acoustic-tail window. Default is the conservative BT value; the controller narrows it for
    // wired/built-in output. Changing it re-evaluates immediately.
    void setDrainMs(int ms);
    int drainMs() const { return m_drainMs; }

signals:
    void quietChanged();

private:
    void onSpeakingChanged();   // either voice's speakingChanged()
    void reevaluate();
    bool anySpeaking() const;
    void setQuiet(bool q);
    int routeDrainMs() const;   // per-route acoustic window: Bluetooth ~400ms, wired/USB/built-in ~150ms

    AssistantVoice* m_conv = nullptr;
    AssistantVoice* m_coaching = nullptr;
    QTimer m_drain;             // single-shot: fires `drainMs` after the last source goes silent
    bool m_quiet = true;
    int m_drainMs = 400;        // conservative default (BT A2DP); Phase-0 measured built-in ≈150ms
};
