#include <QtTest>

#include "barista/speechchunker.h"

using barista::SpeechChunker;

// [barista-fork] Unit tests for the streaming speech chunker (BARISTA_VOICE_STREAMING_DESIGN.md §1.2).
// The chunker is the one part of the streaming voice pipeline that is pure and clock-free, so it is the one
// part that can be pinned in software — every boundary edge case (decimals, abbreviations, min/max sizing, the
// first-chunk fast rule, barge-in reset) that would otherwise only surface as choppy or mis-split TTS on the
// tablet is caught here instead. The SSE parser and TTS queue that consume it are on-device-verified.
class TestSpeechChunker : public QObject {
    Q_OBJECT

private slots:
    // A complete short reply below minChunkChars buffers until flush — it is not emitted mid-stream.
    void shortReplyWaitsForFlush() {
        SpeechChunker c;
        QVERIFY(c.feed(QStringLiteral("Nice and even.")).isEmpty());  // < 25 chars, no more coming yet
        QCOMPARE(c.flush(), QStringLiteral("Nice and even."));
    }

    // The first chunk is released at the first real boundary once it clears the (relaxed) first-chunk floor.
    void firstChunkEmitsAtFirstBoundary() {
        SpeechChunker c;
        const QStringList out = c.feed(QStringLiteral("Let me pull that up for you. Give me one moment here."));
        QVERIFY(!out.isEmpty());
        QCOMPARE(out.first(), QStringLiteral("Let me pull that up for you."));
        QVERIFY(c.hasEmitted());
    }

    // A later chunk holds to minChunkChars: two short sentences merge rather than emitting a 3-char fragment.
    void laterChunkMergesShortSentences() {
        SpeechChunker c;
        // Land the first chunk (the capital 'O' after "shot." makes it a real boundary and flips past the
        // first-chunk relaxation); "Onward now." then buffers because it is under minChunkChars.
        c.feed(QStringLiteral("This is a clear opening line for the shot. "));
        c.feed(QStringLiteral("Onward now. "));
        QVERIFY(c.hasEmitted());
        const QStringList out = c.feed(QStringLiteral("OK. Go. "));  // each < 25 chars → must merge, not emit
        QVERIFY2(out.isEmpty(), "short trailing sentences should merge, not emit tiny chunks");
        QCOMPARE(c.flush(), QStringLiteral("Onward now. OK. Go."));
    }

    // A decimal point is not a sentence boundary.
    void decimalDoesNotSplit() {
        SpeechChunker c;
        const QStringList out =
            c.feed(QStringLiteral("Set the pressure to 9.5 bar and keep the yield near 36 grams. Then taste."));
        QVERIFY(!out.isEmpty());
        QCOMPARE(out.first(), QStringLiteral("Set the pressure to 9.5 bar and keep the yield near 36 grams."));
    }

    // A ratio/clock colon (digit:digit) is not a clause boundary; a normal colon is. Kept under the 60-char
    // first-chunk force-split so the only thing that could break the buffer is the colon or the decimal.
    void ratioColonDoesNotSplit() {
        SpeechChunker c;
        const QStringList out = c.feed(QStringLiteral("The ratio 1:2.5 runs a bit long."));
        QVERIFY2(out.isEmpty(), "1:2.5 must not split on the colon or the decimal");
        QCOMPARE(c.flush(), QStringLiteral("The ratio 1:2.5 runs a bit long."));
    }

    void plainColonSplits() {
        SpeechChunker c;
        const QStringList out = c.feed(QStringLiteral("Here is what I would try next time you dial in: grind finer."));
        QVERIFY(!out.isEmpty());
        QCOMPARE(out.first(), QStringLiteral("Here is what I would try next time you dial in:"));
    }

    // A known abbreviation does not end a sentence even when a capital follows (the hard case the lowercase
    // heuristic can't catch). Short enough that only the abbreviation logic could break the buffer.
    void abbreviationDoesNotSplit() {
        SpeechChunker c;
        const QStringList out = c.feed(QStringLiteral("Roast, e.g. Ethiopian, is nice."));
        QVERIFY2(out.isEmpty(), "e.g. must not split even though 'Ethiopian' is capitalised");
        QCOMPARE(c.flush(), QStringLiteral("Roast, e.g. Ethiopian, is nice."));
    }

    // A lowercase word after a period means the sentence is continuing (extra abbreviation safety net).
    void lowercaseAfterPeriodDoesNotSplit() {
        SpeechChunker c;
        const QStringList out = c.feed(QStringLiteral("It ran 28 sec. that reads well."));
        QVERIFY(out.isEmpty());
    }

    // Streaming token-by-token yields the same first chunk as one big delta — and never mid-sentence.
    void tokenByTokenStreams() {
        SpeechChunker c;
        QStringList emitted;
        for (const QString& tok : {QStringLiteral("Give "), QStringLiteral("me "), QStringLiteral("just "),
                                   QStringLiteral("a "), QStringLiteral("second "), QStringLiteral("while "),
                                   QStringLiteral("I "), QStringLiteral("check "), QStringLiteral("that. "),
                                   QStringLiteral("The "), QStringLiteral("answer ")}) {
            emitted += c.feed(tok);
        }
        QCOMPARE(emitted.size(), 1);
        QCOMPARE(emitted.first(), QStringLiteral("Give me just a second while I check that."));
    }

    // The first-chunk deadline releases buffered text with no boundary yet, so the lead-in is never late.
    void firstChunkDeadlineReleasesEarly() {
        SpeechChunker c;
        // No terminator, under 60 chars → normally buffers; deadline forces it out.
        QVERIFY(c.feed(QStringLiteral("Looking at your last three shots")).isEmpty());
        const QStringList out = c.feed(QStringLiteral(" now"), /*firstChunkDeadlinePassed=*/true);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.first(), QStringLiteral("Looking at your last three shots now"));
    }

    // The deadline only relaxes the FIRST chunk; once emitted it has no effect on the tail.
    void deadlineDoesNotAffectLaterChunks() {
        SpeechChunker c;
        // The capital 'N' after "me." makes it a real boundary, so the first chunk actually emits here.
        c.feed(QStringLiteral("That first pour looked nicely balanced to me. Now "));
        QVERIFY(c.hasEmitted());
        const QStringList out = c.feed(QStringLiteral("a bit more"), /*firstChunkDeadlinePassed=*/true);
        QVERIFY2(out.isEmpty(), "deadline must not force later chunks — only the first");
    }

    // A long clause-free run force-splits at a word/comma boundary rather than growing unbounded.
    void longRunForceSplitsAtWord() {
        SpeechChunker::Config cfg;  // defaults: first 60, max 220
        SpeechChunker c(cfg);
        c.feed(QStringLiteral("Alright here is the first thought. "));  // clear the first chunk
        const QString longRun = QStringLiteral(
            "the water keeps flowing steadily through the puck and the pressure holds and the flow stays even "
            "and nothing looks off and the shot just keeps going and going well past any normal length without a "
            "single pause or stop anywhere in the whole long pour at all so far");
        const QStringList out = c.feed(longRun);
        QVERIFY(!out.isEmpty());
        for (const QString& chunk : out) {
            QVERIFY2(chunk.size() <= cfg.maxChunkChars, "no chunk may exceed maxChunkChars");
            QVERIFY2(!chunk.endsWith(u' '), "chunks are trimmed");
        }
    }

    // reset() drops the buffer and the emitted flag — barge-in starts clean.
    void resetClearsState() {
        SpeechChunker c;
        c.feed(QStringLiteral("Half a sentence that never"));
        c.reset();
        QVERIFY(!c.hasEmitted());
        QVERIFY(c.buffer().isEmpty());
        QCOMPARE(c.flush(), QString());
    }

    // flush() on an empty buffer returns nothing and does not mark emitted.
    void flushEmptyIsNoOp() {
        SpeechChunker c;
        QCOMPARE(c.flush(), QString());
        QVERIFY(!c.hasEmitted());
    }
};

QTEST_APPLESS_MAIN(TestSpeechChunker)
#include "tst_speechchunker.moc"
