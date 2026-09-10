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

## ⏸️ Voice streaming — PARKED 2026-09-09 (on-device reality killed the value)
Built end-to-end (1a+1b, committed+pushed, flag `voiceStreaming` **default OFF**), but the owner's on-device
test made it moot: streaming is **Anthropic-only**, the owner runs **Gemini** (Anthropic Sonnet is ~10s to
first audio — model latency streaming can't fix; Gemini ~2s whole-reply, so they're staying on Gemini). Left
**parked** (flag-off, dormant) per owner. ⚠️ It also has **3 known bugs from code review** (NOT fixed — not
worth polishing an unused path): C1 mic reopens between chunks on native/Android (`updateSpeaking` runs before
`onSpeechClipFinished`), C2 desktop cloud path exposed to same, I1 a no-finish-signal `speak()` wedges the
queue. If anyone ever puts the barista on Anthropic, fix those first (see memory `decenza-barista-voice-streaming`).
1c (2-deep lookahead) DROPPED as moot. **Real issue surfaced instead → see the new forward item 1.**

## Forward plan (priority order)
> **Order set by the owner 2026-09-10:** do **Proactive coaching Increment 2** first, then the **Turn-cost
> architecture**. The History & Data ScrollView conversion stays open but deferred behind those.

1. ✅ **Proactive coaching — Increment 2 (similar-bean / community "opening read") — BUILT + committed (audit
   2026-09-10).** Both increments were already on the branch: Increment 1 (cross-bean `palateProfile` + OPENING
   READ clause) and Increment 2 (`buildSimilarBeanBlock` → `similarBeanExperience` + the SIMILAR BEANS &
   COMMUNITY persona clause, `4e1768bb`; the brand-new-bean case 2b, `338130d9`). Implementation matches
   `docs/barista/PROACTIVE_COACHING.md`; `tst_aimanager`/`tst_aiproviders`/`tst_closeintent` green. The earlier
   "not started" note (here + memory) was stale. **What's left is owner on-device *listening*, not code** — the
   Increment-1 behaviour cases in `PROACTIVE_COACHING.md` cover it. Memory: `decenza-barista-proactive-coaching`.
2. **Turn-cost architecture** — `docs/barista/Barista_Turn_Cost_Architecture_DESIGN.md` (see its STATUS block).
   A code audit (2026-09-10) found most of the 8-step design was ALREADY built (module-gating / Step 0,
   Anthropic caching / Step 7, rolling summary / Step 6, the math short-circuit / Step 5 all live). Work done
   this session:
   - ✅ **Slice 1 (Step 1) — SHIPPED** (`87aae3b1`): relocated the coaching-framework prose (~900 tokens) out
     of the always-resident core into a gated `_mods.coaching` that rides `includeDialin`, cutting ~900 tokens
     off every casual/greeting turn (a straight win on Gemini). Verbatim move → no voice change on coaching
     turns → no ear-test needed. Build clean, AI tests green.
   - ⏸️ **Slice 2 (Step 4, per-turn tool filtering) — DEFERRED by owner.** Safely buildable only via sticky
     `active.*` gating (a current-utterance filter would drop `apply_dial_change`/`log_tasting_feedback` on an
     approval turn — silent no-op), realistic win modest (~500 tok, decaying), live change needing on-device
     validation. Owner chose to skip for now.
   - ✅ **Slice 3 (Step 5, deterministic math) — was already built + tested** (`tryQuickMath`).
   Remaining: finish Step 1 trimming further if warranted; Slice 2 available if the token cut is wanted later.
3. ✅ **Full ScrollView conversion of the History & Data settings tab — DONE (2026-09-10), owner visual owed.**
   Wrapped the top-level `RowLayout` in a `Flickable` (`contentFlickable`, `contentHeight:
   mainLayout.implicitHeight`, `VerticalFlick` + `StopAtBounds` + `ScrollBar.vertical`), mirroring
   `SettingsMachineTab`'s Flickable-over-RowLayout pattern — the closest well-structured sibling. The three
   columns went from `Layout.fillHeight: true` to content-sized + `Layout.alignment: Qt.AlignTop` (the two card
   Rectangles get `implicitHeight: <innerColumn>.implicitHeight + margins`, the shipping MachineTab idiom; the
   right ColumnLayout is intrinsically content-sized). Key structural finding: the RowLayout does NOT close near
   the 3 columns — every dialog/Connections/Timer is declared *inside* it and it closes at EOF (line ~2385); all
   those are Popups/non-visual/`parent: Overlay.overlay`-reparented, so the layout only ever manages the 3
   columns, which is why the wrap needed no 2,300-line re-indent. The `44e88b55` band-aid comment on the Enable
   Server card (fill-height-collapse rationale, now structurally impossible) was rewritten. Single-file diff
   (+29/−9). **Verified:** clean build, QML diagnostics gate clean 251/251, `ctest` 124/126 (the 2 reds —
   `failonwarning_lint`, `tst_qmlregistration/Barista` — are pre-existing C++ barista-fork failures, not this
   change), app launches with zero binding-loop / QML warnings. **Owed:** owner eyeballs the live scroll on
   macOS (the tab loads lazily via a Loader; couldn't drive the Qt UI to that exact sub-tab headlessly). Not
   committed yet.
4. **ElevenLabs "trips up / gets quieter"** (owner's daily Gemini + ElevenLabs path). NOT a Bluetooth/speaker
   issue (owner confirmed no BT speaker) — it's the **turbo model's synthesis stutter + loudness instability**.
   FIRST fix is zero-code: owner switches the ElevenLabs model in barista settings → **Voice** tab from
   **"Turbo — fastest, more stutter"** to **"Multilingual v2 — steadier, slower."** If that's not enough, small
   code tweak in `synthElevenLabs`: raise `stability` 0.5→~0.65 + a loudness-consistency setting, build into an
   APK. *Owner is testing the model switch.*

## ⏸️ Parked / done (not active)
- **Voice streaming** — PARKED, see the "Voice streaming — PARKED" section above (moot for Gemini; 3 known
  code-review bugs unfixed; flag `voiceStreaming` default OFF; nothing to do unless the barista goes Anthropic).
- **Optional dev lever** — a QT_DEBUG ShotServer endpoint driving `BaristaConversation` remotely (see "honest
  remaining gap" above); only if on-device voice iteration proves too slow. Not needed now.

## Key pointers
- Design: `BARISTA_VOICE_STREAMING_DESIGN.md`, `BARISTA_TwoWay_Comms_Redesign.md`.
- Build/test on this Mac: `cmake`/`ctest` in `build/Qt_6_11_2_for_macOS_Debug` (NOT the Qt Creator MCP —
  see `FORK_STATE.md`); Android APK via `build.sh` per `PLATFORM_BUILD.md`.
- Remote access how-to: `REMOTE_ACCESS.md`. Tablet was `192.168.189.186:8888` (IP can DHCP-rotate).
