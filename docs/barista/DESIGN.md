# Decenza Proactive Voiced Barista — Design & Build Plan

**Status:** design (no functional code yet). **Date:** 2026-07-03.
**Nature:** a private, modular fork feature — must stay additive so upstream `main` can keep being merged with near-zero conflict.

---

## 1. Goal & principles

Turn the app's AI from *reactive search* (a "Discuss" button you must open) into a *proactive, voiced barista* across the whole shot lifecycle:

- On machine-wake / Espresso tap: **greet by name**, ask *"same coffee as last time?"* (update settings if new).
- **Propose plan/recipe improvements** with one-tap apply, from the user's own history **and** community docs, with **clear attribution** of every recommendation's source.
- **Verbal flags** during the shot and steam (already built).
- **Typed *and* voice** interface; a **nameable** assistant; **choosable pleasant voices**; a **custom bell**; **beginner/amateur/pro** verbosity + mute; a **smooth end-of-shot** close-out (no re-clicking/re-typing).

Design ethos (user's words): suggestions not dashboards — one card, one coffee-language sentence, one tap to act, one tap to ask why. Keep it about coffee, not engineering.

## 2. Key reframe: ~70% is already built, just not stitched

| Capability | Where it already lives |
|---|---|
| Structured recommendation (grind/dose/temp/expected/why) | `structuredNext` JSON contract; parsed by `AIManager`; rendered by the post-shot coaching card |
| "Same coffee?" + best recipe per bean×person | bean memory (`requestBeanRecipe`, `buildBeanBestShotBlock` in `src/ai/dialing_blocks.cpp`) |
| Named, multi-user identity | barista roster (name/color/avatar/`prefs_json`) |
| Verbal flags during shot / steam | `src/ai/liveshotcoach.*` + `livesteamcoach.*` → speak via `AccessibilityManager` (`QTextToSpeech`) |
| Multi-turn memory, prompt caching | `src/ai/aiconversation.*`, Anthropic provider cache |
| Reasoning inputs | `buildUserPromptObjectForShot`, `enrichUserPromptObject` |

**Genuinely new work:** (a) speech input, (b) a nicer/own voice output, (c) the conversation **orchestrator**, (d) the portable **knowledge store**, (e) the bell. Everything else is integration.

## 3. Decisions (fixed)

1. **Primary device = the DE1's Samsung Android tablet** (modern, up-to-date → Android 13/14, on-device `SpeechRecognizer` + Google/Bixby neural TTS). Native, on-device, offline-capable is the default; cloud is opt-in.
2. **Knowledge base = BOTH**: proactive continuity from existing history now, **and** a portable, user-relocatable, backed-up store that ingests community docs.
3. **Posture = HYBRID**: local/native voices + on-device reflexes = free/offline/private default; cloud premium voice + richer Claude planning = opt-in.
4. **⭐ Overriding constraint = MODULARITY for a private fork.** Keep merging upstream `main`; the feature must be maximally additive so rebases/merges stay trivial.

## 4. Architecture — 5 layers

1. **Identity & Knowledge** — barista roster (who) + `shots.db` history (read-only) + bean memory + profile KB + **new `assistant.db`** (community docs, persona, plans).
2. **Reasoning (two-speed — already the pattern)** — Claude = pre-shot planner + teacher (async, cached); local C++ = real-time reflexes (the live coaches). Voice never gates the ms path.
3. **Conversation Orchestrator (new)** — one C++ state machine driving *both* typed and voice off one state.
4. **I/O (mostly new)** — voice-out (own `QTextToSpeech`), voice-in (JNI STT behind an interface), typed chat, bell.
5. **Persistence & portability** — selectable `assistant.db` path, backup bundle, provenance on every recommendation.

### 4.1 Module pattern (the modularity core)

All new code in additive locations only:

```
src/barista/          # BaristaModule, AssistantOrchestrator, VoiceEngine(+impls), KnowledgeStore, AssistantSettings, Persona
qml/assistant/        # AssistantOverlay, ChatSheet, PlanCard, CoachingCard (extracted), PersonaSettings
android/src/.../voice/DecenzaVoice.java   # new file, no edits to upstream Java
cmake/barista.cmake   # target_sources + assistant.qrc + DECENZA_BARISTA option
resources/assistant.qrc
```

Facade `BaristaModule::install(QQmlApplicationEngine&, Deps)` receives **raw pointers** (`MachineState*`, `DE1Device*`, `AIManager*`, `ShotHistoryStorage*`, `Settings*`, `AccessibilityManager*`) and does exactly **one** `setContextProperty("Barista", ...)`.

**Feature flags:** CMake `option(DECENZA_BARISTA ON)` (build a vanilla binary to bisect upstream regressions) **and** a runtime kill-switch in the module's own `QSettings` (this runs a kitchen appliance — a bad assistant state must never brick coffee).

### 4.2 Measured upstream conflict surface & tactics

Ranked by upstream churn (Mar–Jul 2026). **Total upstream edit budget ≈ 6 lines**, each tagged `// [barista-fork] hook` so rebase = re-add the marked line.

| File | Churn | Tactic |
|---|---|---|
| `versioncode.txt` | 330 | `.gitattributes`: `versioncode.txt merge=ours` — never think about it |
| `src/main.cpp` | 149 | ONE line: `BaristaModule::install(engine, deps)` near `setContextProperty` block (~2460) |
| `CMakeLists.txt` | 138 | ONE line: `include(cmake/barista.cmake)` |
| `src/controllers/maincontroller.*` | 105/42 | **Touch zero** — module gets pointers via `install()`, not hung off MainController |
| `qml/main.qml` | 97 | ONE `Loader { source: "qrc:/qml/assistant/AssistantOverlay.qml"; active: Barista.enabled }` in the overlay stack (~1900–2150) |
| `src/history/shothistorystorage.*` | 78 | **Touch zero** — separate `assistant.db`; `shots.db` read-only via existing async APIs |
| `src/core/settings.*` | 76/44 | **Touch zero** — module's own `AssistantSettings` over its own `QSettings` group |
| `qml/pages/PostShotReviewPage.qml` | 75 | **HIGHEST LIABILITY.** Confirmed: `pr/proactive-coaching` inserted **+647/-0**. Extract to `qml/assistant/CoachingCard.qml` + ~5-line `Loader` hook |
| `qml/components/BrewDialog.qml` | — | `pr/bean-memory` inserted **+176/-0** — extract to `qml/assistant/BeanRecipeCard.qml` + hook |
| `src/ai/aimanager.*`, `shotsummarizer.*` | 66/38 | Consume public/Q_INVOKABLE only; **duplicate a ~30-line `structuredNext` parser** in the module rather than widen upstream visibility |
| `de1-qt.qrc` | — | Don't touch — ship `resources/assistant.qrc` |

### 4.3 Separate `assistant.db` (emphatic)

New SQLite file, own connection + worker thread (copy the `SerialDbWorker` *pattern* from `src/history`, don't link its internals), own trivial `schema_version`. Rationale: decouples from `shots.db`'s hand-rolled linear migration chain (the worst possible conflict surface); makes the KB independently relocatable + backup-bundleable (requirement); upstream backup/restore never needs to know about us. `shots.db` is read **read-only** through existing async query APIs. *(The `baristas` table already added to `shots.db` by `pr/barista-identity` is spent risk and rides upstream backup — fine. Draw the line there: nothing new in `shots.db`.)*

Tables: `persona`, `plans` (proposed/applied + provenance JSON, the audit + future outcome-learning signal), `kb_documents`, `kb_chunks`.

### 4.4 Conversation orchestrator

`AssistantOrchestrator` (C++, `src/barista/`). Tap, keyboard, and `VoiceEngine::finalText` all funnel into `handleUtterance(text, source)`; QML renders state, voice narrates state.

```
Dormant ─(MachineState Sleep→Idle | espresso tap)─▶ Wake
Wake:  bell + "Morning, {barista}."                         [LOCAL]
  └▶ ConfirmBean: "Same {bean} as yesterday?"               [LOCAL: bean memory]
        ├ yes ─▶ ProposePlan
        └ no  ─▶ existing bean picker ─▶ ProposePlan
ProposePlan: plan card + spoken summary + attribution
  [ASYNC Claude → structuredNext; OFFLINE → local best-recipe plan, provenance=history]
  ├ "apply" ─▶ Apply (write grind/dose/temp dial memory — same writes the coaching card does)
  └ "keep"  ─▶ Armed
Armed: orchestrator SILENT; live coaches own the voice channel (untouched); read passively for recap
  └(shot ends, phase→Idle)─▶ CloseOut
CloseOut: "How was it — sour, balanced, bitter?"  [3 buttons + voice]
  → one-tap taste write (reuse existing enjoyment+note path)
  → request next structuredNext, store as tomorrow's plan in assistant.db
  → "Noted. Grind half a step finer next time. Enjoy." ─▶ Learn ─▶ Dormant
```

- **Latency split:** greet/confirm are 100% local (instant); the Claude plan is fired in parallel at Wake and lands mid-conversation. The user never waits on the network to be greeted. AI-off degrades gracefully to the local best-recipe card.
- **Reuse `structuredNext` unchanged**; compose the prompt with existing builders + a module-owned system-prompt suffix.
- **Verbosity** (beginner/amateur/pro/mute) = template table keyed by (cue, verbosity) + a one-line length instruction for Claude turns. Mute = card-only, still fully functional.

## 5. Voice

- **TTS — no JNI needed.** `QTextToSpeech` on Android already fronts the native engine incl. neural voices; `availableVoices()` is the voice picker for free. Give the module its **own second `QTextToSpeech` instance** (not routed through `AccessibilityManager`, keeping the persona channel separate from the live-coach cue channel). Bell = `QSoundEffect` + bundled `.wav`.
- **Speech arbiter:** the assistant must never talk over the live coaches. One-directional: it stays silent during Espresso/Steam phases (except its own end-of-phase lines) and defers queued utterances. *(Spike two `QTextToSpeech` instances on the tablet week one — verify queues don't interleave badly.)*
- **STT — JNI behind a pure interface.** `VoiceEngine` (interface) → `AndroidVoiceEngine` (new `DecenzaVoice.java` wrapping `android.speech.SpeechRecognizer`, driven on the Android UI thread) + `DesktopVoiceEngine` (stub, typed only). Copy the JNI idiom from `src/usb/androidusbhelper.cpp`. Permission: one line `RECORD_AUDIO` in `AndroidManifest.xml.in` + runtime `QMicrophonePermission` (Qt 6.5+) requested lazily on first voice enable.
- **Trigger — no wake word** (a grinder would wreck it). On `Sleep→Idle` / espresso tap: bell → greeting → open ~8 s listen window (restart after each question). A persistent press-to-talk mic button for everything else. Mic **hard-closed** during grind/brew. Short answers ("yes/no/same/apply") matched **locally** via a synonym table (no LLM round-trip for a yes); free text → Claude.

## 6. Knowledge store & attribution

- **Storage/portability:** `assistant.db` at a user-selectable path (default beside `shots.db`; path in module settings). "Export assistant data" = one zip (`assistant.db` + `docs/` originals + persona), independent of upstream's `DatabaseBackupManager`.
- **Community-doc ingestion — plain lexical retrieval, NOT embeddings** (over-engineering at home-barista corpus scale). Ingest `.md/.txt` (+ PDF→text) into `kb_documents` + `kb_chunks` (chunk by heading, ~500–800 tokens). Retrieval: SQLite **FTS5** if present in Qt's bundled Android SQLite (probe `PRAGMA compile_options`), else a trivial in-code BM25. Query = bean descriptors + profile + last-shot symptom keywords → top 3–4 chunks as labeled prompt blocks (`[COMMUNITY: "{title}" §{heading}]`), composing with Anthropic prompt caching. Cloud embeddings = later opt-in behind the same `KnowledgeStore::retrieve()` interface.
- **Attribution = a first-class `basis` field on `structuredNext`** (additive; unknown keys pass through upstream's generic JSON parser untouched):
  ```json
  "basis": [
    {"type": "history",   "ref": "shot#1412 (best rated on this bean)"},
    {"type": "community", "ref": "Dialing Guide — Fast blondes"},
    {"type": "profile",   "ref": "Blooming Espresso KB"}
  ]
  ```
  Constrain `type` to that enum; the orchestrator **validates refs** (community refs must match an ingested doc id — no hallucinated citations get voiced). The module's PlanCard renders chips ("From your history" / "From: {doc}"); voice speaks the short form. Every proposed/applied plan logs to `assistant.db.plans` with provenance.

## 7. Feasibility & top risks (verify in order)

1. **STT on the tablet** — *largely resolved:* modern up-to-date Samsung → Android 13/14, on-device recognition + neural voices available. Still confirm once on-device with `SpeechRecognizer.isRecognitionAvailable()` and a kitchen-ambience test. Isolated behind `VoiceEngine`, so a Vosk fallback can slot in if ever needed.
2. **The existing `pr/*` stack is the real merge bomb** (confirmed: 647 + 176 + 21 lines in hot files). The P0 extraction is **not optional**.
3. **Dual-`QTextToSpeech` contention** on Android — spike two instances week one; if queues interleave badly, serialize through one engine with per-utterance voice switching.
4. **Wake-time cold-cache latency** (Anthropic ~1 h prompt-cache TTL) — the local-greeting/async-plan split covers UX; measure real tablet Wi-Fi and let the state machine say "suggestion in a moment."
5. **FTS5 in Qt's Android SQLite** — cheap `PRAGMA compile_options` probe; BM25-in-code caps the downside.

## 8. Phasing (each shippable + rebase-safe)

- **P0 — Scaffold + consolidate (prerequisite).** Create `src/barista/`, `qml/assistant/`, `cmake/barista.cmake`, `BaristaModule::install`, flags, empty overlay `Loader`, `.gitattributes merge=ours`. **Extract** the +647 `PostShotReviewPage` insertion → `qml/assistant/CoachingCard.qml` and the +176 `BrewDialog` insertion → `qml/assistant/BeanRecipeCard.qml`, each behind a ~5-line `Loader` hook; **move** the proactive-coaching keys out of `settings_app.*` into `AssistantSettings`. Collapse the `pr/*` stack onto one integration branch. **Exit:** vanilla build (flag off) ≈ upstream; a full `git merge upstream/main` dry-run conflicts *only* on marked hooks + `versioncode.txt`.
- **P1 — Typed proactive concierge.** Orchestrator (Wake→…→CloseOut), card/tap/typed only. Greeting by name, "same bean?", local best-recipe + async Claude plan, one-tap apply, one-tap taste close-out. *Most of the felt value; zero audio.*
- **P2 — Voice out.** Own `QTextToSpeech`, persona (name/voice/pitch/bell), verbosity + mute, speech arbiter.
- **P3 — Voice in.** `VoiceEngine` + `AndroidVoiceEngine` JNI + mic permission, wake-window + press-to-talk, local yes/no grammar, free text → Claude.
- **P4 — Knowledge store.** `assistant.db`, doc ingestion UI, FTS/BM25 retrieval, `basis` provenance in card + voice, export bundle, relocatable path.
- **P5 — Premium opt-ins.** Cloud TTS voice, richer multi-turn planning, plan-outcome learning loop from `plans`.

## 9. Concrete P0 / P1 build plan (file-by-file)

### P0 (scaffold + consolidate) — no behavior change
- `cmake/barista.cmake` (NEW): `option(DECENZA_BARISTA ON)`; `target_sources(Decenza PRIVATE src/barista/*.cpp)`; register `resources/assistant.qrc`; `target_compile_definitions` for the flag.
- `CMakeLists.txt`: **+1** `include(cmake/barista.cmake)` `// [barista-fork] hook`.
- `.gitattributes` (NEW): `versioncode.txt merge=ours` (+ define the `ours` driver in git config).
- `src/barista/baristamodule.{h,cpp}` (NEW): `install(engine, Deps)`; empty `Barista` facade (just `enabled` for now) + `AssistantSettings`.
- `src/main.cpp`: **+1** `BaristaModule::install(...)` `// [barista-fork] hook`.
- `qml/assistant/AssistantOverlay.qml` (NEW, empty shell).
- `qml/main.qml`: **+1** `Loader` in the overlay stack `// [barista-fork] hook`.
- **Extractions (the real work):** `qml/assistant/CoachingCard.qml` ← the +647 lines from `PostShotReviewPage.qml`; `qml/assistant/BeanRecipeCard.qml` ← the +176 from `BrewDialog.qml`; each original page keeps a ~5-line `Loader`. Move `pr/proactive-coaching` settings keys → `AssistantSettings`.
- Collapse `pr/*` onto one integration branch; run the `merge upstream/main` dry-run; confirm exit criteria.

### P1 (typed concierge) — additive, in `src/barista/` + `qml/assistant/`
- `src/barista/assistantorchestrator.{h,cpp}` (NEW): the state machine; subscribes `MachineState` (Sleep→Idle, phase→Idle); `handleUtterance(text, source)`; local synonym matcher.
- `src/barista/planbuilder.{h,cpp}` (NEW): composes the pre-shot prompt from existing builders + module system-prompt suffix; local best-recipe fallback; ~30-line `structuredNext` parser (incl. `basis`).
- `qml/assistant/PlanCard.qml` (NEW): renders `structuredNext` + `basis` chips + Apply/Why (reuse existing Apply writes).
- `qml/assistant/ChatSheet.qml` (NEW): typed I/O bound to the orchestrator.
- Wire `AssistantOverlay.qml` to render orchestrator state.
- No upstream-file edits beyond the P0 hooks.

---

*Grounding & rationale preserved in agent memory `decenza-ai-coaching.md`. This doc is the durable anchor; update it as phases land.*
