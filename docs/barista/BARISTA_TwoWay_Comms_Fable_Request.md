# Fable Design-Review Request — Barista Two-Way Voice Conversation Redesign

*An independent design pass to cross-check and strengthen a proposed clean-slate redesign before we implement it. This is core interaction code (the barista's live voice conversation on a Decent Espresso DE1 tablet), so we want a second, independent design generated from the same evidence — not just a rubber-stamp of ours.*

## What to read first

1. **`BARISTA_TwoWay_Comms_Redesign.md`** (same folder) — our proposed design: one authoritative C++ state machine (`Idle / Priming / Listening / Thinking / Speaking / Closing / NeedsTap`), a single mic/TTS arbiter (mic hot **iff** `Listening`), deterministic `Closing`, and `NeedsTap` instead of silent re-listen. Read it fully; it also contains the current-architecture diagnosis and the two bugs.

## The system (constraints Fable must design within)

- **Stack:** Qt 6.11 / C++17 + QML, Android (on-device `SpeechRecognizer` via a Java `DecenzaSpeech` JNI wrapper) + `QTextToSpeech`. Cross-thread: JNI callback → Qt queued invoke → main thread.
- **Today's fragility (what we're replacing):** conversation state is smeared across QML `AssistantOverlay` (~16 boolean latches + 8 timers), C++ `AssistantOrchestrator` (`Present/Conversing`), C++ `VoiceInput` (`listening/paused/errorStreak/`400 ms echo window), and Java `DecenzaSpeech`. The mic is driven from three places (main turn, coaching voice, filler). Behaviour emerges from interacting flags, so every fix spawns an edge case.
- **Two confirmed bugs:** (A) says "that'll be all" but doesn't close — logged: `endAfter=false` for a whole farewell session because neither the local regex nor the model's `end_conversation` tool fired; (B) "listening loop" — first utterance silently dropped (STT `ERROR_CLIENT`/no-match, or the echo window), no "tap to talk" fallback, so the user repeats themselves.
- **Hard real-world facts the design must respect:** Android `SpeechRecognizer` is flaky (frequent `ERROR_CLIENT`/`NO_MATCH`, must sometimes destroy+recreate the instance); Bluetooth output latency (~100–300 ms) + room echo mean the speaker is still emitting *after* TTS reports `speechEnded` (so the mic can transcribe the barista's own tail); a model round-trip is 1–4 s (hence fillers/lead-ins); the barista UI overlay is a plain resource bundle **not** covered by any lint/type gate, so a green build never proves it works — only on-device does.

## What we want from Fable

Design the **cleanest possible state machine** for this two-way conversation, independently. Then reconcile with ours. Specifically:

1. **The state model.** Is our 7-state set right — too many, too few, wrong boundaries? Where should `state` live (extend `AssistantOrchestrator`, a new `BaristaConversation`, or lower)? Give the full state list, the **total** transition table (every event × state, incl. timeouts), and which single component owns it.

2. **The mic/TTS arbiter.** We propose "mic hears iff `Listening`." Pressure-test it against: the coaching voice (steam/espresso live coaches speak independently), fillers/lead-ins that speak *during* `Thinking`, and barge-in. Does a one-rule arbiter hold, or is a small explicit sub-model needed?

3. **The echo/acoustic-tail problem (highest technical risk).** We plan to keep only a short *measured* acoustic re-arm on entering `Listening` (BT latency + room echo), deleting the stale-buffer half. Is there a cleaner primitive than a time window — e.g. gating STT start on a real "speaker output drained" signal, an audio-focus/route event, or an echo-canceller? What does `speechEnded`/`audible` actually guarantee vs. speaker output, and how would you make first-utterance capture reliable **without** transcribing the barista?

4. **Robust close.** Our close arms on either the model's `end_conversation` **or** a generous local intent check, with deterministic teardown + one watchdog. Is there a more reliable close-intent mechanism (e.g. always let the model decide but make the decision cheap/forced; or an explicit "are you done?" confirm)? How do you guarantee close without either a brittle regex or trusting the model?

5. **Bounded STT recovery.** Given Android's flakiness, what's the right recovery ladder (retry → recreate recogniser → `NeedsTap`)? Thresholds, and how to avoid both the silent loop and a trigger-happy "tap to talk."

6. **The migration risk we already found.** Our Phase 1 ("C++ owns `state`, QML becomes a view") may not cleanly separate from the entangled filler/lead-in/read-along logic (`_pendingSpeech`, `_leadinSpokenText`, dup-guards) which reads `_thinking`/`_spokeThisTurn`. Propose a phasing/migration that de-risks the everyday path — including whether a compatibility shim is warranted, or whether the filler logic must move to C++ in the same phase.

7. **What to delete.** Confirm (or expand) the list of latches/timers our design retires, and flag anything we're keeping that a cleaner model wouldn't need.

## Deliverable

A design document that: (a) presents Fable's own state model + transition table + ownership; (b) explicitly agrees/disagrees with each section of `BARISTA_TwoWay_Comms_Redesign.md`, with reasons; (c) gives a concrete answer to the echo/acoustic-tail primitive (item 3) and the close mechanism (item 4); (d) a de-risked phasing. Ground every recommendation in the constraints above — no design that assumes Android STT is reliable, that `speechEnded` means "speaker silent," or that the model always calls its tools.
