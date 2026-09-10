# Barista proactive coaching — making the opening intelligent

**Problem (owner, 2026-09-08):** today the barista loads the recipe/last settings and says nothing;
the user must ask "how did yesterday go?" then separately ask "any recommendations?". It feels
automated, not intelligent. It should open with a broad, informed read grounded in *the whole*
rating history (what the user likes across beans), similar beans, and community knowledge.

**Decisions (owner):** deliver the read **on first engagement** (not unprompted-on-wake), and make
it **layered** — a short warm intelligent lead + an easy "want the full rundown?" hook, NOT a
dashboard. This *extends* the existing proactive path; it keeps the "one proactive thing, not a
dashboard" discipline and upgrades the *quality* of that one thing.

## Increment 1 — WRITTEN, not yet built

### A. New data: a cross-bean palate profile
`buildPalateProfileBlock(db)` in `src/ai/aimanager.cpp` (anonymous namespace, beside
`buildProactiveRecBlock`). Emits a `palateProfile` block on the barista context:
- `ratedShots`, `topRatedBeans` (≤3), and `highestRatedShotsShareThisDial`
  (`typicalRatio` / `typicalTempC` / `preferredRoastLevel`).
- **Source discipline:** built from `shots.db` `enjoyment` (the populated base). Ratio/temp are read
  off the top-rated shots via `loadShotRecordStatic` (the trusted per-shot path) — NOT `AVG()` over
  the sparse override columns (`temperatureOverrideC` is 0 when the profile default was used).
- **Cold-start guard:** returns `{}` below `kPalateMinRatedShots` (6); roast preference asserted only
  on a plurality (≥2). Wired at the same seam as `fullHistory` (declared, populated in the shots.db
  scope, added to the `invokeMethod` capture, inserted into the payload when non-empty).

### B. Behavior: the opening leads with the read
New **OPENING READ** clause in the `mayNudge` branch of the persona
(`qml/assistant/AssistantOverlay.qml`, ~line 1113). On the first reply when coffee is fair game:
answer first, then add ONE palate-grounded read + the single most useful suggestion + a spoken
go-deeper hook. Same gates (proactivityLevel, 6h bean cooldown), same one-proactive-thing cap.
Degrades to a current-bean offer when `palateProfile` is absent. "Go deeper" needs no new tool —
the model expands from `palateProfile` + `topRatedBeans` + existing tools (`query_shots`,
`search_tasting_feedback`, `look_up_bean`).

### If the read doesn't appear after building — debug the GATE first, not the prose
The OPENING READ lives in the `else if (mayNudge)` branch. `mayNudge` is
`(proactivityLevel !== "off") && (hasUndiscussedShot || consumeProactiveNudge(bean, 6h))`. On a
first session-engage with default `proactivityLevel="full"` and no recent nudge for the bean it is
true, but that was reasoned, not observed. So if the read is absent post-build, inspect the
gate/cooldown state (has a nudge for this bean been consumed in the last 6h?) before touching the
clause wording. (Likely root cause of today's silence: the pre-existing recipe offer correctly
offers nothing when the bag is in its peak window — the palate read fixes that by giving the model
something grounded to lead with.)

### Optional automated signal
`buildPalateProfileBlock` is an anonymous-namespace static, so reaching it from a test needs a
`DECENZA_TESTING` seam. If added, the justified new-defect-shape tests are: returns `{}` below the
6-rated-shot floor, and asserts a roast/bean preference only on a plurality (≥2). Deferred this
increment (seam surgery vs. increment scope — owner call).

### Verify — build/test on this Mac is `cmake`/`ctest`, NOT the Qt Creator MCP
The MCP bridge isn't exposed in headless CLI sessions; build directly (see `FORK_STATE.md`).
**Done 2026-09-08:** `aimanager.cpp` compiles clean (both increments); `tst_aimanager`,
`tst_aiproviders`, `tst_closeintent` PASS via `ctest` in `build/Qt_6_11_2_for_macOS_Debug`.
⚠️ A full `cmake --build . -j` fails at the **qmllint gate**, but only on PRE-EXISTING unrelated
in-progress QML files (BrewDialog/main/IdlePage/PostShotReviewPage/BaristaChipRow/BaristaItem/…) —
`AssistantOverlay.qml` (this work's only QML edit) adds no new warnings. That gate is red on the
branch independent of this change; clean it up separately.

Still worth doing in the live app (behavior the tests don't cover):
1. Engage the barista at session start on a bean with rated history → first reply should lead with a
   grounded read + one suggestion + a go-deeper offer, not just answer literally.
2. Cold-start: a fresh profile (<6 rated shots) → no palate line, normal current-bean offer, no
   mention of missing history.
3. A bean sharing roast/origin with others you've rated → `similarBeanExperience` surfaces and the
   barista can advise "on your other washed Ethiopians…".

## Increment 2 — BUILT + committed (2026-09-10 audit)

Landed in `4e1768bb` ("open with a cross-bean palate read and similar-bean advice") alongside
Increment 1; 2b landed in `338130d9`. The C++ data side (`buildSimilarBeanBlock` + the
`similarBeanExperience` wiring) and the QML persona side (the SIMILAR BEANS & COMMUNITY clause) are
both present on `feat/barista` and match the spec below. `tst_aimanager`/`tst_aiproviders`/
`tst_closeintent` pass. What's left is the same as Increment 1: **owner on-device *listening*, not
code** (verify the opening read actually surfaces similar-bean/community advice in the live app —
the behaviour cases at the end of Increment 1 cover it).

### A. New data: similar-bean transfer learning
`buildSimilarBeanBlock(db, current)` in `src/ai/aimanager.cpp` emits `similarBeanExperience` — the
user's OWN best-rated shots on beans that RESEMBLE the current one. Matching: parse the current
shot's `beanBaseJson` (`origin`/`process`) + `roastLevel`, scan the top-40 rated shots on OTHER
beans, dedupe to the best per bean (≤12), load each via `loadShotRecordStatic` (cursor drained
first), score shared attributes (roast +2, origin +2, process +1), keep score ≥ 2, emit the top 2
with `sharedWith` + `bestRating` + `ratio`/`grind`. Returns `{}` when nothing similar is rated.
- **Guarded on `!beanFilterMissed`** at the call site — only builds when the anchor genuinely IS the
  current bean, else it'd match "similar" to the latest-overall (different) bean.
- Wired at the same seam as `palateProfile` (declared, populated, captured, inserted when non-empty).

### B. Behavior: similar-bean + community in the opening/go-deeper path
New **SIMILAR BEANS & COMMUNITY** clause after OPENING READ (`AssistantOverlay.qml`): lean on
`similarBeanExperience` when the bean is new/thin (borrowed grind = starting hint, not a target);
when the bean is new to them or they go deeper, reach for the **community tools** already declared —
`look_up_bean` (roaster origin/process/roast/tasting notes) and `search_visualizer_shots` (how
others pulled it) — and fold ONE attributed takeaway in. Community stays a tool reached on demand
(no per-turn network fetch), so latency/cost stay bounded.

### Increment 2b — DONE (the new-bean local case)
When the current bean was NEVER pulled (`beanFilterMissed`) there's no shot to read its attributes
from — which is exactly when transfer advice matters most. Now `requestBaristaContext` reads the
CURRENT bean's origin/process (from the active bag's `dyeBeanBaseData` JSON) + roast (`dyeRoastLevel`)
on the main thread, guarded to the case where the bag's bean matches the requested bean, and passes
them into the worker. `buildSimilarBeanBlock` was refactored to take explicit attributes + the
identity to exclude + a `currentIsNew` flag (set on `beanFilterMissed`), so it matches similar beans
for a brand-new bean too. The block gains `currentBeanIsNew: true` in that case, and the persona's
SIMILAR BEANS clause leads with it ("you haven't dialed this one yet, but your other washed
Ethiopians landed around a finer grind"). Community tools (`look_up_bean` / `search_visualizer_shots`)
are now the fallback for when *nothing rated resembles* the bean, or the user wants to go deeper.
