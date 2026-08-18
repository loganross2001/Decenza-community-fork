# Decenza Barista — Streaming Voice-Interaction Design

**Prepared by:** Claude Fable 5, 2026-07-10, against `Barista_Voice_Interaction_Research_Brief.md` (Claude Code's file:line-grounded map — taken as ground truth; no re-derivation). **For:** Claude Code to implement on `feat/barista`. *(Note: saved in ~/f4coach for handoff — move alongside the brief in ~/Decenza-fork.)*
**Design goal in one line:** collapse the serial pipeline into four overlapping streams — model tokens → speakable chunks → TTS synthesis → playback — so the barista is *always either usefully silent for <1s or talking*, with turn-taking run by an explicit state machine instead of timing races.

---

## 1. The target pipeline (architecture)

```
STT final ──► POST (stream:true) ──► SSE deltas ──► SpeechChunker ──► SpeechQueue ──► audio out
                                        │                                  ▲
                                        ├─ toolUseStarted ─► ack guarantee ┘   (lead-in already speaking)
                                        └─ tool exec ─► re-POST (stream) ─► … (loop unchanged, each round streams)
```

Four new/changed seams, everything else untouched:

### 1.1 `AnthropicProvider` — add a streaming path (the core seam)
- Add `"stream": true` to the messages POST for barista/coaching turns only (feature flag `barista.streaming`; the whole-body path stays for non-voice analysis callers — do not touch them).
- Parse SSE incrementally on `QNetworkReply::readyRead()` (line-buffered `event:`/`data:` frames; carry partial lines across reads). Handle: `message_start`, `content_block_start` (text | tool_use), `content_block_delta` (`text_delta` → emit; `input_json_delta` → accumulate per block index), `content_block_stop`, `message_delta` (stop_reason), `message_stop`.
- New signals: `textDelta(QString)`, `toolUseStarted(QString name)`, `toolUseReady(name, inputJson)`, `streamTurnComplete(stopReason)`. The existing tool loop (`MAX_TOOL_ROUNDS`, `pause_turn` continuations) is *unchanged in logic* — each round just streams instead of blocking; tool_use args come from the accumulated `input_json_delta` buffers.
- **Accumulate ALL streamed text into `m_accumulatedText` including tool-round lead-ins** — this deletes the §4.4 interim-not-accumulated bug class at the root.

### 1.2 `SpeechChunker` (new, ~80 lines) — tokens → speakable units
- Buffers deltas; emits `chunkReady(QString)` at sentence/clause boundaries (`. ! ? ; :` + em-dash), with guards: don't split decimals/abbreviations ("1.5 bar", "e.g."), min chunk ~25 chars, max ~220 (force-split at last comma/space).
- **First-chunk fast rule:** emit at the first boundary OR 60 chars OR 1.2 s since first delta, whichever first — this is what makes the lead-in land at ~1 s.
- Flush on `content_block_stop`/`streamTurnComplete`. Reset on barge-in.

### 1.3 `AssistantVoice` — `SpeechQueue`: sentence-pipelined TTS (the MVP that avoids risky streaming decode)
- FIFO of chunks with a **2-deep synthesis lookahead**: chunk N playing while N+1's cloud request is in flight (start N+1's request when N's *download* completes, not its playback). Playback stays the existing temp-file `playMp3` per chunk — sequential, gapless-enough with lookahead pre-buffering.
- Native `QTextToSpeech`: no queue support → drive the FIFO off `stateChanged→Ready`; it's already low-latency, gains chunking for free.
- `speaking` stays true from first chunk enqueued until queue empty (mic gating correctness); `m_speakGen` barge-in flushes queue + chunker + aborts in-flight synth requests.
- **Deliberately NOT in MVP:** true chunked-MP3 progressive decode (`QMediaPlayer` on a sequential `QIODevice` is unreliable on Android). Sentence-pipelining captures ~90 % of the win. Phase-6 option if inter-chunk gaps annoy: `QAudioSink` + a minimal MP3 decoder fed by chunked transfer — flagged, not required.
- Latency math (mid-range tablet, cloud voice): first chunk ≈ 1 sentence ≈ 0.4–0.9 s synth → **first audio ~1.5–2.5 s after user stops talking** (vs 5–9 s today); subsequent chunks hidden behind playback.

### 1.4 Turn-taking FSM (replaces the pause/resume race)
States: `IDLE → LISTENING → COMMITTED → THINKING ⇄ SPEAKING → COOLDOWN → LISTENING`.
- Mic hard-off in THINKING/SPEAKING (unchanged — no AEC on this hardware).
- **COOLDOWN = post-TTS ignore window, 400 ms** (tunable): any recognizer result *arriving* in it is dropped; any result whose utterance *overlapped* SPEAKING (compare timestamps) is dropped unless it survives the echo filter.
- **Textual self-echo filter:** normalize (lowercase, strip punctuation) and compare incoming transcription against the last ~300 spoken chars; ≥60 % token overlap → drop silently. Cheap, catches TTS-tail pickup that slips past timing.
- **Transient-restart policy:** cap 6→**3** with backoff 250/500/1000 ms, counter reset on any successful result; on cap-out, transition to a visible *tap-to-talk* state (never a silent loop). The 20 s session timer is unchanged.
- Barge-in stays tap-based (mic is off while speaking, correctly, on AEC-less hardware).

## 2. The acknowledgment guarantee (≤1 s, model-generated, never canned)

Three layers, in order:
1. **Streaming makes the model's own lead-in the primary ack** — it now *speaks at ~1–1.5 s* instead of arriving with the answer. Persona addition (one line): *"When you're about to look something up, first say one short, natural, varied phrase telling me so — then call the tool."* With streaming, this is reliable ~90 % of the time and always fast.
2. **The no-lead-in fallback — a model-generated ack pool.** When `toolUseStarted` fires with zero text spoken this turn: pop a line from a small pool that the *model itself generated earlier in this session* (at `engage()`, piggybacked on the prime call or one cheap background turn: "generate 8 short varied ways you'd naturally say 'give me a second while I check'"). Each line used once, pool refilled off-turn, regenerated per session so wording drifts. This honors the owner's rule — the strings are model-authored at runtime, never compiled in — while making the ack deterministic and instant. Optionally pre-synthesize the pool-head's audio during idle (flag: it sends one TTS request slightly ahead of need — surface as a setting if the owner's privacy reading objects).
3. **Non-verbal remains the sub-second floor:** keep `tick.wav`/avatar beat at tool start and between tool rounds >2 s; retune `slowOpTimer` from 5 s → 2.5 s since it's now the *backup*, not the show.

**Tool-loop feedback (Q3):** each round's lead-in streams and speaks (round-2 lead-ins included — they're accumulated now); tool execution gaps >2 s get the non-verbal cue; no spoken filler between rounds beyond what the model naturally emits — with streaming, the longest silent gap in the weather flow drops to the tool GET itself (~0.3–1 s), which needs nothing.

## 3. Goodbye semantics (correct end-of-turn, not a patch)

When a round's executed tools include `end_conversation`: **terminate the loop locally — do not re-POST.** Speak the accumulated text (which now includes the sign-off — §1.1), then close the session when the queue drains. This removes the empty round-2 POST entirely (one less network call, no empty-`end_turn` case to special-case). Belt-and-braces: if any terminal streamed turn ends with empty content but tools succeeded this turn, treat as success with no speech — `analysisFailed` is reserved for transport/API errors. (Supersedes the quick-win patch once streaming lands.)

## 4. Startup (Q4)

At `engage()`: (a) **TLS pre-warm** to api.anthropic.com + the active TTS host (connection open only — no payload; consistent with privacy rule since the user just opened the assistant); (b) fire the Bean-Base search **fully async — turn 1 never blocks on it**: if it resolves mid-conversation, inject as a system-role context update on the next turn ("bean identified: …"); the 4 s/11 s timers become irrelevant to latency. (c) **Don't include weather/stock/news tools in the turn-1 request** (tools are per-request; the model can't volunteer what isn't offered) — include them from turn 2 or when the first utterance plainly asks; `end_conversation`, dial-change, and shot-history tools stay always-on. No persona contortions needed — absence beats instruction.

## 5. Implementation sequencing (each phase shippable, flag-gated)

| Phase | Scope | Acceptance (on the tablet) |
|---|---|---|
| **P1** | SSE streaming in `AnthropicProvider` + `SpeechChunker` + speak-per-chunk on **native** TTS; accumulate-all fix; goodbye local-termination | plain question: first audio < 2 s; goodbye: clean sign-off, zero errors |
| **P2** | `SpeechQueue` sentence-pipeline for OpenAI/ElevenLabs + TLS pre-warm | cloud voice: first audio < 2.5 s; inter-sentence gaps < 300 ms; barge-in flushes cleanly |
| **P3** | Turn-taking FSM + 400 ms cooldown + echo text-filter + restart cap 3/backoff/tap-to-talk | 20-turn session with zero self-triggers on the Samsung; no silent loops |
| **P4** | Ack guarantee: persona lead-in line + session ack-pool + retuned non-verbal (2.5 s) | 10 tool queries: audible natural ack ≤ 1.5 s in 10/10 |
| **P5** | Startup: async Bean-Base + turn-1 tool gating | cold start → greeting response < 2.5 s, no volunteered weather/news |
| **P6** *(optional)* | True streaming TTS decode (QAudioSink) if P2 gaps audible; mic-during-speech barge-in if headset/AEC detected | — |

**Risk notes for the implementer:** SSE parsing must survive partial frames + `ping` events + mid-stream `error` events (fall back to whole-body on stream failure — the old path stays as the fallback for the whole feature flag); `input_json_delta` accumulates per content-block index, not globally; the coaching-voice arbiter must now arbitrate *queues*, not single utterances (gate at `SpeechQueue` head, not `speak()`); temp-file churn (one MP3 per sentence) — reuse a small ring of temp files; QML is runtime-verified — test `onSpeakingChanged` behavior against the new queue-long `speaking` semantics on device.

**What this deletes from the symptom list:** dead air (streams overlap), the 4 s/never ack (streamed lead-in + pool), "no response" at goodbye (no empty POST), the session-start stall (async Bean-Base, gated tools), and the listen/hear loop (FSM + cooldown + echo filter + capped restarts).
