# qmllint gate backlog — analysis & remediation plan

> **✅ RESOLVED 2026-09-08 — the gate is now GREEN (clean 251/251).** The keystone (Barista →
> compile-time QML singleton) landed, and the remaining delegate/layout/qualification items were
> cleared with this repo's established patterns (`pragma ComponentBehavior: Bound` where delegates
> already declared their roles, `postShotReviewPage.`-qualified page accesses, `implicitWidth/Height`
> on layout children, `// qmllint disable` for the two URL-loaded Loader items and the intentional
> tap-overlay anchors, `Number().toFixed`, and a missing `accessibleName`). Verified: app compiles +
> links, ctest green, runtime smoke clean, full gate passes. **ONE residual, pre-existing and
> separate:** the `DECENZA_BARISTA=OFF` vanilla-bisect build does not compile (main.cpp:2190-2193
> declare `AssistantVoice*`/`CoachPhrasebook*` outside any `#ifdef`) — predates this work; tracked
> below. Two delegate screens (People-settings tab, Barista edit dialog) got the pragma but were not
> exercised at runtime — a quick on-device look is worth it, though the change is the low-risk pattern.
> The analysis below is kept as the record of how it was done.

**Why the gate is red and was not noticed:** the fork verifies via `ctest` (unit tests), but the
qmllint gate is a separate cmake target (`qmllint_check`) that only runs on a full `cmake --build . -j`
or an explicit `--target qmllint_check`. So it has been latently red. A full build surfaced it
(2026-09-08). It is **not** a required status check, so nothing blocked merges on it.

## Done this pass (safe, no app-testing needed)
1. **15 assistant-module-exemption items** — the `qml/assistant/**` files are bundled via
   `qt_add_resources` in `cmake/barista.cmake` (loaded by URL), deliberately outside
   `qt_add_qml_module` for fork modularity. Added them to `NOT_IN_MODULE_BY_DESIGN` in
   `scripts/qmllint_report.py` (with reason). They are bundled at runtime but not qmllint-linted —
   an accepted tradeoff of the URL-Loader design.
2. **Real broken-feature bug (fixed):** `PostShotReviewPage.qml:1083` called
   `conversation.processShotForConversation(...)`, which **upstream #1857 removed** (change-detection
   moved into the prose envelope). The call threw a `TypeError`, breaking the post-shot coaching
   card. Fixed to use the `buildShotAnalysisProseForShot` prose directly, matching the reference
   call sites (`ConversationOverlay.qml`, `ShotDetailPage.qml`). A merge-integration miss.
3. **IME fix:** `BaristaEditDialog.qml:75` `Qt.inputMethod.commit()` → `Keyboard.commit()` (the
   compile-time singleton; `Qt.inputMethod` is typed as bare `QObject` so its `.commit()` is
   unresolvable — CLAUDE.md gotcha).

## Remaining — NOT done, by design (needs an owner decision and/or the running app)

### KEYSTONE: the `Barista` context-property singleton (~22 warnings)
`main.qml` (11 unqualified + `baristaOverlay.item.reservedWidth` on QObject), `IdlePage.qml` (3),
`BaristaItem.qml` (8) are all `typeof Barista !== "undefined" && Barista.*`. `Barista` is exposed via
`BaristaModule::install` → `setContextProperty("Barista", …)`. qmllint cannot see context properties
(CLAUDE.md's #1 QML gotcha), so every access is "unqualified" and the URL-loaded overlay's `item` is
`QObject` (so `.reservedWidth`/`.implicitHeight` are "missing-property").
- **Proper fix (CLAUDE.md-prescribed):** expose `Barista` as a **compile-time QML singleton** via a
  `QML_FOREIGN` wrapper (mirror `src/core/contextsingletons_qml.h`, which already exists), not
  `setContextProperty` (currently `baristamodule.cpp:1082`). Install order is already fine —
  `BaristaModule::install` runs at `main.cpp:4148`, before `engine.load()` at `:4199`.
- **Exact scope (assessed 2026-09-08 — it is bigger than "wrap BaristaModule"):**
  1. `QML_FOREIGN`+`QML_SINGLETON` wrapper for `BaristaModule` returning the app-owned instance
     (contextsingletons pattern avoids the default-constructible trap).
  2. **`AssistantOrchestrator` AND `AssistantSettings` also need QML registration**
     (`QML_FOREIGN`+`QML_UNCREATABLE`) — they are currently bare C++ QObjects, so even once `Barista`
     resolves, `Barista.orchestrator.lastShotId` / `Barista.settings.assistantName` would stay
     "member not found on QObject" without it.
  3. **The `typeof Barista !== "undefined"` guards are LOAD-BEARING for the `DECENZA_BARISTA=OFF`
     bisect build** (a documented fork feature — build vanilla to bisect upstream regressions). A
     compile-time singleton changes that contract: per QML_GOTCHAS a registered singleton with no
     instance is *truthy*, so the guards must become member guards (e.g. `Barista.enabled ?? false`),
     and **an OFF build must still register the singleton returning null** or `main.qml`/`IdlePage`
     break for it. Get this wrong and the app's MAIN SCREEN breaks for ALL users, not just barista.
- **Why deferred:** must be verified in the running app in **both** build configs (ON and OFF); the
  failure modes (null-instance member access at load, OFF-build regression) are invisible to the
  build and to `ctest`. This is the one decision that unblocks ~85% of the remaining gate, and it is
  a "verify at the machine" change, not a blind one.

### Delegate `ComponentBehavior: Bound` (needs the screen open)
`BaristaChipRow.qml` (20), `BaristaEditDialog.qml` (4 remaining), `SettingsHistoryDataTab.qml` (7)
unqualified accesses of outer ids (`root`, `peopleCard`, `editDialog`) from inside `Repeater`
delegates. qmllint's remedy is `pragma ComponentBehavior: Bound` — but QML_GOTCHAS is explicit that
the pragma **silently breaks model-role injection** unless every delegate also gains
`required property var modelData` (+ `index`/`model` where used). These files use `modelData`, so the
pragma + required-property additions must be done together and **the screen opened to verify**
(`ReferenceError: modelData is not defined` is invisible to the test suite).

### `PostShotReviewPage.qml` (9 unqualified) — safe but low-value alone
qmllint gives the exact id (`postShotReviewPage.editShotData`, etc. — page-root properties, not
delegate roles). Safe to qualify with `qml_qualify.py`-style edits, but this file stays red on its
QObject/Loader `missing-property` (part of the keystone), so do it in the same pass as the keystone.

### `Quick.layout-positioning` (8) — safe, mechanical
`width`/`height` on layout-managed items → `implicitWidth`/`implicitHeight`; `anchors.fill: parent`
→ `Layout.fillWidth/Height`. In `SettingsHistoryDataTab.qml` (623/624/672/673) and `BaristaChipRow.qml`
(178/205/206/235). Real UB. Fold into the ComponentBehavior pass on those same files.

### `missing-property` (5 remaining)
- `main.qml:1262`, `PostShotReviewPage.qml:1821` — Loader `item` is `QObject` → resolved by the keystone.
- **`BrewDialog.qml:308` `ProfileManager.currentProfileKbId()` — REAL BUG, needs product intent.**
  This method **never existed** on `ProfileManager` (no accessor, no git history), so
  `requestBeanRecipe()` throws and the bean-recipe card in BrewDialog silently never populates.
  ProfileManager exposes `currentProfileName`/`currentProfileTitle`/… but no kbId. The correct fix
  depends on how the current profile's kbId should be derived (owner/author call — do NOT guess).
- `BrewQuickSelectDialog.qml:75,81` `v.toFixed(...)` on `QJSPrimitiveValue` — qmllint false positive
  (works at runtime); needs a cast (`Number(v).toFixed(...)`) or a scoped disable, not a real fix.

## How to run the gate
```
cd build/Qt_6_11_2_for_macOS_Debug && cmake --build . --target qmllint_check
# or, to see everything (not just regressions):
python3 scripts/qmllint_report.py --report --qmllint ~/Qt/6.11.2/macos/bin/qmllint --import-path <build-dir>
```
Do NOT `--update-baseline` to paper over the keystone — the gate is meant to stay honest.
