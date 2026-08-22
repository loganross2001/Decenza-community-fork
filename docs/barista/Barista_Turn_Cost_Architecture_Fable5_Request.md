# Fable 5 Design Request — Barista Conversational Architecture

**A strong, low-cost architecture for a voice-first AI barista that still feels like a real, continuous conversation.**

---

## 0. What we're asking you to do

Design the **conversational architecture** for Decenza's in-app AI "barista" — the layer that turns a live, spoken, back-and-forth discussion into LLM calls and back.

The single hard requirement: **drive the per-turn LLM cost and latency down by an order of magnitude, WITHOUT making the discussion feel like a stateless Q&A box.** It must stay a warm, continuous, naturally-occurring conversation that remembers the session, references what was just said, and gives espresso-dialing advice grounded in the user's real shot data.

This is a request for an **architecture and a migration path**, not code. Be opinionated. Where two designs trade off, pick one and say why. Ground every recommendation in the "What exists today" section below — reuse the seams that are already there; do not propose a greenfield rewrite.

---

## 1. What the barista is

Decenza is a Qt/C++ cross-platform controller for the Decent Espresso DE1 machine. It runs on an **Android tablet mounted at the machine** (also desktop/iOS, but the tablet is the target). The "barista" is a **voice-first AI assistant**: the user talks to it while making coffee and it talks back.

The live loop today:

```
speech (Android SpeechRecognizer, JNI)  →  STT final text
   →  LLM turn (system prompt + history + tools)  →  answer text (+ optional tool calls)
   →  TTS (ElevenLabs)  →  spoken reply
   →  back to listening
```

A dedicated state machine owns turn-taking: `BaristaConversation` (`src/barista/baristaconversation.{h,cpp}`), states `enum class State { Idle, Priming, Listening, Thinking, Speaking, Closing, NeedsTap }`. The mic obeys one rule — `micLive = (state == Listening) && speakerGate.quiet()`. Assume this voice/turn machinery is **good and stays** — this request is about the *content and cost of each LLM turn*, not the state machine.

What the barista can do (and must keep doing):
- **Dial-in coaching** grounded in the user's real shots: "that shot was sour, go finer," referencing actual recorded dose/yield/time/pressure and prior ratings.
- **Tools** (46 of them, `src/barista/baristatools.cpp`): apply a grind change, look up any shot from full history (`query_shots`), manage coffee bags (`add_bag`/`update_bag`/…), add a bean from a **photo** (`open_bag_camera` → vision), reminders/maintenance, and three keyless web tools (weather/news/stock).
- **Memory**: it knows the current bean, recent shots, tasting feedback on this bean, saved user facts, due reminders.
- **Grounding**: it must NEVER invent shot numbers or a taste rating — those come from the local database, not the model's imagination.

Providers are **user-selectable and swappable**: Anthropic (Claude), Google (Gemini), OpenAI, OpenRouter, Ollama (`src/ai/aiprovider.cpp`). The architecture cannot assume a single provider.

---

## 2. The problem — the current architecture is "send the whole brain every turn"

Every conversational turn sends a **monolithic ~30,000-token prompt** to the LLM. Measured on-device (Gemini usage logs): `prompt: 30293`, `prompt: 30099`, … turn after turn, barely varying.

The prompt is **built once per session** (`AssistantOverlay.qml` assembles `persona + sessionContext + dataBlock`) and **reused unchanged on every turn** (`AIConversation::followUp()` → `AIManager::analyzeConversation(m_systemPrompt, …)`). Rough composition of the ~30k:

| Part | ~tokens | Static across turns? | Needed for a given turn? |
|---|---|---|---|
| Persona / instructions (identity, dialing framework, recipe semantics, tool-usage rules) | ~5k | Static | Some, rarely all |
| **Dial-in data block** (10 recent shots, best shot, grinder history, bean freshness, profile guidance, tasting feedback, saved user facts) | **~20k** | Stable within a session | Only for a dialing/shot question — a "good morning" needs **none** of it |
| Tool definitions (46 tools) | ~1.6k | Static | Almost never all at once |
| Conversation history + current utterance | ~2.5k | Grows | Yes |

Consequences:
- **Cost**: ~30k input tokens **per turn**, multiplied again by every tool round-trip within a turn (each tool call is a fresh full-context round-trip). A 10-turn chat re-sends the same ~20k data block ten-plus times.
- **Latency**: on a provider that **caches** the static prefix (Anthropic's `buildCachedSystemPrompt` caches the system prompt at ~90% off), a cache hit is cheap and fast. On a provider with **no caching in our code** (Gemini — `GeminiProvider` has no cache path), the full ~30k is reprocessed every turn → 2–5 s per turn, worse with tool round-trips, ~10 s on the first (cold) turn.
- **Architecture smell**: the model is handed the entire knowledge base every turn and asked to find what's relevant itself. That is the wrong division of labor.

We shipped a stopgap (per-question gating of the *camera* and *web* instruction modules) — it proved a routing mechanism but only shaved a few hundred tokens. **The ~20k data block is the mass, and it still ships every turn.** We deliberately did **not** reach for provider-specific caching as the fix: it's provider-locked (helps Claude, not Gemini), lives in a shared merge-sensitive file, and can't be exercised off-device.

---

## 3. The core tension you must resolve

**Low cost pulls toward a minimal, question-scoped request. A rewarding natural discussion pulls toward rich context, memory of the session, and continuity.** A naive "just send less" degrades the conversation into a forgetful Q&A box; a naive "send everything for safety" is what we have now.

The design has to give the model **exactly what this turn needs — no more — while preserving the *feeling* of a continuous, knowledgeable companion.** Reconciling those is the heart of this request.

A guiding principle we believe in (adapt or challenge it): **the app owns FACTS, the LLM owns LANGUAGE.** Deterministic code should compute the espresso numbers, retrieve the relevant shot, and hold session memory; the LLM should reason and speak. The more the app pre-digests, the less the model must be told.

---

## 4. Specific design questions

Answer these concretely, grounded in §6.

1. **Turn-scoped context assembly.** How should each turn decide *what context to include* — which shots, which feedback, which facts, which tools — so a greeting carries ~identity-only and a dialing question carries the full dial-in set? Crucially: **without a pre-classification LLM round-trip** (that would re-add the latency we're removing). Local intent routing? Retrieval over a small embedded index? A tiered/progressive-context scheme? Name the mechanism and its failure modes (what happens when the router guesses wrong and the model lacks a fact it needed).

2. **Facts vs. language split.** Where exactly should the boundary sit? Which of today's "dump it in the prompt" blocks (recent-shot table, grinder history, tasting feedback, saved facts, profile guidance) should become **on-demand retrievals / tool results** instead of always-on prose, and which are cheap/central enough to keep resident? Give a concrete target composition for a typical dialing turn (aim: what token budget, made of what).

3. **Session continuity without resending history.** How does the barista stay *continuous* — "as I said, go finer," "back to that 1:2.5 we set" — while not paying to resend the full transcript every turn? Rolling summary? Structured session-state object the app maintains and injects compactly? Provider-native conversation state where available? How do we keep it grounded (the summary must not drift from the real data)?

4. **Provider-agnostic statefulness.** Given providers differ wildly (Anthropic prompt caching with a 1h TTL; Gemini implicit/explicit context caching; OpenAI; local Ollama), what's the right *portable* strategy so the win doesn't depend on which model the user picked? Should the architecture lean on a stable-prefix + cache-breakpoint layout it can exploit where available and degrade gracefully where not? Or avoid caching entirely in favor of just-send-less? Recommend one.

5. **Tool-round-trip minimization.** Each tool call today is another full-context round-trip (a turn can be 3–4). How do we cut that — pre-fetching the obvious context so common questions need zero tools, parallel tool calls, a cheaper "planner" step, or letting the app answer deterministic questions ("what ratio is 18→48?") without the model at all?

6. **The natural-discussion layer.** What makes it *rewarding*, not just cheap and correct? Consider: interim acknowledgments to fill processing gaps ("let me look at that trace…"), proactive-but-not-pushy surfacing (a due reminder, a new-bean nudge) without bloating every turn, brevity/pacing of replies for a spoken medium, and personality/memory that make it feel like the same barista across sessions. How does the cost architecture *support* rather than fight this?

7. **Migration path.** We can't stop-the-world rewrite. Give an **incrementally-shippable** sequence — each step independently testable and reversible — from today's monolith to the target. We are already mid-way through one such increment (per-question module gating). Order the steps by (impact ÷ risk), and flag which steps need on-device testing because they can't be validated on a dev machine (no live model, Android-only voice/camera).

---

## 5. Constraints & non-negotiables

- **On-device Android tablet** at the machine; the local data (shots, beans, tasting feedback, facts) is in **SQLite** on the device. Retrieval can be local and cheap.
- **Latency is felt** — it's a spoken turn-taking loop. A design that adds a round-trip to save tokens is usually a bad trade.
- **Provider-swappable** — must work acceptably on **both** Gemini (no caching today) and Claude (caches), and not fall over on OpenAI/Ollama.
- **Grounding is sacred** — no invented shot metrics, ratings, or bean facts. Real data only.
- **Do not regress the voice/turn state machine** (`BaristaConversation`) — it's good; build around it.
- **Reuse-first** — build on the seams in §6; call out exactly which existing functions/files each part of your design touches. No greenfield rewrite.
- **Keep every capability** — dialing coaching, all 46 tools, photo bean-add, memory, proactive surfacing. Cheaper, not lesser.

---

## 6. What exists today (grounding — build on these seams)

| Concern | Where | Notes |
|---|---|---|
| Turn/voice state machine | `src/barista/baristaconversation.{h,cpp}` | The 7-state loop; owns the mic. Keep. |
| Turn dispatch (live path) | `qml/assistant/AssistantOverlay.qml` `onTurnRequested()` | Calls `beginSession`/`followUp`; **already routes through `_scopedSystemPrompt()`** (the stopgap gating hook — extend this). |
| System-prompt assembly | `AssistantOverlay.qml` (`persona`, `sessionCtx`, `block` → `_coreSystemPrompt` + `_promptModules`) | Where "what goes in the prompt" is decided today. |
| Per-turn prompt override seam | `AIConversation::setSessionSystemPrompt()` (`src/ai/aiconversation.{h,cpp}`) | Lets a turn send a different system prompt without wiping history. Load-bearing for tailoring. |
| Turn send / provider call | `AIConversation::followUp()` → `AIManager::analyzeConversation()` → `AIProvider` | `src/ai/aimanager.cpp`, `src/ai/aiprovider.cpp`. |
| The ~20k data block builder | `AIManager::requestBaristaContext()` (`aimanager.cpp:1702`) + `BaristaContextBuilder` (`src/barista/baristacontextbuilder.cpp`, `buildBeanBlock`, `m_beanBlock`, `m_profileBlock`) | Built once per session; the mass to tier/retrieve. |
| Tools (46) | `src/barista/baristatools.cpp` (`buildTools()`); dispatched via `m_toolExecutor` | Tool defs + descriptions; gating candidate. |
| Providers & caching | `src/ai/aiprovider.cpp`: `AnthropicProvider::buildCachedSystemPrompt` / `messagesWithCachedFirstUser` (caches system prompt + first user msg, 1h TTL). **`GeminiProvider` (~line 1737) has NO caching.** OpenAI/OpenRouter/Ollama vary. | The provider asymmetry that makes Gemini slow. |
| Coffee-science corpus | `resources/barista/coffee_knowledge.json` (82 KB, ~21k tokens) | **Not** injected into the prompt — accessed via tools already. A model for "retrieve, don't dump." |
| Local data | SQLite (`ShotHistoryStorage`, bean storage, `assistant.db` for feedback/reminders/facts) | On-device; cheap to query/retrieve. |
| Fork reuse index | `.fork/INDEX.tsv` | Grep it before proposing anything new. |

**Attach when sending this request:** the current on-device prompt-size logs (the `Gemini usage — prompt: ~30293` lines and the `[barista] scoped prompt chars=…` instrumentation), so the design is anchored to measured reality.

---

## 7. Deliverable we want back

1. **Target architecture** — a clear picture of how a turn is assembled and sent, with the facts/language boundary drawn, the continuity mechanism named, and a **target token budget** for each turn type (greeting / dialing question / tool action).
2. **The reconciliation** — an explicit account of how it stays a rewarding, continuous discussion while being cheap (§3, §4.6).
3. **Provider strategy** — the portable statefulness/caching approach (§4.4).
4. **Migration path** — incrementally-shippable steps ordered by impact ÷ risk, each independently testable, flagging on-device-only validation (§4.7).
5. **Reuse map** — which existing seams (§6) each piece builds on; what (if anything) genuinely must be new.

Be concrete, be opinionated, and stay grounded in what's already there.
