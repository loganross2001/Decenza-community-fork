# Decenza — contribution summary (vs v1.8.0)

A set of espresso/steam **dialing-by-weight** features plus a home-screen rework,
built on top of the upstream `v1.8.0` tag. Compiles on desktop (macOS/clang) and
Android (Qt 6.11.1, NDK r27c); both verified.

## Features at a glance

| # | Feature | Core-worthy? | Files |
|---|---------|--------------|-------|
| 1 | **`StableWeightCapture`** — reusable, event-driven scale-settle detector (no guard timers) | ✅ strongly | `StableWeightCapture.qml` (new) |
| 2 | **Bean auto-capture** — rest the dose cup → net dose is set automatically + ding | ✅ | `BrewDialog.qml`, `IdlePage.qml`, `settings_brew.*` |
| 3 | **Dose-cup tare** — store empty-cup weight, subtract for net beans | ✅ | `settings_brew.*`, `BrewDialog.qml` |
| 4 | **Weight-timed steaming** — reference-milk → duration baseline; auto-scale steam time by measured milk | ✅ | `SteamPage.qml`, `IdlePage.qml`, `settings_brew.*` |
| 5 | **Capture "ding"** — pleasant bell on capture, independent of accessibility mode | ✅ | `accessibilitymanager.*`, `resources.qrc`, `ding.wav` (new) |
| 6 | **Steam editor polish** — scrollable editor, Weigh buttons, 0.1 g inputs, **Purge** button | ✅ | `SteamPage.qml` |
| 7 | **"Wait for the bell" prompt** — flashing hint while the scale settles | ✅ (small) | `IdlePage.qml`, `SteamPage.qml` |
| 8 | **Home-screen rework** — 3 setup buttons, bottom stat bar, status-bar border line, layout anchoring | ⚠️ opinionated | `IdlePage.qml`, `ShotPlanItem.qml`, `LayoutCenterZone.qml` |

**Recommended cherry-pick:** features 1–7 are broadly useful and fairly self-contained.
Feature 8 is tailored to one user's preferences and **repurposes the `shotPlan`
layout widget**, so review it before adopting (see `DETAILS.md` §8 for how to make it
opt-in instead).

## What changed, in one paragraph each

- **Weight capture (1–3):** A new `StableWeightCapture` QML component fires `stableCaptured(grams)`
  when a net weight holds within a tolerance for ~2.5 s, and re-arms when the load is
  removed/changed. The brew dialog and idle page use it to auto-set the espresso dose
  (net of a stored dose-cup tare), dinging on success. A flashing "wait for the bell"
  prompt discourages lifting before the measurement locks.
- **Weight-timed steaming (4):** Each steam pitcher preset gains a *reference milk*
  weight paired with its duration. When milk is measured, steam time scales
  proportionally (`duration × measuredMilk / referenceMilk`) and the DE1 auto-stops
  there. Works from both the home-screen steam flow and the steam page; the steam page
  shows the live "expected steam time".
- **Ding (5):** `AccessibilityManager::playCaptureDing()` plays a synthesized bell
  (`resources/sounds/ding.wav`), pre-loaded at startup and ungated by the accessibility
  toggle.
- **Steam editor (6):** The per-pitcher editor is now scrollable, inputs are 0.1 g, the
  empty-pitcher and reference-milk fields have **Weigh** buttons, and there's a **Purge**
  button (editor + live steaming view) that triggers the DE1 steam purge.
- **Home screen (8):** The center "shot plan" area becomes three setup shortcuts
  (Espresso-Shot Setup / Choose Espresso Profile / Milk-Steaming Setup); a high-contrast
  bottom stat bar shows Profile · Scale · Ratio · Beans · Milk; the duplicate center
  status readouts are hidden (the global status bar already shows them); a thin line
  underlines the status bar; and the center block is top-anchored so it never slides
  behind the header.

See `DETAILS.md` for the full per-feature discussion (design rationale, gotchas,
integration notes) and `README.md` for how to apply.
