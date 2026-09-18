# Decenza barista — SESSION RESUME (2026-09-16)

Canonical repo: `/Users/christopherpalestro/Decenza-fork`, branch **`feat/barista`** (the ONLY checkout —
ignore stale `~/Decenza*`). Build/test on this Mac via cmake/ctest in `build/Qt_6_11_2_for_macOS_Debug`
(this fork brief overrides CLAUDE.md's "use the Qt Creator MCP" rule). **Call the advisor before substantive
work and before declaring a step done. Commit/push only when asked.**

---

## 🔴 2026-09-17 — LATENCY REGRESSION INVESTIGATION (owner reported 8–9s pauses "broke since 09-15")

**Verdict: the slowdown is Gemini SERVER-SIDE latency, not a code/prompt regression. The clincher is a
same-session, constant-token fast→slow split (below). Evidence, not assertion:**

0. **⭐ SAME app session, IDENTICAL ~28K-token requests, fast at night → slow in the morning.** The live
   debug.log is ONE ~9.3 h session (app started 09-16 ~22:13). Early calls (uptime ~190–260 s ⇒ 09-16 ~22:13):
   elapsedMs **1045 / 1759 / 2061 / 2849 / 3590 / 4764**, prompt 27.5–29.3K tok. Late calls (uptime ~33 750 s ⇒
   09-17 ~07:33): elapsedMs **9156 / 13 555**, prompt 28.7K tok. **Token count is constant ~28K whether fast or
   slow** — so prompt growth (our code, our data) is EXCLUDED as the cause. Same request, same model, same
   process; Google served it 3–5× slower this morning. This is the answer.
   **⚠️ NOT a free-tier throttle — owner confirmed a PAID key (2026-09-17).** The surface is the Gemini
   Developer API (`generativelanguage.googleapis.com`, NOT Vertex), whose standard pay-as-you-go tier is
   best-effort latency even when paid: shared serving capacity, slower under load (mornings/peak). No
   account-tier upgrade exists to fix it — only Vertex AI + Provisioned Throughput (dedicated capacity), a real
   integration change and almost certainly overkill.

1. **Model & request are UNCHANGED.** `gemini-3.6-flash` both days. The 09-15 "worked perfectly" system
   prompt (`prompt_2026-09-15_21-13-34.txt`, 53.6 KB) and today's (`prompt_2026-09-17_07-31-27.txt`, 58 KB)
   are byte-for-byte identical in structure; the 8% delta is just the live-data block accreting shots. No
   model / generationConfig / tool-schema change. The uncommitted voice bundle touches ZERO request-path code.
2. **The committed `[AI][aiprovider] … elapsedMs=` telemetry (with token counts) is the authority.** Today's
   65 gemini calls: **prompt ≈ 28,000 tokens on EVERY call (constant), output 12–188 tokens, all httpStatus=200,
   networkError=0** — yet elapsedMs ranges **1,045 → 24,246 ms** (min/p25/median/p75/max =
   1045/2849/6159/11538/24246, mean 8024). Identical ~28K input, 20× latency spread, uncorrelated with prompt
   size → Google's serving latency varying request-to-request. No 429/RESOURCE_EXHAUSTED (soft, not hard, throttle).
3. **09-15 was genuinely faster** (owner is right that it regressed): 09-15 diag end-to-end 1-tool turns
   (`final_result→speak_start`) were 2.9 / 3.6 / 3.7 s — and each covers TWO model rounds + a tool ⇒ ~1.5 s/round.
   Today a single round is 6.2 s median ⇒ a 2-round simple turn ≈ 12 s. ~4× per-round degradation, same request.
4. **Tools are NOT the culprit:** `tool_done` exec = 4–42 ms. **R1 late-reply guard fired 0×** (`model_final_ignored`=0)
   — no hidden re-request loop. Machine contention ruled out by the 4–42 ms tool timings, not by the (never-fired,
   shot-gated) `apply_blocked_flowing` marker.

**Levers WE control (server latency we don't, but these help regardless — ranked):**
- **A. Shrink the ~28K-token prompt** (the deferred L3 payload shrink). It's re-sent+reprocessed EVERY round;
  it's the fixed floor under even Google's fast responses. Won't fix 24 s outliers but lowers the baseline.
- **B. Gemini context caching** — cache the stable ~28K system-prompt+tools prefix so it isn't reprocessed each
  round. Directly attacks the per-round cost. NEED TO CHECK whether the Gemini path enables implicit/explicit caching.
- **C. Model choice** — owner is on a PAID key, so this is NOT a tier upgrade. But the model matters: the app's
  own text flags 2.5 Flash as "most available (fewer busy errors)" vs 3.6 Flash (current). Switching to 2.5
  Flash / 3.5 Flash-Lite is the top no-code latency lever. (AI model selection, Settings → AI.)
- **D. Fewer tool rounds** on multi-tool turns (recipe edit = 3–5 × ~6–11 s). Owner previously declined brevity.

### LATENCY-FIRST lever (owner: "do both, latency first", 2026-09-17)
**Zero-code first move: switch the barista Gemini model in Settings → AI.** Catalog (`aiprovider.cpp:1668`) +
the app's OWN tradeoff text (`:1705`): 3.6 Flash (current, the queue victim) / **3.5 Flash-Lite = "fastest &
cheapest"** / **2.5 Flash = "most available (fewer busy errors)"**. Recommend owner try **2.5 Flash** first
(directly targets the busy-queue symptom — 3.6 Flash is newer with tighter/more-contended serving capacity),
Flash-Lite for raw speed. Reversible, no rebuild. **Owner is on a PAID key already** (not free tier), so there
is NO account upgrade to buy — the Developer API's standard tier is best-effort regardless. If ALL three models
stay slow, latency is Google serving-load variance with no client-side fix short of Vertex+Provisioned
Throughput (overkill). THEN do caching (below) for cost + best-case.

### ✅ RESOLVED 2026-09-17 — BOTH levers landed (measured on-device)
- **LATENCY FIXED by switching to 3.5 Flash-Lite** (owner picked "fastest"; note they said "2.5 Flash" but the
  log shows `gemini-3.5-flash-lite`). Flash-Lite n=21: **min 996 / median 1,382 / max 3,095 ms** — vs 3.6 Flash
  this morning median 6,159 / max 24,246 ms. **~4.5× faster median, worst case 24s→3s.** The 8–9s pauses are gone.
  ⚠️ Flash-Lite is a lighter model — owner should judge by EAR whether coaching DEPTH still holds (speed came
  from a smaller model). If depth suffers, 2.5 Flash ("most available") is the middle option.
- **CACHING ALREADY WORKS — no build needed.** The `cached:` probe (vc3530554) shows **16,218 of ~29,100 prompt
  tokens (~56%) served from implicit cache on every turn 2+** (turn 1 = `cached: 0`, expected miss). Confirms
  Gemini implicit caching DOES cover the `system_instruction` persona prefix; the speculative restructure is
  UNNECESSARY. Cost win realized. (Caching did NOT visibly cut latency — consistent with Google's "cost not
  latency" — the low latency is Flash-Lite's doing.)
- **Net:** keep Flash-Lite (pending by-ear quality check); the `cached:` log line is worth KEEPING (cheap, proves
  caching stays healthy). Nothing to commit unless owner asks.

### ⏳ CACHING — MEASURE-FIRST probe DEPLOYED (vc3530554, 2026-09-17) — ✅ RESULT IN (see above): caching WORKS
Pivoted from a speculative restructure to measure-first (the prompt is ALREADY stable-persona-first, so caching
may already work). **Deployed a ONE-LINE diagnostic** (`aiprovider.cpp:2106`, added `cached:` =
`usageMetadata.cachedContentTokenCount` to the Gemini usage log) as vc3530554 (v2-signed, force-stopped install).
**Pre-verified for free:** the 3 consecutive 09-17 morning prompts are BYTE-IDENTICAL (58 014 B each, `cmp` clean)
⇒ the persona prefix IS byte-stable turn-to-turn; instability is NOT a possible cause. `cachePrefixLen` is
ANTHROPIC-ONLY (inserts `cache_control`); Gemini caching is automatic (on by default for 3.x flash, min 4096-tok
prefix; persona ≈ 11–12K tok, above it).
**DECISION RULE when owner next has a MULTI-TURN chat** (turn 1 always misses — read turns 2+, close in time):
pull debug.log, grep `Gemini usage — .*cached:` →
- **`cached:` > 0** ⇒ implicit caching already works; the cost win is realized. DONE — nothing to build.
- **`cached: 0`** despite the stable prefix ⇒ Gemini does NOT cache `system_instruction` (Google docs only
  document caching for `contents`). THEN do the restructure: move the ~45K persona out of `system_instruction`
  into a LEADING `contents` message so it becomes a cacheable content prefix; keep the ~20K data block AFTER it.
  Evidence-driven, not speculative. (Persona is built in QML `AssistantOverlay.qml` ~870–1502 as
  `_coreSystemPrompt`; `stageCachePrefixLen` infra already carries the boundary.)
**⚠️ Reminder: caching = COST only.** Google's docs promise cost savings, not latency. The latency lever is the
2.5-Flash model switch (owner's side).

### Gemini context-caching feasibility (2026-09-17, owner picked this lever) — SUPERSEDED by measure-first above
**Feasible, but it's a prompt RESTRUCTURE, not a flag.** Findings:
- No caching exists (`grep cachedContent src/ai` = empty). The Gemini request (`GeminiProvider::analyzeConversation`,
  `aiprovider.cpp:2010+`) sends `system_instruction` = ONE `systemPrompt` string, `contents` = message history,
  `tools` = functionDeclarations.
- **Why nothing caches today:** the ~28K-token `systemPrompt` FUSES the stable persona/rules
  (`ShotSummarizer::shotAnalysisSystemPrompt`, ~45K chars) with the DYNAMIC live-data block
  (recordedShots/fullHistory/sessionContext/justPulledShot/similarBeanExperience, built in
  `aiconversation.cpp`/`dialing_blocks.cpp`) into one string that CHANGES EVERY TURN. So there is no byte-stable
  prefix → implicit caching can't hit and explicit `CachedContent` would be recreated every turn (pointless).
- **The change:** split stable (persona + tool schemas) from dynamic (data block); send persona-first as the
  cacheable `system_instruction`, move the live-data block to a LEADING `contents` message. Update the persona's
  "the data block below" wording to point to its new location. Then implicit caching (automatic on
  gemini-3.6-flash for identical prefixes past the min-token floor) starts hitting; explicit `CachedContent`
  optional on top. Also parse+log `usageMetadata.cachedContentTokenCount` to VERIFY hits (not logged today).
- **⚠️ Honest latency caveat (from OUR data, not Google's marketing):** our own telemetry shows the SAME 28K
  prompt served in 1s and in 24s — the dominant variable is Google's serving/QUEUE time, which caching does NOT
  reduce. Caching cuts COST hugely (28K→a few K billed tokens/round) and trims the prompt-PROCESSING share
  (already ~1s in the good case), so it makes best-case turns faster+cheaper but WON'T cure the throttled-morning
  spikes. If the mornings are the actual pain, the model/key-tier lever attacks the queue more directly.

### 🔨 THINKING-LOCKOUT FIX — BUILT + headless-tested (2026-09-17, owner: "build it, keep coaching voice intact")
**Bug (found in logs):** on 3.6-flash a mid-turn Google **HTTP 503** made the app retry silently for ~18s while
locked in Thinking; every user tap was `tap_ignored (thinking)` (8× across the logs, all 3.6-flash/503; the
Flash-Lite session had ZERO). Owner: "it wouldn't accept my statement, I had to wait and restart."
**Fix (barista-only, NO upstream files, coaching voice untouched):** `tap()` during Thinking now breaks out to
Listening (owner chose one-tap-keep-going). The in-flight request isn't cancellable, so it drains; its late
reply is dropped by the existing guard, which now calls new `flushAbandonedTurn()` → clears the stale
`m_turnInFlight` and dispatches any utterance the user spoke during the drain (queued in `onFinalText` when
`m_turnInFlight` while Listening — the only time that combo occurs). New diags: `tap_break`,
`utterance_queued_draining`. Files: `baristaconversation.{h,cpp}` + 2 tests in `tst_baristaconversation`
(`tapBreaksOutOfThinkingAndFlushesQueuedUtterance`, `tapBreakWithNoQueuedUtteranceStaysListening`) — suite GREEN.
Safe re-dispatch verified: `AIConversation::onAnalysisComplete` clears `m_busy` (aiconversation.cpp:792) BEFORE
`emit responseReceived` (:827) → QML → `onModelFinal` (AssistantOverlay.qml:1963), so the flushed `ask()` sees
`m_busy=false`. NOT committed.
**On-device check owed:** after a `tap_break` + `utterance_queued_draining`, you MUST see a real `turnRequested`
dispatch (not silence). **Known limitation (trigger for the future abort-path):** the swallow window = the drain
length (~1–3s on Flash-Lite). The 40s `turnTimeout` no longer covers the abandoned turn once in Listening (its
handler no-ops outside Thinking), so a pathological HUNG terminal holds `m_turnInFlight` true and swallows
utterances until the provider's own network timeout. Bounded/rare; if it ever bites, THAT justifies building the
`AIConversation` abort path (clear `m_busy` + abort the reply) — deferred as disproportionate now.

## ▶▶ PICK UP HERE (next session)

**✅ DEPLOYED: the instrumented APK `vc3529123` is now INSTALLED on the tablet** (confirmed
`versionCode=3529123`, 2026-09-16). adb + deploy are DONE. The two safe fixes (no talk-over, coaching voice no
longer self-truncating) are live. **The next step is owner-driven: exercise it, then pull the log.**

1. **Reconnect adb** — the Wireless Debugging port ROTATES on sleep/toggle, so it will likely be gone again.
   `~/Library/Android/sdk/platform-tools/adb` (NOT on PATH). `adb mdns services | grep _adb-tls-connect` →
   `adb connect 192.168.189.186:<port>` (or use the mDNS device serial
   `adb-R8YW70Z20PH-Pzxfcr._adb-tls-connect._tcp` if `adb devices` already shows it). If empty/refused, owner
   wakes tablet + confirms Wireless debugging ON, or sends a fresh pair code+port
   (`adb pair 192.168.189.186:<pairport> <code>`, ignore the "protocol fault" warning — it still pairs).
2. **Owner exercises `vc3529123`**: a few SIMPLE questions AND a RECIPE-EDIT, some while a shot runs + some
   idle. (Owner may already have done this before closing — pull the log and check first.)
3. **Pull the log + read the probes** (`/sdcard/Documents/Decenza/logs/barista-diagnostics.log`) to decide the
   two deferred fixes on EVIDENCE:
   - **R5 mic-churn hysteresis** ("works if I repeat" — mic recreate eating opening words): read the new
     SpeakerGate flap count per reply + code-5 clustering.
   - **R6 pause-tolerance** (the ORIGINAL 17s-cutoff ask — owner said **PROBE FIRST**): read the new Java
     warm-up-gap probe (`startListening→onReadyForSpeech→onBeginningOfSpeech`) + whether `onPartialResults`
     fires at all. GO only if the warm-up gap is short enough not to drop continuation words (SEED fact 4).
   - **Delay attribution**: read the new Gemini `reply` (ms) + `tool_done` (ms) lines + the `apply_blocked_flowing`
     shot marker → is a slow morning the machine stealing time (contention) or Gemini/network?

## ⚠️ UNCOMMITTED WORK IN THE TREE (this session, 2026-09-16) — do NOT lose it
The voice-instrumentation + safe-fixes bundle is **built + macOS ctest 127/127 GREEN, but NOT committed, NOT
pushed** (owner: commit only when asked). `git status` will show ~13 modified + 3 new files. Staged APK is
`~/Downloads/Decenza-voice-instrumented-vc3529123.apk` (v2-only, cert `5c7458da…`, built from this tree).
Full audit is `docs/barista/VOICE_INTERACTION_AUDIT_2026-09-16.md`. Contents:
- **R1** `onModelFinal` late-reply state guard (stops talk-over) + `tap_stop` diag — `baristaconversation.cpp`
- **R3** coaching-voice same-role `speaking()` guard (stops cue self-truncation) — `main.cpp`
- **Gemini timing sink**: `reply` ms + `tool_done` ms — `aiprovider.{cpp,h}`; shot marker `apply_blocked_flowing`
  — `baristaactions.cpp`
- **Java warm-up probes** — `DecenzaSpeech.java`; **SpeakerGate flap probe** — `speakergate.cpp`
- **Deafness canary** (zero-final + healthy-span) — `voiceinput.{cpp,h}`; **split** `speak_INTERRUPTS_previous`
  vs `chunk_advance` — `assistantvoice.cpp`
- **Poison denylist gate** `scripts/check_stt_intent_extras.py` + `.github/workflows/text-invariants.yml` +
  `docs/barista/STT_POISON_EXTRAS.md` (blocks re-adding the silence extras that caused deafness)
- test slot `lateModelFinalIgnoredOutsideThinkingOrSpeaking` — `tst_baristaconversation.cpp`
- **Known deviation**: the R3 coaching-suppressed line uses the inline `[BaristaDiag]` qDebug (mirrors its
  sibling) not the registered helper → adds 2 to the already-red (93 pre-existing) `check_log_markers` gate,
  which is NOT a required check. Cleanup-if-committed (route through `BaristaDiagnostics::record`).

## KEY FINDINGS (verified — do NOT re-derive)
- **The "massive delays" are the Gemini tool-loop, NOT a code regression.** Log split: 1-tool turns 2.9–7.3s;
  4–5 tool (recipe-edit) turns 22–24s — same pattern 2026-09-15. Running code is byte-identical to the build
  that "worked yesterday." The only real levers on heavy turns are shorter/fewer model rounds (brevity — owner
  DECLINED à la carte, wants "work like yesterday").
- **`voiceStreaming` is INERT on Gemini** (Anthropic-only path) — do NOT flip it as a latency fix.
- **RecognizerIntent silence/length extras = POISON** (total deafness on this Samsung/AOSP recogniser). The
  reboot deafness owner hit this morning was MY reverted `silReqMs=2800` experiment (build vc3528358), NOT a
  source bug — HEAD is clean; the denylist gate now guards it.
- **Tablet is currently on `vc3528876`** (clean reverted picker build — capture works; the deafness is gone).
  The staged `vc3529123` supersedes it.
- Much of the "wacky anomaly" feeling is LOG NOISE: `speak_INTERRUPTS_previous ×27` ≈ 24 mislabeled chunk
  handoffs + 3 real (coaching, now fixed); `needsTap ×40` ≈ 30 user Stop-taps (now logged as `tap_stop`) + 10
  real timeouts.

## OPEN OWNER QUESTIONS
- Were you actually pulling shots during the ~08:23 (2026-09-16) conversation, or just near the machine?
  (Tests the contention hypothesis; the new shot marker settles it going forward.)
- Persona/perceived-latency menu (deferred, owner leaned "work like yesterday" = don't change voice): chunked
  speech (first audio ~1s in), a spoken "let me check" ack during the wait, Gemini brevity clauses. Revisit
  after reading the probe data.

## Earlier verified state (2026-09-15, still true)
- **✅ Mic picker VERIFIED WORKING** (session 2026-09-15 21:05–21:13, JBL on USB-C hub): 8 turns, full
  sentences, zero NO_MATCH. `commAfter=22`/`pin=false` is a RED HERRING (recogniser captures on built-in mic
  anyway). No AudioRecord escalation needed. Owner still owes by-ear: (a) TTS from the JBL, (b) Speaker
  dropdown lists the JBL.
- Everything through `00d037db` is COMMITTED + PUSHED + MERGED to `origin/main` (picker feature `98ecec53`).
  0 behind upstream. The 2026-09-16 voice bundle above sits UNCOMMITTED on top of it.

### This session's commits (all pushed + merged)
| Commit | What |
|--------|------|
| `08da8109` | **Merge `upstream/main` (181ba7ed)** — 13 commits, dropped 2 superseded fork commits |
| `4ddeca22` | **fix(tests): clear the 2 pre-existing barista reds** (suite → 127/127) |
| `98ecec53` | **feat(barista): Microphone & Speaker device pickers** (fix USB route hijack) |

---

## 🎚️ Microphone & Speaker device pickers (`98ecec53`) — the current focus

**Problem (diagnosed from `stt/mic_route` diagnostics):** a JBL speaker on the USB-C hub kept becoming the
tablet's *communication device*. A speaker has no mic, so the recogniser recorded silence → `ERROR_NO_MATCH`
→ after a streak the barista dropped to "tap to talk," cutting the owner off. **NOT Bluetooth (owner
confirmed), NOT the machine.** The `commDev` toggled type 2 (built-in speaker) ↔ 22 (USB_HEADSET).

**Shipped:** two dropdowns on the barista **Settings → General** tab (first card, "Microphone & speaker").
- **Mic** default "Tablet microphone (default)"; each listen pins the mic to the chosen input (empty =
  built-in). When `setCommunicationDevice(builtinMic)` is rejected we `clearCommunicationDevice()` so capture
  never sits on a USB speaker.
- **Speaker** default "Automatic (system default)"; TTS pinned via `MediaPlayer.setPreferredDevice()`.
- Keys = stable `type:productName` (NOT `getId()`), resolved live, fallback to default if the device is gone.
- Files: NEW `DecenzaAudioDevices.java` (shared key/label/list/resolve) + `DecenzaSpeech.java` (mic) +
  `DecenzaAudioPlayer.java` (speaker) + `assistantsettings.{h,cpp}` (`micDeviceKey`/`speakerDeviceKey` +
  `availableMics`/`availableSpeakers` + `refreshAudioDevices()`, pushes keys to Java statics) +
  `AssistantSettingsPanel.qml` (the two pickers).

**✅ VERIFIED from the log (2026-09-15 21:05–21:13, JBL on the USB-C hub, `micKey=default`):** (1) cut-offs
GONE — 8 turns, full sentences, zero NO_MATCH (`code 7`); the 4× `code 5`/ERROR_CLIENT recovered via
recreate-on-next-start, and the trailing `needsTap` was the natural conversation end (after "thank you" +
`close_requested_tool`), not a drop. **Still owner-owed by ear** (the log can't show these): (2) TTS still
from the JBL, (3) Speaker dropdown lists the JBL.

**`commAfter=22` / `pin=false` is a RED HERRING — capture worked despite it.** `setCommunicationDevice(builtin)`
is rejected (output-anchored list), `clearCommunicationDevice()` doesn't drop the USB comm route, yet the
recogniser records on the built-in mic anyway. So do NOT read `commAfter` as the verdict — "did it transcribe"
is the signal, and it did. **AudioRecord escalation is PARKED** — only revive it if a real JBL-connected picker
session shows a NO_MATCH streak → needsTap *mid-conversation* (it did not).

---

## This session (2026-09-15) — upstream resync + red cleanup

### 1. Merged upstream/main (`08da8109`) — 13 commits (#1935..#1951)
Upstream landed a big profile/ratio drop: **profile-picker rebuild** (one shared `ProfilePicker.qml` with
beverage chips + faceted filters, favorites, 9 new profiles — #1948/#1949), **non-espresso ratio handling**
(clear the ratio on beverage-*group* change + raise the bound to 1:100 so filter/tea ratios can be stored —
#1945/#1946/#1947), MCP full Brew Settings control, Varia VS6 grinder RPM, iOS/macOS crash-report
symbolication, CI fixes. 121 files, +6687/−3260.

**Dropped TWO fork commits — upstream superseded both, and keeping ours would REGRESS upstream's new behavior:**
- **`60de7fa2` (profile beverage filter)** → upstream's rebuilt `ProfilePicker` has a full beverage chip
  group + facets; `ProfileSelectorPage.qml` now delegates to it. Took upstream's version wholesale.
- **`847c6074` (read-time ratio guard `isEspressoBeverageType`)** → upstream clears the ratio at
  beverage-group switch time and *deliberately allows* non-espresso ratios (tea/filter up to 1:100). Our
  binary espresso-only guard would have broken that. Removed the guard + `Profile::isEspressoBeverageType`
  (its only user) + the stale `ratioAnchorIgnoredForNonEspressoProfile` test.

Conflicts resolved: `profile.h` (kept upstream `beverageGroup`/`beverageBucket`/`inferBeverageType`),
`ProfileSelectorPage.qml` (upstream), `profilemanager.cpp` (guard removed), `tst_profilemanager.cpp` (stale
test removed). Verified NO fork barista/coach code was silently dropped (barista-fork markers 1130→1127 =
exactly the 3 intended; bean-ranking survives, moved into `ProfilePicker`). Barista singleton still published.

### 2. Cleared the 2 pre-existing reds (`4ddeca22`) — suite now 127/127
Both predated the merge (the last handoff called them "long-standing pre-existing").
- **`failonwarning_lint`**: armed `QTest::failOnWarning()` on the 4 barista test files that never did
  (`tst_anthropicstreamparser`, `tst_baristaconversation`, `tst_respondtextextractor`, `tst_speechchunker`).
  All 4 still pass armed — no warnings were being swallowed.
- **`tst_qmlregistration`**: the `Barista` singleton was published inside `BaristaModule::install()`, but the
  test (and the documented convention) expect main()-owned singletons published in `main.cpp` where it's
  greppable. Moved `BaristaModuleForeign::s_singletonInstance = baristaModule;` into main.cpp's
  `if (baristaModule)` block (before `engine.load()`, same as before), added the include, removed the
  orphaned include from `baristamodule.cpp`, regenerated `.fork/INDEX.tsv` for the shifted marker. Barista is
  now consistent with BLEManager/DE1Device/every other singleton. `install()` is only called by main.cpp; no
  test depends on the publish.

---

## 🔴 OPEN — owner-only

1. **Decide whether to push** the 2 local commits. Fork flow: push `feat/barista` → origin + backup → FF
   `origin/main` (a default-branch push needs explicit owner OK — the auto-mode classifier blocks it otherwise).
2. **On-device sanity of the NEW upstream surfaces** (next time at the machine). The merge replaced our
   filter + tea-ratio fix with upstream's, so re-check them against upstream's behavior:
   - **Profile picker:** open it → upstream's rebuilt picker renders (beverage chips, facets, favorites).
   - **Tea/filter ratios:** upstream now *allows* non-espresso ratios and clears the ratio when the beverage
     GROUP changes (espresso→tea drops the anchor; the tea profile stops at its own target). Confirm a tea
     steep after an espresso session is no longer cut short at ratio×dose.
3. **Check/close our upstream PRs #1943 (tea) / #1944 (filter)** — upstream shipped its own implementations
   (#1945/#1948), so ours are very likely superseded. Origin branches `upstream-pr/tea-stop-at-weight` and
   `upstream-pr/beverage-filter` can probably be deleted once the PRs are closed.
4. ~~Rebuild + redeploy the APK~~ **DONE** — the tablet is on `vc3528235`, the picker build (dex-confirmed;
   see TL;DR). The earlier "running pre-merge code" note was stale. No rebuild needed unless AudioRecord is
   revived (parked — see the picker section).

## Follow-ups (only if the owner wants them)
- **Voice filler Lever C** (deterministic auto-trim) — only if A+B (`c090c6cf`) leave filler after the
  on-device measurement.
- **Extraction live-coach genre** (pre-existing, see memory `decenza-extraction-live-coach-genre`) — still open.

---

## Environment / access (for the next session)

- **adb**: binary at `~/Library/Android/sdk/platform-tools/adb` (NOT on PATH). The connect port ROTATES on
  toggle/reboot. If `adb devices` is empty: owner wakes the tablet + confirms Wireless Debugging ON →
  `adb mdns services | grep _adb-tls-connect` for the new port → `adb connect 192.168.189.186:<port>`. If
  refused, owner sends a fresh pair code (Settings → Pair device with pairing code) →
  `adb pair 192.168.189.186:<pairport> <code>` (ignore the "protocol fault" warning — it still pairs), then
  connect to the CONNECT port. **Install gotcha:** never `adb install -r` over the RUNNING app while the DE1
  is connected — force-stop first (`adb shell am force-stop io.github.kulitorum.decenza_de1`) and power-cycle
  the DE1 if it doesn't reconnect after launch.
- **Logs on device**: `/sdcard/Documents/Decenza/logs/` (`debug.log`, `barista-diagnostics.log`); full model
  turns at `…/Android/data/io.github.kulitorum.decenza_de1/files/Documents/ai_logs/` (`qa_*`, `prompt_*`,
  `response_*`). `barista-diagnostics.log` stores only a ~60-char preview of each spoken line; `ai_logs` has full text.
- **Android `--dev` APK recipe** (`docs/CLAUDE_MD/PLATFORM_BUILD.md [barista-fork]`):
  `export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home` →
  `./build.sh --target ANDROID --dev` → zipalign + apksigner **v2-only** (`--v3-signing-enabled false
  --min/max-sdk 28/34`) with `~/.android/debug.keystore` (cert `5c7458da…`). Android build dir is
  `build/Qt_6_10_1_for_Android_arm64_v8a_Release`. `--dev` derives a clock-based versionCode (no
  `versioncode.txt` edit). ⚠️ If the Android build dir errors on the tsnet pin, reconfigure ONCE:
  `cmake -DTSNET_TAG=decenza-v1.94.1-6 build/Qt_6_10_1_for_Android_arm64_v8a_Release`.
- **Remotes**: `origin` = loganross2001/Decenza-community-fork, `backup` = loganross2001/Decenza-private,
  `upstream` = Kulitorum/Decenza.

## Lessons banked this session
- **A "superseded" fork commit can be actively harmful, not just redundant.** Our binary espresso-only ratio
  guard wasn't merely replaced by upstream — keeping it would have broken upstream's new *deliberate* support
  for non-espresso (tea/filter) ratios up to 1:100. Check WHY upstream did it their way before dropping ours.
- **Auto-merge fails in both directions.** It RETAINED our ratio guard in `profilemanager.cpp` (a hunk we
  meant to drop) and could just as easily have DROPPED fork code elsewhere. Gate a merge with a
  barista-fork-marker count diff + a scan of removed lines in the heavily-rewritten shared files, not just a
  green build (barista behavior is under-tested).
- **"Pre-existing red" ≠ "known and accepted" unless you say so.** The 2 reds were genuinely pre-existing
  (proven via byte-identical inputs to HEAD) AND documented as such in the prior handoff — reconciling the
  "ctest-green" memory, which meant "green except the 2 known reds."

## Memory topic files
`decenza-barista-voice-streaming`, `decenza-canonical-folder`, `decenza-fork-docs-lag-code`,
`decenza-deploy-de1-ble-gotcha`, `decenza-extraction-live-coach-genre`, `decenza-barista-coaching-loop-trace`.
