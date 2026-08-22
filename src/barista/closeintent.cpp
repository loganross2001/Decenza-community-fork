#include "closeintent.h"

#include <QRegularExpression>

namespace barista {

// Generous local close-intent (a latency accelerator + fallback — the forced per-turn respond(text,
// end_conversation) bit is the structural mechanism). Short utterance + a normalised farewell phrase.
//
// The old version anchored the farewell at ^…$ with NO prefix handling, so every polite goodbye missed:
// "thanks, that'll be all" didn't start with a listed farewell (the "thanks" prefix broke the anchor) and
// wasn't in the narrow `thanks,? (that's (it|all)|bye)` branch either — the reported can't-close bug. Fix:
// strip leading politeness/filler prefixes FIRST, then match the (broadened) farewell set on what remains.
bool looksLikeClose(const QString& raw)
{
    QString t = raw.trimmed().toLower();
    if (t.isEmpty() || t.length() > 60)
        return false;
    // Commas → spaces so "no, that's all" reads like "no that's all" for the core matcher. (Doing this also
    // lets a comma-free "no more questions" keep its "no" — see why "no" is NOT a stripped prefix below.)
    t = t.replace(QLatin1Char(','), QLatin1Char(' ')).simplified();
    // Peel up to two stacked leading prefixes ("ok thanks …", "alright cool …") so the farewell that follows
    // anchors cleanly. Bare politeness ("thanks") strips to empty and does NOT close (too aggressive). "no" is
    // deliberately NOT a prefix: it is meaningful in "no more (questions)" / "no thanks" / "no that's all",
    // which the core matcher handles directly — stripping it there broke "no more questions".
    static const QRegularExpression prefix(
        QStringLiteral("^(ok(ay)?|alright|all right|right|so|well|now|um+|uh+|yeah|yep|yes|cool|nice|"
                       "great|perfect|awesome|lovely|thank you so much|thank you|thanks|cheers|"
                       "appreciate it|i appreciate it)[.!\\s]+"));
    for (int i = 0; i < 2; ++i) {
        const qsizetype before = t.size();
        t.remove(prefix);
        t = t.trimmed();
        if (t.size() == before)
            break;   // nothing stripped this pass
    }
    if (t.isEmpty())
        return false;
    static const QRegularExpression re(
        QStringLiteral("^("
                       "that'?s (it|all|everything|me|us|enough)( for now| then| done)?|"
                       "that'?ll (be all|do( it)?)( for now| then)?|"
                       "that will (be all|do)|"
                       "we'?re (done|good|all set|all done|finished|set)|"
                       "i'?m (done|good|all set|all done|finished|fine|set)|"
                       "(that'?s all|no more|nothing else|nothing more|no more questions)|"
                       "good ?night|good ?bye|bye( now| bye)?|see ya|see you( later)?|catch you later|"
                       "all done|all set|we can stop|let'?s stop|stop( there)?|that'?s enough|"
                       "no (that'?s (it|all)|i'?m (good|done)|thank you|thanks?)"
                       ")[.!]?$"));
    return re.match(t).hasMatch();
}

// Stall detector: is the model's WHOLE final reply just a promise to continue ("let me check on that", "one
// moment", "checking…") with no actual answer? Tight on purpose — anchored ^…$ so it matches only a bare
// stall, never a real answer that happens to open with "let me…". A match means: speak it as a lead-in, keep
// the turn alive, and fetch the real answer via one continuation (see BaristaConversation::onModelFinal).
bool looksLikeStall(const QString& raw)
{
    QString t = raw.trimmed().toLower();
    if (t.isEmpty() || t.length() > 60)
        return false;
    t.remove(QRegularExpression(QStringLiteral("[.!,?…]+$")));   // drop trailing punctuation
    // Peel one leading filler ("ok, …", "sure — …") so the stall stem anchors.
    t.remove(QRegularExpression(QStringLiteral("^(ok(ay)?|sure|alright|right|well|hmm+|so|yeah|yep)[,.!\\s]+")));
    t = t.trimmed();
    // [barista-fork] Broadened 2026-07-31 (on-device evidence, two rounds): the ^…$ matcher below only catches a
    // BARE stall, so a promise with a trailing object shipped straight to Listening and the fetch never ran —
    // first "One sec, checking the weather.", then "Let me check the rain chance for tomorrow." A reply is a stall
    // when it OPENS with a time-stall ("one sec", "hold on", …) OR a first-person promise-to-retrieve ("let me
    // check …", "I'll pull up …") OR a "-ing" retrieval ("checking the weather"), AND carries no ANSWER SIGNAL.
    // A real answer states a figure or pivots to a delivered result, which we proxy by a digit, an em-dash, or a
    // colon/semicolon — so "checking the numbers, your yield was 36 grams" (digit) and "let me check on that …
    // — actually it was a great shot" (em-dash) keep their answer signal and stay NOT-stalls. Bare imperatives
    // ("Try grinding …", "Grab the portafilter") don't open with these forms, so they're untouched.
    static const QRegularExpression answerSignal(QStringLiteral("[0-9]|\\x{2014}|:|;"));
    if (!t.contains(answerSignal)) {
        static const QRegularExpression stallOpen(QStringLiteral(
            "^((just )?(a|one) (sec|second|moment|minute)|hold on|hang on|one moment"
            "|give me (a )?(sec|second|moment|minute))\\b"
            "|^(let me|let me just|i'?ll|i will|gonna|going to)\\s+"
            "(check|look|pull|find|fetch|verify|confirm|dig|search|see)\\b"
            "|^(check|look|pull|search|fetch)ing\\b"));
        if (stallOpen.match(t).hasMatch())
            return true;
    }
    static const QRegularExpression re(QStringLiteral(
        "^(let me |i'?ll |i will |let me just |give me |just |gonna |going to )?"
        "(check|look|see|find|pull|dig|verify|confirm|find out|look into|check on|look that up|"
        "hold on|hang on|checking|looking|searching|"
        "(a|one) (sec|second|moment|minute)|just (a|one) (sec|second|moment|minute)|one moment)"
        "( that| it| on that| on it| into that| into it| up| that up| it up| for you| for a moment)*$"));
    return re.match(t).hasMatch();
}

// [barista-fork] "Go / take it now" matcher for voice-driven bag-photo capture. Fires the shutter only while the
// camera is open awaiting a shot (BaristaConversation::m_awaitingBagCapture), where the user was just told "say
// ready when it's framed" — so within that narrow window it can be generous with go-words. But a LEADING
// negation or hold must win outright ("no wait", "not yet", "hold on", "don't"), leaving the shutter untouched
// rather than snapping a photo the user was trying to stop. Mirrors looksLikeClose's prefix-peel shape.
bool looksLikeAffirmative(const QString& raw)
{
    QString t = raw.trimmed().toLower();
    if (t.isEmpty() || t.length() > 40)
        return false;
    t = t.replace(QLatin1Char(','), QLatin1Char(' ')).simplified();
    t.remove(QRegularExpression(QStringLiteral("[.!?]+$")));
    t = t.trimmed();
    // A leading negation / hold cancels the go-reading before any prefix peel, so "no go", "not yet", "hold on",
    // "don't" never reach the affirmative set.
    static const QRegularExpression negate(
        QStringLiteral("^(no|not|nope|nah|never ?mind|wait|hold|hang|don'?t|cancel|stop)\\b"));
    if (negate.match(t).hasMatch())
        return false;
    // Peel one leading filler so the go-word anchors ("ok ready", "alright go", "um yeah").
    t.remove(QRegularExpression(QStringLiteral("^(ok(ay)?|alright|all right|right|so|well|um+|uh+|and|now)[.!\\s]+")));
    t = t.trimmed();
    static const QRegularExpression re(QStringLiteral(
        "^("
        "ready( to go| now| when you are)?|i'?m ready|all set|set|"
        "go( ahead| for it)?|"
        "yes|yeah|yep|yup|sure|okay|ok|"
        "take (it|the (photo|picture|shot|pic))|"
        "snap( it)?|capture( it)?|shoot( it)?|do it|now"
        ")$"));
    return re.match(t).hasMatch();
}

}  // namespace barista
