# STT poison extras — never set RecognizerIntent silence/length windows

**Rule:** never set `EXTRA_SPEECH_INPUT_COMPLETE_SILENCE_LENGTH_MILLIS`,
`EXTRA_SPEECH_INPUT_POSSIBLY_COMPLETE_SILENCE_LENGTH_MILLIS`, or
`EXTRA_SPEECH_INPUT_MINIMUM_LENGTH_MILLIS` on the barista recogniser intent
(`DecenzaSpeech.java`). Enforced by `scripts/check_stt_intent_extras.py` in
`text-invariants.yml`.

**Why:** the Decent tablet (Galaxy Tab A8, One UI 6.1) has no Google Play Services, so
`ACTION_RECOGNIZE_SPEECH` is serviced by the Samsung/AOSP on-device recogniser, not
Google's. These extras are advisory by contract; this recogniser reacts to them by
returning **zero** final results — only `ERROR_CLIENT(5)` / `NO_MATCH(7)`. Setting them
(2026-09-16, an attempt to widen pause tolerance) made the barista completely deaf on
the tablet; reverting restored capture immediately.

**The right fix for pause tolerance is app-layer** — buffer/merge speech segments in
`VoiceInput`/`BaristaConversation`, never widen the platform endpointer. See
`docs/barista/VOICE_INTERACTION_AUDIT_2026-09-16.md`.
