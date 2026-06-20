# Decenza — detailed change discussion (vs v1.8.0)

This document explains each change: what it does, why it's designed that way, the
gotchas discovered during implementation, and notes for integrating it into core.
Conventions from `CLAUDE.md` were followed (Theme tokens, no timers-as-guards,
`Tr`/`TranslationManager` for strings, accessibility roles on interactive elements).

---

## 1. `StableWeightCapture` — the shared building block (NEW file)

`qml/components/StableWeightCapture.qml` — a non-visual, **event-driven** scale-settle
detector. The parent binds `weight` to a net value (e.g. `scaleWeight − tare`). When
that value holds within `tolerance` of a candidate for `stableMs`, it emits
`stableCaptured(grams)` exactly once and latches. It re-arms when the load is removed
(`< removeThreshold`) or changes materially (`> rearmDelta`).

**Design notes**
- **No guard timers.** Detection is driven off `onWeightChanged`. A low-frequency
  poll `Timer` (150 ms, only while armed) re-checks elapsed time so a scale that streams
  a perfectly constant value still graduates — this is legitimate periodic polling, not
  a timer-as-guard.
- Tunables exposed as properties: `minWeight`, `maxWeight`, `tolerance`, `stableMs`,
  `removeThreshold`, `rearmDelta`. Beans use a tight tolerance (0.5 g) and a `maxWeight`
  (~45 g) so a milk pitcher never trips the bean detector; milk uses 1.5 g tolerance.
- Outputs: `isCaptured`, `capturedValue`, signal `stableCaptured(real grams)`.

**Integration:** registered in `CMakeLists.txt`'s `qt_add_qml_module` file list (required
or it won't be in the resource system at runtime). Pure QML, no C++.

---

## 2 & 3. Dose-cup tare + bean auto-capture

- **`SettingsBrew::doseCupTareWeight`** (persistent, clamped ≥ 0): the empty dosing-cup
  weight, subtracted from the scale so the recorded dose is *net beans*.
- **Auto-capture:** `BrewDialog.qml` and `IdlePage.qml` each host a `StableWeightCapture`
  bound to `scaleWeight − doseCupTareWeight`. On `stableCaptured`, the dose is set
  (same math as the existing "Get from scale" button) and the ding plays.
  - Idle: active on the home screen *except* while steam/hot-water/flush presets are
    showing (those use the scale for milk/water), bounded to ≤ 45 g.
  - Brew dialog: active while the dialog is open.
- The old prominent "Weigh beans" button became a small live readout ("`18.0 g on scale`",
  flashing "Place Beans on Scale / (and wait for the beep before removing)" when empty),
  since auto-capture replaces the manual tap.

**Gotcha:** auto-setting the dose from a stray reading is mitigated by the stable-hold
requirement + the dose-range `maxWeight`. The one rough edge is the first time you weigh
an *empty* cup to set the tare (net ≈ cup weight) — it can briefly capture; placing beans
re-captures the correct value.

---

## 4. Weight-timed steaming

The core idea is pure proportion: `steamTime = referenceDuration × measuredMilk / referenceMilk`.

- **`SettingsBrew::setSteamPitcherCalibration(index, calibMilkG)`** stores `calibMilkG`
  (the reference milk weight) on the preset; pairs with the preset's existing `duration`
  and `pitcherWeightG` (empty-pitcher tare). 0/negative clears it.
- **Measurement:** `currentMeasuredMilk() = scaleWeight − pitcherWeightG` (or the raw
  reading if the user tared the scale instead — `pitcherWeightG` optional).
- **Auto-capture + lock:** a `StableWeightCapture` on the steam page (and an equivalent
  on the idle steam flow) locks `steamTimeout` to the scaled value when the milk settles,
  dings, and shows a banner. The value persists while the pitcher is lifted to the wand.

### The important bug we fixed (read this before adopting)

Several code paths *re-sync* `steamTimeout` to the **unscaled** preset duration, which
silently wiped the scaled value:
1. **Steam start with the pitcher already lifted** — the live scale reads ~0, so the
   scale-based computation fell back to the fixed duration. Fix: idle `onPresetSelected`
   falls back to the **last measured milk** (`measuredMilkG`) when the live reading is
   empty.
2. **Steam-page activation re-sync** — runs just *before* the steaming state is confirmed,
   when the pitcher is off the scale. Fix: a `syncSteamTimeout()` helper that keeps the
   already-scaled value when the preset is calibrated but no milk is on the scale, and
   only falls back to the fixed duration when the preset isn't calibrated.
3. **Pitcher-pill taps on the steam page** hard-set `modelData.duration`; now they use the
   scaled value when milk is present.

The live steaming countdown shows `shotTime / steamTimeout`, so the denominator is the
authoritative check that the scaled value reached the DE1 (`TargetSteamLength`).

**Also reactive-binding gotcha:** the reference-milk display didn't refresh after the
Weigh button because its getter didn't read `Settings.brew.steamPitcherPresets` (the
change-signal source). Fixed by reading it inside the getter so the binding re-evaluates.

---

## 5. Capture "ding"

- **`AccessibilityManager::playCaptureDing()`** plays `qrc:/sounds/ding.wav` via a
  dedicated `QSoundEffect`, **pre-loaded in the constructor** (`initDingSound()`), ungated
  by `m_enabled`/`m_tickEnabled` (it's a general UI cue, not an accessibility cue).
- Pre-loading at startup matters: a lazily-created `QSoundEffect` is still `Loading` on
  the first call, so the first ding would be silent otherwise.
- `resources/sounds/ding.wav` is a synthesized struck-bell tone (fundamental + partials,
  exponential decay), registered in `resources/resources.qrc`.

**Integration note (style):** this lives in `AccessibilityManager` only because it already
owns the `QSoundEffect` machinery and is exposed to QML. A cleaner home would be a small
dedicated `SoundManager` context property — easy refactor if core prefers domain
separation.

**Don't forget the binary:** a text patch can't carry `ding.wav`; copy it from the zip
(`README.md` has the command). Without it the build still compiles but the ding is silent.

---

## 6. Steam editor polish

- **Scrollable editor:** the per-pitcher settings panel was a fixed-height column that
  clipped its lower rows; it's now wrapped in a `Flickable` (with a `ScrollBar`). Note the
  "Off"-preset placeholder switched from `Layout.fillHeight` to a fixed preferred height
  (fillHeight is meaningless inside a content-sized Flickable).
- **0.1 g inputs:** empty-pitcher and reference-milk `ValueInput`s use `stepSize: 0.1`,
  `decimals: 1` (dose and dose-cup were already 0.1 g).
- **Weigh buttons:** inline buttons set the empty-pitcher weight / reference milk straight
  from the scale (the reference-milk Weigh captures `currentMeasuredMilk()`).
- **Purge button:** in the editor and on the **live steaming view**, calling
  `DE1Device.requestIdle()` (the codebase's documented steam-purge trigger).

---

## 7. "Wait for the bell" prompt

A small flashing reminder ("Wait for the bell before you take it off the scale") shown
only while something is on the scale and the matching `StableWeightCapture` hasn't fired
yet (`active && !isCaptured && weight ≥ minWeight`). Disappears the instant it captures.
Present for both beans (espresso context) and milk (steam context, incl. the steam page).
Uses a `SequentialAnimation on opacity` (allowed: animation, not a guard timer).

---

## 8. Home-screen rework  ⚠️ opinionated — review before adopting

These changes are tuned to one user's 1920×1200 tablet and **repurpose existing layout
widgets**, so they're the least "drop-in":

- **`ShotPlanItem.qml` full mode repurposed.** Instead of the shot-plan text, it now
  renders three white/blue setup buttons centered in thirds: *Espresso-Shot Setup*
  (opens the brew dialog), *Choose Espresso Profile* (pushes `ProfileSelectorPage`),
  *Milk-Steaming Setup* (pushes `SteamPage` — avoids the long-press). **Compact (bar) mode
  is unchanged.** This is a semantic change to the `shotPlan` widget; core may prefer a
  *new* widget type (register in the 4 places per `CLAUDE.md`) so the shot-plan text
  survives.
- **`LayoutCenterZone.qml`** gained a `distributeItems` mode (for `centerStatus`/
  `centerMiddle`) that spreads items across the full width with equal cells instead of
  clustering them centered.
- **`IdlePage.qml`:**
  - A bottom **stat bar** (Profile · Scale · Ratio · Beans · Milk) with a high-contrast
    light background / blue text. Even spacing uses `Layout.fillWidth` + equal
    `Layout.preferredWidth` + per-cell `horizontalAlignment` (don't rely on
    `Layout.alignment: AlignHCenter` for centering inside a fillWidth cell — it shifts).
  - The center **status readouts** (`centerStatus` zone) are hidden — duplicated in the
    global status bar. *(Hardcoded `visible: false`; core may prefer leaving this to the
    layout editor.)*
  - A thin line pinned at `Theme.statusBarHeight` as the **bottom border of the global
    status bar**. The empty `topLeft`/`topRight` zones (which reserve a full
    `bottomBarHeight` via `LayoutBarZone.implicitHeight`) are hidden so they don't push
    content down.
  - The center block is **anchored from the top** (below the border line) rather than
    vertical-center, so when the preset row expands it grows **downward** and the four
    main buttons never slide behind the header.

**If adopting only the universal parts:** features 1–7 do not depend on feature 8. The
`settings_brew` additions (`doseCupTareWeight`, `setSteamPitcherCalibration`) and the
`StableWeightCapture` component are the shared substrate.

---

## Build / verification

- Qt 6.11.1. Desktop (macOS arm64, clang) and Android (`arm64-v8a`, NDK r27c
  `27.2.12479018`, JDK 17) both build clean (0 errors). Android built with `-DBUILD_TESTS=OFF`
  (host unit tests require desktop OpenSSL, unavailable in the cross-build).
- New QML file and the sound resource are registered (`CMakeLists.txt`, `resources.qrc`).
- New translation keys all have English fallbacks, so they render immediately; proper
  translations can be added via the normal flow. Keys added include: `brewDialog.*`,
  `idle.button.espressoSetup` / `chooseProfile` / `steamSetup`, `idle.status.*`,
  `idle.label.placeBeansOnScale` / `placeBeansHint`, `steam.label.referenceMilk` /
  `weigh` / `purge` / `expectedSteamTime`, `steam.capture.locked`, `scale.waitForBell`.
