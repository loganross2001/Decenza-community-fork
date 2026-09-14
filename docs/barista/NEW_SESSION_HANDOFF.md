# Decenza barista — NEW SESSION HANDOFF (2026-09-13)

Canonical repo: `/Users/christopherpalestro/Decenza-fork`, branch **`feat/barista`** (the ONLY checkout;
ignore stale `~/Decenza*`). Build/test on THIS Mac via cmake/ctest in
`build/Qt_6_11_2_for_macOS_Debug` (NOT the Qt Creator MCP — the fork brief overrides CLAUDE.md). Android
APK: `export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home` then
`./build.sh --target ANDROID --dev`, then zipalign + apksigner v2-only w/ `~/.android/debug.keystore`
(cert `5c7458da…`, `--v3-signing-enabled false --min/max-sdk 28/34`) — see
`docs/CLAUDE_MD/PLATFORM_BUILD.md [barista-fork]`. Commit/push only when asked (fork flow: commit
feat/barista → push origin+backup → FF origin/main). **Call the advisor before substantive work and
before declaring a step done.**

## Session 2026-09-13 (cont.) — voice filler + tea stop-at-weight + profile filter

Three changes landed on `feat/barista` this session (committed at the end; see git log for hashes),
all desktop-built + tested, all deployed to the tablet (APK `Decenza-filter-vc3525243`):

1. **Voice filler (A+B)** — cut filler, not length. **A** (`aiprovider.cpp` `GeminiProvider::onAnalysisReply`):
   drop the round-1 tool lead-in ("Checking the shot detail now…") instead of gluing it onto the post-tool
   answer — mirrors the Anthropic end-state; dead-air is covered by the non-verbal thinking-loop pulse. **B**
   (`AssistantOverlay.qml` `_scopedSystemPrompt`): a tight SPOKEN-STYLE contract + one contrastive example
   appended in the recency slot (rules were buried ~45k chars deep, after the 20k data block). Owner picked
   "A+B then measure"; auto-trim (C) deferred unless filler survives. **Owner on-device read still owed.**
2. **Tea stop-at-weight fix** (`profile.h` `isEspressoBeverageType` + `profilemanager.cpp` `targetWeight()` +
   `tst_profilemanager.cpp`). A persisted espresso brew-by-ratio anchor survived a switch to a tea profile and
   cut the steep short at ratio×dose (~48 g on-device). Fix: a ratio anchor resolves ONLY for espresso-class
   profiles; non-espresso honors its own `target_weight` (0 = no stop, matching de1app). Regression test
   red-check-proven. **Upstream bug filed: Kulitorum/Decenza#1941.** Owner tea-shot confirmation owed.
3. **Profile beverage filter** (`ProfileSelectorPage.qml`) — a Coffee · Tea & Water · Maintenance · All tab row
   above the search on the shared picker/Settings surface, driven by `beverage_type`, AND-combined with the
   ownership filter; search now works in every view. Defaults to Coffee on open. **Upstream request filed:
   Kulitorum/Decenza#1942.** Owner visual check owed (QML compiles ≠ renders).

de1app reference clone (for the tea-fix comparison) was at `scratchpad/de1app` — stop-at-weight is profile-owned
there; tea profiles ship `final_desired_shot_weight 0`. NEXT: owner on-device thumbs-up on all three, then open
the upstream PR for the tea fix off #1941.

## Repo state (PRIOR baseline — this session's commits sit on top; 0 behind upstream)

Before this session (all COMMITTED + PUSHED; origin/main == feat/barista == `313fb5f2`):
1. **`c09f7675` — voice = whole reply** (`speakInChunks` default flipped ON→OFF). The chunked+C1+prefetch
   stack stays in-tree but DORMANT behind `speakInChunks`. Owner confirmed whole-reply on-device before commit.
2. **`35e80b41` — merge `upstream/main` (`bf88aff6`)** into feat/barista. Was 21 behind; now 0 behind / 369
   ahead. 7 conflicts resolved (rerere-recorded): shothistorystorage (upstream added ZERO migrations — pure
   `qDebug`→`DIAG_` logging refactor; kept fork's superset migration numbers + sequential gates), AI seams
   (`analyzeConversation` signature UNION = fork bools `webSearch/clientTools/streaming` + upstream trailing
   `qint64 shotId`; grafted upstream's `AIOperationLog` diagnostics-correlation onto the fork body),
   aiprovider (kept fork goodbye-turn logic + adopted `PROVIDER_DEBUG`), locationprovider (kept privacy gate),
   tst_aimanager (additive test union). `versioncode.txt`=3619 (fork, `.gitattributes merge=ours`).
   **⚠️ tsnet pin bumped → `decenza-v1.94.1-6`: a stale build dir needs `cmake -DTSNET_TAG=decenza-v1.94.1-6 .`
   ONCE (this Mac's is already reconfigured).**
3. **`5b7d9711` — roast-band bug (#4) FIXED.** `CoffeeKnowledgeBase::bucket()` bucketed by first-substring-hit
   over `light,dark,medium`, so "medium-dark" matched "dark" → got the DARK roast_specific_path (extremes carry
   OPPOSITE moves). Fix: bucket() now resolves by LONGEST matching keyword (+ explicit `med-dark`/`med-light`
   keywords since the "med" abbrev loses on length) → compounds fall to neutral medium. Regression test proven
   red-without-fix. Owner chose compounds→neutral-medium.
4. **`313fb5f2` — extraction live-coach genre FIXED.** The espresso cue's SPOKEN line is swapped for a
   CoachPhrasebook model variant; the phrasebook prompt said "always say WHAT to do to WHICH part" → for
   extraction (no live action) the model invented next-shot regrind advice. Fix: split the phrasebook prompt
   by genre — STEAM stays live-actionable; EXTRACTION (no-puck/channeling/flow-fast/flow-slow) is now
   OBSERVE-ONLY. Also softened 2 deterministic lines (no-puck, flow-slow) to observation.

Full suite = 2 pre-existing reds only (`tst_qmlregistration`, `failonwarning_lint` — the latter = 4 fork voice
test files missing `QTest::failOnWarning()`). Both unrelated to any of the above.

## ⚠️ DEPLOY GAP — the owner's tablet is running an OLDER APK

The tablet has **`Decenza-wholereply-vc3523647.apk`** (whole-reply voice). The **roast fix (`5b7d9711`) and the
genre fix (`313fb5f2`) are NOT on the tablet.** To verify them on-device, build+install a fresh `--dev` APK
(DE1 BLE gotcha: force-stop app + power-cycle DE1 before every `adb install -r`).

## 📱 adb / tablet access — CONNECTION LEFT LIVE for the next session

**As of 2026-09-13 the tablet is CONNECTED and reachable at `192.168.189.186:34983`, and the pairing is
trusted (this Mac).** The adb server is a persistent background daemon, so a fresh Claude session should find
it STILL connected. **Next session: FIRST run `adb devices` — if the tablet shows as `device`, you're already
in, no pairing needed.** adb binary: `/Users/christopherpalestro/Library/Android/sdk/platform-tools/adb` (NOT on
PATH). Two entries (IP + mDNS name) = same device → target with `-s 192.168.189.186:<port>`.

If it has dropped (tablet slept / WiFi-debugging toggled / **port ROTATES** on toggle-or-reboot): owner enables
Wireless Debugging (screen awake) → `adb mdns services | grep _adb-tls-connect` for the new port → `adb connect
192.168.189.186:<newport>`. Only if that is unauthorized/refused is a fresh pair needed: owner taps "Pair device
with pairing code" and sends the pairing IP:port + 6-digit code → `adb pair 192.168.189.186:<pairport> <code>`
(ignore the adb "protocol fault" warning — it still pairs; then connect to the CONNECT port, not the pair port).

Logs: `/sdcard/Documents/Decenza/logs/` (`debug.log`, `barista-diagnostics.log`) — `adb pull` works (shared
storage). Barista-diagnostics format: `HH:MM:SS.mmm #seq [channel] event key=val` (channels: voice, conv, stt,
mic, tool, coach, gate). Backup KB db under `/sdcard/Documents/Decenza Backups/BaristaKnowledgeBackups/`.

## 🔴 TOP PRIORITY NEXT — VOICE UX (from on-device log diagnosis 2026-09-13)

Pulled the owner's 09:07–09:10 session logs and quantified his "bad experience" report. **These outrank the
remaining backlog.** Full detail in memory [[decenza-barista-voice-streaming]].

- **FILLER is the dominant problem — NOT length per se (owner correction 2026-09-13).** A long answer is FINE
  when it carries real substance; what's wrong is the PADDING — throat-clearing, restating the question,
  spec-sheet recitation, hedging, saying the same thing multiple ways. Spoken replies ran up to 1012 chars →
  **35–77-SECOND monologues** (session: 19 replies ≥400 chars, 18 audio clips ≥30s, 7 ≥45s) and much of that
  was filler. Gemini IGNORES the L1/L2 persona brevity rules. The filler drives BOTH the pain AND the latency
  (whole-reply synth scales with chars). **Do NOT cap length; cut filler/density and let warranted depth
  through.** See the "discipline is on numbers/filler, NOT depth" rule in [[decenza-barista-voice-streaming]].
- **LATENCY (measured):** model TTFT (stt-final→speaking) 5–10s; whole-reply ElevenLabs synth
  (speak_start→audio) **2.7s @191ch / 7.7s @533ch / 9.7s @674ch** (scales with length — whole-reply). Long turn
  ≈ 20s to first audio, then 46s of speech.
- **STT CUTOFF confirmed:** a turn finalized `heard="…that will likely push the"` — truncated mid-sentence.
  Android SpeechRecognizer endpoints on ~2s silence when the user pauses to think. 26× code=5 + 18× code=7
  errors/restarts in the session (recogniser unstable).
- **STOP button:** every reply auto-goes Speaking→listening; `tap()` during Speaking = barge-in then drains to
  Listening (`baristaconversation.cpp:244`). "Stop" stops the audio but REOPENS THE MIC instead of going quiet.

### ✅ VOICE #1 (filler) — Levers A+B BUILT 2026-09-13 (desktop-green, NOT committed, NOT on-device)

Owner chose "cut filler, not length" via **A + B, then measure** (rejected the auto-trim safety net C for now).
Diagnosis grounded in the real 09-xx session (pulled `ai_logs/response_*.txt` + `qa_*.txt` off the tablet):
the persona ALREADY bans this filler; Gemini ignores buried rules. Three confirmed causes — (1) pre-tool
narration glued to the answer, (2) long replies are numbered-list/markdown monologues violating "ONE change" +
"no lists" (the "ONE change" rule sits ~45k chars deep, after the 20k data block), (3) throat-clearing openers.

- **Lever A (code, Gemini-only): `GeminiProvider::onAnalysisReply` (aiprovider.cpp ~2160)** — the round-1 tool
  lead-in ("Checking the shot detail now…", "Let me pull up your history…") was buffered into `m_accumulatedText`
  and prepended to the post-tool answer → glued into one spoken utterance (confirmed on-device: `speak_start
  chars=396 say="Checking the shot detail now... The flow trace looks…"`, and no-space glue `"…community
  data.On similar…"`). Fix: `if (m_toolRounds > 1) m_accumulatedText += text;` — DROP the first round's lead-in,
  mirroring the Anthropic end-state (`finalizeConversationResponse` comment: "final analysisComplete carries only
  the post-tool answer"). NOT speak-early via interimText: interimText is Anthropic-only wired (aimanager.cpp:534),
  and dead-air is already covered by the non-verbal `thinking_loop sound=pulse` (verified in the log), so the
  verbal narration is pure redundant filler. No empty-final fallback (existing 2219 guard handles it). Note: this
  drops the lead-in from BOTH the spoken and displayed reply.
- **Lever B (prompt): closing SPOKEN-STYLE CONTRACT at the end of `_scopedSystemPrompt` (AssistantOverlay.qml
  ~1785)** — appended in the recency slot (last thing before the conversation, every turn incl. casual) since the
  brevity/one-change/no-list rules are buried where Gemini under-weights them. Tight recap + ONE contrastive
  example (the exact 3-bold-header numbered-list failure → the same advice as one spoken lead move with the why
  kept) because the model follows examples > rules. Frames depth-is-welcome so warranted depth isn't clipped.
  Covers turn 1 (onTurnRequested→_scopedSystemPrompt→beginSession, verified).
- **Verified desktop:** Decenza app + tst_aiproviders + tst_aimanager build exit 0; `ctest` — aiproviders,
  aimanager, baristaconversation, closeintent, speechchunker, speechnormalize, anthropicstreamparser,
  respondtextextractor all PASS. QML compiled through qmlcachegen (app target green).
- **OWED — on-device measure (the point of A+B):** build a fresh `--dev` APK, install (DE1 BLE gotcha), owner runs
  a real Gemini session, pull `barista-diagnostics.log` + `ai_logs/`, check: no glued "let me look…" openers, no
  numbered-list monologues, replies shorter WITHOUT losing substance. Add Lever C (auto-trim) only if filler
  survives. adb live at `192.168.189.186:34983` (port rotates).

**Three fixes, leverage order:**
1. **Cut FILLER, not length (HIGHEST leverage; fixes monologues + latency together).** The reply padding
   (throat-clearing, restating the question, spec-sheet recitation, hedging, redundant rephrasing) is the
   target — a genuinely substantive long answer is allowed. Gemini won't self-limit on persona rules alone, so
   this likely needs stronger/programmatic enforcement. ⚠️ **APPROACH IS OPEN — owner did NOT like the first
   options offered** (they were LENGTH-CAP framed: speak-a-lead+show-full-text / hard cap / "want the details?"
   — all WRONG because they cap length rather than cut filler). Re-explore the shape fresh with the owner:
   anti-filler prompt discipline, tighter density, cutting spec-sheet recitation, possibly brevity-via-provider
   (owner rejected switching the coach to Claude earlier, but it could resurface). Tie to L3 payload shrink
   (less to recite) and the existing L1/L2 work.
2. **STT: stop cutting the user off** — tune VoiceInput recognizer silence/endpoint + error-restart handling.
3. **Stop = quiet** — make Stop go to NeedsTap (silent, tap to resume), not reopen the mic.
⚠️ The voice FSM (`baristaconversation.cpp`) is delicate and its tuning was "confirmed by log line X" — read the
logs before editing it; don't change timeouts/transitions blind.

## Remaining backlog (owner said "complete 1/2/3" = the first three below, but voice UX now outranks them)

- **#2 advice-tracker transfer advice** — owner-decided scope "BOTH, labeled differently": exact-bean patterns
  stated strong + same-type (roast/origin/process) patterns HEDGED as transfer. Enhance
  buildTrackRecord/classifyTier/persona. NOT built. See [[decenza-barista-coaching-loop-trace]].
- **#3 verbosity L3 (shrink ~20k dial block)** — MEASUREMENT + tiered cut proposal DONE this session (offline):
  `buildAdvisorContextBlocks` emits 7 blocks; heavy = `grinderCalibration` (~33-row table, `Include` on BOTH
  barista call sites `aimanager.cpp:1504` [default] + `:1826`; MCP already OMITS it #1164) and `dialInSessions`
  (5 sessions site-1 / 3 site-2). Barista has NO on-demand calibration tool. **Tier1** omit calibration from
  barista turns (+ add a fetch tool) — cleanest, precedented. **Tier2** unify+trim historyLimit to 2–3.
  **Tier3** per-field trim. CUT gated on: owner listening to L1/L2 first + exact byte numbers from device logs
  (prompt-size lines). Given Gemini ignores brevity, the spoken-reply cap (voice #1) may matter more than L3.
- **#5 T4 detector** — optional pressure-spike-on-ramp detector in ShotAnalysis, corpus-FP-audited. Not started.
- **Log-marker debt (~90 pre-existing fork bare-logs)** — upstream's stricter `check_log_markers.py` (pulled in
  by the merge) now flags them; non-required gate, NONE merge-introduced. Mechanical DIAG_ cleanup, deferred.

## On-device confirms still OWED (owner-only)

- Roast fix (#4) real Q&A (dark vs medium-dark) — needs a build with `5b7d9711` on the tablet.
- Extraction genre fix — actual SPOKEN extraction cues (phrasebook runs on Gemini, no local key) — needs
  `313fb5f2` on the tablet.
- Whole-reply voice (already installed) — one continuous response, no sentence pauses.
- Inspect a real `coach_plans` row (ledger capture) — tap "Back up now" after a qualifying dial-change turn,
  then pull the KB backup db.

## Memory topic files
`decenza-barista-voice-streaming` (voice whole-reply + upstream merge + **log diagnosis**),
`decenza-extraction-live-coach-genre` (genre fix), `decenza-barista-coaching-loop-trace` (ledger + roast fix),
`decenza-canonical-folder`, `decenza-fork-docs-lag-code`, `decenza-deploy-de1-ble-gotcha`.
Full status: `docs/barista/WORK_PLAN.md`. DoR: `docs/barista/Barista_Coaching_Loop_And_Trace_Fable5_DESIGN.md`.
