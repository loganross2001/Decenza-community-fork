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

}  // namespace barista
