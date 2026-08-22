#!/usr/bin/env python3
"""gen_index.py — generate .fork/INDEX.tsv from [fork-index] markers.

The reuse index answers "what fork capability already exists and what symbol do I call?"
It is GENERATED from one-line markers placed at each public fork capability's definition
site, so it can never silently disagree with the code (it IS the code's comments) and it
survives upstream refactors (markers ride in fork-authored lines).

Marker shape (in a fork-authored source file):

    // [fork-index] tool=add_bag | domain=barista | change=<openspec-change-id>
    //   what: registers a coffee bag from assistant conversation
    //   seam: bagOp -> CoffeeBagStorage::requestCreateBag
    //   refs: CoffeeBagStorage::requestCreateBag

Header tokens (pipe-separated): the first is `<kind>=<name>` (kind ∈ tool|api|pill|entry|seam);
then `domain=<barista|bean|recipe|voice|ai-extract>` and `change=<id or ->`.
Continuation lines `//   <key>: <value>` accept: what, seam, gate, deps, refs.
`refs:`/`deps:` naming an UPSTREAM symbol are verified on re-sync by check_fork_index.py.

Usage:
    python3 .fork/gen_index.py            # rewrite .fork/INDEX.tsv
    python3 .fork/gen_index.py --check    # exit 1 if committed != freshly generated

READ-ONLY over the source tree. Scans only fork-owned paths (FORK_GLOBS); never upstream files.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INDEX = REPO / ".fork" / "INDEX.tsv"

# Fork-owned paths only. Directories are scanned recursively for source files;
# explicit files are the fork-added items that live inside otherwise-shared dirs.
FORK_DIRS = ["src/barista", "qml/assistant"]
FORK_FILES = [
    "qml/components/layout/items/ProfileQuickSelectItem.qml",
    "qml/components/layout/items/BrewQuickSelectItem.qml",
    "qml/components/layout/items/TempQuickSelectItem.qml",
]
SRC_EXTS = {".cpp", ".h", ".hpp", ".qml", ".js", ".mm"}

HEADER_RE = re.compile(r"//\s*\[fork-index\]\s*(.+)")
CONT_RE = re.compile(r"//\s{2,}([a-z]+):\s*(.+)")
KINDS = {"tool", "api", "pill", "entry", "seam"}
DOMAINS = {"barista", "bean", "recipe", "voice", "ai-extract"}

COLUMNS = ["kind", "name", "domain", "file:line", "change", "what", "gate/deps/refs"]


def fork_source_files():
    files = []
    for d in FORK_DIRS:
        base = REPO / d
        if base.is_dir():
            files += [p for p in base.rglob("*") if p.suffix in SRC_EXTS and p.is_file()]
    for f in FORK_FILES:
        p = REPO / f
        if p.is_file():
            files.append(p)
    return sorted(set(files))


def parse_markers():
    rows, errors = [], []
    for path in fork_source_files():
        rel = path.relative_to(REPO).as_posix()
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        i = 0
        while i < len(lines):
            m = HEADER_RE.search(lines[i])
            if not m:
                i += 1
                continue
            header, lineno = m.group(1).strip(), i + 1
            toks = [t.strip() for t in header.split("|")]
            first = toks[0]
            if "=" not in first:
                errors.append(f"{rel}:{lineno}: marker header missing <kind>=<name>: {header!r}")
                i += 1
                continue
            kind, name = (s.strip() for s in first.split("=", 1))
            attrs = {}
            for t in toks[1:]:
                if "=" in t:
                    k, v = (s.strip() for s in t.split("=", 1))
                    attrs[k] = v
            domain = attrs.get("domain", "")
            change = attrs.get("change", "-") or "-"
            if kind not in KINDS:
                errors.append(f"{rel}:{lineno}: unknown kind {kind!r} (allowed: {sorted(KINDS)})")
            if domain not in DOMAINS:
                errors.append(f"{rel}:{lineno}: unknown/missing domain {domain!r} (allowed: {sorted(DOMAINS)})")
            # continuation lines
            fields, j = {}, i + 1
            while j < len(lines):
                c = CONT_RE.search(lines[j])
                if not c:
                    break
                fields[c.group(1)] = c.group(2).strip()
                j += 1
            what = fields.get("what", "")
            extra = " ; ".join(
                f"{k}={fields[k]}" for k in ("seam", "gate", "deps", "refs") if k in fields
            )
            rows.append([kind, name, domain, f"{rel}:{lineno}", change, what, extra])
            i = j
    rows.sort(key=lambda r: (r[2], r[1], r[3]))
    return rows, errors


def render(rows):
    out = ["\t".join(COLUMNS)]
    out += ["\t".join(r) for r in rows]
    return "\n".join(out) + "\n"


def main():
    check = "--check" in sys.argv[1:]
    rows, errors = parse_markers()
    if errors:
        sys.stderr.write("fork-index marker errors:\n  " + "\n  ".join(errors) + "\n")
        return 2
    generated = render(rows)
    if check:
        current = INDEX.read_text(encoding="utf-8") if INDEX.exists() else ""
        if current != generated:
            sys.stderr.write(
                "FAIL: .fork/INDEX.tsv is stale. Regenerate and commit:\n"
                "  python3 .fork/gen_index.py\n"
            )
            return 1
        print(f"OK: .fork/INDEX.tsv current ({len(rows)} rows).")
        return 0
    INDEX.write_text(generated, encoding="utf-8")
    print(f"wrote {INDEX.relative_to(REPO)} ({len(rows)} rows).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
