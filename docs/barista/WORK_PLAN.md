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
   change), app launches with zero binding-loop / QML warnings. Committed `24bedeb4` (feat/barista + main FF'd).
   **✅ Owner-confirmed on-device 2026-09-11:** installed to the tablet (SM-X200) via adb wireless and the
   History & Data tab scrolls cleanly. Item fully closed.
4. **ElevenLabs "trips up / gets quieter"** (owner's daily Gemini + ElevenLabs path). NOT a Bluetooth/speaker
   issue (owner confirmed no BT speaker) — it's the **turbo model's synthesis stutter + loudness instability**.
   FIRST fix is zero-code: owner switches the ElevenLabs model in barista settings → **Voice** tab from
   **"Turbo — fastest, more stutter"** to **"Multilingual v2 — steadier, slower."** If that's not enough, small
   code tweak in `synthElevenLabs`: raise `stability` 0.5→~0.65 + a loudness-consistency setting, build into an
   APK. *Owner is testing the model switch.*

## ▶ Coaching — next directions (owner-chosen 2026-09-11) + remote voice DEFERRED
Owner wants to advance coaching on **two** fronts, and **defer remote voice dev** until the two-way-comms
AI-dispatch wiring lands (the barista conversation state machine's `turnRequested()` output is currently
**unwired** — `baristamodule.cpp:222` "the rest of the wiring is the next increment"; coaching itself runs
through `AIManager` directly, so it does NOT depend on that). An automated scoping pass **over-estimated
readiness** (claimed a `plans` table + a ready trace-signature detector — **neither exists**; verified), so
the two chosen directions were re-scoped against real code and handed to Fable:
1. **Plan-outcome learning loop** — greenfield: new `assistant.db` table recording each recommendation + the
   following shot's outcome, matched/confidence-gated, fed back into coaching. Seam: `requestBaristaContext`
   block pattern + `feedbackstorage`/`tasksstorage`.
2. **Trace→KB grounding bridge** — enrich `objectiveTraceShape` (`baristatools.cpp:67`, today only
   channeling/grind→coarse 4-value axis) and decide how far to assert the KB's 18 descriptive-only
   `trace_signatures` (most undetectable today) vs. hand to the model — so the opening read leads with a cited
   profile move, never a fabricated signature.
The shared hard problem is the **grounding / false-attribution policy** (§3 of the request). **Fable 5 design
request drafted:** `docs/barista/Barista_Coaching_Loop_And_Trace_Fable5_Request.md`.

**✅ Fable ruling delivered + owner-approved 2026-09-11** → `docs/barista/Barista_Coaching_Loop_And_Trace_Fable5_DESIGN.md`.
Key finding **W1**: upgrade 1's premise was wrong — a per-session closed loop ALREADY exists (issue #1053:
`structuredNext` predictions + `buildRecentAdviceBlock`/`computeAdherence`/`computeOutcomeInPredictedRange`),
so the ledger is a *persistence+aggregation layer over the existing grader*, not a new matcher. Also W2 (no
assistant.db migration runner — idempotent `ensureSchemaStatic` instead), W3 (no temp detector — `temp_flat`
is model-read). Migration path = T1–T4 (trace) + P1–P5 (ledger); **trace lands first**. Owner chose **follow
Fable's order**.

**Build progress:**
- **✅ T1 BUILT + ctest-green 2026-09-11 (NOT committed).** New pure unit `src/barista/baristatrace.{h,cpp}`:
  `BaristaTrace::objectiveTraceSignatures(ShotProjection)` maps detector verdicts → KB `trace_signatures`
  with a `{signatureId, grounding("measured"|"inferred"), evidence}` contract. Emits 5 MEASURED
  (chokedPuck→choke, yieldOvershoot→gusher, channeling sustained→flow_exceeds_pressure,
  transient→pressure_notch_heal, tooFine+flow-falling→flow_stall_after_pi) + 1 INFERRED
  (tooFine-alone→trace_flow_never_caps). **Per advisor: the two threshold-dependent inferred signatures
  (`early_first_drops`, `trace_fast_pressure_bleed`) are DEFERRED** — firing them off the near-universal
  `preinfusionObserved` boolean would assert a fault on normal shots; needs a rate threshold validated vs the
  shot_eval corpus. `trace_pressure_spike_ramp` gated to T4 (new detector). Added
  `CoffeeKnowledgeBase::traceSignature(id)` accessor (activates the previously-dead `m_traceSignatures`).
  Registered unconditionally in `cmake/barista.cmake`; 7 new slots in `tst_coffeeknowledgebase.cpp` (19 pass/0
  fail) incl. the build-time contract that every emittable id resolves in the shipped KB.
- **✅ T2 BUILT + ctest-green 2026-09-11 (NOT committed).** (a) `BaristaTrace::buildLastShotTraceRead(shot, kb)`
  added to `baristatrace.{h,cpp}` — joins bridge hits to KB meaning/next_change/1 citation, splits
  `measured`(assert)/`inferred`(maybe), caps 2 entries, carries the grounding `note`; empty on a clean shot.
  Inserted as `lastShotTraceRead` at the `requestBaristaContext` call site (`aimanager.cpp`, right after the
  barista-only `descriptor`). (b) `machine_data.traceSignatures` (flat `{signatureId,grounding,evidence}` array)
  added to the `recommend_next_shot` executor (`baristatools.cpp`). Both consumers compile+link+green via
  `tst_aimanager`; 3 new builder slots in `tst_coffeeknowledgebase` (now 22 pass/0 fail). `baristatrace.cpp`
  added to both test targets. `ctx.trace` (4-axis) untouched → diagnostics matching unchanged.
- **✅ T3 BUILT 2026-09-11 (NOT committed) — needs on-device listening.** TRACE-GROUNDED OPENING persona clause
  in `qml/assistant/AssistantOverlay.qml` (mayNudge branch, after OPENING READ): lead with a MEASURED curve
  fault + cited nextChange, hedge INFERRED as a maybe, prep-class → prep first, REPLACES the palate suggestion
  for that turn (still one proactive thing). Prose behavior is on-device-only (not headless-testable).
- **✅ P1 BUILT + ctest-green 2026-09-11 (NOT committed).** Ledger foundation, per Fable §5 order (P1
  next, not T4 — the ledger's payoff decays with delay because its aggregates need weeks of accumulated
  shots, so the data clock starts ASAP; T4 stays optional/last). Two parts:
  - **W5 hoist (behavior-neutral):** `computeAdherence`/`computeOutcomeInPredictedRange`/
    `synthesizeRecommendationSummary` lifted out of `dialing_blocks.cpp`'s anon namespace into
    `DialingBlocks::` scope + declared in `dialing_blocks.h` (definitions stay in the .cpp beside their
    file-local helpers; `inRange` made `static`). `tst_dialing_blocks` + `tst_aimanager` green, zero test
    changes → confirmed neutral. The judge pass (P3) will reuse these, not re-implement the #1053 grader.
  - **`CoachPlanStorage`** (`src/barista/coachplanstorage.{h,cpp}`) — third co-resident assistant.db store,
    mirrors FeedbackStorage/TasksStorage: idempotent `ensureSchemaStatic` (coach_plans schema §1.2 incl.
    frozen outcome-half cols; NEVER touches the shared `schema_version` marker — W2), async `requestLogPlan`
    via SerialDbWorker, sync statics (`insertPlanStatic`, `supersedeOpenPlansStatic`, `fetchOpenPlansStatic`,
    `writeJudgmentStatic` [immutable — guards double-judge], `fetchPlansStatic`) shared by all readers (one
    query path). Pure `deriveLeverDirection(structuredNext, anchor)` (grind/dose/profile/repeat/multi +
    finer/coarser/up/down/switch/unclear) via GrinderAliases; prose→grind/unclear, ≥2 levers→multi,
    ranges-only→repeat. Registered UNCONDITIONALLY in `cmake/barista.cmake`; `[fork-index] seam=CoachPlanStorage`
    (INDEX.tsv regenerated, --check clean). New `tests/tst_coachplanstorage.cpp` (18 slots: derivation table +
    schema-omits-schema_version + insert/supersede/immutable-judge/filter). **Full Decenza app compiles+links
    with the new TU.** NOT wired into aimanager/baristamodule yet — that is the first step of P2 (capture),
    where the store goes live; P1 ships silently (nothing reads/writes it).
- **✅ P2 (capture) BUILT + app-links + ctest-green 2026-09-11 (NOT committed). The data clock is running.**
  - **P2a ownership:** `baristamodule` creates `CoachPlanStorage(this)`, `initialize(assistantDb)` (same file as
    feedback/tasks), `ai->setCoachPlanStorage(...)`; `AIManager` holds `m_coachPlanStorage` + setter/getter.
  - **P2c KB lineage:** the `recommend_next_shot` branch of the client-tool executor (`aimanager.cpp`) wraps
    `done` to sniff `{recommendation.id, confidence, prep_gate}` into `m_pendingKbRecommendation` WITHOUT altering
    the model payload; cleared per-turn at the `analyzeConversation` choke point alongside `m_pendingToolStructuredNext`.
  - **P2b capture:** anchor snapshot (`aimanager.cpp` requestBaristaContext worker) extended with
    `profileKbId`/`equipmentId`/`rpm` from the SAME matched shot (advisor fix: all 3 scope keys describe ONE shot,
    else P3's judge query reads one shot's timestamp and filters by another's profile). New
    `AIManager::recordCoachPlan(sn, source)` builds the field-map from the app-side snapshot + `takePendingKbRecommendation()`
    + `deriveLeverDirection` (anchor ShotProjection rebuilt from snapshot dial), calls `requestLogPlan`. Triggered from
    `AIConversation::onAnalysisComplete` gated on `m_toolsEnabled` (barista-turn marker; advisor `ask()` clears it →
    fails closed) && `structuredNext.has_value()`; `source`=fenced|tool_applied. **Skips when the anchor has no
    `profileKbId`** (unscopable → never judgeable). `tst_aimanager` now links `coachplanstorage.cpp` (green).
  - **Honest status: P2 is BUILT, capture UNOBSERVED.** compile+link+`tst_aimanager`-green prove P2 doesn't
    *regress*; NO test writes a real row (recordCoachPlan isn't exercised end-to-end). The field-map↔`kWritableCols`
    key contract was hand-verified (all 12 keys match). Storage/derivation/supersede covered by `tst_coachplanstorage`.
  - **Instrumented for on-device verify (advisor):** `recordCoachPlan` logs to the barista-diagnostics `coach`
    channel — `plan_write` (source/lever/direction/anchorShotId/kbRecId) on success, `plan_skip` with reason
    (`no_storage`/`empty_prediction`/`no_profile_scope`) on each early-return. So "table empty after a dial-in"
    is one glance (pull `/sdcard/Documents/Decenza/logs`), not a guess among the 3 silent causes (toolsEnabled
    false / no structuredNext / no profile scope).
- **✅ P4-0 RESOLVED — the per-prime anchor is CORRECT by design; no anchor code needed (advisor reconciled).**
  Both the anchor snapshot AND the #1053 shotId latch are per-prime: `_stampTurn()` (overlay:1639, per-turn) reads
  `lastBaristaAnchorId()` = `m_lastBaristaAnchorId`, set in the SAME per-prime `requestBaristaContext` worker
  (aimanager.cpp:2047) as the snapshot — so for the voice-barista path there is NO independent per-turn shot source
  (the earlier "latch-vs-snapshot" fix was a false alarm; the per-turn latch exists only on the report/advisor path,
  ConversationOverlay:698). This MATCHES the #1053 engine the DoR requires mirroring (per-turn would create the
  divergence W1 exists to prevent). And it's not corruption: the dial-in block is **prime-built** (`_mods.dialin`
  assigned at context assembly, only gated per-turn by `_scopedSystemPrompt`), so mid-session the model never sees a
  newly-pulled shot — every turn's advice reasons about the prime shot, so anchoring every plan in a session to that
  shot anchors to *the shot the advice was based on* (consistent), and superseding the older same-scope plan is the
  DoR's "multiple open recs" rule. The only gap — two Tier-A confirmations from a multi-shot-within-one-LIVE-session
  (before the 20s auto-close), twice, same (bean,lever,direction) — is negligible and shared with #1053. Documented,
  not defended. **Invariant P4 still honors: judge-runs-before-record — satisfied by judge-at-prime** (prime N+1
  judges plan N before session N+1 writes) + judge-at-recall.
- **✅ P3 (judge pass + tier classifier) BUILT + app-links + ctest-green 2026-09-11 (NOT committed).**
  - **Judge pass** = new `src/barista/coachplanjudge.{h,cpp}` `runJudgePassStatic(assistantDb, shotsDb, nowSecs)`
    (own TU so its DialingBlocks+ShotHistoryStorage deps don't fan out onto lean `tst_coachplanstorage`; registered
    in `cmake/barista.cmake`). For each open plan: load anchor shot, find first follow-up on same profileKbId +
    `AdviceScope` equipment postdating it (buildRecentAdviceBlock matcher shape), freeze `computeAdherence`/
    `computeOutcomeInPredictedRange` via `writeJudgmentStatic`. Idempotent (open-only + status guard); `nowSecs`
    injected. Anchor-deleted / no-follow-up → left open, retried.
  - **Tier classifier** = pure `CoachPlanStorage::classifyTier(judgedPlan, anchor, followUp)` → {tier A/B/C,
    deltaSign}. Tier A = followed + single directional lever (grind/dose/profile) + same bean + ≤14d + follow-up
    not channeling + BOTH rated (delta improved/worse/flat, flat=|Δ|≤5); B = followed but multi/unrated; C =
    not-followed/channeled/bean-drift/>14d. Applied at read/aggregation (P4), not in the judge pass.
    **Intentional DoR tightening:** the DoR's literal Tier-A gate is `lever ∉ {multi,''}` / `direction ≠ 'unclear'`;
    the code instead allowlists `lever ∈ {grind,dose,profile}` + non-empty direction, so `repeat`/empty-direction
    fall to hedged Tier B rather than asserting a degenerate "repeat → improved" pattern. Safer; errs toward B.
  - Tests: `classifyTier` (pure A/B/C + delta) in `tst_coachplanstorage` (no new deps); 3 two-DB judge-integration
    cases (freeze-followed+idempotent, no-follow-up-stays-open, wrong-profile-ignored) in `tst_dialing_blocks`
    (already links the graders + shot loader + ShotRowFixtures). All green.
- **✅ P4 (read/aggregation + judge trigger) BUILT + app-links + ctest-green 2026-09-11 (NOT committed).**
  - **Read-side engine** added to `coachplanjudge.{h,cpp}` (co-located with the judge pass; shares its deps):
    `buildTrackRecord(assistantDb, shotsDb, bean…, nowSecs)` → the `coachTrackRecord` JSON, and
    `recallOutcomes(…, lever, limit, …)` → per-event rows. Both **run the judge pass FIRST** (judge-before-read;
    and judge-before-record for the session since prime precedes any turn — the P4-0 invariant), then classify each
    judged plan via `CoachPlanStorage::classifyTier` over LIVE anchor/follow-up shots (one shared `classifyRows`).
  - **P4a `coachTrackRecord` block:** built in the `requestBaristaContext` worker via a nested `withTempDb`
    (assistant.db + shots.db, both paths already in the worker), inserted into `obj` beside
    `recentTastingFeedbackOnThisBean`. Confirmed patterns = ≥2 Tier-A consistent-sign (no counter-example) per
    (lever,direction); single Tier-A → `confirmed:false` events; cap 3; + `lastPlan` (most recent judged) + the
    grounding `note`. **Absent below the §1.5 floor** (no Tier-A data AND last plan not `followed` — Tier-C-only
    history is the recall tool's job). `tst_aimanager` links `coachplanjudge.cpp`.
    **⚠️ `confirmed`/`effect` contract fix (advisor):** `confirmed:true` conflated improved/worse/flat and the
    DoR `note` said confirmed = "worked" — so a reliably-BAD move (finer→worse ×2) would ship as
    `confirmed:true` + "finer's worked here". FIXED: each pattern line now carries a machine-readable
    `effect` (improved|worse|flat), and the `note` was rewritten so confirmed = "the same result REPEATED — read
    `effect`", with a confirmed `worse` = "steer AWAY". **The `note` is owner-approved DoR copy — flag for owner
    review** (this was a correctness fix, not a style change). Test `trackRecord_confirmedWorseCarriesEffect…`
    pins it.
  - **P4b `recall_coaching_outcomes` tool** (`baristatools.cpp`, beside `search_tasting_feedback`): off-main, two
    nested `withTempDb` → `CoachPlanJudge::recallOutcomes`; per-event rows INCLUDING Tier-C ("suggested, not
    tried"). Optional bean/lever/limit; `[fork-index] tool=recall_coaching_outcomes` (INDEX.tsv 56 rows, regen).
    Barista client tool (not MCP) → not subject to the 80-tool MCP budget.
  - Tests: P4 block/floor/absence + recall-Tier-C, end-to-end THROUGH the judge pass (open plans + shots →
    judge → classify → aggregate) in `tst_dialing_blocks`; classifyTier pure cases in `tst_coachplanstorage`. Green.
- **✅ P5 (persona) BUILT 2026-09-11 (NOT committed), app relinks clean.** TRACK RECORD clause in
  `AssistantOverlay.qml` (after SIMILAR BEANS, inside the proactive branch; extends OPENING READ, no new slot):
  read each line's **`effect` not `confirmed`** — confirmed `improved`=lean in, confirmed `worse`=steer AWAY/pick
  another lever, `flat`=no move; don't re-push advice the record shows didn't help; cite as shared HISTORY in
  plain words, **never read the ratings aloud** (folds in L1/L2 ground-not-recite), never as law; single/
  `confirmed:false`=specific history not a pattern; `recall_coaching_outcomes` for go-deeper. On-device-listening-
  gated (prose; not headless-testable) — like T3.
- **🎉 LEDGER PROGRAM P1–P5 COMPLETE (engine) + trace T1–T3 + verbosity L1/L2 — all BUILT, app-links, ctest-green,
  NOT committed.** Remaining engineering: **T4** (optional pressure-spike detector) and **L3** (payload shrink,
  owner greenlight). Owner owes ON-DEVICE: listen to T3 + L1/L2 + P5 **together** (the throughline test — does
  Gemini recite P4's rating-pairs or ground them?), inspect a real `coach_plans` row (P2 capture unobserved),
  review the reworded `coachTrackRecord` `note` (owner DoR copy). **Full suite (not just touched targets) is the
  commit gate for the whole stack.**
- **⚠️ Throughline collision (advisor) — verify L1/L2 + P4 TOGETHER on-device, not separately.** P4 injects
  rating-pairs (`62→78, 71→80`) into the always-on `coachTrackRecord` block on the SAME Gemini that's ignoring the
  L1/L2 "don't recite numbers" rules (the owner's "sea of numbers" complaint). The block's `note` says "grounds
  your suggestion, not a report to recite" — but that's exactly the instruction-following Gemini drops. So the real
  test of "ground, not recite" is P4's numbers under L1/L2's discipline in one on-device pass. If Gemini recites
  the track-record numbers, that's the strongest evidence yet for the L3 payload-shrink and/or the Claude-vs-Gemini
  question.

## ▶ Barista VERBOSITY (owner flag 2026-09-11) — "lost in a sea of numbers"
Owner: the coaching discussion *surrounding* the suggestion is overly verbose (stat after stat), not the count
of suggestions. Diagnosed from this morning's on-device logs (`/sdcard/Documents/Decenza/logs`): a 23s reply to
an open "let's talk about yesterday's shot" and a **44s** two-day spec-sheet reply to "clarify exactly what the
settings were". **Barista runs on GEMINI** (no Anthropic key on this config; confirmed via `Gemini usage` log
lines) — which follows the persona's already-strong brevity rules ("1–2 short sentences"; "NEVER recite a spec
sheet") far less reliably than Claude. Prompt is ~27–32k tok. Owner chose 3 levers (NOT switch-to-Claude):
- **✅ L1 (number-density) + L2 (bound 'when asked') BUILT 2026-09-11 (NOT committed), app relinks clean.**
  Persona edits in `qml/assistant/AssistantOverlay.qml`. **⚠️ The discipline is on NUMBERS, NOT on depth —
  owner explicitly: "don't kill the robustness of the discussion we just built."** So the VOICE rule now says
  the limit is on figures, not substance: grounded reasoning (trace read, WHY behind a lever, how past advice
  panned out) is expressly WELCOME and to be given in plain words — what to cut is figures stacked up. (a) even
  asked for "the settings", lead with the two or three that matter + OFFER the rest, don't read a spec sheet or
  two shots' figures in a row; (b) bounded the "quote a figure when asked" exception — one figure at a time,
  keep the descriptive SHAPE; (c) new UNITS rule — one unit per value, never °C+°F duals. (An earlier draft had
  a hard ~2-sentence cap "even for factual Qs / never a monologue" — RETARGETED after owner flag, since that
  would have gutted the coaching substance.) **On-device listening owed (prose; not headless-testable).**
- **⏳ L3 (shrink prompt) — MEASURED, NOT cut.** The ~27–32k = a **~20k dial-in DATA block** that rides every
  non-casual turn (`AssistantOverlay.qml:1740`; already dropped on clearly-casual turns via `_scopedSystemPrompt`)
  + persona. That block is BOTH the size problem and the recitation source (L1+L3 converge on it), but it's the
  advisor-grade coaching context, built by the SHARED `buildAdvisorContextBlocks` (cuts hit MCP/advisor too) and
  on-device-only validated — so NOT blind-cut. Plan (owner greenlight on depth): reshape per-shot scalar rows →
  descriptors/shapes, trim history depth, drop rarely-read blocks. Do AFTER owner listens to L1/L2.
- **Throughline flag:** the coaching build keeps ADDING numbers to the payload (T2 evidence strings; P4
  `coachTrackRecord` `62→78`). "Ground, not recite" is the DoR principle but the live Gemini model recites what
  it's handed — so whether grounding data is spoken plainly is the open on-device question over the whole strategy.

## ▶ Post-deploy findings (2026-09-11, on vc3522387)
- **✅ DEPLOYED vc3522387 to tablet + DE1 reconnected clean** (force-stop → owner DE1 power-cycle → `install -r`
  → relaunch; DE1 found CA:ED:25:A8:4A:69, 18 chars registered). adb reconnect needed the Wireless-Debugging
  port from the owner (39835 shown; actual transport via mDNS auto-discovery once the tablet screen woke).
- **⏳ P2 capture STILL UNOBSERVED** — owner did a turn and it "seems correct," but I have NOT yet pulled the
  `coach` diagnostics to confirm a `plan_write`/`plan_skip`. Do this next: pull `barista-diagnostics.log`, grep `[coach]`.
- **🗣️ OWNER DECISION — advice-tracker scope = "BOTH, LABELED DIFFERENTLY"** (supersedes DoR §3.2 exact-bean-only).
  Keep EXACT-bean confirmed patterns stated strongly ("finer's worked on THIS bean twice") AND add SAME-TYPE
  (roast/origin/process, like similarBeanExperience grouping) patterns surfaced HEDGED as transfer ("finer's
  tended to help your washed Ethiopians"). Enhancement to `buildTrackRecord`/`classifyTier` + persona: a second
  aggregation keyed on bean-type with its own `scope:"same-type"` marker + hedged persona wording. NOT built yet.
- **🐢 VOICE LATENCY (owner-flagged): ~1–5s gap between the text reply appearing and speech starting, scaling with
  reply length** (measured `speak_start`→`native_playback_started`: 41ch→1.1s, 115ch→1.7s, 261ch→3.4s, 402ch→5.3s).
  Root cause = **ElevenLabs is non-streaming: the WHOLE reply is synthesized before ANY audio plays**, so
  time-to-first-word grows with length. Compounded by **no quick-filler on Gemini** (the "give me a sec" Haiku
  pre-ack needs an Anthropic key → no-ops on this config), so nothing covers the gap but the thinking pulse. NOT a
  regression from P1–P5 (that's LLM/text latency, which is fast); and the **L1/L2 verbosity fix HELPS** (shorter
  replies synthesize faster). Structural fix = **chunked/streaming TTS** (speak sentence 1 while the rest
  synthesizes) → ~1s time-to-first-word regardless of length.
  **✅ CHUNKED-TTS FIX BUILT 2026-09-11 (owner approved "give it a shot, reversible"; desktop-compiles, Android
  building).** New `AssistantVoice::speakChunked(rawText)` runs the COMPLETE reply through the EXISTING
  SpeechChunker + serial speech queue (`m_chunker.feed(whole)` → `enqueueChunks` → serial `speakNextChunk` →
  `endStream` flush/drain) — first sentence synths+plays now, rest queue. NOTE this is NOT the parked SSE
  voice-STREAMING path (that needs Anthropic deltas, moot on Gemini); it reuses only the chunk+queue core on a
  complete reply, so it's provider-agnostic. Gated behind new `AssistantSettings.speakInChunks` **default ON**;
  wired at `AssistantOverlay.qml:426` (`speakChunked` when on, plain `speak()` when off = the reverse switch,
  persisted key `barista/speakInChunks`). On-device-verifiable only. Reverse = toggle the setting off (or flip the
  default + redeploy). Verbosity L1/L2 still compounds the win (shorter replies = fewer/faster chunks).

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
