# plan-widgets Specification

## Purpose
Defines the text-rendering rules for the `shotPlan` widget's shot-plan and steam-plan sentences: how the configured display-item list maps to sentence and fragment formats, override indicators for temperature and yield, the cleaning-profile warning, page-aware switching between shot and steam plans, and the wrap-before-elide overflow behavior.
## Requirements
### Requirement: Shot plan sentence content follows the display toggles

The Shot Plan text (used by the `shotPlan` widget) SHALL render the display items named by the instance's ordered item list (`shotPlanItems`), in list order, in one of two formats selected by the instance's Sentence style option (`shotPlanSentence`, default ON).

The eight display items are: Profile (`profile`), Temperature (`temperature`), Roaster (`roaster`), Coffee (`coffee`), Grind (`grind`), Roast date (`roastDate`), Recipe (`recipe`), and Dose & yield (`doseYield`). Each item SHALL contribute exactly its named content, and an item absent from the list SHALL contribute nothing:

- **Profile** (`profile`): the profile name.
- **Temperature** (`temperature`): the brew temperature (following the C/F display unit and the temperature-override rendering).
- **Roaster** (`roaster`): the roaster brand only.
- **Coffee** (`coffee`): the coffee (bean) name only.
- **Grind** (`grind`): the grinder setting, plus the grinder RPM when one is recorded (`Settings.dye.dyeGrinderRpm` > 0).
- **Roast date** (`roastDate`): the roast date.
- **Recipe** (`recipe`): the active recipe (drink) name, and nothing when no recipe is active.
- **Dose & yield** (`doseYield`): the dose ("{dose} in") and the target output weight.

**Fragment format** (Sentence style OFF): every present item's segment SHALL be joined with the standard separator in item-list order.

**Sentence format** (Sentence style ON): the sentence scaffold has a single "using {anchor}" slot. The anchor SHALL be filled by the Profile item's profile name when the Profile item is present and a profile name is available; otherwise, when the Recipe item is present and a recipe is active, the anchor SHALL be filled by the active recipe name. (Recipe thus stands in for Profile in the same grammatical slot — "using {recipe}" — for the intended use of replacing Profile with Recipe.) When an anchor is available, the scaffold SHALL consume the Dose & yield and Temperature items together with the anchor item and the beverage word (wherever they sit in the list), and all other present items SHALL trail after the sentence as a separator-joined tail in item-list order. With the Stacked details option (`shotPlanStacked`, default OFF) enabled, the tail SHALL render on its own line(s) below the sentence instead of trailing after a separator — on the display path only: the accessibility string SHALL remain one separator-joined sentence, and compact (bar) placements SHALL ignore the option. The scaffold SHALL degrade by what is present/available:

- yield + anchor + temperature → "Brew {yield} of {beverage}, using {anchor} at {temperature}"
- anchor + temperature, no yield → "Brew {beverage}, using {anchor} at {temperature}"
- yield + anchor, Temperature item absent → "Brew {yield} of {beverage}, using {anchor}"
- anchor only → "Brew {beverage}, using {anchor}"

When both the Profile and Recipe items are present with values, the Profile name SHALL fill the anchor and the Recipe name SHALL trail as an ordinary tail item; Recipe never displaces a present, available Profile.

No anchor available (the Profile item absent or with no profile name, AND no active recipe available to fill the slot) → the sentence has no anchor, so it falls back to the profile-less "recipe" sentence: the scaffold instead consumes the Dose & yield, Temperature, Roaster, and Coffee items (wherever they sit in the list) together with the beverage word, and only the Grind and Roast date items SHALL trail after it as a separator-joined tail in item-list order. The beverage word SHALL always anchor this sentence, so Sentence style SHALL NOT degrade to fragment format while it is ON — regardless of which other items are present:

- yield + temperature + dose + roaster + coffee → "Brew {yield} of {beverage} at {temperature} from {dose} of {roaster} {coffee}"
- yield only, no temperature/dose/roaster/coffee → "Brew {yield} of {beverage}"
- no yield, dose present, no roaster/coffee → "Brew {beverage} from {dose}"
- no yield, no dose, roaster + coffee only → "Brew {beverage} from {roaster} {coffee}"

The beverage word SHALL follow the current profile's `beverage_type`: "Espresso" for espresso (and unset), "tea" for tea types, and "coffee" for any other coffee beverage (filter, pourover, …). A cleaning or descale profile SHALL replace the plan entirely — in both formats — with a cleaning notice carrying a do-not-load-coffee warning, rendered bold in the error (red) color with no item segments.

The plain (accessibility) text and the rich (displayed) text SHALL be produced by one shared builder so their content cannot drift.

#### Scenario: Fragment format follows chip order

- **WHEN** Sentence style is OFF and the item list is `["grind", "coffee", "doseYield"]`
- **THEN** the plan renders "grind {setting} · {coffee} · {dose}g in · {yield}g" — the grind segment first, in exactly the configured order

#### Scenario: Recipe item renders the active recipe name in fragment order

- **WHEN** Sentence style is OFF, a recipe named "Morning Latte" is active, and the item list is `["recipe", "doseYield"]`
- **THEN** the plan renders "Morning Latte · {dose}g in · {yield}g" — the recipe name first, in the configured order

#### Scenario: Recipe replaces Profile as the sentence anchor

- **WHEN** Sentence style is ON, a recipe named "Morning Latte" is active, the item list is `["doseYield", "temperature", "recipe", "grind"]` (no `profile`)
- **THEN** the plan renders "Brew {yield} of Espresso, using Morning Latte at 92°C" followed by the tail "grind {setting}" — the recipe name filling the "using" slot exactly where a profile name would

#### Scenario: Profile takes the anchor when both Profile and Recipe are shown

- **WHEN** Sentence style is ON, a recipe named "Morning Latte" is active, a profile is loaded, and the item list is `["doseYield", "profile", "temperature", "recipe"]`
- **THEN** the plan reads "Brew {yield} of {beverage}, using {profile} at {temperature}" with "Morning Latte" trailing as a tail item — the profile keeps the anchor

#### Scenario: Recipe item contributes nothing when no recipe is active

- **WHEN** the `recipe` item is present but no recipe is active
- **THEN** the recipe segment is empty; in fragment mode it renders nothing, and in sentence mode it neither fills the anchor nor trails, so the plan renders exactly as if the `recipe` item had been absent

#### Scenario: Stacked details move the tail to its own line

- **WHEN** Sentence style and Stacked details are both ON and the item list includes trailing items
- **THEN** the widget displays the sentence on the first line and the separator-joined tail on the line(s) below it, while a screen reader still hears one joined sentence

#### Scenario: Sentence consumes its core items and the tail keeps user order

- **WHEN** Sentence style is ON and the item list is `["roastDate", "doseYield", "profile", "temperature", "grind", "coffee"]`
- **THEN** the plan renders "Brew {yield} of Espresso, using {profile} at {temperature}" followed by the tail "roasted {date} · grind {setting} · {coffee}" — the tail in list order, the scaffold in template order

#### Scenario: Temperature chip removed drops the at-clause

- **WHEN** Sentence style is ON and the item list contains `profile` and `doseYield` but not `temperature`
- **THEN** the sentence reads "Brew {yield} of {beverage}, using {profile}" with no temperature anywhere

#### Scenario: Profile chip removed renders the profile-less recipe sentence

- **WHEN** Sentence style is ON, no recipe is active, and the item list is `["temperature", "coffee"]` (no `profile`, no `doseYield`)
- **THEN** the plan renders "Brew {beverage} at {temperature} from {coffee}" — a sentence anchored on the beverage word, not a fragment list

#### Scenario: No profile loaded renders the profile-less recipe sentence even with the Profile chip shown

- **WHEN** Sentence style is ON, the item list includes `profile`, no profile is currently loaded (no profile name available), and no recipe is active
- **THEN** the plan renders the profile-less recipe sentence, exactly as if the Profile chip had been removed

#### Scenario: Filter profile says coffee, not espresso

- **WHEN** the current profile's beverage_type is "filter" (or "pourover")
- **THEN** the sentence reads "Brew {yield} of coffee, using {anchor} at {temperature}"

#### Scenario: Profile without a target weight keeps the beverage word

- **WHEN** the current profile has no target weight (e.g. a filter or tea profile with no stop-at-weight) but the Profile and Temperature items are present with values
- **THEN** the plan reads "Brew {beverage}, using {profile} at {temperature}", not a fragment list without the beverage word

#### Scenario: Cleaning profile warns instead of planning

- **WHEN** the current profile's beverage_type is "cleaning" or "descale"
- **THEN** the widget shows a cleaning notice including a warning not to put coffee in the portafilter, with no item segments, regardless of the Sentence style option

#### Scenario: Grind item shows grind and RPM

- **WHEN** the `grind` item is present, the grinder setting is "2.5", and the recorded RPM is 90
- **THEN** the plan's grind segment shows the grind setting and "90 rpm"

#### Scenario: Grind item without RPM

- **WHEN** the `grind` item is present and no RPM is recorded (dyeGrinderRpm = 0)
- **THEN** the grind segment shows only the grinder setting, with no RPM placeholder

### Requirement: Override indicators

The Shot Plan text SHALL highlight only the individual overridden item(s), not the whole sentence: when a deliberate temperature override is active (the override differs from the surface's temperature baseline by more than 0.1°C) the temperature item SHALL render in the override-highlight color; when a deliberate yield override is active (the target differs from the surface's yield baseline by more than 0.1 g) the yield item SHALL render as "{baselineYield} → {target}g" in the override-highlight color. Every other part of the sentence, and the plan icon, SHALL remain the default text color. The override-highlight color SHALL be `Theme.highlightColor` — the same color the Brew Settings values use for an override — so the two surfaces read consistently. Natural drift between measured dose and profile dose SHALL NOT trigger either indicator.

Because the highlight lives in the shared `ShotPlanText` component, it SHALL apply on **every** surface that renders the Shot Plan — the idle home widget, recipe cards, the shot-detail and post-shot-review snapshot lines, and the screensaver preview — not only the idle page. Each surface's highlight SHALL be driven by **that surface's own** override inputs: the live dial for the home widget, and the shot's frozen snapshot values for the shot-detail and post-shot-review lines. A frozen-shot surface SHALL NOT reflect the live dial's override state; if it does not render the temperature or yield item, that item simply shows no highlight (it does not borrow the live override).

**Recipe cards have no override inputs of their own and SHALL render with no highlight**: a card shows a static recipe definition, not a live per-brew comparison, so neither its temperature nor its yield item ever carries the override-highlight color. The temperature segment SHALL still be resolved from the recipe's own (possibly not-currently-loaded) profile's frame temperatures, shifted by the recipe's stored `tempOffsetC`, and rendered as the resulting value only — see `recipe-quick-switch` for the full rendering rule. The yield segment SHALL render as the plain resulting value with no arrow.

#### Scenario: Highlight appears on non-idle surfaces
- **WHEN** a shot snapshot line renders a plan whose shot carries a yield or temperature override
- **THEN** that overridden item is highlighted using the same per-item scheme as the idle widget, driven by the shot's own values

#### Scenario: Recipe cards never highlight
- **WHEN** a recipe card renders a recipe with `tempOffsetC` = −3 on a profile whose frames are 84 · 94°C, or a recipe whose stored yield differs from its profile's target
- **THEN** the card's temperature and yield segments both render in the default text color — no tag, no arrow, no highlight

#### Scenario: Frozen surfaces do not borrow the live override
- **WHEN** a live temperature override is active and the user opens a historical shot whose snapshot had no override
- **THEN** the shot-detail plan line shows no highlight (it reflects the shot's frozen state, not the live dial)

#### Scenario: Temperature override highlights only the temperature item
- **WHEN** a deliberate temperature override is active on the live dial or a shot snapshot
- **THEN** that surface's temperature item renders in `Theme.highlightColor`
- **AND** the rest of the sentence and the icon remain the default text color

#### Scenario: Yield override shows the arrow, highlighted
- **WHEN** the user dials a yield override of 40.0 g on a profile whose default yield is 36.0 g
- **THEN** the plan shows "36.0 → 40.0g" and that yield fragment renders in `Theme.highlightColor`
- **AND** the rest of the sentence remains the default text color

#### Scenario: No override, no arrow, no highlight
- **WHEN** no yield or temperature override is set
- **THEN** the plan shows the single target weight with no arrow, and the whole sentence renders in the default text color

#### Scenario: Natural dose drift does not highlight
- **WHEN** the measured dose differs from the profile dose but no deliberate override flag is set
- **THEN** no item is highlighted

### Requirement: Steam plan sentence

The Steam Plan text (the steam-context rendering of the `shotPlan` widget) SHALL summarise the next steam as "Steam {milk} of milk, using the {pitcher} pitcher for {duration}", degrading to a separator-joined list when a piece is missing, and SHALL render nothing when the selected pitcher preset is the disabled ("Off") preset. When the selected preset's name already contains the word "pitcher" (case-insensitive), the sentence SHALL NOT append another "pitcher", so a preset named "Large Pitcher" never renders "Large Pitcher pitcher". The displayed duration SHALL be the selected preset's effective steam time (scaled when weight-timed steaming has a measured/reference milk, else the preset's base duration), resolved by the single shared `SettingsBrew` helper — never the residue another code path wrote to `steamTimeout`.

#### Scenario: Pitcher name containing "Pitcher"

- **WHEN** the selected preset is named "Large Pitcher"
- **THEN** the sentence reads "…using the Large Pitcher for 30s" (no duplicated word)

#### Scenario: Duration reflects the selected preset

- **WHEN** the user selects a pitcher preset without tapping it to start
- **THEN** the displayed duration matches that preset's effective steam time, not a stale value from a previous session

### Requirement: Page-aware steam mode of the Shot Plan widget

The `shotPlan` widget SHALL display the Shot Plan text except in steam context — steam selected on the idle screen, the steam page active, or the machine actively steaming — where it SHALL display the Steam Plan text, unless the instance's Steam plan option (`shotPlanShowSteamPlan`, default ON) is off. Page/mode state SHALL be read from the app's single existing page-state source (the `Theme` singleton), not from a duplicate copy. Outside steam mode the widget SHALL be an activatable control that opens Brew Settings; in steam mode it SHALL be a read-only summary, and its accessibility role and name SHALL match the mode.

#### Scenario: Switches to steam plan

- **WHEN** the user selects steam on the idle screen and the instance's Steam plan option is on
- **THEN** the widget shows the steam plan and is announced as a read-only "Steam plan"

#### Scenario: Steam plan option off

- **WHEN** the instance's Steam plan option is off and the machine is steaming
- **THEN** the widget keeps showing the shot plan

#### Scenario: Shot side opens Brew Settings

- **WHEN** the widget is showing the shot plan and is tapped (or activated via a screen reader)
- **THEN** Brew Settings opens

### Requirement: Shot plan overflow wraps before eliding

When the Shot Plan text is wider than the width available to the widget, it SHALL wrap onto a second line, and only content that does not fit within two lines SHALL be elided. The text SHALL NEVER be clipped mid-word at the widget or screen edge. Wrapping and eliding SHALL apply to both the sentence and fragment formats and preserve the rich-text styling (bolded live values).

#### Scenario: Long plan wraps to a second line

- **WHEN** the rendered plan's natural single-line width exceeds the width granted to the widget
- **THEN** the text wraps onto a second line and all content remains readable

#### Scenario: Extreme overflow elides, never clips

- **WHEN** the rendered plan does not fit even within two lines at the granted width
- **THEN** the text ends with an ellipsis at the end of the second line, with no characters cut off at the widget edge

#### Scenario: Short plan stays on one line

- **WHEN** the rendered plan fits the granted width on one line
- **THEN** it renders on a single line, centered as today

### Requirement: Recipe item defaults off and is offered by both layout editors

The Recipe (`recipe`) display item SHALL default OFF: it SHALL NOT appear in the canonical default item order, and no widget configuration saved before this change SHALL gain it. It becomes visible only when a user explicitly adds it to an instance's `shotPlanItems` list. The shared item catalog (`allKeys`) SHALL include `recipe` so both layout editors can offer it.

Both layout editors SHALL offer `recipe` as an addable/removable item with a translatable "Recipe" label: the in-app chip editor (Shot Plan Settings — the "Shown" and "Available" rows) and the ShotServer web layout editor (item catalog and labels). The two editors SHALL stay in sync on the item set and its label.

#### Scenario: Recipe is off in the default layout

- **WHEN** a Shot Plan widget uses the canonical default item order (no `shotPlanItems` saved, or the pre-change defaults)
- **THEN** the recipe name is not shown, and the widget renders exactly as before this change

#### Scenario: Recipe appears in both editors' available items

- **WHEN** a user opens the in-app Shot Plan Settings, or the ShotServer web layout editor, for a Shot Plan widget that does not yet show Recipe
- **THEN** "Recipe" is offered in the Available items and can be added to the Shown list, and once shown can be reordered and removed like any other item

### Requirement: Recipe name is the active recipe live and the frozen recipe on shot surfaces

On the live idle widget the Recipe item SHALL render the currently active recipe's name (gated on there being an active recipe). On a frozen-shot surface — the shot-detail and post-shot-review snapshot lines — the Recipe item SHALL render that shot's own recorded recipe name, driven by the surface's own snapshot input rather than the live active recipe, consistent with how those surfaces already supply the shot's frozen dose, grind, temperature, and beverage. A surface that supplies no recipe name (no recipe on the shot, or a card with no recipe) SHALL render the item empty, and SHALL NOT let an empty recipe fill the sentence anchor.

#### Scenario: Idle widget shows the live active recipe

- **WHEN** the Recipe item is shown on the idle Shot Plan widget and the active recipe changes
- **THEN** the widget updates to the newly active recipe's name

#### Scenario: Shot snapshot shows the shot's own recipe, not the live one

- **WHEN** the Recipe item is shown on the shot-detail or post-shot-review snapshot line for a past shot, and a different recipe is now active live
- **THEN** the line shows the recipe recorded for that shot, not the currently active recipe

