# Decenza barista — SESSION RESUME (2026-09-15)

Canonical repo: `/Users/christopherpalestro/Decenza-fork`, branch **`feat/barista`** (the ONLY checkout —
ignore stale `~/Decenza*`). Build/test on this Mac via cmake/ctest in `build/Qt_6_11_2_for_macOS_Debug`
(this fork brief overrides CLAUDE.md's "use the Qt Creator MCP" rule). **Call the advisor before substantive
work and before declaring a step done. Commit/push only when asked.**

---

## TL;DR — where everything is right now

- **`feat/barista` is 2 commits ahead of where the last session left it (`55c832cc`), and those 2 are NOT
  pushed.** `origin/feat/barista` / `origin/main` / `backup` are still at `55c832cc`.
- **Merged `upstream/main` (`181ba7ed`) — now 0 behind / 376 ahead.** The merge dropped two of our own fork
  commits because upstream re-solved them more completely (see below).
- **Full desktop suite is now 127/127 GREEN** — the two long-standing pre-existing reds
  (`tst_qmlregistration`, `failonwarning_lint`) were fixed this session. Zero known reds remain.
- **Nothing was pushed and nothing is on the tablet yet.** The APK still running on the tablet
  (`Decenza-filter-vc3525243`) predates this merge.

### The two new commits (local only)
| Commit | What |
|--------|------|
| `08da8109` | **Merge `upstream/main` (181ba7ed)** into feat/barista — 13 commits, dropped 2 fork commits |
| `4ddeca22` | **fix(tests): clear the two pre-existing barista test reds** (suite now 127/127) |

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
