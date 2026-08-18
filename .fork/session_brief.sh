#!/usr/bin/env bash
# Fork session brief. Plain stdout is injected as SessionStart context (incl. after /compact),
# which is where path-scoped rules do NOT survive — hence a hook, not a rule.
# Never blocks: every command is guarded; exit is always 0. Kept under ~2000 chars.
cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
{
  echo "=== Decenza fork (feat/barista) ==="
  echo "Fork of Kulitorum/Decenza. Fork-only surface: barista AI assistant, bean/recipe"
  echo "management, voice (STT/TTS), AI bean extraction. Upstream owns CLAUDE.md,"
  echo "docs/CLAUDE_MD/**, openspec/**. Re-sync is regular; class-2 ledger in .fork/MERGE.md."
  echo
  echo "Fork contract: .fork/FORK.md"
  if [ -f .fork/INDEX.tsv ]; then
    n=$(( $(wc -l < .fork/INDEX.tsv 2>/dev/null || echo 1) - 1 ))
    echo "Reuse index: .fork/INDEX.tsv (${n} capabilities) — grep it before building any"
    echo "  barista / bean / recipe / voice capability (add_bag, look_up_bean, bagOp, ... exist)."
  fi
  echo "Fork docs: docs/barista/"
  echo
  br=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
  echo "Branch: ${br}"
  [ -f .fork/NOW ] && echo "In flight: $(head -c 200 .fork/NOW 2>/dev/null)"
} 2>/dev/null | head -c 2000
exit 0
