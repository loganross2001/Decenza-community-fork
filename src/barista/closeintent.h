#pragma once

#include <QString>

// [barista-fork] Pure close/stall intent matchers for the voice conversation, factored out of
// BaristaConversation so they can be unit-tested in isolation (they need only QtCore — no voice/mic/AI
// machinery). See tests/tst_closeintent.cpp. Behaviour is documented at each definition in closeintent.cpp.
namespace barista {

// True when a user utterance is a farewell / "we're done" close signal (a latency accelerator + fallback on
// top of the model's end_conversation tool). Prefix-aware: "thanks, that'll be all" matches.
bool looksLikeClose(const QString& raw);

// True when the model's WHOLE final reply is only a promise-to-continue ("let me check on that", "one moment")
// with no actual answer — the cue to speak it as a lead-in and auto-fetch the real answer.
bool looksLikeStall(const QString& raw);

// True when a short utterance is a "go / take it now" signal ("ready", "yes", "go", "take it", "snap"). Used
// ONLY while the bag-photo camera is open awaiting a shot, to trigger the shutter by voice. Deliberately
// rejects a hesitation or cancel ("no", "not yet", "wait", "hold on") so those never fire the shutter.
bool looksLikeAffirmative(const QString& raw);

// Answer a PURE espresso ratio/dose/yield math question locally, returning the spoken answer — or "" when the
// utterance isn't a clean, whole-utterance arithmetic query (so anything conversational falls through to the
// model). Lets the barista answer "1:2.5 off 19 grams" or "ratio of 18 in 40 out" instantly with no LLM
// round-trip, and never gets the arithmetic wrong. Deliberately narrow to avoid firing on a real discussion.
QString tryQuickMath(const QString& raw);

}  // namespace barista
