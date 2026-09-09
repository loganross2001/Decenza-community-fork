#include <QtTest>

#include "barista/anthropicstreamparser.h"

using barista::AnthropicStreamParser;
using barista::SseEvent;

// [barista-fork] Unit tests for the Anthropic SSE stream parser (BARISTA_VOICE_STREAMING_DESIGN.md §1.1).
// The parser is the pure, headless-verifiable half of the streaming turn — the readyRead wiring and tool-loop
// integration in the shared aiprovider.cpp are the on-device half. Everything a real byte stream throws at it —
// frames split mid-line across reads, ping heartbeats, per-block input_json_delta fragments that must be
// attributed to the right tool block, CRLF endings, mid-stream error frames, unmodelled delta types — is a
// defect shape that no other test covers and that would otherwise only surface as a wedged or mis-parsed live
// conversation on the tablet.
class TestAnthropicStreamParser : public QObject {
    Q_OBJECT

private:
    // Collect the kinds in order, for terse sequence assertions.
    static QVector<SseEvent::Kind> kinds(const QVector<SseEvent>& evs) {
        QVector<SseEvent::Kind> k;
        for (const SseEvent& e : evs) {
            k.append(e.kind);
        }
        return k;
    }

private slots:
    // A whole plain-text turn delivered in one chunk parses into the expected event sequence and text.
    void plainTextTurn() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"}}\n"
            "\n"
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
            "\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Nice \"}}\n"
            "\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"shot.\"}}\n"
            "\n"
            "event: content_block_stop\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n"
            "\n"
            "event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n"
            "\n"
            "event: message_stop\n"
            "data: {\"type\":\"message_stop\"}\n"
            "\n";
        const QVector<SseEvent> evs = p.feed(stream);
        QCOMPARE(kinds(evs), (QVector<SseEvent::Kind>{
                                 SseEvent::MessageStart, SseEvent::ContentBlockStart, SseEvent::TextDelta,
                                 SseEvent::TextDelta, SseEvent::ContentBlockStop, SseEvent::MessageDelta,
                                 SseEvent::MessageStop}));
        QString text;
        for (const SseEvent& e : evs) {
            if (e.kind == SseEvent::TextDelta) {
                text += e.text;
            }
        }
        QCOMPARE(text, QStringLiteral("Nice shot."));
        // The stop_reason rides on message_delta.
        const SseEvent& md = evs.at(5);
        QCOMPARE(md.stopReason, QStringLiteral("end_turn"));
    }

    // A frame split across two feed() calls — mid-line — still parses once completed. This is the core
    // real-socket hazard: readAll() returns whatever bytes have arrived, not whole lines.
    void frameSplitMidLine() {
        AnthropicStreamParser p;
        const QByteArray part1 =
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_de";
        const QByteArray part2 =
            "lta\",\"text\":\"hello\"}}\n\n";
        QVERIFY2(p.feed(part1).isEmpty(), "no event until the data line is complete");
        const QVector<SseEvent> evs = p.feed(part2);
        QCOMPARE(evs.size(), 1);
        QCOMPARE(evs.first().kind, SseEvent::TextDelta);
        QCOMPARE(evs.first().text, QStringLiteral("hello"));
    }

    // A tool_use turn: the block start carries name + id, and input_json_delta fragments accumulate (by index)
    // into the full tool input. This is the whole point of streaming a tool call.
    void toolUseInputAccumulates() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":"
            "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"getShotHistory\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
            "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"limit\\\":\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
            "{\"type\":\"input_json_delta\",\"partial_json\":\"3}\"}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n";
        const QVector<SseEvent> evs = p.feed(stream);

        QCOMPARE(evs.first().kind, SseEvent::ContentBlockStart);
        QCOMPARE(evs.first().blockType, QStringLiteral("tool_use"));
        QCOMPARE(evs.first().toolName, QStringLiteral("getShotHistory"));
        QCOMPARE(evs.first().toolId, QStringLiteral("toolu_9"));
        QCOMPARE(evs.first().index, 1);

        // Accumulate the way the consumer will: concatenate partial_json for the block's index.
        QString input;
        for (const SseEvent& e : evs) {
            if (e.kind == SseEvent::InputJsonDelta && e.index == 1) {
                input += e.partialJson;
            }
        }
        QCOMPARE(input, QStringLiteral("{\"limit\":3}"));

        // The turn ends asking for a tool, not end_turn.
        QCOMPARE(evs.last().kind, SseEvent::MessageDelta);
        QCOMPARE(evs.last().stopReason, QStringLiteral("tool_use"));
    }

    // ping heartbeats are surfaced as Ping (so a consumer could reset a watchdog) and never mistaken for data.
    void pingIsParsedAndInert() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "event: ping\n"
            "data: {\"type\":\"ping\"}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"x\"}}\n\n";
        const QVector<SseEvent> evs = p.feed(stream);
        QCOMPARE(kinds(evs), (QVector<SseEvent::Kind>{SseEvent::Ping, SseEvent::TextDelta}));
    }

    // A mid-stream error frame surfaces as Error with the message, so the turn can fail loudly instead of hanging.
    void errorFrameSurfaces() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "event: error\n"
            "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n";
        const QVector<SseEvent> evs = p.feed(stream);
        QCOMPARE(evs.size(), 1);
        QCOMPARE(evs.first().kind, SseEvent::Error);
        QCOMPARE(evs.first().text, QStringLiteral("Overloaded"));
    }

    // CRLF line endings are tolerated (some proxies rewrite LF to CRLF).
    void crlfTolerated() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"ok\"}}\r\n\r\n";
        const QVector<SseEvent> evs = p.feed(stream);
        QCOMPARE(evs.size(), 1);
        QCOMPARE(evs.first().text, QStringLiteral("ok"));
    }

    // An unmodelled delta type (e.g. signature_delta on thinking) is Unknown, not a crash or a misparse.
    void unknownDeltaIsUnknown() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"signature_delta\",\"signature\":\"abc\"}}\n\n";
        const QVector<SseEvent> evs = p.feed(stream);
        QCOMPARE(evs.size(), 1);
        QCOMPARE(evs.first().kind, SseEvent::Unknown);
    }

    // A thinking_delta carries its fragment in `text`.
    void thinkingDelta() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}\n\n";
        const QVector<SseEvent> evs = p.feed(stream);
        QCOMPARE(evs.size(), 1);
        QCOMPARE(evs.first().kind, SseEvent::ThinkingDelta);
        QCOMPARE(evs.first().text, QStringLiteral("hmm"));
    }

    // A malformed data line (not valid JSON) is skipped, not emitted or crashed on.
    void malformedJsonSkipped() {
        AnthropicStreamParser p;
        const QByteArray stream =
            "data: this is not json\n\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        const QVector<SseEvent> evs = p.feed(stream);
        QCOMPARE(evs.size(), 1);
        QCOMPARE(evs.first().kind, SseEvent::MessageStop);
    }

    // reset() drops a half-received line so a new turn starts clean.
    void resetClearsPartialLine() {
        AnthropicStreamParser p;
        p.feed(QByteArray("data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_d"));
        p.reset();
        // The dangling fragment is gone; a fresh complete line parses on its own.
        const QVector<SseEvent> evs =
            p.feed(QByteArray("data: {\"type\":\"message_stop\"}\n\n"));
        QCOMPARE(evs.size(), 1);
        QCOMPARE(evs.first().kind, SseEvent::MessageStop);
    }
};

QTEST_APPLESS_MAIN(TestAnthropicStreamParser)
#include "tst_anthropicstreamparser.moc"
