# Decenza barista — SESSION RESUME (2026-09-15)

Canonical repo: `/Users/christopherpalestro/Decenza-fork`, branch **`feat/barista`** (the ONLY checkout —
ignore stale `~/Decenza*`). Build/test on this Mac via cmake/ctest in `build/Qt_6_11_2_for_macOS_Debug`
(this fork brief overrides CLAUDE.md's "use the Qt Creator MCP" rule). **Call the advisor before substantive
work and before declaring a step done. Commit/push only when asked.**

---

## TL;DR — where everything is right now

- **Everything below is COMMITTED + PUSHED + MERGED to `origin/main`.** All refs (`feat/barista`,
  `origin/feat/barista`, `origin/main`, `backup`) are at **`98ecec53`**. Tree clean. 0 behind upstream.
- **Full suite 127/127 GREEN.** The two long-standing pre-existing reds were fixed this session; zero known reds.
- **Tablet is on `vc3528235`** (the audio-device-pickers build).
- **⚠️ The ONE open thing is owner on-device by-ear verification of the mic/speaker pickers** (see below) —
  the code shipped before that check at the owner's instruction.

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

**⚠️ OWED — owner on-device by-ear check:** (1) cut-offs gone, (2) TTS still from the JBL, (3) Speaker
dropdown lists the JBL. **If the pin doesn't hold** (still cut off, or TTS jumped to the tablet speaker):
escalate the mic path to an own `AudioRecord` + `RecognizerIntent.EXTRA_AUDIO_SOURCE` capture — the cheap
`setCommunicationDevice`/`clear` approach was the first attempt, and the `mic_route` log now records
`micKey`/`pin`/`commAfter` to show what held.

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
4. **Rebuild + redeploy the APK** when ready — the tablet is running pre-merge code.

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
