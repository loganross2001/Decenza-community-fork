# Fable 5 Design Request — Barista Coaching: Closing the Loop & Grounding the Trace

**Make the AI barista's dialing coaching (a) learn from whether its own past advice actually worked, and (b) lead with what the machine *measured* about the shot's curve — without ever inventing a fact or over-asserting a signature it can't reliably detect.**

---

## 0. What we're asking you to do

Design two related upgrades to Decenza's in-app AI "barista" coaching, as **one coherent piece of work** with a shared grounding discipline:

1. **A plan-outcome learning loop.** Today the barista says "go finer, it should sweeten" and then *never finds out if it did*. Design the mechanism that records each coaching recommendation, matches it to the shot that followed, judges whether the predicted change happened, and feeds validated (and invalidated) patterns back into future advice — so the barista can say "last time we lengthened pre-infusion on a light roast it evened out; let's do that again," grounded in real recorded outcomes.

2. **A trace→knowledge-base grounding bridge.** Today the opening read leads with palate/similar-bean summaries, and the coaching brain reasons over a coarse 4-value `trace` axis (`channeling`/`fast`/`slow`/`smooth`) derived from the shot-quality detectors. Design how the barista should lead the opening read with a **cited, trace-grounded move** ("last shot spiked at 10.5 bar on the ramp — that's the profile, not the grinder; let's cap the peak") — deciding how far the *app* should assert a machine-detected trace signature versus letting the *model* read it, given that most of the rich signatures aren't detectable from today's code.

This is a request for an **architecture, a data model, and a grounding policy** — not code. Be opinionated; where two designs trade off, pick one and say why. **Ground everything in §6 (what exists today) and reuse those seams — no greenfield rewrite.** The single most important thing to get right is the grounding/false-attribution policy in §3.

---

## 1. What the barista is (and how coaching flows today)

Decenza is a Qt/C++ controller for the Decent Espresso DE1, running on an **Android tablet at the machine**. The "barista" is a voice-first AI assistant that coaches espresso dial-in grounded in the user's **real** shot history and ratings (never invented numbers).

**Key architecture fact for this request:** coaching turns run **through `AIManager` directly**, NOT through the voice/turn state machine (`BaristaConversation`). So this work is about the **context the app assembles and the tools/KB the model reasons over** — it is testable headlessly (`ctest`, no tablet), except the persona-prose behavior which is confirmed on-device by listening.

The coaching intelligence today is three layers:
- **Context blocks** the app builds once per session and injects (`AIManager::requestBaristaContext`): `palateProfile` (cross-bean read of what the user's ratings like), `similarBeanExperience` (transfer learning from resembling beans), current-bean/shot facts, recipes, people. Increments 1/2/2b of "proactive coaching" shipped these — see `docs/barista/PROACTIVE_COACHING.md`.
- **A coffee-science knowledge base** (`resources/barista/coffee_knowledge.json`, ~82 KB): top-level `descriptors`, `levers`, `causal_edges`, `diagnostics`, `trace_signatures` (18), `goals`, every fact carrying provenance/tier/confidence. It is **retrieved via tools, not dumped in the prompt**.
- **Tools** (`src/barista/baristatools.cpp`): `translate_taste`, `recommend_next_shot`, `plan_for_goal`, `query_shots`, `get_shot_detail`, `compare_shots`, `look_up_bean`, `search_visualizer_shots`, `log_tasting_feedback`, `create_related_profile`, and more.

---

## 2. The two problems

### 2A. Coaching never closes the loop
The barista makes a testable prediction ("finer → sweeter") and then has **no memory of whether it came true**. There is **no `plans` / recommendation-outcome store anywhere in the codebase** (verified — the `assistant.db` has `feedbackstorage` and `tasksstorage`, but nothing recording "we recommended X, then shot Y happened, and here's how it compared to the prediction"). Consequences:
- The barista can't say "that worked last time" or "we tried that and it didn't help — let's try the other lever."
- It can repeat advice that already failed for this user/bean.
- The single richest coaching signal — *did my own advice work?* — is thrown away every session.

### 2B. Coaching under-uses what the machine measured about the curve
Two facts about the current grounding:
- `objectiveTraceShape()` (`baristatools.cpp:67`) translates the shot-quality **detector verdicts** into the KB's coarse `trace` axis: `channeling` (prep gate) → then grind direction `tooCoarse`/`yieldOvershoot` → `fast`, `tooFine`/`chokedPuck` → `slow`, `onTarget` → `smooth`, else `""`. This is fed to `recommend_next_shot` as `objectiveTrace` (call sites `baristatools.cpp:1490,1514`). It is **grounded but coarse** — four buckets.
- The KB *also* has a separate, richer `trace_signatures` array — 18 entries like `trace_pressure_spike_ramp`, `trace_early_gush_post_pi`, `trace_flow_never_caps`, `trace_flat9_late_flow_rise`, `choke`, `gusher`, `flow_exceeds_pressure`. **These are descriptive-only** (`id` + natural-language `signature`/`meaning`/`class:prep|profile`/`next_change` + provenance) — **no machine-matchable numeric criteria**. They were written for a *model* to match against a trace, and **most are NOT produced by any detector** (`src/ai/shotanalysis.{h,cpp}` detects channeling, grind direction/choke, truncated pour, temperature stability — it does NOT detect "pressure spike on the ramp," "flow never caps," "flat-9 late flow rise," etc.).

So the opening read leads with palate/bean summaries and reasons over 4 coarse trace buckets, while a richer profile-fault vocabulary sits unused — and the honest reason it's unused is that **the app can't reliably detect most of those signatures today**.

---

## 3. The core tension you must resolve (grounding vs. confident wrongness)

Both upgrades pull the same way and carry the same risk: **the more the app asserts as a deterministic fact, the more grounded and useful the coaching becomes — and the more confidently, specifically wrong it can be.**

- **Plan-outcome:** attributing a shot's improvement to the barista's advice is a *causal* claim over noisy, confounded data. The user may have changed something else, skipped the advice, or the bean drifted. A loop that asserts "my advice worked" on a coincidence teaches the barista (and the user) a false lesson — worse than saying nothing.
- **Trace grounding:** telling the user "your shot spiked at 10.5 bar, that's a profile fault" when the app *inferred* rather than *measured* that signature is the exact "invent a fact" failure the barista is forbidden from (grounding is sacred — real data only). But refusing to assert anything the detectors don't cover leaves the coaching coarse.

**The heart of this request:** draw the line between *what the app asserts as measured fact*, *what it offers the model as a grounded-but-hedged observation*, and *what it leaves to the model to read* — such that coaching gets sharper and more loop-aware **without** ever crossing into fabricated shot metrics, fabricated trace signatures, or fabricated causal claims. A guiding principle we hold (adapt or challenge it): **the app owns FACTS, the model owns LANGUAGE and JUDGMENT** — deterministic code should only assert what it truly measured; anything inferred is handed over as an explicitly-hedged signal for the model to weigh, never as ground truth.

---

## 4. Specific design questions

Answer concretely, grounded in §6.

### Plan-outcome learning loop
1. **What to record, and where.** Define the recommendation-outcome data model (likely a new `assistant.db` table alongside `feedbackstorage`/`tasksstorage`). What captures a recommendation (the lever + direction + the predicted taste/measurable change + bean identity + timestamp + confidence), and what captures its outcome (the following shot's id, the measured deltas, the enjoyment rating, whether the predicted change actually occurred)? Keep it minimal but sufficient to feed coaching.
2. **The matching/attribution logic.** How does a *later* shot get matched to the recommendation it followed — timestamp + same bean + the change actually being present in the new shot's dial? How do you handle the confounders in §3 (user changed something else, ignored the advice, multiple recs open, bean drift)? **Specify the confidence gate**: under what conditions is an outcome trustworthy enough to reuse, versus recorded-but-not-asserted?
3. **How it re-enters coaching.** A new resident context block (à la `palateProfile`/`similarBeanExperience`, built in `requestBaristaContext`, inserted when non-empty), a tool the model calls on demand, or both? Give the concrete shape of what the model receives ("on THIS bean: finer→sweeter confirmed 2×, hotter→no change 1×") and the persona clause that uses it. Respect the "one proactive thing, not a dashboard" discipline the opening read already follows.
4. **Cold-start & guards.** What's the minimum evidence before the loop asserts a pattern (mirror the `palateProfile` ≥6-rated-shots / plurality guards)? What does it emit when there's not enough data — nothing, or a neutral note?

### Trace→KB grounding bridge
5. **Reconcile the two trace concepts.** What is the right relationship between the coarse 4-value `trace` axis (which `objectiveTraceShape` feeds and `recommend_next_shot` consumes) and the rich 18-entry `trace_signatures` array? Should `objectiveTraceShape` be *extended* to emit richer signatures for the faults the detectors CAN support (channeling→`flow_exceeds_pressure`/`gusher`, choke→`choke`, truncated→…, temp-unstable→`temp_flat`)? Should the `trace_signatures` array gain a machine-matchable criteria field for the subset that's detectable? Or should the two stay separate with a clear "axis = asserted, signatures = model-read" split? Recommend one.
6. **Asserted vs. inferred vs. model-read, per signature.** Classify the 18 signatures into: (a) *reliably detectable today* from existing detectors → the app may assert; (b) *partially inferable* → hand to the model as a hedged observation; (c) *needs a new detector* → is it worth building a curve-shape classifier (like the channeling dC/dt detector) for the high-value profile signatures (`trace_pressure_spike_ramp`, `trace_flow_never_caps`, `trace_flat9_late_flow_rise`), or should those stay model-read? Give the cost/benefit and a recommended scope boundary.
7. **How the opening read leads with it.** Concretely: when the last shot carries a *profile-class* (not prep-class) trace signal, how should the opening read lead with the cited profile move (with provenance from the KB) instead of a generic grind guess — while degrading gracefully to the current palate/bean read when the signal is absent or only prep-class? Where does the trace signal enter the context (a field on the existing shot facts, or a new block), and what's the persona clause?

### Cross-cutting
8. **Migration path.** Give an **incrementally-shippable** sequence for BOTH upgrades — each step independently testable (prefer headless `ctest` where possible) and reversible — ordered by (impact ÷ risk). Flag which steps are headless-testable C++ (data blocks, storage, `objectiveTraceShape` mapping) versus which need on-device listening (persona-prose behavior). Say which upgrade should land first and why.

---

## 5. Constraints & non-negotiables

- **Grounding is sacred.** No invented shot metrics, ratings, bean facts — and, new for this request, **no invented trace signatures and no unearned causal claims.** An inferred signal must be labeled as inferred when it reaches the model.
- **Reuse-first.** Build on the §6 seams; name the exact functions/files each part touches. No greenfield rewrite. Grep `.fork/INDEX.tsv` before proposing anything new.
- **Headless-testable where it can be.** Coaching runs through `AIManager`, not the voice FSM, so the C++ data/storage/mapping parts must be unit-testable via `ctest` (build dir `build/Qt_6_11_2_for_macOS_Debug`); reserve on-device validation for persona-prose behavior only, and flag it.
- **`assistant.db` schema discipline.** A new table is a one-time migration run with the app stopped — don't over-engineer concurrency around it (mirror the existing storage classes' migration pattern).
- **Provider-agnostic.** The user's provider is swappable (Anthropic/Gemini/OpenAI/OpenRouter/Ollama); nothing here may assume one.
- **Keep the discipline of the opening read** — one proactive, grounded thing; a warm lead + go-deeper hook, not a dashboard.
- **Cheaper-or-equal per turn.** These add *grounding*, not prompt mass — prefer on-demand tools / compact resident blocks over dumping more prose every turn (consistent with the turn-cost architecture direction in `Barista_Turn_Cost_Architecture_DESIGN.md`).

---

## 6. What exists today (grounding — build on these seams)

| Concern | Where | Notes |
|---|---|---|
| Coaching context assembly | `AIManager::requestBaristaContext` (`src/ai/aimanager.cpp:1613`) | Builds blocks on a worker thread, inserts each when non-empty (`palateProfile` ~2072, `similarBeanExperience` ~2078). **The seam for any new resident block.** |
| Existing context blocks (pattern to copy) | `buildPalateProfileBlock` (`aimanager.cpp:235`), `buildSimilarBeanBlock` (`aimanager.cpp:346`), `buildProactiveRecBlock` (`aimanager.cpp:156`) | Anonymous-namespace builders over `QSqlDatabase&`; cold-start guards; the template for a `buildOutcomeLoopBlock`. |
| Proactive-coaching increments (shipped) | `docs/barista/PROACTIVE_COACHING.md`; persona in `qml/assistant/AssistantOverlay.qml` (OPENING READ / SIMILAR BEANS clauses) | The behavior discipline + where persona clauses live. |
| Detector→KB trace bridge | `objectiveTraceShape()` (`src/barista/baristatools.cpp:67`); consumed at `baristatools.cpp:1490,1514` | Maps detector verdicts → coarse `trace` axis; feeds `recommend_next_shot`. **The seam to enrich for 2B.** |
| Shot-quality detectors | `src/ai/shotanalysis.{h,cpp}` | Detects channeling (dC/dt), grind direction/choked puck, truncated pour, temperature stability. **Does NOT detect the profile-curve signatures.** The reference for any new curve-shape classifier. |
| Coffee KB | `resources/barista/coffee_knowledge.json` (`descriptors`/`levers`/`causal_edges`/`diagnostics`/`trace_signatures`(18, descriptive-only)/`goals`); loaded by `src/barista/coffeeknowledgebase.{h,cpp}` | Provenance-bound facts; `trace_signatures` carry no numeric criteria today. |
| Coaching tools | `src/barista/baristatools.cpp` — `translate_taste`, `recommend_next_shot`, `plan_for_goal`, `query_shots`, `get_shot_detail`, `compare_shots`, `look_up_bean`, `search_visualizer_shots`, `log_tasting_feedback`, `create_related_profile` | On-demand retrieval surface; a new "recall outcomes" tool would live here. |
| Assistant-side storage | `src/barista/feedbackstorage.{h,cpp}`, `src/barista/tasksstorage.{h,cpp}` (`assistant.db`) | The pattern + DB for a new recommendation-outcome table. **No `plans`/outcome table exists yet.** |
| Local shot data | `ShotHistoryStorage` (`shots.db`) + `loadShotRecordStatic` (trusted per-shot path) | Real dose/yield/time/pressure/enjoyment; how to read a following shot's actual dial for attribution. |
| Existing KB test | `tests/tst_coffeeknowledgebase.cpp` | Where new KB/mapping tests belong. |
| Fork reuse index | `.fork/INDEX.tsv` | Grep before proposing anything new. |

**Note for the scoper:** an earlier automated scoping over-estimated readiness (it claimed a `plans` table and a ready-made trace-signature detector — **neither exists**). This request reflects the *verified* code. Trust §6.

---

## 7. Deliverable we want back

1. **Plan-outcome data model** — the recommendation + outcome schema, the matching/attribution logic, and the confidence gate that decides trustworthy-vs-recorded-only.
2. **Trace grounding policy** — the assert / hedge / model-read classification of the 18 signatures, the reconciliation of the two trace concepts, and whether any new curve-shape detector is worth building (with a scope boundary).
3. **How both re-enter coaching** — the concrete context-block/tool shapes and persona clauses, respecting the one-proactive-thing discipline.
4. **The grounding contract** — an explicit statement of what the app asserts as fact vs. hands over as hedged inference, for both upgrades (this is §3, made concrete).
5. **Migration path** — incrementally-shippable, testable, reversible steps for both upgrades, ordered by impact ÷ risk, flagging headless-vs-on-device validation, and which lands first.
6. **Reuse map** — which §6 seams each piece builds on; what (if anything) genuinely must be new.

Be concrete, be opinionated, and stay grounded in what's already there.
