#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "barista/respondtextextractor.h"

using barista::RespondTextExtractor;

// [barista-fork] Unit tests for the streamed-`respond` text extractor (BARISTA_VOICE_STREAMING_DESIGN.md §1.2,
// corrected). Because the barista's forced-`respond` turns deliver the spoken reply as `input_json_delta`
// fragments of `{"text":"…"}` rather than as `text_delta`, the one part of the streaming pipeline that reads
// the reply out of that JSON is pure and clock-free — so every fragment-split hazard (a `\` ending one read
// and the escape body starting the next, a `\uXXXX` whose hex straddles two reads, a surrogate pair across two
// escapes, the key itself split mid-token) is pinned here instead of surfacing as garbled TTS on the tablet.
class TestRespondTextExtractor : public QObject {
    Q_OBJECT

private slots:
    // The whole object in one fragment: the value decodes and the closing quote marks it done.
    void wholeObjectOneFragment() {
        RespondTextExtractor x;
        QCOMPARE(x.feed(QStringLiteral("{\"text\": \"Nice and even.\"}")), QStringLiteral("Nice and even."));
        QVERIFY(x.done());
        QCOMPARE(x.text(), QStringLiteral("Nice and even."));
    }

    // Each feed returns ONLY the text newly decoded by that fragment — the incremental contract SpeechChunker
    // relies on. The opening `{"text": "` fragment yields nothing speakable; the body fragments yield their own.
    void incrementalReturnsOnlyNewText() {
        RespondTextExtractor x;
        QCOMPARE(x.feed(QStringLiteral("{\"text\": \"")), QString());
        QCOMPARE(x.feed(QStringLiteral("Let me ")), QStringLiteral("Let me "));
        QCOMPARE(x.feed(QStringLiteral("check that.")), QStringLiteral("check that."));
        QVERIFY(!x.done());
        QCOMPARE(x.feed(QStringLiteral("\"}")), QString());  // just the closing quote → no new text
        QVERIFY(x.done());
        QCOMPARE(x.text(), QStringLiteral("Let me check that."));
    }

    // The value's own bytes may arrive one character at a time (the pathological split) and still reassemble.
    void characterAtATime() {
        RespondTextExtractor x;
        const QString json = QStringLiteral("{\"text\":\"Hi.\"}");
        QString acc;
        for (const QChar ch : json)
            acc += x.feed(QString(ch));
        QCOMPARE(acc, QStringLiteral("Hi."));
        QVERIFY(x.done());
    }

    // A backslash ending one fragment and the escape body starting the next must decode as one escape, not two
    // literal chars — the split that timing-based framing gets wrong.
    void escapeSplitAcrossFragments() {
        RespondTextExtractor x;
        x.feed(QStringLiteral("{\"text\":\"line one"));
        QCOMPARE(x.feed(QStringLiteral("\\")), QString());          // lone trailing backslash → held, nothing emitted
        QCOMPARE(x.feed(QStringLiteral("nline two\"}")), QStringLiteral("\nline two"));  // resolves to a newline
        QCOMPARE(x.text(), QStringLiteral("line one\nline two"));
        QVERIFY(x.done());
    }

    // The common JSON escapes decode to their characters; an escaped quote does NOT end the value.
    void jsonEscapesDecode() {
        RespondTextExtractor x;
        const QString out = x.feed(QStringLiteral("{\"text\":\"a\\\"quote\\\", a tab\\tand a slash\\/end\"}"));
        QCOMPARE(out, QStringLiteral("a\"quote\", a tab\tand a slash/end"));
        QVERIFY(x.done());
    }

    // A `\uXXXX` escape decodes, even when its six characters are split across three feeds.
    void unicodeEscapeSplit() {
        RespondTextExtractor x;
        x.feed(QStringLiteral("{\"text\":\"caf\\u"));
        QCOMPARE(x.feed(QStringLiteral("00")), QString());          // only 2 of 4 hex digits — held
        QCOMPARE(x.feed(QStringLiteral("e9 later\"}")), QString::fromUtf8("é later"));  // é
        QCOMPARE(x.text(), QString::fromUtf8("café later"));
        QVERIFY(x.done());
    }

    // A surrogate pair spread across two \u escapes reassembles into the single code point (emoji).
    void surrogatePairReassembles() {
        RespondTextExtractor x;
        const QString out = x.feed(QStringLiteral("{\"text\":\"hi \\ud83d\\ude00\"}"));  // U+1F600 😀
        QCOMPARE(out, QString::fromUtf8("hi \xF0\x9F\x98\x80"));
        QVERIFY(x.done());
    }

    // The key token itself may be split across fragments; nothing is emitted until it is contiguous.
    void keySplitAcrossFragments() {
        RespondTextExtractor x;
        QCOMPARE(x.feed(QStringLiteral("{\"te")), QString());
        QCOMPARE(x.feed(QStringLiteral("xt\": \"go\"}")), QStringLiteral("go"));
        QVERIFY(x.done());
    }

    // Whitespace between the key, colon, and opening quote is tolerated.
    void whitespaceAroundColon() {
        RespondTextExtractor x;
        QCOMPARE(x.feed(QStringLiteral("{ \"text\"  :   \"ok\" }")), QStringLiteral("ok"));
        QVERIFY(x.done());
    }

    // Once done, further fragments (e.g. a trailing tool-call frame) are ignored.
    void ignoresAfterClose() {
        RespondTextExtractor x;
        x.feed(QStringLiteral("{\"text\":\"done\"}"));
        QVERIFY(x.done());
        QCOMPARE(x.feed(QStringLiteral("garbage")), QString());
        QCOMPARE(x.text(), QStringLiteral("done"));
    }

    // reset() returns the extractor to a fresh state for the next turn / a barge-in.
    void resetClearsState() {
        RespondTextExtractor x;
        x.feed(QStringLiteral("{\"text\":\"first\"}"));
        x.reset();
        QVERIFY(!x.done());
        QVERIFY(x.text().isEmpty());
        QCOMPARE(x.feed(QStringLiteral("{\"text\":\"second\"}")), QStringLiteral("second"));
    }

    // A non-string value where a string was expected stops cleanly (done, nothing spoken) rather than guessing —
    // the whole-body path still delivers the answer as the fallback.
    void nonStringValueStopsClean() {
        RespondTextExtractor x;
        QCOMPARE(x.feed(QStringLiteral("{\"text\": 42}")), QString());
        QVERIFY(x.done());
        QVERIFY(x.text().isEmpty());
    }

    // A realistic Anthropic fragmentation of a forced-`respond` turn: the opening frame, several body chunks
    // that each yield their own speakable text, then the close.
    void realisticRespondStream() {
        RespondTextExtractor x;
        QString spoken;
        spoken += x.feed(QStringLiteral("{\"text\": \""));
        spoken += x.feed(QStringLiteral("Your last shot ran a touch fast. "));
        spoken += x.feed(QStringLiteral("Try grinding one step finer"));
        spoken += x.feed(QStringLiteral(" and pull it again."));
        spoken += x.feed(QStringLiteral("\"}"));
        QCOMPARE(spoken, QStringLiteral("Your last shot ran a touch fast. Try grinding one step finer and pull it again."));
        QVERIFY(x.done());
    }

    // A configurable key still works (defensive: not every future tool names its field "text").
    void configurableKey() {
        RespondTextExtractor x(QStringLiteral("reply"));
        QCOMPARE(x.feed(QStringLiteral("{\"reply\":\"hey\"}")), QStringLiteral("hey"));
        QVERIFY(x.done());
    }

    // In the live pipeline the SAME respond.text is decoded twice: this extractor drives the early streamed
    // speech, while QJsonDocument (inside assembleAnthropicResponse) decodes it for the final analysisComplete
    // text that reaches the screen/history and is spoken as the fallback. If this hand-rolled un-escaping ever
    // disagreed with Qt's parser, the audio and the screen would diverge. Pin agreement here, headless, so that
    // drift can never first surface as an audio/text mismatch on the tablet. Every escape class the two must
    // agree on — quote, newline, tab, slash, backslash, `\uXXXX`, and a surrogate pair — is in one string,
    // fed one character at a time (worst-case fragmentation).
    void agreesWithQtParserOnEscapes() {
        const QByteArray json =
            "{\"text\":\"q=\\\"x\\\" nl=\\n tab=\\t sl=\\/ bs=\\\\ e=\\u00e9 emoji=\\ud83d\\ude00 end\"}";
        const QString viaQt = QJsonDocument::fromJson(json).object().value(QStringLiteral("text")).toString();
        RespondTextExtractor x;
        for (const QChar ch : QString::fromUtf8(json))
            x.feed(QString(ch));
        QVERIFY(x.done());
        QCOMPARE(x.text(), viaQt);
    }

    // Belt-and-braces: whatever Qt CHOSE to emit when it serialized a tricky value (literal UTF-8, control-char
    // escapes), the extractor decodes back to the same value — the round trip both decoders see in production.
    void agreesWithQtParserOnCanonicalEncoding() {
        const QString tricky = QString::fromUtf8("She said \"hi\".\nRatio 1:2.5\tcaf\xC3\xA9 \xF0\x9F\x98\x80 end");
        QJsonObject o;
        o[QStringLiteral("text")] = tricky;
        const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
        RespondTextExtractor x;
        for (const QChar ch : QString::fromUtf8(json))
            x.feed(QString(ch));
        QVERIFY(x.done());
        QCOMPARE(x.text(), tricky);
    }
};

QTEST_MAIN(TestRespondTextExtractor)
#include "tst_respondtextextractor.moc"
