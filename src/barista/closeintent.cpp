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
    // [barista-fork] Broadened 2026-07-31 (on-device evidence: "One sec, checking the weather." shipped straight
    // to Listening and the weather was never fetched — the ^…$ matcher below only catches a BARE stall, so the
    // trailing "checking …" promise broke the anchor). A reply that OPENS with a TIME-STALL ("one sec", "give me
    // a second", "hold on", "one moment") is a stall even when a promise clause follows — UNLESS it carries an
    // actual answer, which we proxy by any DIGIT (a real answer states a figure: "…your yield was 36 grams").
    // This preserves the test contract: "checking the numbers, your yield was 36 grams" has no leading time-stall
    // AND a digit → not a stall; the em-dash-answer case has no leading time-stall → not a stall.
    static const QRegularExpression digitRe(QStringLiteral("[0-9]"));
    static const QRegularExpression timeStall(QStringLiteral(
        "^((just )?(a|one) (sec|second|moment|minute)|hold on|hang on|one moment"
        "|give me (a )?(sec|second|moment|minute))\\b"));
    if (!t.contains(digitRe) && timeStall.match(t).hasMatch())
        return true;
    static const QRegularExpression re(QStringLiteral(
        "^(let me |i'?ll |i will |let me just |give me |just |gonna |going to )?"
        "(check|look|see|find|pull|dig|verify|confirm|find out|look into|check on|look that up|"
        "hold on|hang on|checking|looking|searching|"
        "(a|one) (sec|second|moment|minute)|just (a|one) (sec|second|moment|minute)|one moment)"
        "( that| it| on that| on it| into that| into it| up| that up| it up| for you| for a moment)*$"));
    return re.match(t).hasMatch();
}

}  // namespace barista
