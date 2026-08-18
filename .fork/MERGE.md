# MERGE.md — upstream re-sync protocol & the class-2 ledger

The fork re-syncs with `upstream/main` regularly. Everything under `.fork/`, `docs/barista/`,
`.claude/rules/fork-*.md`, `src/barista/**`, and `qml/assistant/**` is **class 1** (fork-exclusive)
and cannot conflict on merge. The real recurring cost is the **class-2** files: shared files that
exist upstream and that the fork also edits. This document is the standing record of the fork's
intent in the files that conflict predictably, so conflict resolution is not re-derived each sync.

> **The fork-knowledge layer (`.fork/**`, `.claude/rules/fork-*.md`, `.claude/settings.json`,
> `docs/barista/`) adds ZERO new class-2 entries.** Every artifact it introduces is class 1. That
> is the claim a future session most needs to be able to check, so it is stated here explicitly.

---

## Fresh-clone setup (once)

Install the fork's git currency gate (zero merge surface — `.git/hooks/` is untracked):

```bash
ln -sf ../../.fork/hooks/pre-commit .git/hooks/pre-commit
```

It refuses a commit whose fork index is stale or whose refs dangle, and blocks a new fork-authored
`.md` at the repo root. Bypass a false positive with `git commit --no-verify`. The session brief and
compaction checkpoint are registered in `.claude/settings.json` (committed, class 1) — no setup step.

---

## After every re-sync — run these three

```bash
python3 .fork/gen_index.py            # recompute file:line after the merge shifted lines
python3 .fork/check_fork_index.py     # assert every marker + upstream ref still resolves
git diff --stat .fork/INDEX.tsv       # review what moved
```

If `check_fork_index.py` fails, upstream renamed or removed something the fork references (a
`refs:`/`deps:` symbol, or a file a marker lives in). The failure names it; the fix is a one-line
marker edit or a real code change. This is the graceful-degradation guarantee: **the index cannot
silently lie, because a merge is always followed by an assertion that it does not.**

---

## The registration hotspots — these conflict almost every sync

The barista fork wires itself into shared files at a handful of registration seams. Upstream edits
the same regions often, so these are the predictable conflicts. Resolution intent for each:

| File | Fork's edit | Resolution intent |
|---|---|---|
| `CMakeLists.txt` | Adds barista QML/sources to `qt_add_qml_module` + source lists (BaristaChipRow, the quick-select pills, TempPickerDialog, `src/barista/*`) | **Union.** Keep upstream's new entries AND the fork's. Watch for a **doubled** entry when both sides add the same file (e.g. `TempPickerDialog.qml`) — dedupe to one. |
| `src/main.cpp` | Barista module/singleton registration | Union; keep both registrations. |
| `src/core/settings_network.cpp` | Adds barista widget-catalog rows (barista, baristaSwitcher, profileQuickSelect, brewQuickSelect) to `widgetCatalogTable()` | Union; keep the fork rows alongside upstream's. The barista rows are the twice-bitten blind spot — confirm they survived. |
| `qml/components/layout/LayoutItemDelegate.qml` | Adds `case "profileQuickSelect"/"brewQuickSelect"` switch arms | Union; keep the fork cases. |
| `qml/pages/IdlePage.qml` | Brew-bar fit-picker fix + barista hooks | Fork side generally wins on the fit-picker region; re-apply if upstream refactors the brew bar. |
| `resources/resources.qrc` | Barista sounds/assets | Union. |
| `src/controllers/maincontroller.{cpp,h}` | `[prime-first-frame]` priming + `bagStorage()` accessor | Keep the fork's `primed`/`origPrevFrameIndex` logic; reconcile with any upstream frame-exit refactor (see the 2026-08-17 sync). |
| `src/history/shothistorystorage*.{cpp,h}` | `preFillInjected` in `AnalysisInputs` + prime-first-frame plumbing | **Union**, not either-side: keep the fork's `preFillInjected` arg AND upstream's changes (e.g. shape-resolution `profileKbResolved`). |
| `src/ai/aiprovider.{h,cpp}` | Barista conversation: options-aware `analyzeConversation` overloads (Anthropic/Gemini), fast/client tool wiring, `forceRespond`, and **vision plumbing** — `RequestOptions.imageData/imageMediaType`, `supportsVision()`, and the `messagesWithImageOnLastUser`/`contentsWithImageOnLastUser` image-attach helpers | **Union.** Keep the fork's overloads and the image-attach in each provider's message-build; if upstream reworks message construction, re-apply the image block (Anthropic) / inlineData part (Gemini) on the last user message. Image rides options, never persisted messages. |
| `src/ai/aimanager.cpp` | Barista conversation entry + bag extraction + the positional `RequestOptions{...}` init + `stagePendingImage`/`currentProviderSupportsVision` + per-turn image consume-and-clear | **Union.** When upstream touches AIManager, keep the barista turn wiring; the positional `RequestOptions{webSearch, clientTools, timeoutMs, forceRespond, imageData, imageMediaType}` init must list every field (C++17, no designated initializers) — add new fields there when the struct grows. The staged image is consumed+cleared each turn (never persisted). |
| `src/ai/aiconversation.{h,cpp}` | `followUpWithImage()` (add-a-bean-from-a-photo) + `readAndDownscaleImage()` worker-thread helper | **Union.** Fork-only methods; keep them. Vision-gated (currentProviderSupportsVision) and image decode is off-main-thread — preserve both if upstream reworks the send path. |

**Migration divergence** is the sharpest recurring landmine: when both sides author DB migrations,
compare by **column/function existence, not migration number** — the fork's schema is generally a
superset; only genuinely-new upstream columns get a fresh fork migration number. (The 2026-08-17
sync had no migration divergence; earlier ones did.)

---

## The full class-2 ledger

89 shared files carry fork edits (as of the 2026-08-17 sync). Regenerate the current list anytime:

```bash
git diff --name-only upstream/main...HEAD | while read f; do \
  git cat-file -e "upstream/main:$f" 2>/dev/null && echo "$f"; done
```

Distribution: `src/` 50, `qml/` 24, `tests/` 7, plus `CMakeLists.txt`, `resources.qrc`,
`build.sh`, `versioncode.txt`, `android/`, `cmake/`, `CLAUDE.md`, `PRIVACY.md`. The hotspots table
above covers the ones that actually conflict; the rest are incidental edits that merge cleanly most
syncs. This distribution is the fork's true merge surface and it exists independently of the
`.fork/` knowledge layer.

---

## Remote safety

`feat/barista` pushes to **backup only** (`Decenza-private`), never to `origin` (the public
community fork). Confirm the remote before any push.
