# Barista fork — living work plan

**Purpose of this file:** one place that says *what we're building, why, where it stands, and what's next*.
Update it in the same change that moves a workstream. Detailed design lives in the other `docs/barista/*.md`
docs and in the memory topic files; this is the tracker. Canonical repo: `~/Decenza-fork`, branch
`feat/barista` (see `FORK_STATE.md`).

## The objective (read this first)

**Make barista development faster and less on-device-bound.** The standing tax: the barista's voice/overlay
code is unlinted and was historically "only verifiable at the machine," so every change meant a slow
APK → sideload → poke-the-tablet loop. We attack that tax from three angles, and every workstream below
should be judged by whether it advances one of them:

1. **Move logic into headless tests** (runs in CI, no tablet).
2. **Make the on-device build/deploy loop cheap** (fast, documented build + sign).
3. **Reach the running machine remotely** (real data + live state without hand-copying).

## Current state (2026-09-09)

### Feature: voice streaming (BARISTA_VOICE_STREAMING_DESIGN.md)
The design predates the Aug two-way-comms work, so its FSM/cooldown/echo/NeedsTap/SpeakerGate parts are
already built. The genuinely-new work is the **streaming core**.
- ✅ `SpeechChunker` — pure text→speakable-chunk splitter, 16-case test. `a0f348d8`. **UNWIRED.**
- ✅ `AnthropicStreamParser` — pure SSE demux, 11-case test. `31dcafb5`. **UNWIRED.**
- ✅ `tst_baristaconversation` — the conversation **state machine now unit-tested headless** (null actuators,
  narrow lib `decenza_baristavoicelib`), pinning the won't-close and stall bugs. `23c9eaa3`. *(Lever 1 win:
  logic that was "device-only" now runs in CI.)*
- ⬜ **NEXT — needs the tablet:** wire the SSE parser into `src/ai/aiprovider.cpp` (barista-only: gate on
  `analyzeConversation`/`m_isConversationRequest` + a new `RequestOptions.streaming` flag, NEVER
  `analyze()`/`analyzeUrl()`, whole-body path stays as fallback); add `SpeechQueue` chunk-aware TTS in
  `assistantvoice.cpp` (2-deep synth lookahead, keep `speaking` true across the queue); add
  `AssistantSettings.voiceStreaming` flag (QSettings `barista/voiceStreaming`, default OFF), each flag added
  in the slice that consumes it.

### Infra: dev-loop (what makes the above cheap)
- ✅ **Fast, documented APK build+sign** — `PLATFORM_BUILD.md` [barista-fork] section (`49054242`):
  `export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home` → `./build.sh --target
  ANDROID --dev` → `zipalign -f 4` + `apksigner` **debug key, v2-only** (`--min-sdk-version 28
  --max-sdk-version 34 --v3-signing-enabled false`; the tablet rejects v3-only as "package invalid"). Cert
  must be `5c7458da…` (matches the installed app → upgrades in place, keeps data). `--dev` clock versionCode
  auto-beats the installed one. C++ caches between builds → a QML-only change repackages in minutes.
- ✅ **Remote read of the real machine works** — `REMOTE_ACCESS.md` (`c91ec712`). Enable via Settings →
  History & Data → **Share Data → Enable Server** (NOT the AI-tab MCP toggle; MCP not needed for the REST
  read). Security off → plain `curl http://<tablet-ip>:8888/api/{shots,database,settings,telemetry,
  power/status}`. Confirmed end-to-end against 1000 real shots + live DE1 config. *(Levers 2 & 3.)*

### Housekeeping (done)
- ✅ `DECENZA_BARISTA=OFF` vanilla-bisect build fixed (`f8afa7ed`).
- ✅ `origin/main` fast-forwarded to `feat/barista`.
- ✅ History & Data "Enable Server" toggle-collapse bug fixed (`44e88b55`) — the fill-height card shrank to
  zero when the People roster grew, hiding the toggle on the tablet; now sizes to content like peopleCard.

## The honest remaining gap
- ✅ **Logic** (headless tests) · ✅ **Real data** (remote read) · ✅ **Deploy loop** (fast build+sign).
- ⚠️ **Still tablet-only:** the overlay *render*, mic/STT, and TTS *audio*. Remote HTTP gives data, not
  UI/voice interaction. The lever that would shrink this: a small **QT_DEBUG dev endpoint on ShotServer that
  drives `BaristaConversation` remotely** (needs a controller pointer threaded from `main.cpp` + a mock/
  injected provider so it's deterministic). Deferred; only build it if the on-device voice iteration proves
  too slow.

## ⚠️ Design correction found 2026-09-09 (before writing any wiring) — read first
The streaming design (`BARISTA_VOICE_STREAMING_DESIGN.md` §1.1/§1.2) assumes the model's spoken reply
streams as top-level **`text_delta`** blocks, which `SpeechChunker` then splits. **That is wrong for this
fork.** Every barista turn runs with `forceRespond` (`AIManager::analyzeConversation` sets
`RequestOptions.forceRespond = clientTools`, always true for the barista → `aiprovider.cpp:1036` appends a
`respond` tool + `tool_choice:{type:"any"}`). Under Anthropic streaming (confirmed against the `claude-api`
skill docs, deterministic — no live probe needed):
  - `tool_choice:"any"` forces the model straight to a tool call, so there is **no leading `text_delta`**.
  - The spoken answer is the `respond` tool's `input.text`, which streams as **`input_json_delta`**
    fragments of that tool_use block — partial JSON concatenating to `{"text":"…"}`.
So the chunker must be fed the *incrementally-decoded value of the `text` key*, extracted from the
`respond` block's `input_json_delta` stream (un-escaping `\"`,`\n`,`\\`,`\uXXXX` that may split across
fragments) — NOT raw `text_delta`. `AnthropicStreamParser` already demuxes `InputJsonDelta` correctly;
the missing piece is a **pure streaming-JSON-string extractor** for the `respond.text` field. (A rare
non-forced turn could still emit `text_delta`; handle both, but the respond path is the norm.)

## Forward plan (priority order)
1. **Wire the voice-streaming pipeline.** Sliced per advisor (headless-first, tablet last):
   - ✅ **1a DONE (CI, no tablet) 2026-09-09 — not committed:** pure `RespondTextExtractor` (respond
     `input_json_delta` → incremental spoken text, `tst_respondtextextractor` 15 cases); pure
     `assembleAnthropicResponse(events)` in `anthropicstreamparser.{h,cpp}` (SSE events → whole-body-equivalent
     `{content, stop_reason}`, 5 byte-identity fixture tests in `tst_anthropicstreamparser` proving it feeds the
     tool loop identically to the whole-body parse); and `onAnalysisReply`'s post-`readAll()` body extracted
     verbatim into `AnthropicProvider::finalizeConversationResponse(root)` (behavior-preserving — `tst_aiproviders`
     + `tst_aimanager` still green) so the streaming path drives ONE identical tool loop. Lever-1 wins.
   - ✅ **1b BUILT 2026-09-09 (macOS app + all affected tests green; NOT yet on-device-verified — needs the
     tablet, and the overlay QML is unlinted so a green build ≠ it works):** `RequestOptions.streaming` +
     `AssistantSettings.voiceStreaming` (QSettings, default OFF); SSE in `aiprovider.cpp` (`onStreamReadyRead`
     emits early `respond`-text via RespondTextExtractor→streamTextDelta; `onStreamReply` assembles events→
     `finalizeConversationResponse`→emits `streamTextEnd` when terminal; `resetStreamState` per round; whole-body
     stays the fallback); threaded `streaming` through `AIManager::analyzeConversation` (gated `streaming &&
     !webSearch`) + `AIConversation.voiceStreaming`; `AssistantVoice` serial `SpeechQueue` (`feedStreamDelta`/
     `endStream`, SpeechChunker + FIFO, `speaking`+`streaming` held across the queue via the 3 clip-finish hooks:
     desktop EndOfMedia, native TTS Ready, Android finished; 1.2s first-chunk timer; barge-in clears in `stop()`);
     `BaristaModule` wires `AIManager::conversationStreamText/End`→voice C++-direct (bypasses QML); double-speak
     guarded on `Barista.voice.streaming` in BOTH paths (`BaristaConversation::onModelSpeakable/onModelFinal` for
     useNewConversation=ON, and `AssistantOverlay` legacy `_speakSanitised` sites). Pure files moved to the
     UNCONDITIONAL cmake block (aiprovider references them even in a DECENZA_BARISTA=OFF build); narrow
     `decenza_baristastreamlib` keeps zero test-source duplication. **NEXT = tablet verify** (enable
     voiceStreaming + turn web search OFF; check first-audio latency, no double-speak, mic stays gated across the
     queue, barge-in). Original 1b detail:
   - **1b (first device trip):** `RequestOptions.streaming` + SSE in `aiprovider.cpp` (barista-only:
     `readyRead`→`AnthropicStreamParser`; on `message_stop` `assembleAnthropicResponse(events)`→synthetic root
     →`finalizeConversationResponse(root)` [both DONE in 1a]; `parser.reset()`+re-arm `readyRead` each re-POST
     round; reset `m_accumulatedText` only at turn start). ⚠️**GATE streaming to `voiceStreaming && !webSearch`**
     — fall back to the whole-body path when web search is ON (`AssistantSettings.webSearchEnabled` defaults
     ON). Reason: a web-search turn stops with `pause_turn` and the continuation echoes `content` VERBATIM on
     re-POST, but `assembleAnthropicResponse` is deliberately LOSSY for server-tool blocks (keeps only `type`,
     drops id/name/input/results — pinned by `tst_anthropicstreamparser::assembleServerToolBlockIsLossy`), so a
     streamed web-search re-POST would be malformed. Do NOT widen streaming to web-search turns without first
     making that reconstruction faithful to server_tool_use/web_search_tool_result (the parser would need to
     carry their input/results too). The `respond`+client-tool turns that streaming DOES cover reconstruct
     faithfully (5 byte-identity fixtures green). Route deltas **C++-direct** (module wires
     provider/manager streaming signal → new `AssistantVoice::feedStreamDelta()`, bypassing QML) and guard
     the QML `_speakSanitised` calls in `AssistantOverlay.qml` with `if (!voiceStreaming)` so the full answer
     isn't ALSO spoken (double-speak). `AssistantVoice` owns a `SpeechChunker` + a **serial** queue: speak
     chunk N, dequeue N+1 on playback-finished (`EndOfMedia`/`handleAndroidPlaybackFinished`/native
     `stateChanged→Ready`) — the queue must intercept that event so `speaking` stays true across the queue
     (else the mic reopens mid-answer; tangles with `m_pendingSynth`/`m_speakGen`). `AssistantSettings.
     voiceStreaming` (QSettings `barista/voiceStreaming`, default OFF) gates the whole feature.
   - **1c (second slice):** the 2-deep synth lookahead (design §1.3) — pure cloud-gap removal; defer.
   Verify with the tablet's real `ttsProvider` (remote read); confirm flag-OFF unchanged; macOS `ctest`
   before the APK. This is the one piece needing the tablet — now cheap to iterate.
2. **Full scroll conversion of the History & Data tab** — it's the only settings tab without a ScrollView
   (every other tab scrolls); the toggle-collapse fix was a targeted band-aid. This is the proper,
   upstream-worthy fix. Big/complex file (2369 lines) — do it carefully, verify on macOS first.
3. **Optional dev lever:** the ShotServer barista dev-endpoint (above), if voice iteration is too slow.

## Key pointers
- Design: `BARISTA_VOICE_STREAMING_DESIGN.md`, `BARISTA_TwoWay_Comms_Redesign.md`.
- Build/test on this Mac: `cmake`/`ctest` in `build/Qt_6_11_2_for_macOS_Debug` (NOT the Qt Creator MCP —
  see `FORK_STATE.md`); Android APK via `build.sh` per `PLATFORM_BUILD.md`.
- Remote access how-to: `REMOTE_ACCESS.md`. Tablet was `192.168.189.186:8888` (IP can DHCP-rotate).
