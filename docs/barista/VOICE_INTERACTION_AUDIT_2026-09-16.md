# Barista voice interaction — full audit (2026-09-16)

Two multi-agent passes (targeted, then a full 8-lens sweep), each adversarially verified. Device evidence:
8 session logs (`barista-diagnostics*.log`). This is the source of truth for the voice-loop remediation.

## Systemic story (the "wacky anomalies" reframed)
The 40+ log signals collapse to a handful of causes — and **a large share are observability artifacts, not
distinct behavior bugs**:
- `speak_INTERRUPTS_previous ×27` = ~24 **mislabels** of the normal sentence-by-sentence chunk handoff
  (barista role) + exactly **3 real** self-truncations (coaching role). Not 27 bugs.
- `needsTap ×40` = ~10 genuine 30s silence timeouts + ~30 **user "Stop" taps that log nothing** — a deliberate
  stop and a real mic-death look identical in the logs.
- The whole **Gemini path is under-instrumented** (0 `tool_done`, 0 reply-ms, 0 partials) — that telemetry was
  only ever wired on the dead Anthropic branch. So the catalog over-counts behavior bugs.

Beneath the noise, the genuine root causes:
1. **Single-duplex by design + no app-layer turn merge.** Mic is live only in `Listening && gate.quiet`, so the
   barista is deaf during the 22–24s Gemini tool loop; and `onFinalText` commits the FIRST ~1s platform
   endpoint as the whole turn (zero pause-tolerance → half-sentences, swallowed continuations). ← the 17s cutoff.
2. **`onModelFinal` missing its state guard.** It's the only actuator input lacking the precondition guard its
   three siblings have, so a late Gemini reply (after the 40s turn-timeout drops to Listening) resurrects
   Speaking and talks over the user.
3. **Two competing mic owners.** A legacy QML `silenceTimer` still calls `VoiceInput.stop()` directly (the
   `if (_nc) return` short-circuit was added to `onFinalText` but not the partial/listening/paused handlers).
   *(Adversarial note: its diag is 0 across all sessions — real invariant violation, but not an active cause of
   the churn. Cleanup, not an urgent bug.)*
4. **Post-reply gate-quiet flap.** The SpeakerGate drain window oscillates `setActive(false/true)` per reply,
   driving destroy+recreate of the recogniser (the 72 `code 5` events) with a warm-up gap that eats the opening
   words of the next turn — the "works if I repeat" symptom.
5. **Unreachable recovery ladder.** `m_errorStreak` resets on every pause/resume/final, so the fatal STT ladder
   can never climb past 1 (confirmed: `streak=1` in all 137 error lines) — genuine deafness never surfaces.

**Latency is the Gemini tool loop, not TTS or streaming.** `voiceStreaming` is inert on Gemini (do not flip).
TTS does add ~10ms/char on long whole-reply blobs — addressable by the existing-but-OFF `speakInChunks` path.

**The reboot total-deafness was the `silReqMs=2800` poison extra — MY reverted experiment, not current source.**
Confirmed: `git log --all -S silReqMs` finds nothing; current HEAD `DecenzaSpeech.java` is clean. The lasting
action is a denylist gate so it can never be re-added, not a source patch.

## Remediation roadmap (bugs/guards before features; each independently shippable)

| # | Item | Type | Risk | Gate |
|---|------|------|------|------|
| R0 | Observability + poison denylist gate: `check_stt_intent_extras.py` in text-invariants.yml; deafness canary (absence-of-final at session close, guarded by a healthy-listen-span so a short greeting doesn't false-fire); `tap_stop` diag; mirror `reply-ms`/`tool_done` into the Gemini path; split the `speak_INTERRUPTS_previous` diag from chunk-handoff | guard+obs | **none** | ship first |
| R1 | `onModelFinal` state guard (stop late replies talking over the user) | bug | low | ship |
| R3 | Coaching-voice same-role `speaking()` guard (stop the real cue self-truncation, `main.cpp`) | bug | low | ship |
| R2 | Disarm legacy QML `silenceTimer` on the new path (`if (_nc) return`) | cleanup | med | after burn-in check |
| R4 | Make the fatal STT ladder observable (no-words restart counter) | bug | med | device probe |
| R5 | Gate-quiet **hysteresis** — kill the per-reply recreate storm ("works if I repeat") | bug | med-high | **device probe** |
| R6 | App-layer endpointing **continuation-merge** (pause tolerance — the original ask) | feature | **high** | **device probe** |
| R7 | Owner-gated: `speakInChunks` default→true (+ fix prefetch never engaging); B2 spoken ack on Thinking-entry; Gemini brevity clauses | feature | persona | owner + by-ear |

## Device probes required BEFORE coding the risky items
- **Warm-restart warm-up gap** (gates R6): instrument the empty `onReadyForSpeech`/`onBeginningOfSpeech` +
  timestamp `startListening` → first speech event. If the gap exceeds an inter-clause pause, the merge DROPS
  continuation words and is net-negative.
- **Do `onBeginningOfSpeech`/`onPartialResults` fire at all** on this Samsung/AOSP recogniser (gates R4/R6).
- **Gate-quiet flap count per reply** (gates R5).
- **code-5 rate under the merge** (gates R6 — it forces a restart after every final).
- **Chunked first-audio latency + `prefetch_play>0`** (gates the R7 speakInChunks flip).

## Owner decisions
1. **speakInChunks default→true** (R7a): first audio ~1s in instead of after up to ~16s of whole-reply synthesis
   on long replies; changes cadence to audible sentence-by-sentence. *Rec: yes, by-ear gated.*
2. **B2 spoken ack during Thinking** (R7b): the user hears words instead of 4–24s of earcon; provider-gated OFF
   Anthropic, never the banned pre-announcement lead-in. *Rec: yes — highest-leverage perceived-latency lever.*
3. **Gemini brevity clauses** (R7c): shorter replies cut TTS time and often a tool round; risk trimming dial-in
   specifics. *Rec: yes, with by-ear review (already owed in memory).*
4. **Pause-tolerance (R6) invest?**: it's the highest-risk fix (warm-up gap can make it net-negative). Run the
   device-probe cycle to validate, or accept current behavior for now. *Rec: probe first, decide on data.*
5. **Barge-in scope**: real voice interruption needs echo-cancelled capture (own AudioRecord + AEC) — device-
   risky. *Rec: keep tap-to-skip, relabel the button "Tap to interrupt".*
