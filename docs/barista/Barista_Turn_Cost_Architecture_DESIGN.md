# Decenza Barista — Turn-Cost Architecture (Design Answer)

Grounded entirely in the seams listed in the request's §6. Nothing below assumes code I haven't been shown — where I propose something genuinely new, it's flagged as new. Before implementing, grep `.fork/INDEX.tsv` as the request instructs; some of this may already exist in another form.

---

## STATUS — code audit 2026-09-10 (much of this design is already built)

Mapping the real seams on `feat/barista` showed the migration table (§7) overstates what's unbuilt.
Ground truth, so we don't rebuild what exists:

- **Step 0 (module gating) — DONE.** `_scopedSystemPrompt()` (`qml/assistant/AssistantOverlay.qml`)
  splits an always-resident `_coreSystemPrompt` from per-turn/sticky gated modules. The ~20k-char
  dial-in DATA block, coffee-brain, query-shots, recipes, profiles, camera, web instruction text are
  **already peeled out of the core and gated** by a keyword classifier (casual / dialin / profiles /
  camera / web). The `chars=` measurement harness (`[barista] scoped prompt chars=…`) is live.
- **Step 6 (async cross-session rolling summary) — DONE.** `AIManager::requestSessionSummary()` →
  `AssistantSettings::setSessionSummary` (QSettings), seeded into the next session's anchor.
- **Step 7 (Anthropic caching) — DONE.** `stageCachePrefixLen(coreSystemPrompt.length)` →
  `AnthropicProvider::buildCachedSystemPrompt(prompt, cachePrefixLen)`. No-op on Gemini (correct).
- **Step 1 (trim resident core) — IN PROGRESS.** Slice 1 (2026-09-10): the coaching-framework prose
  (DIALING FRAMEWORK / SHOT TYPES / TALKING ABOUT A SHOT, ~900 tokens) was relocated verbatim out of
  the resident core into a gated `_mods.coaching` that rides `includeDialin` — cutting ~900 tokens off
  every casual/greeting turn (a straight win on Gemini). JUST-PULLED / log-taste stays resident on
  purpose (a one-word taste reaction trips the casual gate and must still log the shot).
- **Step 2 (structured C++ session-anchor struct) — DEFERRED.** A string anchor already carries
  bean/profile/prior-summary and live data rides `requestBaristaContext`; the struct is marginal.
- **Step 3 (intent router → prompt scoping) — substantially DONE** via Step 0's classifier; only the
  per-bucket *retrieval-plan* extension and an embedding fallback remain, and the fallback is
  add-only-if-measured.
- **Step 4 (per-bucket tool filtering) — DECLINED this session (2026-09-10).** All ~39 main-set tools
  still ship every `clientTools` turn (the ~7 web/visualizer tools are already gated on `webSearch`). A
  naïve per-turn filter is UNSAFE: the casual regex matches approval/taste words ("yeah", "perfect"), so
  trimming on the current utterance would drop `apply_dial_change` / `log_tasting_feedback` on the exact
  turn they must fire — a silent success-that-did-nothing. It's safely buildable via STICKY `active.*`
  gating (always-on writes + escape reads; sticky-gate only recipe-CRUD / camera / reasoning-reads), but
  the realistic win is modest (~39→~20 tools, ~500 tok on Gemini, decaying) and it's a live behavioral
  change needing on-device validation. Owner chose to defer it in favour of Step 5.
- **Step 5 (deterministic math short-circuit) — DONE.** `barista::tryQuickMath()` (`src/barista/
  closeintent.cpp:144`) answers pure ratio/dose/yield questions locally and speaks them (wired at
  `baristaconversation.cpp:320`, skipping the model turn entirely); high-precision (returns "" for
  anything conversational, bounds-checked), 14 test cases in `tests/tst_closeintent.cpp` incl. the
  "discussion not math" false-positive guard.

Remaining real work: finish Step 1 trimming as warranted. Step 4 is deferred (see above); Steps 0/5/6/7
are built.

---

## 1. Turn-scoped context assembly

**Mechanism: a local, deterministic intent router — no LLM round-trip.**

Two layers, cheapest first:

- **Lexical/keyword classifier.** A small lookup table mapping utterance patterns to coarse intent buckets: `greeting`, `dial_in`, `bean_management`, `reminder_maintenance`, `recipe_lookup`, `ambient` (weather/news/stock), `photo_vision`, `tool_action`, `ambiguous`. This is microseconds — no network round-trip, no added latency.
- **Small embedded-index fallback**, used only when the keyword layer scores low confidence — catches paraphrase ("that pull tasted flat" → `dial_in`) without exact keyword hits. Still on-device, still no LLM call.

Each bucket maps to a **retrieval plan**: which SQLite queries to run and which tool defs to include. This generalizes the existing `_scopedSystemPrompt()` hook — proven by the camera/web module stopgap — from "static module on/off" to "intent bucket → {data queries, tool subset, prompt module}."

**Failure modes:**

- *False negative* (router under-scopes — e.g. reads a real dial-in question as small talk): the model lacks a fact it needs. Mitigation is a safety net, not perfect routing — keep `query_shots`/`get_current_bean` available even in `greeting` mode as a cheap escape hatch. A misroute costs one extra tool round-trip, not a wrong or invented answer. This is why grounding survives a bad guess: the model can always ask the app for the fact instead of making one up.
- *False positive* (router over-scopes small talk into `dial_in`): pure cost regression, never a correctness problem. Bias the router toward mild over-inclusion on ambiguous utterances — over-inclusion costs money, under-inclusion costs trust.
- *Multi-turn drift* ("what about half a gram less" — ambiguous alone): solved by making routing **stateful**, not re-classified blind each turn. The session anchor (§2) carries an `open_thread` field the app updates whenever a dial-in tool fires, so turn N+1 inherits "we're still dialing" instead of re-guessing from a fragment.

---

## 2. Facts vs. language split — concrete budget

**Boundary:** anything the app can compute or fetch deterministically and that changes turn-to-turn becomes retrieval or a tool result. Anything stable — persona, dialing philosophy, tool-use rules, spoken-medium pacing — stays resident. Nothing that's a live number gets written into prose once and reused; it gets recomputed or re-queried.

Reclassification of today's blocks:

| Today | Target | Where it lives |
|---|---|---|
| Persona/instructions (~5k) | Trim to ~900–1,200 resident tokens | Layer 0. Move rare policy text (photo walkthrough, edge cases) behind a tool result or on-demand playbook call. |
| Recent-shot table | On-demand, scoped to current bean, last 3–5 shots, terse numeric rows | Layer 2, `dial_in` bucket only |
| Best shot | Stays resident — small, referenced constantly | Layer 1 (session anchor) |
| Grinder history | On-demand, same as recent-shot table | Layer 2 |
| Bean freshness | Resident as one app-computed field (e.g. "day 9, peak window") | Layer 1 |
| Profile guidance | On-demand, tool-backed (same pattern as `coffee_knowledge.json`) | Tool result |
| Tasting feedback (this bean) | On-demand, scoped to current bean, compact bullets not prose | Layer 2 |
| Saved user facts | Split: load-bearing ones (name, units, standing preferences) resident; rest tool-backed | Layer 1 / tool result |

**Target composition, dialing turn:**

| Layer | Content | ~Tokens |
|---|---|---|
| 0 — Identity | Persona, dialing philosophy, tool-use rules, pacing | 900 |
| 1 — Session anchor | Bean, freshness, best shot, current target, open thread, nudge flag | 350 |
| 2 — Turn-scoped retrieval | Last 3–5 shots on this bean, relevant tasting notes | 1,200–1,800 |
| 3 — Tool defs (filtered) | Dial/query/log tools only (~8–10 of 46) | 500 |
| 4 — Recent turns + utterance | Last 2–3 exchanges | 600–900 |
| **Total** | | **~3,600–4,500** |

Greeting turn: 900 + 350 + 0 + ~200 (ambient tools) + ~300 = **~1,750**.
Tool-action turn: 900 + 350 + ~200 (1–3 relevant tool defs) + ~400 = **~1,850**.

That's a 7–8x cut on dialing turns, 15–17x on the more common light turns — the order-of-magnitude target holds on the harder case and beats it on the easier, more frequent one.

---

## 3. Session continuity without resending history

Two mechanisms, doing different jobs:

**(a) Structured session-state object — the session anchor (Layer 1), app-maintained, not model-maintained.** Bean, freshness, best shot, current ratio target, last grind change, open thread, due-reminder count. The app writes it from real DB state after every tool call, so it can't drift — it's not a summary of what was *said*, it's a snapshot of what's *true*. "Back to that 1:2.5 we set" doesn't need the model to remember a sentence from three turns ago; the app already knows the live target.

**(b) Rolling summary for what was discussed but isn't structured data** — "serving a dinner party tonight," "doesn't like fruity notes." This genuinely is language, so it can't be a DB field. Update it every 4–6 turns (or on session close) with a small, separate summarization call — run **async, after TTS has already started speaking**, so it never adds felt latency to the turn the user is waiting on. Keep it to ~150–300 tokens.

**Grounding rule:** the structured object always wins over the summary if the two would ever conflict (e.g. summary says "grinder set to 14," anchor says the app changed it since) — the model is instructed to treat Layer 1 as ground truth and the summary as color only.

**(c)** Raw immediacy ("as I said, go finer") is covered by keeping the last 2–4 raw turns verbatim (Layer 4) — you don't need the summary for something said one turn ago, only for things said many turns back.

This is exactly why `AIConversation::setSessionSystemPrompt()` is load-bearing as flagged in §6: it lets a freshly-assembled system prompt (identity + anchor + summary + retrieval) go out every turn while the conversation object's actual message history stays a short tail, not a growing transcript.

---

## 4. Provider-agnostic statefulness

**Recommendation: "send less" is the primary strategy; caching is an opportunistic accelerant layered on top, never load-bearing.**

Reasoning: Gemini has no caching path today and the constraint is explicit that both providers must work *acceptably*. Any design that depends on caching for acceptable latency fails exactly on the provider that's called out by name. So the Q1–Q3 reduction (30k → ~2–4.5k) has to carry the latency win on its own, on every provider, with zero caching.

Caching then becomes free upside specifically for Anthropic: keep **one** prompt-assembly path (Layers 0–4), and let `AnthropicProvider` mark the now-small, now-truly-stable Layer 0 (and optionally the per-bucket Layer 3 tool defs, which are stable within a bucket) as a cache breakpoint via the existing `buildCachedSystemPrompt`/`messagesWithCachedFirstUser`. Every other provider just receives the same small assembled prompt uncached — no behavior difference, no second code path to maintain. This matches the reuse-first constraint: it's a small extension of existing Anthropic caching code, not a new caching subsystem.

---

## 5. Tool-round-trip minimization

- **Pre-fetch the obvious.** For the `dial_in` bucket, the app already ran `query_shots`/`get_current_bean` while building Layer 2 — the model never needs to call them itself. This converts the majority of *read* tool calls from model-invoked round-trips into app-side context assembly. It's the single largest round-trip cut.
- **Reserve real tool calls for writes and the unpredictable tail** — `apply_grind_change`, `add_bag`, `update_bag`, `open_bag_camera`, reminder mutations, and anything the router didn't anticipate (the Q1 escape hatch). These have side effects or are genuinely unpredictable; a round-trip there is correct, not wasteful.
- **Deterministic math with no model involved at all** for pure arithmetic ("what's 18→48," "1:2.5 off 19g"). Pattern-match locally and either answer without invoking the model or hand it the pre-computed number to speak — removes both tokens and a round-trip for a narrow, common question class, and removes any risk of the model doing the arithmetic wrong.
- **Parallel tool calls** for genuinely independent multi-read turns ("how's my morning bean doing and what's due today") — batch them in one round-trip rather than serial calls; `m_toolExecutor` reads against SQLite are safe to run concurrently (writes still serialize).
- **No separate LLM-based "planner" step.** The local router from §1 already *is* the planner, and it's non-LLM — adding a second model call on top would reintroduce the exact "round-trip to save tokens" anti-pattern the request explicitly warns against.

---

## 6. The natural-discussion layer

- **Interim acknowledgments during `Thinking`.** Use the existing `Thinking` state: a short, locally-spoken filler ("let me check that trace…") plays via TTS the instant the state is entered, fully decoupled from the model call — costs zero tokens because the model never generates it. Pure app-side polish on top of the existing state machine, not a change to it.
- **Proactive-but-not-pushy surfacing.** A single compact `pending_nudge` field in the session anchor, not a paragraph re-injected every turn. The app decides *whether* something's worth surfacing (deterministic); the model decides *when* it's natural to mention it, at most once per session — that split is exactly "app owns facts, LLM owns language."
- **Brevity/pacing for spoken delivery.** This is a persona concern, not a scoping concern — keep explicit pacing rules (short sentences, one idea per turn, no lists) resident in Layer 0, since it's cheap and needed on every single turn regardless of bucket.
- **Cross-session personality.** The rolling summary (§3b) plus the small resident load-bearing facts (§2) persist to `assistant.db` so a new session's Layer 1 anchor seeds from the last session's summary rather than starting cold — no transcript resend required.
- **Why cost and warmth aren't actually in tension:** Layer 0 + Layer 1 together run ~1,250–1,650 tokens and are present on *every* turn, including the cheapest greeting. That's the part doing the actual work of "feeling like the same barista." The 20k that got cut was mostly raw data, which was never what made the conversation feel continuous in the first place — it just made it expensive.

---

## 7. Migration path

Ordered by impact ÷ risk. On-device flags noted explicitly.

| Step | What | Risk | Impact | On-device only? |
|---|---|---|---|---|
| 0 (done/in progress) | Per-question module gating via `_scopedSystemPrompt()`; keep the `chars=…` instrumentation as the measurement harness for every step below | — | — | — |
| 1 | Trim Layer 0 to ~900–1,200 resident tokens; move rare policy text behind tool results | Low | High (shaves ~4k off every single turn immediately) | No — pure prompt edit, testable with any/mocked provider |
| 2 | Build the session-anchor struct (Layer 1) from existing `BaristaContextBuilder`/SQLite sources; inject via `setSessionSystemPrompt()` | Medium | High (unblocks Layer 2 no longer needing to re-derive "current bean" etc.) | Mostly no — logic testable against recorded data; only live-voice naturalness needs on-device |
| 3 | Local intent router + retrieval-plan table, wired into `_scopedSystemPrompt()`. Ship keyword-only first, measure misroute rate, add embedding fallback only if needed | Higher | Largest single token-reduction | Router logic no — non-LLM, unit-testable on a corpus. Grounding-under-misroute check yes — needs live tool-call behavior under real mic conditions |
| 4 | Filter `buildTools()`'s 46 down to bucket-relevant subset; parallelize independent tool-call dispatch in `AIManager` | Low–medium | Moderate (cuts ~1.6k tool-def cost + round-trip count) | Filtering no. Parallel-dispatch timing yes — needs on-device network/voice conditions |
| 5 | Deterministic math short-circuit for ratio/weight/time questions | Low | Modest but real (removes tokens + a full round-trip for a common class) | No |
| 6 | Async rolling summary (§3b), persisted to `assistant.db` for cross-session seeding | Medium | Continuity, not cost — sequence after 1–3 are stable | Summarization logic no. Cross-session feel yes |
| 7 | Wire Anthropic caching (`buildCachedSystemPrompt`) to the now-small, now-stable Layer 0 (+ optionally per-bucket Layer 3) | Lowest | Anthropic-only bonus | Yes — real cache-hit behavior needs a live Anthropic key |
| 8 | Interim-acknowledgment / pacing polish on top of `Thinking` state | Low | Polish, not the cost fix — finishing touch once reduction is proven | Yes — felt pacing can't be evaluated off-device |

---

## Reuse map

| Piece | Builds on |
|---|---|
| Layer 0/1 assembly + injection | `AssistantOverlay.qml` persona/sessionCtx/block assembly + `AIConversation::setSessionSystemPrompt()` — extended, not replaced |
| Layer 1 source data | `BaristaContextBuilder` (`buildBeanBlock`, `m_beanBlock`, `m_profileBlock`), `ShotHistoryStorage`, `assistant.db` — reused as data sources, restructured as compact output instead of prose |
| Layer 2 retrieval | `AIManager::requestBaristaContext()` — extended into a per-bucket retrieval plan instead of one fixed block |
| Router/gating | `_scopedSystemPrompt()` — the exact hook already proven by the camera/web stopgap |
| Tool filtering | `baristatools.cpp`'s `buildTools()`, filtered before `m_toolExecutor` |
| Caching | `AnthropicProvider::buildCachedSystemPrompt` / `messagesWithCachedFirstUser` — unchanged mechanism, smaller/stabler input |
| Voice/turn machinery | `BaristaConversation` — untouched throughout; `Thinking` state reused (not modified) for interim acknowledgments |
| Genuinely new | Local intent router + retrieval-plan table (§1/Step 3); session-anchor struct (§2/Step 2); async rolling-summary job (§3b/Step 6); deterministic-math short-circuit (§5/Step 5) |

Everything else is a restructuring of an existing seam, not new machinery.
