#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "barista/anthropicstreamparser.h"

using barista::AnthropicStreamParser;
using barista::SseEvent;
using barista::assembleAnthropicResponse;

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

    // --- assembleAnthropicResponse: streamed events → whole-body-equivalent {content, stop_reason} ------------
    // The invariant the streaming path depends on: parsing a recorded SSE stream and folding it back with
    // assembleAnthropicResponse yields the SAME `content` array + `stop_reason` that parsing the equivalent
    // whole-body JSON would — so aiprovider.cpp::finalizeConversationResponse drives one identical tool loop
    // for both paths. Each test feeds realistic Anthropic SSE bytes (split across two reads to exercise the
    // line buffer) and compares against the non-streaming JSON for the same logical response.

    // The barista's norm: a forced-`respond` turn. The answer is the respond tool's input.text, streamed as
    // input_json_delta — reconstructed identically to the whole-body tool_use block.
    void assembleForcedRespondTurn() {
        const QByteArray sse =
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"role\":\"assistant\"}}\n\n"
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"respond\",\"input\":{}}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"text\\\": \\\"\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"Grind one step finer.\\\"}\"}}\n\n"
            "event: content_block_stop\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
            "event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n"
            "event: message_stop\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        const QByteArray wholeBody =
            "{\"stop_reason\":\"tool_use\",\"content\":["
            "{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"respond\",\"input\":{\"text\":\"Grind one step finer.\"}}]}";
        assertStreamMatchesWholeBody(sse, wholeBody);
    }

    // A real tool turn: a spoken lead-in (text block) followed by a get_weather tool_use — both reconstructed,
    // in order, exactly as the whole-body content array carries them.
    void assembleLeadInPlusToolTurn() {
        const QByteArray sse =
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"One moment.\"}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
            "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"get_weather\",\"input\":{}}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"location\\\":\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\" \\\"Paris\\\"}\"}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        const QByteArray wholeBody =
            "{\"stop_reason\":\"tool_use\",\"content\":["
            "{\"type\":\"text\",\"text\":\"One moment.\"},"
            "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"get_weather\",\"input\":{\"location\":\"Paris\"}}]}";
        assertStreamMatchesWholeBody(sse, wholeBody);
    }

    // A plain text end_turn reply (the rare non-forced path) reconstructs to a single text block.
    void assemblePlainTextEndTurn() {
        const QByteArray sse =
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Nice and \"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"even.\"}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        const QByteArray wholeBody =
            "{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"Nice and even.\"}]}";
        assertStreamMatchesWholeBody(sse, wholeBody);
    }

    // Web-search-style multi-text-block reply: Anthropic splits one sentence across two text blocks at citation
    // boundaries. Both blocks are preserved, so the terminal path concatenates them exactly as whole-body does.
    void assembleMultiTextBlocks() {
        const QByteArray sse =
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"The weather is \"}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
            "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"sunny today.\"}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        const QByteArray wholeBody =
            "{\"stop_reason\":\"end_turn\",\"content\":["
            "{\"type\":\"text\",\"text\":\"The weather is \"},"
            "{\"type\":\"text\",\"text\":\"sunny today.\"}]}";
        assertStreamMatchesWholeBody(sse, wholeBody);
    }

    // A tool that takes no arguments (no input_json_delta at all) reconstructs input as {}, matching whole-body.
    void assembleNoArgTool() {
        const QByteArray sse =
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_end\",\"name\":\"end_conversation\",\"input\":{}}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        const QByteArray wholeBody =
            "{\"stop_reason\":\"tool_use\",\"content\":["
            "{\"type\":\"tool_use\",\"id\":\"toolu_end\",\"name\":\"end_conversation\",\"input\":{}}]}";
        assertStreamMatchesWholeBody(sse, wholeBody);
    }

    // KNOWN LIMITATION, pinned so it is not "fixed" by accident: assembleAnthropicResponse does NOT faithfully
    // rebuild server-tool blocks (server_tool_use / web_search_tool_result) — it keeps only their `type`,
    // dropping id/name/input/results. That matters because a web-search turn stops with `pause_turn` and the
    // continuation path in aiprovider.cpp echoes `content` VERBATIM as the assistant turn on the re-POST; a
    // lossy reconstruction would make that re-POST malformed. This is WHY 1b gates streaming to
    // `voiceStreaming && !webSearch` (webSearchEnabled defaults ON) and falls back to the whole-body path when
    // web search is on. If you widen streaming to web-search turns, you must first make this reconstruction
    // faithful to server-tool blocks (the parser would need to carry their input/results too).
    void assembleServerToolBlockIsLossy() {
        const QByteArray sse =
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"server_tool_use\",\"id\":\"srvtoolu_1\",\"name\":\"web_search\",\"input\":{}}}\n\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"pause_turn\"}}\n\n";
        AnthropicStreamParser p;
        const QJsonObject root = assembleAnthropicResponse(p.feed(sse));
        QCOMPARE(root.value(QStringLiteral("stop_reason")).toString(), QStringLiteral("pause_turn"));
        const QJsonArray content = root.value(QStringLiteral("content")).toArray();
        QCOMPARE(content.size(), 1);
        // Lossy on purpose: only the type survives — no id/name/input to echo back. Documents the gate above.
        QJsonObject expected;
        expected[QStringLiteral("type")] = QStringLiteral("server_tool_use");
        QCOMPARE(content.first().toObject(), expected);
    }

private:
    // Feed the SSE bytes through the parser in two halves (to exercise the mid-frame line buffer), assemble the
    // events, and assert the reconstructed content + stop_reason equal what parsing the whole-body JSON yields.
    static void assertStreamMatchesWholeBody(const QByteArray& sse, const QByteArray& wholeBody) {
        AnthropicStreamParser p;
        QVector<SseEvent> evs;
        const qsizetype mid = sse.size() / 2;
        evs += p.feed(sse.left(mid));
        evs += p.feed(sse.mid(mid));
        const QJsonObject streamRoot = assembleAnthropicResponse(evs);
        const QJsonObject bodyRoot = QJsonDocument::fromJson(wholeBody).object();
        QCOMPARE(streamRoot.value(QStringLiteral("stop_reason")),
                 bodyRoot.value(QStringLiteral("stop_reason")));
        QCOMPARE(streamRoot.value(QStringLiteral("content")),
                 bodyRoot.value(QStringLiteral("content")));
    }
};

QTEST_APPLESS_MAIN(TestAnthropicStreamParser)
#include "tst_anthropicstreamparser.moc"
