#!/usr/bin/env python3
"""Refuse the RecognizerIntent silence/length extras that make STT go totally deaf.

On the Decent tablet (Galaxy Tab A8, One UI 6.1, no Google Play Services) speech
recognition is serviced by the Samsung/AOSP on-device SpeechRecognizer, not Google's.
Setting the endpointing extras

    EXTRA_SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS
    EXTRA_SPEECH_INPUT_POSSIBLY_COMPLETE_SILENCE_LENGTH_MILLIS
    EXTRA_SPEECH_INPUT_MINIMUM_LENGTH_MILLIS

made the recogniser return ZERO final results — only ERROR_CLIENT(5)/NO_MATCH(7) —
i.e. the barista went completely deaf. Reverting removed the deafness. They are
advisory by contract and this recogniser reacts to them by breaking. See
docs/barista/STT_POISON_EXTRAS.md.

This gate is build-free (pure text over the source), mirrors the other
scripts/check_*.py checks, and runs in text-invariants.yml. Run --self-test to
exercise the negative fixture through this same checker.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Any RecognizerIntent extra whose name ends in a silence-length or minimum-length
# window. Matched by the stable suffix so a future OEM/name variant is caught too.
POISON = re.compile(r"EXTRA_SPEECH_INPUT_\w*(?:SILENCE_LENGTH|MINIMUM_LENGTH)_MILLIS")

COVERED_GLOBS = [
    "android/**/*.java", "android/**/*.kt",
    "src/**/*.cpp", "src/**/*.h", "src/**/*.mm", "src/**/*.m",
]


def covered_files():
    for glob in COVERED_GLOBS:
        for path in REPO.glob(glob):
            # Skip generated build copies (android-build mirrors android/src at build time).
            if "android-build" in path.parts or "/build/" in path.as_posix():
                continue
            yield path


def scan_text(text):
    """Return the list of (lineno, line) that name a poison extra, ignoring
    comment lines that only DOCUMENT the ban (so this file and the doc are clean)."""
    hits = []
    for i, line in enumerate(text.splitlines(), 1):
        if not POISON.search(line):
            continue
        stripped = line.lstrip()
        # A pure comment mentioning the extra by name is allowed (docs/warnings).
        if stripped.startswith(("//", "*", "#", "/*")):
            continue
        hits.append((i, line.strip()))
    return hits


def main():
    if "--self-test" in sys.argv:
        bad = "intent.putExtra(RecognizerIntent.EXTRA_SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS, 2800);"
        good = "intent.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);"
        assert scan_text(bad), "self-test: poison extra was not caught"
        assert not scan_text(good), "self-test: benign extra was flagged"
        assert not scan_text("// EXTRA_SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS is banned"), \
            "self-test: a documenting comment was flagged"
        print("check_stt_intent_extras: self-test OK")
        return 0

    failures = []
    for path in covered_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in scan_text(text):
            failures.append(f"{path.relative_to(REPO).as_posix()}:{lineno}: {line}")

    if failures:
        print("POISON RecognizerIntent silence/length extra(s) found — these cause total STT")
        print("deafness on the Samsung/AOSP recogniser. Remove them; pause-tolerance must be")
        print("app-layer, never the platform endpointer. See docs/barista/STT_POISON_EXTRAS.md.\n")
        for f in failures:
            print("  " + f)
        return 1

    print("check_stt_intent_extras: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
