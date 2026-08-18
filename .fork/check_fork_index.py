#!/usr/bin/env python3
"""check_fork_index.py — currency + accretion gate for the fork knowledge layer.

Three assertions, all about facts (never style):

  1. INDEX current   — committed .fork/INDEX.tsv == a fresh generation from the markers.
  2. Refs resolve    — every row's file exists; every `refs=`/`deps=` symbol naming an
                       UPSTREAM (or any) symbol is findable in the tree. A dangling ref means
                       upstream renamed/removed something the fork points at — the class-3
                       silent-rot case this gate converts into a loud, named failure.
  3. No root fork docs — no fork-authored *.md at the repo root (root holds only the known
                       upstream-authored set).

Wire this into the existing test suite (one test that shells out here); do NOT add a CI workflow
(that is upstream territory). It globs .fork/** and fork-owned source only — it must never fire on
an upstream file. Keep that property when editing.

Exit 0 = pass. Exit 1 = a named, one-line-fixable failure.
"""
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INDEX = REPO / ".fork" / "INDEX.tsv"
GEN = REPO / ".fork" / "gen_index.py"

# Root .md that are upstream-authored and legitimately live at the root.
KNOWN_ROOT_MD = {"AGENTS.md", "CLAUDE.md", "PRIVACY.md", "README.md"}


def fail(msg):
    sys.stderr.write("FAIL: " + msg + "\n")


def check_index_current():
    r = subprocess.run([sys.executable, str(GEN), "--check"], capture_output=True, text=True)
    if r.returncode != 0:
        fail(r.stdout.strip() + r.stderr.strip())
        fail("fix: python3 .fork/gen_index.py  (then commit .fork/INDEX.tsv)")
        return False
    return True


def check_refs_resolve():
    if not INDEX.exists():
        fail(".fork/INDEX.tsv missing — run python3 .fork/gen_index.py")
        return False
    lines = INDEX.read_text(encoding="utf-8").splitlines()
    ok = True
    for row in lines[1:]:
        cols = row.split("\t")
        if len(cols) < 7:
            continue
        name, fileline, extra = cols[1], cols[3], cols[6]
        # the marker's file must exist
        fpath = fileline.rsplit(":", 1)[0]
        if not (REPO / fpath).exists():
            fail(f"row '{name}': marker file missing: {fpath}")
            ok = False
        # each refs=/deps= symbol must be findable somewhere in the tree
        for part in extra.split(" ; "):
            part = part.strip()
            if not (part.startswith("refs=") or part.startswith("deps=")):
                continue
            sym = part.split("=", 1)[1].strip()
            # take the bare identifier after any '::' and before any '(' for the grep
            bare = sym.split("::")[-1].split("(")[0].strip()
            if not bare:
                continue
            found = subprocess.run(
                ["git", "grep", "-l", "-w", bare], cwd=REPO,
                capture_output=True, text=True,
            )
            if found.returncode != 0:  # git grep exits 1 on no match
                fail(f"row '{name}': referenced symbol not found in tree: {sym} "
                     f"(upstream may have renamed/removed it — update the marker)")
                ok = False
    return ok


def check_no_root_fork_docs():
    # Only git-TRACKED root .md count — a gitignored local note (e.g. RESUME.md) is the
    # operator's scratchpad, not repo accretion, and is invisible to the repo anyway.
    r = subprocess.run(
        ["git", "ls-files", "--", "*.md"], cwd=REPO, capture_output=True, text=True
    )
    roots = {line for line in r.stdout.splitlines() if "/" not in line}
    stray = sorted(roots - KNOWN_ROOT_MD)
    if stray:
        for s in stray:
            fail(f"fork-authored doc at repo root: {s}  -> move to docs/barista/")
        return False
    return True


def main():
    results = [
        ("index current", check_index_current()),
        ("refs resolve", check_refs_resolve()),
        ("no root fork docs", check_no_root_fork_docs()),
    ]
    if all(ok for _, ok in results):
        print(f"OK: fork index gate passed ({sum(1 for _ in results)} checks).")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
