#include <QtTest>

#include "barista/closeintent.h"

using barista::looksLikeClose;
using barista::looksLikeStall;

// [barista-fork] Locks the two voice-conversation intent matchers. Both have regressed on the owner more than
// once (a polite farewell not closing; a "let me check" stall dropping to Listening), and both are pure string
// functions with no runtime deps — exactly what a unit test should pin. Positives are the reported phrasings +
// variants; negatives are ordinary requests that must NOT trip the matcher (false close / false stall are the
// dangerous failures — closing mid-conversation or re-querying a real answer).
class tst_CloseIntent : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }

    void close_data();
    void close();
    void stall_data();
    void stall();

    void closeIsCaseAndPunctuationInsensitive();
    void stallNeverMatchesARealAnswer();
};

void tst_CloseIntent::close_data()
{
    QTest::addColumn<QString>("utterance");
    QTest::addColumn<bool>("expected");

    // --- Must close: the reported bug + prefixed/polite farewells ---
    QTest::newRow("reported: thanks, that'll be all") << "thanks, that'll be all" << true;
    QTest::newRow("thanks that'll be all (no comma)")  << "thanks that'll be all"  << true;
    QTest::newRow("ok thanks, that's all")             << "ok thanks, that's all"  << true;
    QTest::newRow("alright, we're done")               << "alright, we're done"    << true;
    QTest::newRow("that's it")                         << "that's it"              << true;
    QTest::newRow("that's all for now")                << "that's all for now"     << true;
    QTest::newRow("i'm all set")                       << "I'm all set"            << true;
    QTest::newRow("no more questions")                 << "no more questions"      << true;
    QTest::newRow("goodnight")                         << "goodnight"              << true;
    QTest::newRow("good night")                        << "good night"             << true;
    QTest::newRow("bye")                               << "bye"                    << true;
    QTest::newRow("thank you, that will be all")        << "thank you, that will be all" << true;
    QTest::newRow("nothing else")                      << "nothing else"           << true;

    // --- Must NOT close: bare politeness + real requests ---
    QTest::newRow("bare thanks does not close")        << "thanks"                 << false;
    QTest::newRow("thank you (bare)")                  << "thank you"              << false;
    QTest::newRow("real request: last shot")           << "can you show me my last shot" << false;
    QTest::newRow("real request: what next")           << "so what should I do next" << false;
    QTest::newRow("compliment is not a close")         << "that's a great espresso" << false;
    QTest::newRow("question about ratio")              << "what's a good ratio for this bean" << false;
    QTest::newRow("empty")                             << ""                       << false;
    QTest::newRow("long sentence")                     << "that's all well and good but I still want to talk about the grind setting please" << false;
}

void tst_CloseIntent::close()
{
    QFETCH(QString, utterance);
    QFETCH(bool, expected);
    QCOMPARE(looksLikeClose(utterance), expected);
}

void tst_CloseIntent::stall_data()
{
    QTest::addColumn<QString>("reply");
    QTest::addColumn<bool>("expected");

    // --- Must be treated as a stall (speak as lead-in, fetch the real answer) ---
    QTest::newRow("reported: let me check on that") << "Let me check on that." << true;
    QTest::newRow("let me check")                   << "let me check"          << true;
    QTest::newRow("let me look that up")            << "let me look that up"   << true;
    QTest::newRow("let me look into that")          << "let me look into that" << true;
    QTest::newRow("one moment")                     << "one moment"            << true;
    QTest::newRow("give me a second")               << "give me a second"      << true;
    QTest::newRow("hold on")                        << "hold on"               << true;
    QTest::newRow("checking")                       << "checking…"             << true;
    QTest::newRow("sure, let me check on that")      << "Sure, let me check on that" << true;
    QTest::newRow("i'll pull that up")               << "I'll pull that up"     << true;
    // [barista-fork] time-stall + trailing promise clause (on-device: "One sec, checking the weather." dropped to
    // Listening because the ^…$ anchor only caught a BARE stall). A leading time-stall with no figure IS a stall.
    QTest::newRow("one sec + trailing promise")      << "One sec, checking the weather." << true;
    QTest::newRow("give me a second + let me pull")   << "Give me a second, let me pull that up" << true;
    QTest::newRow("hold on + trailing promise")       << "Hold on, looking that up for you" << true;

    // --- Must NOT be a stall: real answers (a false stall re-queries a delivered answer) ---
    QTest::newRow("real answer: shot metrics")      << "Your last shot was 18 grams in, 36 grams out, in 28 seconds." << false;
    QTest::newRow("answer opening with let me tell") << "Let me tell you — that shot looked great." << false;
    QTest::newRow("i'll pull up your last shot + content") << "I'll pull up your last shot: 18 in, 36 out." << false;
    QTest::newRow("advice sentence")                << "Try grinding a touch finer next time." << false;
    QTest::newRow("bare sure is not a stall")        << "Sure!"                  << false;
    QTest::newRow("empty")                          << ""                       << false;
    QTest::newRow("a real question back")           << "What bean are you using?" << false;
}

void tst_CloseIntent::stall()
{
    QFETCH(QString, reply);
    QFETCH(bool, expected);
    QCOMPARE(looksLikeStall(reply), expected);
}

void tst_CloseIntent::closeIsCaseAndPunctuationInsensitive()
{
    QCOMPARE(looksLikeClose("THAT'S ALL!"), true);
    QCOMPARE(looksLikeClose("  Goodbye.  "), true);
    QCOMPARE(looksLikeClose("Thanks, That'll Be All"), true);
}

void tst_CloseIntent::stallNeverMatchesARealAnswer()
{
    // A stall is always SHORT; a genuine answer that merely opens with a stall-ish verb must not match,
    // because the ^…$ anchor requires the whole reply to be the stall.
    QCOMPARE(looksLikeStall("let me check on that for you — actually it was a great shot"), false);
    QCOMPARE(looksLikeStall("checking the numbers, your yield was 36 grams"), false);
}

QTEST_MAIN(tst_CloseIntent)
#include "tst_closeintent.moc"
