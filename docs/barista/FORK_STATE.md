# Decenza fork — canonical state & how to resume

**This is the authoritative "where things are" note. It is tracked, and it lives here (not at
the repo root) per `.claude/rules/fork-docs.md`.** If anything below drifts from reality, fix it
here in the same change.

## ⭐ The one thing that keeps getting confused: WHICH FOLDER

**The canonical working copy is `/Users/christopherpalestro/Decenza-fork`, on branch `feat/barista`.**
It is the ONLY git checkout of Decenza on this Mac. Work here and nowhere else.

These sibling folders are **STALE June 2026 artifacts — do NOT use them, do NOT build from them,
do NOT read them for current state:**

| Folder | What it is | Status |
|---|---|---|
| `~/Decenza/Decenza-1.8.0` | Old Qt Creator scratch copy (no git) | stale, ignore |
| `~/Decenza-contribution/` | June contribution package (patch + write-ups, no git) | stale, ignore |
| `~/decenza-hardening-backup-20260620/` | June backup (no git) | stale, ignore |
| `~/Documents/Decenza Backups/` | Backups | stale, ignore |

If you find yourself reading any resume/state note that mentions branches `community-fork` or
`weight-dialing`, or "PR #1348 to Kulitorum" — that is the **obsolete** June workflow. Ignore it.
The current workflow is `feat/barista` + squash-merge PRs (see `CLAUDE.md` → Git Workflow).

## Remotes

| Remote | URL | Role |
|---|---|---|
| `origin` | `loganross2001/Decenza-community-fork.git` | `feat/barista` tracks `origin/feat/barista` |
| `backup` | `loganross2001/Decenza-private.git` | private mirror |
| `upstream` | `Kulitorum/Decenza.git` | the real project; re-synced regularly (see `.fork/MERGE.md`) |

## App data (the real databases)

The running desktop app keeps its data at
`~/Library/Application Support/DecentEspresso/Decenza/` — `shots.db` (shot history + enjoyment
ratings) and `assistant.db` (barista feedback KB, reminders, personal dates). **The copy on this
Mac is a tiny dev sandbox (a handful of test shots, mostly blank/"Live DYE" beans).** The owner's
real coffee history lives on the DE1's Samsung tablet, not here — do not reason about real
coverage from the local DB.

## Building & testing

`CLAUDE.md` (upstream) says builds/tests go through the **Qt Creator MCP** only. **On THIS Mac in a
headless Claude CLI session that rule does not apply — the MCP bridge is not exposed, and you build +
test directly with `cmake`/`ctest` against the existing build dir.** (Do not re-abort a task waiting
for the MCP; a prior session did exactly that. The MCP-only rule is for the IDE-integrated flow.)

- Build dir: `build/Qt_6_11_2_for_macOS_Debug` (Qt 6.11.2; the older `Qt_6_10_1_..` dir is corrupted).
- `ninja` is not on PATH — cmake drives it via `CMAKE_MAKE_PROGRAM=~/Qt/Tools/Ninja/ninja` from the
  cache, so build with `cmake --build .`, never bare `ninja`.
- Incremental build: `cd build/Qt_6_11_2_for_macOS_Debug && cmake --build . -j`. After a CMakeLists or
  wide-header change, reconfigure first (`cmake .`). Run tests: `ctest --output-on-failure` (add
  `-R '^tst_name$'` for one). zsh: use `$pipestatus[1]`, not `$PIPESTATUS`.
- Details in memory `decenza-fork-run-tests-cli.md`.

## Orientation pointers

- Fork contract: `.fork/FORK.md` · re-sync ledger: `.fork/MERGE.md` · reuse index: `.fork/INDEX.tsv`
- Barista design: `docs/barista/DESIGN.md` (+ the other `docs/barista/*.md` design/request docs)
- Session brief (auto-injected at every SessionStart): `.fork/session_brief.sh`
