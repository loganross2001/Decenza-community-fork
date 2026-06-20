# Decenza weight-dialing — detailed change reference (vs `v1.8.0`)

A feature-by-feature technical reference for reviewing/integrating the changes on this fork.
All line numbers refer to the files on the `community-fork`/`weight-dialing` branches (identical
code); the authoritative diff is [`fork-notes/changes-vs-1.8.0.patch`](changes-vs-1.8.0.patch).
Conventions from the project's `CLAUDE.md` were followed throughout (Theme tokens, no
timers-as-guards, `Tr`/`TranslationManager` strings with English fallbacks, accessibility roles).

## Build & verification
- Qt 6.11.1. Builds clean (0 errors) on desktop (macOS/clang) and Android (`arm64-v8a`, NDK r27c, JDK 17; `-DBUILD_TESTS=OFF` for the cross-build).
- New translation keys all carry English fallbacks, so they render immediately.

## File manifest

| File | Change | ± |
|------|--------|---|
| `qml/components/StableWeightCapture.qml` | **NEW** — event-driven scale-settle detector | +84 |
| `src/core/settings_brew.h` | dose-cup tare property + steam calibration method decls | +10 |
| `src/core/settings_brew.cpp` | impls (tare getter/setter, `setSteamPitcherCalibration`) | +30 |
| `src/core/accessibilitymanager.h` | `playCaptureDing()`, `initDingSound()`, `m_dingSound` | +6 |
| `src/core/accessibilitymanager.cpp` | ding impls + startup pre-load | +23 |
| `qml/components/BrewDialog.qml` | bean auto-capture, dose-cup tare row, net-dose math | +102 / −3 |
| `qml/pages/SteamPage.qml` | weight-timed steaming, weigh/purge buttons, scrollable editor, hints | +417 / −7 |
| `qml/pages/IdlePage.qml` | bean+milk capture, bottom stat bar, layout rework, prompts | +334 / −7 |
| `qml/components/layout/items/ShotPlanItem.qml` | full-mode repurposed into 3 setup buttons | +143 / −16 |
| `qml/components/layout/LayoutCenterZone.qml` | equal-cell `distribute` mode | +21 / −6 |
| `resources/resources.qrc` | register `sounds/ding.wav` | +1 |
| `resources/sounds/ding.wav` | **NEW** binary (16-bit mono 44.1 kHz PCM, 75 KB) | — |
| `CMakeLists.txt` | register `StableWeightCapture.qml` in `qt_add_qml_module` | +1 |

---

# 0. Shared foundation — `StableWeightCapture` (NEW)

Three features (bean capture, milk capture, the "wait for the bell" hints) hang off this one
component, so it's documented first.

**Purpose.** A non-visual, event-driven detector that watches a bound net-weight value and emits
`stableCaptured(grams)` once the reading holds within `tolerance` for `stableMs`, then latches and
auto-re-arms when the load is removed or materially changes.

**File:** `qml/components/StableWeightCapture.qml` (new, 84 lines). Registered at `CMakeLists.txt:735`.

**Input properties (defaults):**

| Property | Type | Default | Meaning |
|---|---|---|---|
| `weight` | real | `0` | Net weight to watch (parent binds, e.g. `scaleWeight − tare`) |
| `active` | bool | `true` | Detect only while true; going false calls `reset()` |
| `minWeight` | real | `20` | Ignore readings below this |
| `maxWeight` | real | `100000` | Ignore readings above this |
| `tolerance` | real | `1.0` | ± grams treated as "the same reading" |
| `stableMs` | int | `2500` | Must hold this long to graduate |
| `removeThreshold` | real | `5` | Below this → load removed → re-arm |
| `rearmDelta` | real | `6` | Change this much from the captured value → re-arm |

**Outputs:** `readonly bool isCaptured`, `readonly real capturedValue`, `signal stableCaptured(real grams)`.

**Detection logic (`_evaluate`, lines 47–75):**
1. `!active` → return.
2. Re-arm gate (when already captured): if `weight < removeThreshold` OR `abs(weight − capturedValue) > rearmDelta` → `reset()`; else return (stay latched).
3. Range gate: outside `[minWeight, maxWeight]` → clear candidate, return.
4. Candidate tracking: if no candidate or `abs(weight − candidate) > tolerance`, (re)start candidate at `Date.now()`, return.
5. Graduation: if `now − candidateSince ≥ stableMs` → set captured, emit `stableCaptured(weight)`.

**Triggers:** `onWeightChanged`, `onActiveChanged` (→ reset when false), and a periodic `Timer`
(`interval 150`, `running: active && !_captured`) so a perfectly constant scale stream still
graduates. (This is genuinely periodic polling, not a guard timer — consistent with the project rule.)

**Notes:** wall-clock (`Date.now()`) dwell timing; re-arm relies on a `weight` change crossing the
thresholds (so topping up / re-dosing re-captures); the latched value survives lifting the vessel.

---

# 1. Dose-cup tare (`doseCupTareWeight`)

**Purpose.** Store the empty dosing-cup weight so a scale reading reduces to *net beans*.

**`Q_PROPERTY` (`settings_brew.h:21`):**
```cpp
Q_PROPERTY(double doseCupTareWeight READ doseCupTareWeight WRITE setDoseCupTareWeight NOTIFY doseCupTareWeightChanged)
```
**Getter (`settings_brew.cpp:117`):** `return m_settings.value("espresso/doseCupTareWeight", 0.0).toDouble();` — QSettings key `espresso/doseCupTareWeight`, default `0.0`, read each call.
**Setter (`settings_brew.cpp:121`):** floors negatives to `0`, exact `!=` change-guard, persists + emits `doseCupTareWeightChanged()`.

**QML:** `Settings.brew.doseCupTareWeight`. Surfaced in BrewDialog as a "Dose cup" `ValueInput`
(`0..100`, `stepSize 0.1`, `decimals 1`, suffix `g`) + a "Weigh" button writing `MachineState.scaleWeight`.
The net-bean expression `Math.max(0, MachineState.scaleWeight − Settings.brew.doseCupTareWeight)` is
used by every dose read (BrewDialog "Get dose from scale", both capture detectors, the IdlePage
readout, the bottom "Scale" cell in espresso mode).

---

# 2. Bean auto-capture (BrewDialog + IdlePage)

**Purpose.** When a dose cup of beans rests stable on the scale, auto-lock the net dose (and on
IdlePage the stop-at-weight = dose × ratio), ding, and show a brief confirmation — replacing the
manual weigh button on the home screen with a live readout.

**BrewDialog** (`BrewDialog.qml:124`): `StableWeightCapture { id: beanCapture }` with
`weight: ScaleDevice.connected ? max(0, scaleWeight − doseCupTareWeight) : 0`,
`active: root.visible && ScaleDevice.connected && !ScaleDevice.isFlowScale`, and overrides
`minWeight 5`, `maxWeight 45`, `tolerance 0.5`, `stableMs 2500`. On `stableCaptured(net)`: clears the
scale warning, sets `targetManuallySet = false` (re-enables auto target recalc), `doseValue = net`,
shows a 3.5 s confirmation, and `AccessibilityManager.playCaptureDing()` (+ `announce` if a11y enabled).
"Get dose from scale" now subtracts the tare (`net = scaleWeight − doseCupTareWeight`, requires `net ≥ 3`).

**IdlePage** (`IdlePage.qml:133`): a parallel `beanCapture` with the same overrides;
`active: ScaleDevice.connected && !ScaleDevice.isFlowScale && activePresetFunction ∉ {steam, hotwater, flush}`
(so milk/water never trip it; the 45 g `maxWeight` is a second guard). On `stableCaptured(net)`
(guards `net < 3`): **persists** `Settings.dye.dyeBeanWeight = net` and
`Settings.brew.brewYieldOverride = net × Settings.brew.lastUsedRatio` (stop-at-weight; `lastUsedRatio`
default `2.0`; `setBrewYieldOverride ≤ 0` clears the keys), dings, shows a 3.5 s banner.

The old "Weigh beans" button became a small live readout `weighBeansText` (`IdlePage.qml:~650`):
shows `"<net> g on scale"`, the captured "Dose set: …", or the two-line blinking prompt when empty
(see §7f).

---

# 3. Weight-timed steaming

> **Inspiration & thanks:** the weight-timed-steaming concept was inspired by **[DSx2](https://github.com/Damian-AU/DSx2)** by **[Damian (Damian-AU)](https://github.com/Damian-AU)**. The implementation below is original (no DSx2 code was used); the idea is theirs, with appreciation.

**Purpose & formula.** Each steam *pitcher preset* carries a **reference milk weight** (`calibMilkG`)
paired with its fixed **duration**. When milk is measured, steam time scales proportionally and the
DE1 auto-stops there:
```
steamTime = clamp( round( preset.duration × measuredMilk / referenceMilk ), 5, 120 )   // seconds
```
`measuredMilk` = net milk = `scaleWeight − pitcherWeightG` (or the raw reading if the user tared the
empty pitcher). If `calibMilkG ≤ 0`, scaling is off and the fixed `duration` is used.

**C++ — `setSteamPitcherCalibration` (`settings_brew.h:113`, `settings_brew.cpp:324`):**
```cpp
Q_INVOKABLE void setSteamPitcherCalibration(int index, double calibMilkG);
```
Reads `steam/pitcherPresets` (JSON array), and for the indexed preset sets `calibMilkG` when `> 0`
or **removes** the field when `≤ 0` (disables scaling); writes back + emits `steamPitcherPresetsChanged()`.
No C++ upper clamp (the QML input bounds `0..1500 g`; the *result* is clamped `[5,120]`).

**Steam pitcher preset JSON shape** (`steam/pitcherPresets`): `name`, `duration` (s), `flow`
(0.01 ml/s units), `disabled?` (Off preset), `pitcherWeightG?` (empty-pitcher tare, via existing
`setSteamPitcherWeight`), `calibMilkG?` (reference milk — this feature). `updateSteamPitcherPreset`
preserves `pitcherWeightG`/`calibMilkG` when editing name/duration/flow (`settings_brew.cpp:257`).

**QML helpers (`SteamPage.qml`):**
- `currentMeasuredMilk()` (`:189`) — net milk, gated to the sane range `(20, 1500) g`, else 0.
- `scaledSteamTimeout()` (`:206`) — live scaled time, or 0 when scaling doesn't apply.
- `steamTimeForMilk(milk)` (`:220`) — scaled time for a specific captured weight.
- `getCurrentPitcherCalibMilk()` (`:229`) — **reactive-binding fix:** starts with `var _ = Settings.brew.steamPitcherPresets` so the binding subscribes to `steamPitcherPresetsChanged` and refreshes after "Weigh" (the imperative `getSteamPitcherPreset()` alone wouldn't). Same pattern at `:1620`, `:1682`, `:1696`.
- `syncSteamTimeout()` (`:241`) — the anti-clobber sync (see the bug below).

**UI:** a "Reference milk" `ValueInput` (`0..1500`, `0.1`, `decimals 1`, `value: getCurrentPitcherCalibMilk()`,
`onValueModified → setSteamPitcherCalibration`) with a "Weigh" button (enabled when
`currentMeasuredMilk() > 0`); an "Expected steam time" readout (`scaledSteamTimeout() + " s"` for the
milk on the scale); and a milk `StableWeightCapture { id: milkCapture }` (`:1823`,
`minWeight 20`, `tolerance 1.5`, `stableMs 2500`, `active: !isSteaming && !steamSoftStopped && connected && !isFlowScale`)
that on capture sets `Settings.brew.steamTimeout = steamTimeForMilk(milk)`, dings, and banners.

**IdlePage** (`:170`–`:519`): `property real measuredMilkG` (last measured this session; shown in the
bottom Milk cell), an `idleMilkCapture` (same 20/1.5/2500, `active` only in steam mode), and the steam
branch of `onPresetSelected` which scales by measured milk and **falls back to `measuredMilkG`** when
the pitcher is lifted off the scale.

### The steam-timeout re-sync bug (fixed)
In v1.8.0 three paths unconditionally reset `steamTimeout` to the fixed `duration`, wiping any scaled
value the moment the page activated, a session ended, or a pill was tapped — which broke the feature
as soon as the pitcher was lifted to the wand (scale reads ~0):

| # | Path | v1.8.0 (clobbering) | Fix |
|---|------|---------------------|-----|
| 1 | `StackView.onActivated` (`SteamPage.qml:34`) | `steamTimeout = getCurrentPitcherDuration()` | `syncSteamTimeout()` |
| 2 | Session end, `onIsSteamingChanged` else (`:131`) | `steamTimeout = getCurrentPitcherDuration()` | `syncSteamTimeout()` |
| 3 | Pill taps — live (`:483`) & `applyPitcher` (`:1081`) | `steamTimeout = modelData.duration` | `= scaledSteamTimeout() > 0 ? scaledSteamTimeout() : modelData.duration` |

`syncSteamTimeout()` encodes the three-way decision: (a) milk on scale now → live scaled time;
(b) **calibrated but no milk on the scale right now → keep the current value** (it was already scaled
from the captured/lifted milk — don't reset to baseline); (c) uncalibrated → fixed duration. Case (b)
is the crux: when the pitcher is at the wand, `scaledSteamTimeout()` returns 0, and the old code
snapped back to the unscaled duration.

**Data flow → DE1:** measured milk → `StableWeightCapture` → programmatic `Settings.brew.steamTimeout = t`
(persisted to `steam/timeout`; **not** baked into the preset) → on steam start flows to the DE1 as
`TargetSteamLength` (SHOT_SETTINGS byte 2, `de1device.cpp:1611`/parse `:1684`), the firmware's auto-stop
denominator. The countdown UI reads the same value: `shotTime + "s / " + Settings.brew.steamTimeout + "s"`.

---

# 4. Capture "ding"

**Purpose.** A pleasant confirmation tone on every weight capture — a **general UI cue**, deliberately
**not** gated by the accessibility-enabled flag (only the spoken `announce()` is). Pre-loaded at startup
so the first ding isn't silent (a lazily-created `QSoundEffect` is still `Loading` on first `play()`).

**C++ (`accessibilitymanager.h/.cpp`):**
- `Q_INVOKABLE void playCaptureDing();` (`.h:83`) — `if (m_shuttingDown) return; initDingSound(); if (m_dingSound) m_dingSound->play();` (`.cpp:258`). Ungated by `m_enabled`/`m_tickEnabled`.
- `void initDingSound();` (`.h:155`, `.cpp:247`) — idempotent; `new QSoundEffect(this)`, `setSource("qrc:/sounds/ding.wav")`, `setVolume(0.9)`.
- `QSoundEffect* m_dingSound = nullptr;` (`.h:172`).
- Constructor pre-loads unconditionally (`.cpp:24`): `initDingSound();` runs even when the tick sound is gated off. (The `DECENZA_TESTING` `TestSkipAudioInit` ctor skips it → `m_dingSound` stays null → `playCaptureDing()` no-ops.)
- Asset: `resources/sounds/ding.wav` (new; registered at `resources.qrc:6`).

---

# 5. Steam editor polish (`SteamPage.qml`)

- **Scrollable editor** (`:1280`): the editor `ColumnLayout` is wrapped in a `Flickable`
  (`contentHeight: editorColumn.implicitHeight`, `clip`, `ScrollBar.vertical AsNeeded`); the column's
  `width` is bound to `editorFlick.width` (a Layout in a Flickable isn't auto-width). The "Off"
  placeholder switched from `Layout.fillHeight: true` to `Layout.preferredHeight: Theme.scaled(160)`,
  and the trailing `Item { Layout.fillHeight: true }` spacer was removed — **`fillHeight` is meaningless
  inside a content-sized Flickable** and breaks `contentHeight`.
- **Inline "Weigh" buttons:** reference-milk (`:1471`, → `setSteamPitcherCalibration(...currentMeasuredMilk())`)
  and empty-pitcher (`:1631`, → `setSteamPitcherWeight(...scaleWeight)`); both gated on
  `connected && !isFlowScale`, enabled by live readings, accent-colored.
- **0.1 g inputs:** the pitcher-weight and reference-milk `ValueInput`s use `stepSize 0.1`, `decimals 1`.
- **Purge buttons** (call `DE1Device.requestIdle()`): one in the editor (`:1314`, shown when the preset
  isn't the Off pill) and one on the **live steaming view** (`:751`, `visible: isSteaming`), under the
  timer/progress bar.

---

# 6. "Wait for the bell" prompt

A flashing reminder shown only while something is settling on the scale and capture hasn't fired
(`active && !isCaptured && weight ≥ minWeight`); it disappears the instant capture latches — which is
also when the ding plays, so the prompt vanishing == the bell. `SequentialAnimation on opacity`
(0.25↔1.0, 450 ms legs, `running` bound to `visible`).
- IdlePage (`:229`, `z 1500`): covers **both** the bean detector and the milk detector (`beansSettling || milkSettling`).
- SteamPage (`:1878`, `z 1000`): the `milkCapture` detector only.
- Key: `scale.waitForBell` → "Wait for the bell before you take it off the scale".

---

# 7. Home-screen rework ⚠️ (most opinionated — see Appendix B)

### 7a. `ShotPlanItem.qml` — full mode repurposed into three setup buttons
**Compact (bar) mode is unchanged.** Full mode no longer renders the `ShotPlanText` summary; it now
renders three white-fill/blue-text buttons centered in thirds via
`anchors.horizontalCenterOffset: −parent.width/3`, `0`, `+parent.width/3` (`:66`, `:109`, `:150`):
- **Espresso-Shot Setup** → `brewDialog.open()`; subtitle = dose→yield (`d.toFixed(1) + " → " + t.toFixed(1) + " g"`, hidden if either is 0).
- **Choose Espresso Profile** → `openProfileChooser()` (pushes `ProfileSelectorPage.qml`); subtitle = `ProfileManager.currentProfileName` (elided).
- **Milk-Steaming Setup** → `openSteamSetup()` (pushes `SteamPage.qml`, avoids the long-press); subtitle = `Math.round(Settings.brew.steamTemperature) + "°C"`.

The root wrapper drops to `Accessible.role: NoRole` in full mode (each button is individually
accessible). Helper functions `openSteamSetup()`/`openProfileChooser()` guard on `pageStack` being
defined (`:187`–`195`). The `modelData.shotPlanShow*` flags now affect compact mode only.

### 7b. `LayoutCenterZone.qml` — equal-cell `distribute` mode
New `readonly bool distributeItems: (zoneName === "centerStatus" || "centerMiddle") && !hasSpacer`
(`:58`). When on, each delegate uses `Layout.preferredWidth: 0` + `Layout.fillWidth: true` (equal
cells regardless of content width) and the auto-centering flanking spacers are removed. Spacing bumped
`scaled(10) → scaled(18)`. Other zones keep the original centered-cluster behavior.

### 7c. Bottom `brewStatusBar` (`IdlePage.qml:795`)
A `Rectangle` (`anchors.bottom: bottomBar.top`, `height scaled(82)`, `color: Theme.primaryContrastColor`
— light fill) with a `RowLayout` of **five equal cells** (Profile · Scale · Ratio · Beans · Milk).
**Even-spacing technique:** each cell is a `ColumnLayout` with `Layout.fillWidth: true` +
`Layout.preferredWidth: 1` (equal share regardless of content), and the label/value both use
`Layout.fillWidth: true` + `horizontalAlignment: Text.AlignHCenter`. Labels `Theme.primaryColor`/`labelFont`;
values `Theme.primaryColor`/`scaled(21)`/bold. Scale is context-aware (net milk in steam mode, net beans
otherwise). Required adding `id: bottomBar` to the action-bar rectangle.

### 7d. Hidden `centerStatus` zone (`:391`) — `visible: false` (duplicated by the global status bar).
### 7e. Top-anchored center block + status-bar border line
- `topStatsBorder` (`:363`): `Rectangle` pinned at `anchors.topMargin: Theme.statusBarHeight` (=`scaled(70)`), full width, `height scaled(2)`, `color: Theme.primaryColor` — the status bar's bottom border, independent of the (often empty) top zones.
- The center `ColumnLayout` is now `anchors.top: topStatsBorder.bottom` + `topMargin scaled(12)` (was vertical-center). So it **grows downward** when the preset row expands — the four main buttons stay put and never slide behind the header.
- `topInfoSection` (`:330`) gets `id` + `visible: topLeftItems.length > 0 || topRightItems.length > 0`. Hiding it when empty reclaims the full `bottomBarHeight` each empty `LayoutBarZone` reserves (`LayoutBarZone.qml:11 implicitHeight: Theme.bottomBarHeight`).

### 7f. Two-line blinking "Place Beans on Scale" prompt (`:658`)
`weighBeansText.showingPlacePrompt = !beanCaptureShown && net < 1`. When prompting, shows
`"Place Beans on Scale" + "\n" + "(and wait for the beep before removing)"` and gently pulses
(`SequentialAnimation on opacity`, 0.45↔1.0, 800 ms legs, `running: showingPlacePrompt`;
`onShowingPlacePromptChanged: if (!showingPlacePrompt) opacity = 1.0`).

---

# 8. Build wiring
- **`CMakeLists.txt:735`** — added `qml/components/StableWeightCapture.qml` to the `qt_add_qml_module` file list (required or the component won't resolve at runtime).
- **`resources/resources.qrc:6`** — added `<file>sounds/ding.wav</file>` under the `/` prefix.

---

# Appendix A — All translation keys added (key → English fallback)

| Key | Fallback |
|---|---|
| `brewDialog.doseCaptured` | `Dose set: %1g` |
| `brewDialog.cupTareLabel` | `Dose cup:` |
| `brewDialog.doseCupWeight` | `Dose cup weight` |
| `brewDialog.weighCup` | `Weigh` |
| `brewDialog.weighEmptyCup` | `Weigh empty cup from scale` |
| `idle.doseCaptured` | `Dose set: %1g` |
| `idle.steamCaptured` | `Steam time: %1s for %2g milk` |
| `idle.label.onScale` | `on scale` |
| `idle.label.placeBeansOnScale` | `Place Beans on Scale` |
| `idle.label.placeBeansHint` | `(and wait for the beep before removing)` |
| `idle.button.espressoSetup` | `Espresso-Shot Setup` |
| `idle.button.chooseProfile` | `Choose Espresso Profile` |
| `idle.button.steamSetup` | `Milk-Steaming Setup` |
| `idle.status.profile` | `Profile` |
| `idle.status.scale` | `Scale` |
| `idle.status.ratio` | `Ratio` |
| `idle.status.beans` | `Beans` |
| `idle.status.milk` | `Milk` |
| `idle.status.none` | `—` |
| `steam.label.referenceMilk` | `Reference milk` |
| `steam.hint.referenceMilk` | `0 = off. Steam time scales with milk weight.` |
| `steam.label.expectedSteamTime` | `Expected steam time` |
| `steam.hint.forMilkOnScale` | `for %1 g milk on scale` |
| `steam.capture.locked` | `Steam time set: %1s for %2g milk` |
| `steam.label.pitcherWeight` | `Milk pitcher` |
| `steam.hint.pitcherWeight` | `Empty pitcher weight` |
| `steam.label.weigh` | `Weigh` |
| `steam.accessible.weighPitcher` | `Weigh empty pitcher from the scale` |
| `steam.accessible.setRefMilk` | `Set reference milk from the scale` |
| `steam.label.purge` | `Purge` |
| `steam.accessible.purge` | `Purge the steam wand` |
| `scale.waitForBell` | `Wait for the bell before you take it off the scale` |

---

# Appendix B — Integration risk & recommendations (candid, ranked)

Features **0–6** (the weight-dialing substrate, dose-cup tare, bean/milk capture, weight-timed
steaming with the bug fixes, the ding, steam-editor polish, the wait-for-bell hint) are
self-contained, theme-respecting, and cherry-pick friendly. The shared substrate is
`StableWeightCapture` + the two `SettingsBrew` additions. The home-screen rework (**§7**) is the
opinionated part:

1. **`shotPlan` full-mode semantic overload (§7a).** This silently redefines what the `shotPlan`
   widget *is* in full mode (passive summary → active 3-button launcher). The layout-editor palette,
   chip label, and `shotserver_layout.cpp` still say "Shot Plan," so the editor now misdescribes it,
   and users who placed `shotPlan` for the summary get buttons instead.
   **Recommendation:** introduce a *new* widget type (e.g. `setupButtons`) registered in the four
   required places, leaving `ShotPlanText`'s full mode intact.
2. **Bottom `brewStatusBar` + force-hidden `centerStatus` (§7c/§7d).** The stat bar and the
   `visible: false` on `centerStatus` bypass the configurable zone system and silently override
   layout-editor choices in code. Color also assumes `primaryContrastColor` is light and
   `primaryColor` is a saturated blue (blue-on-blue on some custom themes).
   **Recommendation:** gate the whole home restructure behind an opt-in setting (e.g. "simplified home
   screen"), or express the bar and the hidden-center decision through the zone/layout system.
3. **`LayoutCenterZone` distribute mode keyed on zone name (§7b)** and the **top-zone geometry tuned
   for the empty case (§7e)** are contained but opinionated hardcodes; a per-zone `distribute` layout
   property would be cleaner.

*This document was prepared with AI assistance from the actual diff against `v1.8.0`.*
