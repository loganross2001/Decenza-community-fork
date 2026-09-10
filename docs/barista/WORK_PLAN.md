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

## Forward plan (priority order)
1. **Wire the voice-streaming pipeline** (SSE parser + `SpeechQueue` + `voiceStreaming` flag). The one piece
   needing the tablet — now cheap to iterate thanks to the build + remote work. This is the product payoff.
2. **Full scroll conversion of the History & Data tab** — it's the only settings tab without a ScrollView
   (every other tab scrolls); the toggle-collapse fix was a targeted band-aid. This is the proper,
   upstream-worthy fix. Big/complex file (2369 lines) — do it carefully, verify on macOS first.
3. **Optional dev lever:** the ShotServer barista dev-endpoint (above), if voice iteration is too slow.

## Key pointers
- Design: `BARISTA_VOICE_STREAMING_DESIGN.md`, `BARISTA_TwoWay_Comms_Redesign.md`.
- Build/test on this Mac: `cmake`/`ctest` in `build/Qt_6_11_2_for_macOS_Debug` (NOT the Qt Creator MCP —
  see `FORK_STATE.md`); Android APK via `build.sh` per `PLATFORM_BUILD.md`.
- Remote access how-to: `REMOTE_ACCESS.md`. Tablet was `192.168.189.186:8888` (IP can DHCP-rotate).
