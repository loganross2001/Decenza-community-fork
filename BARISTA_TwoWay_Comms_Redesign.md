# Barista Two-Way Communication — Clean-Slate Redesign

*Design doc for review. Nothing is implemented yet. Goal: replace the accreted flag/timer web that drives the barista's voice conversation with one authoritative state machine, so the two recurring bugs (won't-close, listening-loop) become impossible **by construction** rather than patched.*

---

## 1. Why we're redoing this

The conversation logic is spread across **four layers with no single source of truth**:

| Layer | What it holds today |
|---|---|
| QML `AssistantOverlay` | ~16 boolean latches (`_thinking`, `_endAfterReply`, `_primed`, `_sessionBegun`, `_paused`, `_awaitingContext`, `_spokeThisTurn`, `_quickFillerSpoke`, `_fillerGateOpen`, `_pendingSpeech`, `_pendingDisplayText`, …) + 8 timers |
| C++ `AssistantOrchestrator` | `Present` / `Conversing` |
| C++ `VoiceInput` | `listening` / `paused` / `errorStreak` / a 400 ms echo window / `recogniserStartedMs` |
| Java `DecenzaSpeech` | recognizer instance lifecycle |

Problems this causes:

- **State is duplicated.** "Conversing" lives in QML `_state` *and* C++ `m_state`. "Is the barista busy" is smeared across `_thinking`, `AIConversation::m_busy`, and `AssistantVoice::m_speaking`. No one field is authoritative.
- **The mic is driven from three places** — the main turn loop, the coaching-voice pause, and the filler path — each calling `pauseMic()`/`resumeMic()` on its own. There is no single owner of "should the mic be hearing right now?"
- **Behaviour emerges from ~16 interacting flags + timers** instead of a small, explicit state graph. Each past fix added a guard; each guard added an edge case. That is the accumulation the owner correctly flagged.
- **Two "waiting" latches have no timeout** (`_thinking`, `_awaitingContext`): a hung network or context build strands the whole machine with no recovery.

## 2. The two bugs, root-caused

**Bug A — won't close on "that'll be all."** Closing depends on one of three brittle things lining up:
1. a **local regex** matching the user's exact words (*"that'll be all"* may not match the pattern), **or**
2. the **model** choosing to call the `end_conversation` tool, **plus**
3. a **9-second fallback timer that only arms if (1) or (2) already fired.**

Miss the regex *and* have the model keep chatting → nothing arms → the dock stays open and listening.

**Confirmed in the diagnostics log:** in a session where the user said *"thanks and good night,"* every `[overlay]` event carries `endAfter=false` for the whole session — `_endAfterReply` was **never armed** (the local matcher didn't fire on that phrasing and the model didn't call `end_conversation`), so the dock kept resuming the mic instead of closing. This is the real failure, not a presumed one.

**Bug B — listening loop.** After TTS ends the mic reopens through a pause/resume web with a **400 ms echo window** and STT error-retries. The user's first utterance can be silently dropped (spoke too soon into the echo window, or an STT no-match/`ERROR_CLIENT`), and there is **no "sorry, didn't catch that — tap to talk."** It just silently re-listens, so the user repeats themselves.

Both bugs share a root cause: **no state owns the transition, so correctness depends on a chorus of guards all behaving.**

---

## 3. Design principles

1. **One authoritative state machine, in C++.** It owns the conversation lifecycle and *drives* the actuators (`VoiceInput`, `AssistantVoice`, `AIConversation`). Everything else observes it.
2. **The mic has exactly one rule:** it hears **iff** we are in `Listening`. No other code calls pause/resume.
3. **Every transition is explicit and total.** For each state, the set of events that can occur and where each leads is enumerated — including timeouts. There is no "and also these six flags."
4. **Teardown is a consequence of a state, not a race between timers.** Entering `Closing` *is* the close; the sign-off + mic/TTS shutdown are its deterministic exit.
5. **No silent failure.** A dropped utterance or STT malfunction surfaces as a visible, tappable prompt — never an invisible re-listen.
6. **QML is a view.** It renders the current state and forwards two things: the user's tap and (via C++, not QML) their speech. It holds no conversation logic.

---

## 4. The state machine

### States

| State | Meaning | Mic | TTS |
|---|---|---|---|
| `Idle` | Dock present, not engaged | off | off |
| `Priming` | Engaged; assembling context/system prompt | off | off |
| `Listening` | Waiting for the user to speak | **hot** | off |
| `Thinking` | Utterance sent; awaiting model (incl. tool loop) | off | off |
| `Speaking` | TTS playing (filler, lead-in, or answer) | off | on |
| `Closing` | Sign-off requested; speak it, then tear down | off | on→off |
| `NeedsTap` | STT gave up / dropped input; show "tap to talk" | off (until tap) | off |

`Thinking` and `Speaking` are distinct only for the UI (thinking-hum vs mouth animation); to the mic they are identical (**off**). They may alternate within one turn (filler → wait → answer) without the mic ever opening.

### The mic/TTS arbiter (the whole thing)

```
onEnter(state):
    VoiceInput.setActive( state == Listening )     // the ONLY mic control in the codebase
    // TTS is started explicitly by the turn logic; it is only ever running in Speaking/Closing.
```

`VoiceInput.setActive(true)` starts a clean recogniser session; `setActive(false)` stops it. Because the mic is **off** throughout `Speaking` and only turns on when we *enter* `Listening` — strictly **after** `AssistantVoice` reports `speechEnded` — there is no *stale buffered* result to guard against, so that half of the old 400 ms window is gone.

**But the acoustic half stays.** The 400 ms window guarded two different things: (a) a stale STT buffer — killed by clean session-gating — and (b) the **acoustic tail of the barista's own speech**. Clean gating does **not** kill (b): `speechEnded` fires when the synth stops *feeding* audio, but Bluetooth output latency (~100–300 ms) plus room echo means the speaker is still emitting for a beat after. Deleting the guard entirely would trade the listening-loop for the barista transcribing *itself*. So on entering `Listening` we keep a **short acoustic re-arm** (a ~250–400 ms window, tuned to measured BT latency) — but now justified explicitly as *acoustic latency*, not as the old logic workaround, and it is the state machine's, not a flag scattered in `VoiceInput`. **Prerequisite:** before setting the value, verify empirically what `speechEnded`/`audible` mean relative to actual speaker output (the diagnostics log's `speak_start`/`speaking_off` timing vs. the first post-resume STT result).

### Transition table

| From | Event | To | Action |
|---|---|---|---|
| `Idle` | user taps dock | `Priming` | request context/system prompt (with timeout) |
| `Priming` | context ready | `Listening` | — |
| `Priming` | timeout | `Listening` | proceed with degraded context; log |
| `Listening` | final utterance (normal) | `Thinking` | send turn |
| `Listening` | final utterance (**close-intent**) | `Thinking` | send turn, **arm closing** |
| `Listening` | silence timeout | `NeedsTap` | show "tap when ready"; mic off |
| `Listening` | STT malfunction > N | `NeedsTap` | show "didn't catch that — tap to talk" |
| `Thinking` | model emits speakable (filler/lead-in) | `Speaking` | speak it |
| `Thinking` | final answer, no speech pending | `Speaking` | speak answer |
| `Thinking` | model calls `end_conversation` | (arm closing) | (stays `Thinking`/`Speaking` for the sign-off) |
| `Thinking` | turn timeout | `Speaking` | speak "that took too long, try again"; then `Listening` |
| `Speaking` | `speechEnded`, turn still cooking | `Thinking` | — |
| `Speaking` | `speechEnded`, turn done, **not** closing | `Listening` | — |
| `Speaking` | `speechEnded`, **closing armed** | `Closing` | (already spoke sign-off) → tear down |
| `Closing` | sign-off `speechEnded` **or** watchdog | `Idle` | stop mic, stop TTS, collapse dock |
| `NeedsTap` | user taps | `Listening` | — |
| *any* | user taps × | `Idle` | full teardown |

**Closing is entered by exactly two triggers** — the model's `end_conversation` tool, *or* a close-intent utterance — and its exit is a single deterministic teardown. The old three-way regex/tool/9s-timer coordination is gone; a single **watchdog** on `Closing`/`Thinking` remains only as a backstop, not the mechanism.

### Close-intent detection (robust, not a brittle regex)

Two independent paths, either sufficient:
1. **Model:** persona keeps instructing `end_conversation` on farewell (unchanged, but no longer the *only* reliable path).
2. **Local:** a **generous** intent check on the final utterance (normalized; matches "that'll be all", "that's it", "I'm good", "we're done", "goodnight", etc. — a maintained list, and easy to extend), which arms closing regardless of what the model does.

Because arming closing only *speeds up* a close the teardown will do deterministically anyway, a false-negative just means the model's tool call (or the user tapping ×) closes it; a false-positive is avoided by requiring the utterance to be *short and* match (won't fire mid-sentence).

---

## 5. How the two bugs die by construction

- **Won't-close:** `Closing` has one entry set and one exit (teardown). Once armed, the sign-off plays and teardown runs — no dependency on a regex *and* a model tool *and* a timer all firing. The watchdog guarantees exit even if TTS never reports done.
- **Listening-loop:** the mic is only hot in `Listening`, and the drop-window shrinks from "a logic guard of unknown necessity" to "a short, *measured* acoustic guard for the speaker's own tail." A genuine STT malfunction, after a bounded retry, lands in `NeedsTap` — a visible prompt — instead of a silent re-listen. The user never "talks into a void."

---

## 6. What gets deleted

The redesign *removes* far more than it adds. Retired:

- **QML latches:** `_thinking`, `_endAfterReply`, `_primed`, `_sessionBegun`, `_paused`, `_awaitingContext`, `_spokeThisTurn`, `_quickFillerSpoke`, `_fillerGateOpen`, `_pendingSpeech`, `_pendingDisplayText` → collapsed into `state` + a handful of typed fields on the controller.
- **QML timers:** `dismissFallbackTimer` (→ single Closing watchdog), the pause/resume wiring on `_voice`/`_coachingVoice` (→ the arbiter).
- **`VoiceInput`:** `m_paused` and the pause/resume surface → `setActive(bool)`. The echo window (`m_ignoreFinalUntilMs`/`kPostTtsIgnoreMs`) is **not** deleted — it moves up to the state machine as the measured acoustic re-arm on entering `Listening` (see §4). Keep only the bounded error/streak recovery, which now ends in `NeedsTap` rather than a silent restart.
- **Duplicated state:** QML `_state` string mirror of C++ `m_state` → one enum, exposed read-only to QML.

## 7. Ownership & wiring

- **New/renamed C++ owner:** a `BaristaConversation` controller (extends or replaces `AssistantOrchestrator`). Holds the `State` enum, drives `VoiceInput.setActive`, calls `AssistantVoice.speak`, sends turns via `AIConversation`, and consumes `end_conversation`.
- **`VoiceInput`** becomes a dumb actuator: `setActive(bool)`, emits `finalText`, `partial`, `malfunction`. Its `finalText` goes **directly to the controller** (C++→C++), not through QML.
- **`AssistantVoice`** unchanged as an actuator; the controller consumes `speaking`/`audible`/`speechEnded`.
- **QML `AssistantOverlay`** binds to `Barista.conversation.state` (+ `partialText`, `message`) for rendering; forwards taps. The filler/read-along visuals key off `state` + `audible`, not off local latches.

## 8. Phased implementation (structure first, per owner's choice)

Each phase builds green and is tested on-device before the next; no long broken window.

- **Phase 1 — stand up the state machine + mic/TTS arbiter (behaviour-preserving).**
  Introduce `BaristaConversation` with the states above; route `VoiceInput.finalText`, `AssistantVoice` signals, and turn dispatch through it; make the mic obey the single arbiter rule. QML starts reading `state` instead of its own latches. Target: identical behaviour, one source of truth. *This is the big structural change.*
- **Phase 2 — robust close.** Implement `Closing` as the single deterministic teardown; add the generous local close-intent; retire the 3-way arm + 9s timer (keep the one watchdog). Delete `_endAfterReply` and friends.
- **Phase 3 — mic recovery + timeouts.** `NeedsTap`; bounded STT recovery into it; `Thinking`/`Priming` timeouts; re-home the echo window as the measured acoustic re-arm (verify BT-latency timing first). Delete the remaining retired latches.
- **Phase 4 — carry the filler/lead-in/read-along nuances onto `state`** and delete the last QML timers.

## 9. Behaviour to preserve (don't lose these in the rewrite)

- Quick-filler ("give me a sec") after a gate, aborted if the real answer beats it.
- Lead-in vs post-tool answer de-duplication (don't say the same thing twice).
- Read-along text reveal tied to **audible** (no text before sound).
- Coaching-voice (steam/espresso) never transcribed — under the new rule the coaching voice speaking simply means the barista mic is not in `Listening`, so this collapses into the arbiter.
- Thinking-hum keepalive on sleepy BT/USB speakers.
- Silence auto-close, screensaver vs normal timing.

## 10. Risks & testing

- **Risk:** the rewrite touches the most-used interaction path; a regression is very visible. *Mitigation:* phased, behaviour-preserving Phase 1; on-device test each phase; keep the `BaristaDiagnostics` timeline (it's how both bugs were caught) and add `state`-transition records to it.
- **Risk — Phase 1 may not be cleanly separable from the filler logic.** The quick-filler / lead-in / read-along machinery (`_pendingSpeech`, `_leadinSpokenText`, dup-guards, filler gate) is deeply entangled with `_thinking`/`_spokeThisTurn`. Moving state ownership to C++ in Phase 1 while that logic still *reads* the old QML latches may not hold. **Prerequisite before committing to the phase split:** trace one concrete path — *utterance → filler → lead-in → answer* — across the proposed Phase-1 boundary and confirm it works with C++ owning `state`. If it doesn't, either Phase 1 must bring the filler logic along (bigger, riskier) or Phase 1 exposes a compatibility shim the QML filler code reads until Phase 4. This is the phase most likely to regress the everyday path, so de-risk it before writing code.
- **Risk:** Android STT flakiness is environmental and can't be fully designed away. *Mitigation:* the design *contains* it (bounded retry → visible `NeedsTap`) rather than trying to make Android perfect.
- **Testing:** the barista is a plain `qt_add_resources` bundle (not qmllint-gated), so a green build does **not** prove the overlay loads or behaves — every phase needs on-device confirmation + a diagnostics-log check.

---

---

## 11. Findings from the pre-implementation de-risk pass (2026-07-30)

**Finding 1 — Phase 1 is a turn-lifecycle move, not a thin structural change.** Reading the actual handlers (`AssistantOverlay.qml` `onInterimReceived` ~1583, `onResponseReceived` ~1610, `onErrorOccurred` ~1700, plus `onSpeakingChanged`/`onFinalText`) confirms these Connections *are* the state machine's transitions — and each is interwoven with the filler abort, the lead-in/answer dup-guard, the text-on-speak reveal, mic pause/resume, and the `_endAfterReply` close. There is no seam where "C++ owns `state`" while this stays untouched in QML. **Consequence:** Phase 1 must relocate the turn *lifecycle* (the transitions + mic/TTS driving + close) into the C++ owner. What legitimately stays in QML is pure presentation — text reveal keyed on `audible`, read-along scroll, avatar, and the *visual* filler timing — reading `state` + a few controller-exposed fields. This is the biggest, highest-regression-risk step, which is why it wants the Fable cross-check and on-device iteration.

**Finding 2 — the current 400 ms acoustic guard is empirically working.** In the diagnostics log, `speaking_off` → `mic resume (echoGuardMs=400)` fires within ~2 ms and the next `final_result` is seconds later (real user speech, no self-transcription). So ~400 ms is a safe starting value for the acoustic re-arm; still re-validate on-device with a Bluetooth speaker (worst case for output latency).

## 12. Resolved open questions (recommendations)

- **(a) Extend, don't replace.** Grow `AssistantOrchestrator` into the single owner (it already holds `Present/Conversing`, the dismiss path, and `lastExchangeAt`, and QML already reaches it as `Barista.orchestrator`). Expand its `State` enum to the full set and move the transitions in, rather than standing up a parallel `BaristaConversation` that would fight it for ownership. (Rename optional.)
- **(b) Relocate the filler behaviour, don't simplify it yet.** Keep quick-filler / lead-in / dup-guard *behaviour* (it's tuned and the owner likes it); move its *control* under the state machine, leave its *visuals* in QML. Simplifying the filler is a separate, later change — bundling it into the rewrite widens the blast radius.
- **(c) `NeedsTap` is a full state.** Mic off, visible "tap to talk" prompt, single `tap → Listening` transition. Cleaner than a gated sub-mode and makes "the mic is not silently listening" explicit.

---

## 13. RECONCILED FINAL DESIGN (post-Fable review, 2026-07-30) — this section is authoritative

Fable's independent pass (`BARISTA_TwoWay_Comms_Fable_Review.md`) converged on the same skeleton (single C++ owner, 7 states, deterministic `Closing`, visible `NeedsTap`, phased). It made **three blocking amendments, all accepted** because each closes a real hole:

1. **Two-input mic gate (accepted — §3 principle 2 was wrong).** "Mic hears iff `Listening`" fails because the **coaching voices** (steam/espresso) are a speech source *outside* the conversation graph and can play while the user is mid-conversation (a shot pulling). New rule:
   ```
   micLive = (state == Listening) && SpeakerGate.quiet
   ```
   A small **`SpeakerGate`** helper (owned by the controller) is the only thing that knows the speaker's acoustic status: it watches **all** TTS sources (conversational *and* coaching), goes `false` the instant any starts, and returns `true` only **D ms after the last `speechEnded`**. QML shows the listening indicator and starts the silence timer from **`micLive`**, not `state`.

2. **Forced per-turn close bit (accepted — removes Bug-A's residue).** The logged failure was *both* optional signals (regex + model tool) missing at once; any close-as-optional design keeps a residue. Fix: the model's reply is delivered through a **forced tool call `respond(text, end_conversation: bool)`** with `end_conversation` a **required** field, so every reply carries an explicit continue/close bit — "the model didn't call the tool" becomes impossible. Local generous close-intent stays as a *latency accelerator* only; `NeedsTap`-idle→silent-close and tap-× remain backstops.
   - **My implementation caveat (multi-provider):** forced tool choice isn't universal — Anthropic / Gemini / OpenAI support it; Ollama/local may not. So `respond(text, end_conversation)` needs a **per-provider strategy**, with a **fallback** on providers lacking forced-tool-choice to the local-intent + optional-tool + `NeedsTap`-idle-close net (i.e. today's mechanism, but now backed by the deterministic teardown). Resolve this in Phase 2.

3. **Filler moves to C++ in Phase 1 behind a runtime flag — no shim (accepted).** The filler/lead-in/answer sequencing *is* the `Thinking ⇄ Speaking` core (the `pendingAnswer` rows), not view logic; a QML shim would be double-bookkeeping in the one un-lintable layer. De-risk with a **`useNewConversation` runtime flag** (a *parallel path*, exactly one active — not a translation shim) so rollback is a toggle, and add a **Phase 0** (instrument + measure, change nothing).

**Other adopted refinements:**
- **Replace, don't extend** `AssistantOrchestrator` (its `Present/Conversing` pair is the vestigial second authority §1 indicts). *(Overrides my earlier "extend" recommendation.)*
- **Extended state = exactly four fields:** `closingArmed`, `turnInFlight`, `pendingAnswer`, `retry{soft,hard}`.
- **Total transition table** = Fable's A.3. Additions my draft table missed: TTS-`error` dispatches as `speechEnded` (a synth failure must not strand the machine); the `pendingAnswer` rows (answer arrives while filler still playing); `NeedsTap` idle-timeout → silent `Closing` (this *is* the preserved silence auto-close); tap-to-skip barge-in in `Speaking`; and the `Thinking` turn-timeout branches on `closingArmed`. Dispatch order in `Speaking`/`speechEnded` is load-bearing: **`pendingAnswer` → `turnInFlight` → `closingArmed` → `Listening`.**
- **Acoustic primitive:** short per-route **D** (BT A2DP ≈ 400 ms, wired/built-in ≈ 150 ms, config-tunable + logged) **+ a self-echo text filter** (drop a first post-resume result with ≥~70% in-order token overlap with the last spoken TTS, within 2 s) **+ auto-widen D by 100 ms** after two self-echo drops in a session. The filter lets D stay short → protects Bug-B without letting the barista transcribe itself.
- **Filler simplified in-move:** one controller timer ("no speakable within ~1.2 s of entering `Thinking` → speak one filler; cancel if the answer wins the race; never cut a filler mid-word"), not a gate object. `_leadinSpokenText` + dup-guards move to C++ (deleted from QML), not deferred to Phase 4.
- **Recovery ladder:** `NO_MATCH`/`SPEECH_TIMEOUT` → silent same-session restart, 3-in-a-row → `NeedsTap`; `ERROR_CLIENT`/`BUSY` → destroy+recreate immediately, 2nd → recreate w/ 800 ms backoff, 3rd → `NeedsTap`; permissions/unavailable → `NeedsTap` directly. Any success resets both counters.

**Final phasing (authoritative):**
- **Phase 0 — instrument + measure, change nothing.** Add `state`/`micLive` transition records to `BaristaDiagnostics`; mine existing + fresh logs for `speak_start`/`speaking_off` → first-post-resume-STT gaps to set per-route **D**; capture reference timelines of *utterance → filler → lead-in → answer*.
- **Phase 1 — controller + `SpeakerGate` + turn/filler sequencing in C++, behind `useNewConversation`.** Old QML path intact + default-on; flip on-device, compare against Phase-0 reference timelines, flip back instantly on any regression. Exit criterion: everyday path timeline-matches under the flag.
- **Phase 2 — robust close:** forced `respond(text, end_conversation)` (+ multi-provider fallback), local close-intent, `Closing` teardown + 2.5 s watchdog.
- **Phase 3 — recovery + timeouts:** ladder, `NeedsTap`, `Priming`/`Thinking` timeouts, self-echo filter, D auto-widen.
- **Phase 4 — delete the old path:** flag, QML latches, timers, `_state` mirror. Deletion last; switch-not-shim throughout.

The design is now settled. Only open implementation detail carried forward: the **multi-provider forced-`respond` strategy** (Phase 2).

## 14. Phase 0 findings (2026-07-30) — the Phase-1 yardstick

Mined from `~/Downloads/barista-diagnostics.log` (an ordinary multi-turn session on the **built-in speaker**).

**Acoustic window `D`:** across the whole session the mic `resume` fires **~6–12 ms** after `speaking_off`, and the first genuine `final_result` is always **seconds** later — **no self-echo occurred** (the 400 ms guard was never even exercised). Conclusion: on built-in/wired output `D ≈ 150 ms` is safe with wide margin; keep **BT A2DP `D = 400 ms`** as the conservative default and **validate on-device with a Bluetooth speaker before Phase 3 ships** (BT output latency can't be measured from these logs). Log `D`, `self_echo_drop`, and D-widen events in Phase 1 so field devices self-tune.

**Reference timelines (Phase 1 under `useNewConversation` must match these):**
- *Simple turn:* `sttFinal → mic off → (~2–3 s) speak(answer) → speechEnded → mic on (+~10 ms) → next sttFinal`.
- *Tool turn (lead-in → answer):* `sttFinal → mic off → (~2.6 s) speak(lead-in "…one sec") → speechEnded → (+~15 ms) speak(answer) → speechEnded → mic on`. **Lead-in and answer are back-to-back with the mic never opening between them** — this is the `Speaking → pendingAnswer → Speaking` path in the new table; it must not regress into a mic-open gap.

**Health note:** the clean session showed no loops/echo/drops — the everyday path is sound when STT behaves, which is why Bug B is *intermittent* (STT errors) and Bug A is about *close*, not the turn loop. The existing diagnostics events (`speak_start`/`speaking_off`/`mic pause`/`mic resume`/`final_result`/`interim_leadin`) already map onto the new states, so Phase 0 needed no code — Phase 1 adds explicit `state`/`micLive` records on top.

## 15. Implementation status (2026-07-30) — Phase 2 partial; Phase 2-forced-tool + Phase 4 DEFERRED

Shipped (C++, behind `useNewConversation`, in APK vc346xxxx):
- **Close-intent matcher fixed (`baristaconversation.cpp::looksLikeClose`).** The old `^…$` regex had NO prefix handling, so every POLITE farewell missed — "thanks, that'll be all" didn't start with a listed farewell and wasn't in the narrow `thanks,? (that's (it|all)|bye)` branch. This was the owner's actual reported "it stays open when I say *thanks that'll be all*" bug. Fix strips up to two leading politeness/filler prefixes (ok/alright/thanks/thank you/no/cool/…) THEN matches a broadened farewell set. Bare politeness ("thanks") strips to empty and does NOT close. Verified by trace; C++/compiled.
- **Walk-away backstop (`m_needsTapIdle`, 45 s).** Once in `NeedsTap` (30 s of Listening silence already elapsed) and never tapped, the dock silently closes (emit `closingConfirmed` → view collapses, → `Idle`). ~75 s total idle → only genuine abandonment. UI auto-dismiss (the one allowed timer use). This is the design's "`NeedsTap` idle → silent close" backstop.

DEFERRED — both turned out LARGER than "hardening"/"deletion"; each needs its own on-device-validated session (owner tests in the morning; a broken barista = failed test, cf. the v2.0.1 "barista does not load" incident):

- **Forced `respond(text, end_conversation)` (Phase 2 structural bit).** BLOCKER discovered: forcing `tool_choice` to a single `respond` tool is INCOMPATIBLE with the barista's multi-tool loop — the model must be free to call `getShotHistory`/`web_search`/`applyDial`/`end_conversation` mid-turn, and a forced single-tool choice forbids that. The workable shape is Anthropic `tool_choice:"any"` (model must call SOME tool; `respond` is the only "answer" tool so every final reply carries the bit) — but that means the model can NEVER emit free text, so the "let me check" lead-in prose (currently emitted as pre-tool text, `interimText`) has to be re-plumbed through a tool arg. That is a turn-protocol change touching the exact aiprovider.cpp tool loop just merged (#1691/#1694). MUST be Anthropic-only (barista's provider) and scoped to `analyzeConversation` ONLY — never `analyze`/`analyzeUrl`/advisor, or it wrecks recipe URL extraction. Fail-safe: a reply without a `respond` call falls back to today's text path + looksLikeClose/end_conversation. Do as a dedicated session with a Bluetooth-speaker on-device test.
- **Phase 4 (delete old path).** NOT a mechanical deletion. The `_nc` path in `AssistantOverlay.qml` (2709 lines, UNLINTED) still depends on `AssistantOrchestrator` for shot-tracking/exchange/recency (`markExchangeCompleted`, `lastShotId`, `markShotDiscussed`, `recencyBucket`). Removing the orchestrator/flag requires migrating that shared infra out of QML first. **Keep the `useNewConversation` flag** — it is the only on-device rollback if the new path has a latent bug. Do as its own session after Phase-2-forced is validated.

## 16. Session close / next-session handoff (2026-07-30)

**State: feat/barista at `0e2df8ef`, 0 behind upstream, pushed to the PRIVATE `backup` remote (Decenza-private). App builds green; tst_closeintent 42/42.**

Latest testable APK: `~/Downloads/Decenza-barista-default-on-tested-vc3460574.apk` (all earlier vc346xxxx APKs are superseded). Contains: upstream merge + stale-data fix + close/stall hardening + the "no more questions" close fix + new engine default-ON.

Shipped this session (commits, newest first):
- `0e2df8ef` — extracted looksLikeClose/looksLikeStall → `src/barista/closeintent.{h,cpp}`; added `tests/tst_closeintent.cpp` (42 cases, RUN locally 42/42); the test caught a real bug (bare "no more questions" didn't close — prefix stripper ate "no") → fixed (commas→spaces, "no" no longer a stripped prefix). Flipped `AssistantSettings::useNewConversation` default → **true** (flag kept as rollback).
- `8ab759ec` — stall fix: model ending a turn with only "let me check on that" (no tool call) dropped to Listening. Now `looksLikeStall()` treats a bare promise-to-continue as a lead-in: speak it, keep turnInFlight true (→ Thinking → tone), and send ONE continuation (`continuationRequested` → overlay `followUp`, deferred via Qt.callLater) to fetch the real answer. Bounded by kMaxAutoContinues=1, skipped when closing.
- `b778868d` — close hardening: prefix-aware `looksLikeClose()` (fixes "thanks, that'll be all") + NeedsTap walk-away auto-close (m_needsTapIdle 45s).
- `9b1c0e5c` — upstream merge (37 commits): migration 38 = enrichment heal (renumbered from upstream 35; see [[decenza-fork-schema-divergence]]); aiprovider.cpp #1691/#1694 truncation+thinking-off interleaved with the barista tool loops; FINAL Q_PROPERTY conflicts; test "latest" assertions → 38.

Verify a fix engaged on-device via `~/Downloads/barista-diagnostics.log`: `close_intent_local`, `stall_autocontinue`, `needstap_idle_autoclose`.

STILL DEFERRED (each its own validated session — see §15 for the full analysis; NOT started):
1. **Forced `respond(text, end_conversation)`** — conflicts with the multi-tool loop (needs Anthropic `tool_choice:"any"` + lead-in re-plumbing), Anthropic-only, scoped to analyzeConversation ONLY. Low marginal value now that close/stall are fixed + tested; do only if the heuristic close proves insufficient on-device.
2. **Phase 4 (delete legacy path)** — for a single user this is pure code-hygiene with ZERO behavior change and real "barista won't load" risk; the `_nc` path still leans on AssistantOrchestrator for shot/exchange/recency. Keep the `useNewConversation` flag until done.

Open validation gap: the barista overlay QML is unlinted/untest-covered — on-device run is the only proof for anything touching AssistantOverlay.qml. Owner has built-in-speaker testing (no Bluetooth); the BT-speaker acoustic-drain (SpeakerGate D) validation for Phase 3 remains unrun.
