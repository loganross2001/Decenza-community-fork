# Tasks

## 1. Resolve the unregistered fork QML

- [ ] 1.1 Decide, per file, whether `qml/assistant/avatars/AvatarFace.qml` and `AvatarOrb.qml` belong in the module: if used, add them to the `qt_add_qml_module` file list in `CMakeLists.txt`; if intentionally not bundled, add them to `NOT_IN_MODULE_BY_DESIGN` in `scripts/qmllint_report.py` with a one-line why. The gate demands exactly one of the two.
- [ ] 1.2 Confirm the other two files in that fork dir (`AvatarBean.qml`, `AvatarCup.qml`) are already registered, so the whole `assistant/avatars/` set is accounted for.

## 2. Make the fork's unqualified accesses resolvable (not silenced)

- [ ] 2.1 Establish the TRUE per-file counts from a fully-resolving build (task 3.1 first) — an under-resolving run inflates `unqualified` and would send this work chasing phantoms (the `#1680` shape the gate script warns about).
- [ ] 2.2 For the barista components the gate calls "new" (`BaristaChipRow`, `BaristaEditDialog`, `TempPickerDialog`, `layout/items/BaristaItem`): reach zero `unqualified` by making identifiers resolvable (singleton/type registration under the module URI), per the "new files must be clean" rule. Do NOT baseline them at a non-zero count.
- [ ] 2.3 For fork edits to files upstream baselines at 0 (`main.qml`, `IdlePage.qml`, `BrewDialog.qml`, `PostShotReviewPage.qml`, `SettingsHistoryDataTab.qml`): resolve the fork-introduced accesses so these files stay at 0. Distinguish genuine undeclared identifiers from delegate-scope ones (`modelData`/`root`/`index`) — only the latter are the recorded-residue kind.
- [ ] 2.4 Clear the non-`unqualified` categories the gate named (`unresolved-type` x4, `missing-property` x6, `Quick.layout-positioning` x8, `required` x1). `unresolved-type` should largely fall out of tasks 1 and 2.2; whatever remains is either fixed or added to `CATEGORY_EXEMPTIONS` with its count (that block only ever loses entries).

## 3. Regenerate the fork baseline correctly

- [ ] 3.1 Do a clean desktop build so `Decenza.qmltypes` is fresh and types resolve; verify the run is NOT under-resolving before trusting any counts (the script's staleness guard). Never pass `--allow-stale` with `--update-baseline`.
- [ ] 3.2 Run `scripts/qmllint_report.py --update-baseline` on the fork tree so `qml-diagnostics-baseline.json` records the fork's `clean` list (incl. the now-clean barista files) and any legitimately-recorded delegate-scope ceilings — replacing the imported upstream baseline.
- [ ] 3.3 Confirm `scripts/qmllint_report.py --check` (and the default-build gate) passes on the fork.

## 4. Stop the merge from re-importing upstream's baseline

- [ ] 4.1 Add the ADDED `qml-diagnostics` requirement (baseline reflects the tree it gates; an upstream sync that replaces it is resolved by regenerating on the fork tree).
- [ ] 4.2 Record the merge-time procedure where fork sync guidance already lives (alongside the migration-renumber and registry-dedup notes): after a sync that touches `qml-diagnostics-baseline.json`, rebuild and re-run `--update-baseline` on the fork tree rather than accepting upstream's file.

## 5. Verify

- [ ] 5.1 `tst_qmlregistration` passes (both subtests) once the app QML build succeeds and regenerates `Decenza.qmltypes` — it fails today only because the gate blocks that build.
- [ ] 5.2 Full `ctest` shows no NEW reds vs. the pre-existing catalogue; the qmllint-cascade failures are gone.
- [ ] 5.3 A no-QML-change rebuild does not re-run the gate (the "costs nothing when idle" requirement still holds).
