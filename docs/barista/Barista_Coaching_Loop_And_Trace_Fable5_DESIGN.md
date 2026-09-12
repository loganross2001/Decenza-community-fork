# Barista Coaching — Closing the Loop & Grounding the Trace
## Fable 5 Design-of-Record ruling

Verified against `feat/barista` @ `b1cbda7b` (`~/Decenza-fork`), 2026-09-11. This document is the deliverable for `docs/barista/Barista_Coaching_Loop_And_Trace_Fable5_Request.md` §7.

---

## 0. Seam verification — what the tree actually says

I opened every file the request's §6 names. Most claims check out; **three are materially wrong, and one of them changes the architecture of upgrade 1.** Design below is grounded in what is actually there.

### Confirmed as claimed

| Claim | Verified |
|---|---|
| `objectiveTraceShape()` at `src/barista/baristatools.cpp:67` | ✔ Exact mapping confirmed: `channelingDetected` OR sustained severity → `"channeling"`; grind direction `tooCoarse`/`yieldOvershoot` → `"fast"`; `tooFine`/`chokedPuck` → `"slow"`; `onTarget` → `"smooth"`; else `""` (explicit "don't assert a shape" comment at :80) |
| Consumed at `baristatools.cpp:1490` (`ctx["trace"]`) and `:1514` (`machine_data.objectiveTrace`) | ✔ Inside the `recommend_next_shot` executor, which also returns `machine_data` (dose/yield/ratio/duration/grind/channeling/grindDirection) and `history` (dialInSessions + beanBestShot, equipment-scoped) |
| `AIManager::requestBaristaContext` at `src/ai/aimanager.cpp:1613`; block insertion `palateProfile` :2072, `similarBeanExperience` :2078 | ✔ Builders at :156 (`buildProactiveRecBlock`), :235 (`buildPalateProfileBlock`, cold-start floor `kPalateMinRatedShots=6`, plurality ≥2), :346 (`buildSimilarBeanBlock`). All in an **anonymous namespace** — and none is directly unit-tested today (`tst_aimanager.cpp` covers vision plumbing and extraction routing, not these builders). A new builder must be made reachable by a test, not copy this precedent. |
| `trace_signatures`: 18 entries, descriptive-only | ✔ 18 confirmed. Shape: `id` / `signature` / `meaning` / `class` / `next_change` / `provenance[]` (source_id, tier, confidence, contested, note). **No numeric criteria.** One correction: classes are **four**, not two — `prep` (4), `profile` (10), `grind` (3: `choke`, `gusher`, `flow_stall_after_pi`), `hardware` (1: `erratic_oscillation`) — and the `grind` class matters below, because those are exactly the detectable ones. |
| `trace_signatures` unused by code | ✔ Loaded into `m_traceSignatures` (`coffeeknowledgebase.cpp:164`) and **never read anywhere**. Dead data today. |
| KB tests at `tests/tst_coffeeknowledgebase.cpp` | ✔ Loads the shipped JSON via `DECENZA_SOURCE_DIR`, asserts prep-gate rails, roast-conditioning, citations. The right home for trace-mapping tests. |
| Persona clauses in `qml/assistant/AssistantOverlay.qml` | ✔ OPENING READ :1126, SIMILAR BEANS & COMMUNITY :1143, one-proactive-thing budget enforced at :1165–1178 (due items > recipe offer > doc change) |
| Coaching runs through `AIManager`, not the voice FSM | ✔ Client tools registered per-provider in `createProviders()` (`aimanager.cpp:513–557`), provider-agnostic (Anthropic + Gemini both get the same executor) |
| Build dir `build/Qt_6_11_2_for_macOS_Debug` | ✔ exists (note CLAUDE.md: an assistant runs tests through the Qt Creator MCP, not raw ctest) |
| `.fork/INDEX.tsv` grep | ✔ 55 rows; no plan/outcome store; relevant reusables found: `apply_dial_change`, `log_tasting_feedback`, `search_tasting_feedback`, `compare_shots`, `detect_grind_drift`, `get_bean_profile` |

### Wrong or materially incomplete in the request

**W1 — "Coaching never closes the loop" is substantially false, and this reshapes upgrade 1.** The tree already contains a working per-session closed loop the request never mentions (issue #1053):

- Every advisor/barista turn can carry a **`structuredNext`** prediction object persisted on the assistant turn (`AIConversation::addAssistantMessage`, `aiconversation.cpp:309`), captured from a fenced ```json block **or** from an `apply_dial_change` tool call (`m_pendingToolStructuredNext`, `aimanager.cpp:524–531`). The prompt contract *requires* `expectedDurationSec` and `expectedFlowMlPerSec` `[low,high]` ranges, optional `expectedPeakPressureBar` (`shotsummarizer.cpp:1219–1221`) — the model is already forced to state a checkable prediction.
- **`buildRecentAdviceBlock`** (`dialing_blocks.cpp:1149`) matches each prior advice turn to the *next shot on the same profile*, loads both shots, and computes **`computeAdherence`** (`:1005` — per-field followed/ignored/partial/unclear, with prose-grinder rejection, JSON-type fail-closed guards, and a no-movement guard) plus **`computeOutcomeInPredictedRange`** (`:1099` — did duration/avg-flow land in the predicted window), and carries `outcomeRating0to100` + `outcomeNotes` from the follow-up shot.
- This reaches **both** the advisor and the barista: `enrichUserPromptObject` inserts `recentAdvice` (`aimanager.cpp:1272`), and `requestBaristaContext` loads 2 recent turns for the conversation key (`aimanager.cpp:1706–1710`).

What is genuinely missing is **durability and aggregation**, not the loop itself: the window is 2–3 turns per conversation key, recomputed on the fly, profile-scoped (not bean-scoped), it never records the KB's own `recommend_next_shot` output (the `recommendation.id`/`hypothesis`/`falsifier` are returned to the model and forgotten), and nothing accumulates "finer→sweeter confirmed 2×" across sessions. **Upgrade 1 is therefore a persistence-and-aggregation layer over an existing, battle-hardened attribution engine — not a new matching engine.** Designing a parallel matcher would violate the repo's one-definition rule and re-fight bugs `computeAdherence` already fixed (prose grinders, unquoted numbers, the no-movement trap).

**W2 — There is no "migration pattern" in assistant.db to mirror.** Both storage classes use **idempotent `ensureSchemaStatic()`** per class; `feedbackstorage.h:27` says plainly "schema_version starts at 1; **no migration runner yet**", and `tasksstorage.h` documents that its `ensureSchemaStatic` never touches the shared marker. So the new store is a third co-resident schema with its own idempotent `ensureSchemaStatic` — *not* a versioned migration. (The shots.db `runMigrations()` discipline in CLAUDE.md applies to shots.db only.)

**W3 — There is no temperature-stability detector.** `shotanalysis.cpp` contains no temperature code; `DetectorResults` has no temp field; the old `temperature_unstable` column was **dropped in shots.db migration 15** (`shothistorystorage.cpp:950–968`). The request's proposed "temp-unstable → `temp_flat`" mapping is impossible today, and `temp_flat` goes in the model-read bucket.

**W4 (minor, in our favor) — the detectors are richer than the request lists.** Beyond channeling/grind/truncated, `DetectorResults` carries: `yieldOvershoot` (gusher arm, `YIELD_OVERSHOOT_RATIO_MIN=1.20`), `chokedPuck` with full gate diagnostics (mean pressurized flow, yield ratio, pressurized duration), `verifiedClean`, `flowTrend` (stable/rising/falling + delta), preinfusion drip weight/duration, `skipFirstFrame`, `pourTruncated` + `peakPressureBar`, `verdictCategory`, and channeling **severity with `Transient` explicitly documented as "self-healed channel"** (`shotanalysis.h:136`). Also: an **ExpertBand** mechanism (pressure-peak / extraction-flow vs a *cited* band, with margins) already exists — infrastructure directly relevant to a pressure-spike detector. The persisted `detectorResults` QVariantMap rides `ShotProjection` (`shotprojection.h:152`).

**W5 — `computeAdherence`/`computeOutcomeInPredictedRange` are anonymous-namespace** (`dialing_blocks.cpp:729…1147`). Reusing them (mandatory per W1) requires hoisting their declarations into `dialing_blocks.h`. Hoist, never copy.

---

## Governing principle (the §3 ruling, stated once)

I adopt the request's principle and sharpen it into three lanes that every artifact below obeys:

1. **MEASURED (app asserts).** A number or verdict a detector computed from recorded samples, and an *event* record ("on Sep 9 we recommended finer; the next shot's grind moved from 5.0 to 4.75; it ran 34 s, inside the predicted 32–38"). The app states these as fact, with the evidence attached.
2. **INFERRED (app hands over, labeled).** A signature or pattern *consistent with* the measurements but not discriminated by them. Always delivered under an `inferred`/hedged key with the reason it is only consistent-with. The model may raise it as a maybe; the persona forbids upgrading it.
3. **JUDGMENT (model owns).** Taste attribution, which lever to pull, all language. The one causal sentence the app never emits: "my advice worked." The app emits adherence + deltas + ratings; "worked" is either a gated *pattern* fact (Tier A below, plurality ≥2) or the model's own hedged phrasing.

Corollary that decides several designs below: **detection criteria live in C++ where they are testable, not as a numeric-criteria DSL in the KB JSON.** The KB stays what it is — provenance-bound *meaning* for the model. The bridge from measurement to meaning is one function with tests.

---

## 1. Plan-outcome data model

### 1.1 Where it lives

A third assistant.db storage class, **`CoachPlanStorage`** (`src/barista/coachplanstorage.{h,cpp}`), mirroring `FeedbackStorage`/`TasksStorage` exactly: same file, own idempotent `ensureSchemaStatic(QSqlDatabase&)`, async `request*` via `SerialDbWorker`, synchronous `*Static` helpers shared by the context builder and the tool (one query path — the centralization rule). Never touches FeedbackStorage's `schema_version` marker.

### 1.2 Schema

```sql
coach_plans(
  id               INTEGER PRIMARY KEY,
  created_at       INTEGER,          -- epoch secs, stamped app-side
  anchor_shot_id   INTEGER,          -- the shot the advice was about (turn.shotId)
  bean_brand       TEXT, bean_type TEXT,   -- from the anchor snapshot, NEVER from the model
  profile_kb_id    TEXT, equipment_id INTEGER,
  source           TEXT,             -- 'fenced' | 'tool_applied'  (tool wins when both, matching #1053 precedence inverted-safe: fenced wins in-conversation, but a tool call is the stronger "acted" signal — record which)
  structured_next  TEXT,             -- the model's prediction object, verbatim JSON
  kb_rec_id        TEXT DEFAULT '',  -- recommend_next_shot recommendation.id this turn, if the tool ran
  kb_confidence    TEXT DEFAULT '',  -- 'high'|'med'|'low' from the KB result
  kb_prep_gate     INTEGER DEFAULT 0,
  lever            TEXT DEFAULT '',  -- derived: 'grind'|'dose'|'yield'|'temp'|'profile'|'repeat'|'multi'
  direction        TEXT DEFAULT '',  -- derived: 'finer'|'coarser'|'up'|'down'|'switch'|''|'unclear'
  -- outcome half, filled once and frozen:
  follow_up_shot_id INTEGER DEFAULT 0,
  adherence        TEXT DEFAULT '',  -- computeAdherence verbatim: followed|partial|ignored|unclear
  in_predicted_range TEXT DEFAULT '',-- computeOutcomeInPredictedRange verbatim JSON
  status           TEXT DEFAULT 'open',  -- 'open' | 'judged' | 'superseded'
  judged_at        INTEGER DEFAULT 0
)
```

Deliberately **not** stored: the follow-up's rating, taste words, or any "worked/failed" verdict. Ratings and taste arrive *later* than the shot (taste picker, `log_tasting_feedback`) and live authoritatively in shots.db / `shot_feedback`; storing a copy at judgment time would freeze a stale value. Aggregation (§3) joins live by `follow_up_shot_id` — the ledger stays minimal, and the no-stale-copy problem never exists.

`lever`/`direction` are derived deterministically at write time from `structured_next` vs the anchor shot's dial (numeric grinder comparison through `GrinderAliases` where scoreable, exactly the machinery `computeAdherence` uses). A prose grinder → `lever='grind', direction='unclear'`. More than one scoreable field moved → `lever='multi'` (excluded from single-lever patterns by construction). Ranges-only prediction → `lever='repeat'`.

### 1.3 Capture points (both are existing seams; no model-prose parsing, ever)

- **Plan row** — written at turn finalization, at the single point where a `structuredNext` is attached to a stored assistant turn (`AIConversation` finalize path; the same place `m_pendingToolStructuredNext` is consumed). Bean identity comes from `m_lastBaristaAnchorSnapshot` (`aimanager.cpp:1968–1988`), which already exists precisely to stamp app-owned provenance on write tools and is documented as "never from the model." No turn-with-prediction, no row: a chatty turn with no `structuredNext` records nothing.
- **KB recommendation enrichment** — in the `recommend_next_shot` executor branch (`baristatools.cpp:1456ff`), stash `{rec id, confidence, prep_gate}` of the returned recommendation on AIManager (a `m_pendingKbRecommendation`, cleared per turn exactly like `m_pendingToolStructuredNext` at the `:2549` choke point). If the turn then commits a `structuredNext`, the plan row carries the KB lineage — closing the gap where the KB's `hypothesis`/`falsifier` were returned to the model and forgotten. If the model gives advice without calling the tool, `kb_rec_id` stays empty; that is honest.

### 1.4 Matching & judgment: **lazy, reusing the #1053 engine, frozen once**

No hook on the shot-save path (nothing new on a path the machine can feel; trivially reversible). Instead, whenever the barista context or the recall tool reads plans, a **judge pass** runs first on the same background DB connection:

For each `status='open'` plan, find the first shot with `timestamp > anchor.timestamp AND profile_kb_id = plan.profile_kb_id AND equipment_id = plan.equipment_id` — the same query shape as `buildRecentAdviceBlock:1178`, tightened by equipment (the `AdviceScope` precedent at `baristatools.cpp:1523` — grind settings don't transfer across baskets, so neither does attribution). If found: load both shots via `loadShotRecordStatic`, compute `adherence = computeAdherence(sn, actual, prior)` and `in_predicted_range = computeOutcomeInPredictedRange(sn, actual)` **with the hoisted functions (W5)**, write them, set `status='judged'`, `judged_at=now`. Judged rows are immutable — one evaluation, one verdict, no drift.

**Confounder handling — where each of §3's failure modes dies:**

| Confounder | Mechanism |
|---|---|
| User changed something else too | `computeAdherence` → `partial`; derived `lever='multi'` — excluded from patterns, kept as event |
| User ignored the advice | `adherence='ignored'` (incl. the ranges-only no-repeat guard already in the code) — recorded, never counted as evidence for or against the advice |
| Model's advice unscoreable (prose grind, malformed JSON) | `'unclear'` — the fail-closed guards already exist |
| Multiple open recs | On writing a new plan for the same (bean, profile, equipment), any older `open` plan in that scope flips to `'superseded'` — a plan is only ever judged against the shot that *directly* followed it |
| Bean drift / long gap | Judgment records regardless; the **Tier gate** (below) requires follow-up within **14 days** of the plan and identical bean identity to count as evidence |
| Puck-prep noise swamping the lever | Tier gate requires the follow-up shot **not** be channeling-flagged (`channelingDetected` or sustained severity) — a channeled follow-up can't confirm or refute a grind/temp hypothesis |

### 1.5 The confidence gate (trustworthy vs recorded-only)

Three tiers, applied at read/aggregation time (pure function over judged rows + live shot data → headless-testable):

- **Tier A — validated, may feed an asserted pattern.** `adherence='followed'` AND single lever (`lever ∉ {multi, ''}`, `direction ≠ 'unclear'`) AND same bean AND ≤14 days AND follow-up not channeling-flagged AND **both** anchor and follow-up shots carry a rating (`enjoyment0to100 > 0`). The rating *delta sign* is then a fact: improved / worse / flat (flat = |Δ| ≤ 5 on the 0–100 scale).
- **Tier B — grounded observation, hedged.** `followed` but missing a rating on either side, or multi-lever. The app can state what happened mechanically ("we lengthened PI; the shot ran in the predicted window") but no taste verdict exists. Handed over labeled.
- **Tier C — recorded, never evidence.** `ignored`/`unclear`/`partial`, superseded, bean-drifted, channeled follow-up. Available to the recall tool as history ("we suggested that on the 4th but it wasn't tried"), never as support for "that worked."

**Pattern assertion floor (cold-start guard, mirroring `palateProfile`):** a per-(bean, lever, direction) pattern line ("finer → rating up") is asserted only with **≥2 Tier-A outcomes of consistent sign and no Tier-A counter-example**; one outcome is surfaced only as a single cited *event*, never a generalization. Below any Tier-A data at all, the block is `{}` and **absent** — silence, not a neutral note, exactly like `buildPalateProfileBlock:245` (the persona already knows how to degrade; a "not enough data yet" note would spend the proactive budget saying nothing).

---

## 2. Trace grounding policy

### 2.1 Reconciling the two trace concepts: **two lanes, one new bridge — keep the axis, activate the signatures**

Ruling: do **not** extend the 4-value axis, do **not** add numeric criteria to the KB, do **not** merge the vocabularies.

- The coarse axis is the **diagnostics matching key**: `matchTrace`/`traceShapes` (`coffeeknowledgebase.cpp:37–65`) and every `diagnostics[].conditions.trace` consume it, and its semantics (a clean trace *negates* channel-family conditions; `""` is neutral) are load-bearing in `recommendNextShot`'s conflict-dropping. Enriching it would ripple through every diagnostic rule for zero gain — the axis answers "which dial-in rule applies," which is inherently coarse.
- The 18 signatures are the **curve-fault vocabulary**, written for meaning and provenance. They stay descriptive. Machine-matchability is delivered by a **new C++ bridge beside `objectiveTraceShape`** (same file, same input, same philosophy — "translate detector verdicts into KB vocabulary"):

```cpp
// baristatools.cpp (or a small baristatrace.{h,cpp} if a second consumer appears)
struct TraceSignatureHit {
    QString signatureId;   // must exist in the KB's trace_signatures
    QString grounding;     // "measured" | "inferred"
    QString evidence;      // the numbers that earned it, human-readable, units included
};
QVector<TraceSignatureHit> objectiveTraceSignatures(const ShotProjection& s);
```

plus one accessor finally reading the dead array: `QJsonObject CoffeeKnowledgeBase::traceSignature(const QString& id)` → `{signature, meaning, class, next_change, citations}` via the existing `citationsFor`. The bridge asserts nothing the KB can't cite and cites nothing the detectors didn't measure — the join of the two is the whole feature.

### 2.2 The 18, classified (from the detectors that actually exist — W3/W4 applied)

**(a) MEASURED — the app may assert (6):**

| Signature (class) | Detector ground | Evidence string carries |
|---|---|---|
| `choke` (grind) | `grindChokedPuck` (either arm) | mean pressurized flow / yield ratio + gate diagnostics (`grindGate*`) |
| `gusher` (grind) | `grindYieldOvershoot` (ratio > 1.20) | yield vs target, ratio |
| `flow_exceeds_pressure` (prep) | channeling severity `sustained` (dC/dt is precisely the flow↔pressure-relationship instrument; `SHOT_REVIEW.md` is source of truth) | severity, spike time |
| `pressure_notch_heal` (prep) | channeling severity `transient` — `shotanalysis.h:136` documents Transient as "self-healed channel", which is this signature's `meaning` verbatim | spike time |
| `flow_stall_after_pi` (grind) | `flowTrend='falling'` + `grindDirection='tooFine'` co-firing | flow-trend delta + flow-vs-goal delta |
| `trace_pressure_spike_ramp` (profile) | **after S-T4 only** (new detector, §2.3); until then bucket (c) | peak bar vs commanded max, time of peak |

**(b) INFERRED — handed over hedged (3):**

| Signature | Basis | Why only consistent-with |
|---|---|---|
| `trace_flow_never_caps` (profile) | `grindDirection='tooFine'` from Arm 1 (flow averaged > 0.4 ml/s below goal) | Arm 1 measures *average* shortfall; "never reaches the target" is a stronger, per-sample claim the summary doesn't discriminate |
| `early_first_drops` (prep) | `preinfusionObserved` with high drip weight over short duration | drip observation is an "observation-only" line, thresholds weren't tuned for this claim |
| `trace_fast_pressure_bleed` (profile) | `flowTrend='rising'` late + short duration | no pressure-tail summary exists; commanded-vs-uncommanded decline is exactly what the KB says distinguishes healthy from fault |

**(c) MODEL-READ — the app stays silent (9):** `normal_saturation`, `pi_transition_spike`, `end_gush_profile`, `trace_early_gush_post_pi`, `trace_choke_then_gush`, `trace_flat9_late_flow_rise`, `trace_clean_decline`, `erratic_oscillation` (hardware — no detector), `temp_flat` (**no temperature detector exists — W3**; the request's proposed mapping is struck). These need phase-resolved curve reading (or, for `trace_choke_then_gush`, cross-*shot* sequencing) that nothing summarizes today. The model can still match them when the user describes the curve or it pulls `get_shot_detail`; the app just never puts its name on them.

### 2.3 Is a new curve-shape detector worth building? **One, narrowly: pressure-spike-on-ramp. The other two: no.**

- **`trace_pressure_spike_ramp` — YES (S-T4, optional-but-recommended).** Cost is genuinely low: `analyzeShot` already walks `pressure` against `pressureGoal` with phase markers; the check is "max actual pressure during ramp/hold exceeds max commanded pressure by ≥ margin," and a tuned margin constant with exactly this role already exists (`EXPERT_BAND_PRESSURE_MARGIN_BAR = 0.3`, deliberately named for shadow-tuning). Value is the highest of the ten profile signatures: it produces the flagship cited sentence ("peaked at 10.4 bar against a 9-bar profile — that's the profile, not the grinder; cap the peak"), and its `next_change` routes to `create_related_profile`, a tool that already exists. Crucially it is **validatable before it ships an assertion**: the `shot_eval` harness + `tests/data/shots/` regression corpus (TESTING.md) lets us measure the false-positive rate across the corpus first — the same audit discipline the choked-yield arm went through (#963/#966). Two `DetectorResults` fields (`pressureSpikeOnRamp`, `pressureSpikePeakBar`), one summary line, corpus-gated.
- **`trace_flat9_late_flow_rise` — NO.** Requires classifying the *profile's* shape (flat-9-ness) before judging the shot's tail; misclassification asserts a profile fault on a profile that isn't flat-9 — the confident-wrongness failure this whole ruling exists to prevent, for one signature whose `next_change` (add a declining tail) the model can already offer as a hypothesis from bucket (c). Stays model-read.
- **`trace_flow_never_caps` — NO detector.** The hedged Arm-1 inference (bucket b) already delivers ~90 % of the coaching value ("flow ran well under the target the whole pour — the frames are fine, the resistance is wrong"); tightening it to per-sample "never" buys a stronger adjective, not a different move. Stays inferred.

**Scope boundary, stated hard:** one detector, inside `ShotAnalysis`, corpus-validated, feeding one new signature into bucket (a). No general curve-shape classifier, no KB criteria fields, no per-signature detector program. Revisit only if the plan-outcome ledger later shows profile-class advice being generated blind where a measurement would have changed the move — that ledger is, conveniently, upgrade 1.

---

## 3. How both re-enter coaching

### 3.1 Trace: a barista-only context field + tool enrichment

**`lastShotTraceRead`** — inserted at the `requestBaristaContext` call site alongside the existing barista-only `descriptor` (`aimanager.cpp:2043–2050` is the documented precedent for "rides at the call site, not in the shared builder"). Built by a pure function over the anchor `ShotProjection` + KB:

```json
"lastShotTraceRead": {
  "measured": [{
    "signatureId": "choke",  "class": "grind",
    "evidence": "mean pressurized flow 0.31 ml/s across 18 s above 4 bar",
    "meaning": "puck too tight for the profile to express",
    "nextChange": "coarsen or drop dose",
    "citations": [ ... ]                       // KB provenance, verbatim
  }],
  "inferred": [{
    "signatureId": "trace_flow_never_caps", "class": "profile",
    "basis": "flow averaged 0.6 ml/s under the profile's target — CONSISTENT WITH this, not confirmed",
    "nextChange": "...", "citations": [ ... ]
  }],
  "note": "measured = the machine detected it on the LAST shot; state it plainly with its evidence. inferred = consistent-with only; raise it as a maybe or not at all. Absent entirely = do not claim any curve fault."
}
```

Cost discipline: emitted **only when non-empty** (a `smooth`/verified-clean shot inserts nothing), entries capped at 2 (measured first), citations capped at 1 each. This is a handful of lines on exactly the shots where it changes the move — cheaper than the prose it displaces, consistent with the turn-cost direction (the dial-in data block is already gated by `_scopedSystemPrompt`'s classifier; this rides inside it).

**Tool enrichment:** in the `recommend_next_shot` executor, add `machine_data.traceSignatures` = the measured ids + evidence. `ctx.trace` (the 4-axis) is untouched — diagnostics matching keeps its semantics; the richer vocabulary rides beside it for the model's prose.

**Persona clause** (appended in the `mayNudge` branch of `AssistantOverlay.qml`, after OPENING READ):

> TRACE-GROUNDED OPENING: when the data block carries `lastShotTraceRead` with a **measured** entry, your single proactive thing for a just-pulled shot is THAT — lead with what the machine saw, in plain words with its one number ("that last one choked — barely a third of a mil per second even at full pressure — let's coarsen a step"), and the cited `nextChange` as your offer. A **profile-class** measured entry beats a generic grind guess: say so ("that's the profile, not your grinder") and offer the profile move (`create_related_profile` on approval). A **prep-class** entry means puck prep first — do not offer grind or profile changes on top of it. An **inferred** entry is a maybe at most ("the flow never quite got where the profile wanted — might be worth a half-step coarser") or nothing; NEVER present it as something the machine detected. No `lastShotTraceRead` → your normal opening read. This replaces, not stacks on, the palate-grounded suggestion for that turn — still ONE proactive thing.

### 3.2 Plan-outcome: a resident block AND a tool (both, each doing what only it can)

**Resident block `coachTrackRecord`** — built in the `requestBaristaContext` worker inside the existing assistant.db `withTempDb` (co-resident with the feedback read at `aimanager.cpp:1778`), via `CoachPlanStorage` statics; inserted when non-empty (`aimanager.cpp:~2082`, beside `recentTastingFeedbackOnThisBean`). Scope: current bean, judged plans, compact:

```json
"coachTrackRecord": {
  "onThisBean": [
    { "pattern": "grind finer", "outcomes": "rating improved 2 of 2 times (62→78, 71→80)", "confirmed": true },
    { "pattern": "temperature up", "outcomes": "tried once, rating unchanged", "confirmed": false }
  ],
  "lastPlan": { "when": "2026-09-09", "advice": "grind 4.75", "adherence": "followed",
                "ranAsPredicted": {"duration": true, "flow": true}, "outcomeRating0to100": 78 },
  "note": "The barista's OWN past advice on THIS bean and what actually followed — measured adherence and the user's own ratings, never assumptions. A 'confirmed: true' pattern (2+ clean confirmations) may be stated as experience ('finer has worked on this bean — twice'). 'confirmed: false' lines and single events may only be recalled as specific history ('last time we tried hotter it didn't move the needle'), never generalized. This GROUNDS your one suggestion; it is not a report to recite."
}
```

Only Tier-A-fed `pattern` lines and the single most recent judged plan; caps: 3 pattern lines, ~120 tokens worst case. Absent below the floor (§1.5).

**Tool `recall_coaching_outcomes`** — in `baristatools.cpp` beside `search_tasting_feedback`, sharing `CoachPlanStorage::fetchJudgedPlansStatic` with the block builder (one query path). Args: `bean_brand`/`bean_type` (optional — default all beans), `lever` (optional), `limit`. Returns per-event rows: date, advice (`structured_next` summarized by the hoisted `synthesizeRecommendationSummary`), adherence, in-predicted-range, follow-up rating, tier label, incl. Tier-C history the block deliberately omits ("suggested, not tried"). This is the go-deeper surface — the resident block stays one-thing-sized because the tool exists. `[fork-index]` marker on both, per `.fork/FORK.md`.

**Persona clause** (extends OPENING READ, does not add a slot):

> TRACK RECORD: when the data block carries `coachTrackRecord`, your suggestion should AGREE with it — prefer a confirmed pattern ("finer's worked on this bean both times we tried it — want to go another quarter step?"), and don't repeat advice whose record shows it was tried and didn't help; say so and pick the other lever ("hotter didn't move it last time, so let's look at ratio instead"). Cite it as shared history ("last time we…", "when we tried…"), never as a law ("finer always…"). If they ask how past advice has panned out, use `recall_coaching_outcomes`. This grounds the SAME single proactive thing — it never becomes a second one.

**Budget interaction, made explicit:** the existing hierarchy (due item > trace-grounded/just-pulled coaching > recipe offer > doc change) is unchanged. `lastShotTraceRead` and `coachTrackRecord` are both *grounding* for the coaching turn's one suggestion — the trace decides **what** to raise; the track record decides **which lever** and the phrasing. Neither adds a proactive slot.

---

## 4. The grounding contract (concrete)

**The app asserts, as fact, with evidence attached:**
- Every `DetectorResults` field and derived bucket-(a) signature hit, each carrying the measured numbers that earned it and KB citations.
- Plan-outcome **events**: that advice X was given (verbatim `structured_next`), that shot Y followed, the measured deltas, `computeAdherence`'s verdict, in-predicted-range booleans, and the user's own ratings on both shots.
- **Patterns** only at Tier A ≥2 consistent, counter-example-free — and even then as counted history ("2 of 2 times"), never as mechanism.

**The app hands over, explicitly labeled as inference:**
- Bucket-(b) signature hits (`inferred`, with `basis` naming exactly what was measured and why it falls short of confirmation).
- Tier-B outcomes (mechanically followed-and-measured, taste unknown).
- Single Tier-A events and unconfirmed patterns (`confirmed: false`).
- `lever/direction='unclear'` derivations.

**The model owns:** taste attribution language, lever choice, all phrasing, matching bucket-(c) signatures from curve descriptions or `get_shot_detail` — always in its own hedged voice.

**Forbidden, everywhere:** the app writing "worked"/"helped"/"confirmed the hypothesis" into any asserted field (the closest it gets is `ranAsPredicted` + a rating delta — both measurements); the model upgrading `inferred`→detected or `confirmed:false`→pattern (persona + block `note`s state this at the point of use, the pattern every shipped block already follows); any signature id leaving the app that isn't in the KB's 18 (the bridge validates ids against `traceSignature()` — a typo'd id is a build-time test failure, not a runtime invention).

---

## 5. Migration path (impact ÷ risk order; every step shippable, testable, reversible)

**Trace lands first.** T1–T3 are a pure function, a context field, and a persona string — days, no schema, no new storage, and the flagship felt win ("led with what the machine saw") arrives immediately. The plan-outcome ledger is higher lifetime value but touches storage + turn finalization, and its **aggregates only become non-empty weeks after the capture step ships** — so start its clock (P1–P2) right behind T2 and let data accumulate while the trace work is being listened to.

| Step | What | Test | Validation |
|---|---|---|---|
| **T1** | `objectiveTraceSignatures()` + `CoffeeKnowledgeBase::traceSignature(id)` (activates `m_traceSignatures`) | New slots in **`tst_coffeeknowledgebase.cpp`** (per CLAUDE.md, functions-in-existing-file over new files): every emitted id resolves in the shipped KB; choked/gusher/sustained/transient/clean projections map per §2.2; clean shot → empty | headless | Reversible: uncalled functions. |
| **T2** | `lastShotTraceRead` at the barista call site + `machine_data.traceSignatures` in `recommend_next_shot` | builder is a *testable* free function (not anonymous-namespace — don't copy the palate builders' untested precedent); shape/cap/absence tests | headless | Reversible: delete two insertions. |
| **T3** | TRACE-GROUNDED OPENING persona clause | — | **on-device listening** (prose behavior only) | Reversible: string. |
| **P1** | `CoachPlanStorage` (schema §1.2, statics, async wrappers) + **hoist `computeAdherence`/`computeOutcomeInPredictedRange`/`synthesizeRecommendationSummary` to `dialing_blocks.h`** (W5; behavior-neutral, existing tests must stay green) | storage statics against a temp DB (the `tst_maintenancedocsync` pattern); lever/direction derivation table incl. prose-grinder → `unclear` | headless | Reversible: unused table in assistant.db. |
| **P2** | Capture: plan row at `structuredNext` finalization + `m_pendingKbRecommendation` in the tool executor (cleared at the `:2549` choke point) | finalize-path unit coverage where reachable; superseding logic | headless (flag: the finalize seam's test reach is partial — verify capture on-device once, by inspecting assistant.db) | Reversible: one write call. **Ships silently — nothing reads it yet.** Start early; this step is the data clock. |
| **P3** | Judge pass (lazy match + freeze) + tier classifier as a pure function | synthetic two-DB fixtures: followed/ignored/multi-lever/superseded/channeled-follow-up/14-day each land in the right tier; judged rows immutable | headless | Reversible: judged rows are inert without readers. |
| **P4** | `coachTrackRecord` block + `recall_coaching_outcomes` tool (+ `[fork-index]` markers) | block shape, plurality floor, absence below floor; tool row shapes | headless | Reversible: insertion + tool registration. |
| **P5** | TRACK RECORD persona clause | — | **on-device listening** | Reversible: string. |
| **T4** *(optional, last)* | Pressure-spike-on-ramp detector in `ShotAnalysis` → promotes `trace_pressure_spike_ramp` to bucket (a) | **shot_eval regression corpus** false-positive audit *before* the assertion is wired; then `tst_shotanalysis` cases | headless | Gate: corpus FP rate ≈ 0 at the chosen margin, else stays model-read. Reversible: `DetectorResults` fields ignored by consumers. |

Every step keeps the invariant *absent = today's behavior*: empty block → current opening read; empty ledger → current `recentAdvice`-only loop; unmapped shot → current 4-axis path. `recentAdvice` itself is untouched throughout — the session-local loop keeps working even if the ledger is rolled back.

---

## 6. Reuse map

| Piece | Builds on (verified seam) | Genuinely new |
|---|---|---|
| Trace bridge | `objectiveTraceShape` siting + philosophy (`baristatools.cpp:63–81`); `DetectorResults` incl. gate diagnostics (`shotanalysis.h:501–622`); `CoffeeKnowledgeBase` loader + `citationsFor`; dead `m_traceSignatures` (`coffeeknowledgebase.cpp:164`) | `objectiveTraceSignatures()`, `traceSignature(id)` accessor (~150 LoC + tests) |
| Trace context entry | barista-only call-site insertion precedent (`descriptor`, `aimanager.cpp:2043`); insert-when-non-empty discipline; `_scopedSystemPrompt` dial-in gating (turn-cost design) | one block builder + one insertion |
| Trace persona | OPENING READ clause structure + one-thing budget (`AssistantOverlay.qml:1121–1178`) | one clause |
| Plan capture | `structuredNext` persistence (`aiconversation.cpp:309`); `apply_dial_change` capture (`aimanager.cpp:524–531`); per-turn clearing choke point (`:2549`); anchor snapshot provenance (`:1968–1988`); `recommend_next_shot` executor (`baristatools.cpp:1456`) | plan-row write + `m_pendingKbRecommendation` |
| Plan storage | `FeedbackStorage`/`TasksStorage` idempotent-`ensureSchemaStatic` + `SerialDbWorker` + statics-shared-with-readers pattern; assistant.db co-residency contract (`tasksstorage.h:17–19`); `withTempDb` | `CoachPlanStorage` (the one honestly-new class) |
| Judgment | `buildRecentAdviceBlock`'s matcher (`dialing_blocks.cpp:1149–1233`); `computeAdherence` + guards (`:1005`); `computeOutcomeInPredictedRange` (`:1099`); `AdviceScope` equipment scoping (`baristatools.cpp:1523`); `GrinderAliases` | header hoist (W5) + the judge pass + tier classifier |
| Outcome re-entry | block-builder + cold-start pattern (`buildPalateProfileBlock:235`, floor+plurality guards); tool siting beside `search_tasting_feedback` (`baristatools.cpp:254`); `loadShotRecordStatic` for live rating joins | `coachTrackRecord` builder + `recall_coaching_outcomes` |
| Spike detector (T4) | `analyzeShot` pressure/pressureGoal walk; `EXPERT_BAND_PRESSURE_MARGIN_BAR` precedent (`shotanalysis.h:282`); `shot_eval` harness + `tests/data/shots/` corpus; #963/#966 audit discipline | one detection arm + two result fields |

Net-new is deliberately thin: one storage class, two pure functions, one detector arm, two context blocks, one tool, three persona clauses. Everything that could drift against an existing definition — adherence scoring, provenance stamping, KB citation, schema discipline, cold-start guards — is the existing definition, reused.

---

*Fable 5 — DoR ruling, 2026-09-11. Supersedes the request's §2A framing per seam finding W1; all other request constraints (§5) honored as written.*
