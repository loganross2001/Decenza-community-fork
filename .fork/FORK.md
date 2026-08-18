# FORK.md — the Decenza feat/barista fork contract

This repository is a **fork** of [`Kulitorum/Decenza`](https://github.com/Kulitorum/Decenza)
(the upstream espresso-machine app). The fork is re-synced with upstream regularly; the
recurring cost of that sync is documented in [`.fork/MERGE.md`](MERGE.md).

**Upstream owns** `CLAUDE.md`, `docs/CLAUDE_MD/**`, `openspec/**`, `AGENTS.md`, `README.md`,
and almost all of `src/**` and `qml/**`. Do not re-architect any of them; that fights the sync.

**The fork adds one thing:** a private **"barista" AI voice assistant** and the features it needs —
conversational bean/bag management, recipe management, a voice (STT/TTS) pipeline, and AI
bean-detail extraction. None of that exists upstream.

---

## Where fork things live

| Kind | Path | Merge class |
|---|---|---|
| Fork-exclusive source | `src/barista/**`, `qml/assistant/**` | 1 (upstream has no file there) |
| Fork machinery + knowledge | `.fork/**` | 1 (dot-dir upstream never authors) |
| Fork design/request/resume docs | `docs/barista/**` | 1 (barista does not exist upstream) |
| Fork read-time conventions | `.claude/rules/fork-*.md` | 1 (upstream ships no `.claude/rules/`; force-added — `.claude/*` is gitignored) |
| Hook registration | `.claude/settings.json` (local) + `.fork/hooks.settings.json` (committed) | 1 (`.claude/settings.json` is gitignored per upstream's `.claude/*`; the canonical copy is committed under `.fork/`) |
| Fork edits inside **shared** files | e.g. `main.cpp`, `settings_network.cpp`, `IdlePage.qml`, `CMakeLists.txt` | **2 — merge-tax; see MERGE.md** |

The fork-exclusive surface is ~66 files / ~20k LOC. The class-2 (shared-edited) ledger is ~89
files — that is the real re-sync cost, enumerated in `MERGE.md`.

---

## The three conventions

**1. Reuse before building.** Before adding any barista / bean / recipe / voice / AI-extraction
capability, grep the reuse index:

```
rg '<capability>' .fork/INDEX.tsv
```

It answers *"what already exists and what symbol do I call?"* — the question OpenSpec structurally
cannot (OpenSpec records *decisions*, indexed by change, not *capabilities*, indexed by name).
The index is why a fresh session should not re-derive that `add_bag`, `look_up_bean`, and the
`AIManager::extractCoffeeBagDetails` pipeline already exist — they are one grep away.

**2. Fork docs live in `docs/barista/`.** New fork design / request / resume documents are created
there, never at the repository root. The root holds only upstream-authored `.md`.

**3. Public fork capabilities carry a `[fork-index]` marker.** Adding a public barista tool,
storage/seam entry point, or extraction entry point includes adding its marker at the definition
site. `.fork/INDEX.tsv` is *generated* from those markers (`.fork/gen_index.py`); the decision
rationale lives in **OpenSpec** under the change id named in the marker — the index never restates it.

Marker shape (in fork-authored lines only):

```cpp
// [fork-index] tool=add_bag | domain=barista | change=<openspec-change-id>
//   what: registers a coffee bag from assistant conversation
//   seam: bagOp -> CoffeeBagStorage::requestCreateBag   (upstream seam — refs verified on re-sync)
```

Fields: `tool=`/`api=`/`pill=`/`entry=` (the name), `domain=` (barista|bean|recipe|voice|ai-extract),
`change=` (OpenSpec change id or `-`), and free-text `what:` / `seam:` / `gate:` / `deps:` / `refs:`
lines. `refs:`/`deps:` naming an **upstream** symbol are checked for resolvability on every re-sync.

---

## Session continuity

`.claude/settings.json` (installed from the committed `.fork/hooks.settings.json`) registers a
`SessionStart` brief (`.fork/session_brief.sh`) that re-anchors any session — including after a
`/compact` — to this contract, the reuse index, and the in-flight task line. Path-scoped rules do
**not** survive compaction, which is why the brief is a hook, not a rule.

The in-flight task line is `.fork/NOW` (gitignored): a session records what it is mid-way through
(`echo "<task>" > .fork/NOW`), and the brief surfaces it on the next start/compaction. It is on
disk, so it survives compaction without a `PreCompact` hook — the honest mechanism, since a hook
cannot know the task the session is holding.

The assistant's out-of-repo working memory is a separate thing and stays out of the repo: `.fork/`
holds facts **about the repository** (committed, diffable, identical on any clone); session memory
holds facts about the operator's working style. If a fork fact a fresh clone would need lives only in
session memory, that is a gap in `.fork/`, and the fix is to record it here.

---

## Remote safety

This fork's private `feat/barista` branch pushes to **backup only** (`Decenza-private`), never to
`origin` (the public community fork). See the project's remote-safety notes before any push.

---

## Currency & enforcement

- `.fork/gen_index.py --check` — the committed index equals a fresh generation from the markers.
- `.fork/check_fork_index.py` — every index row resolves to a real file/symbol; every `refs:` to an
  upstream symbol still exists; no new fork-authored `.md` at the repo root.
- Both run inside the existing test gate (no new CI workflow — that would be upstream territory).
- After every re-sync, run the three commands in `.fork/MERGE.md`.
