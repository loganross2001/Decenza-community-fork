#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// [barista-fork] Pure incremental parser for Anthropic Messages-API server-sent events, for the streaming
// voice pipeline (BARISTA_VOICE_STREAMING_DESIGN.md §1.1). The barista turn will POST with "stream": true and
// receive SSE deltas on QNetworkReply::readyRead(); this class turns the raw byte chunks — which split at
// arbitrary boundaries, mid-line and mid-frame — into a clean sequence of typed events the tool loop can drive.
//
// Deliberately pure: QtCore only (QByteArray/QString/QJson), no QObject, no network, no signals, so the whole
// frame-parsing job (partial lines carried across reads, ping heartbeats, mid-stream error frames, per-block
// input_json accumulation) unit-tests against recorded fixtures the way speechchunker and closeintent do. The
// one part that genuinely needs the running app — the readyRead wiring and the turn-loop integration in the
// shared aiprovider.cpp — stays out of here.
//
// The parser emits granular deltas and does NOT itself accumulate assistant text or tool-input JSON; the
// consumer accumulates (aiprovider.cpp already does this for the whole-body path — m_accumulatedText and the
// per-index input_json buffers). Keeping accumulation in the consumer keeps this class a pure demux.
namespace barista {

struct SseEvent {
    enum Kind {
        MessageStart,       // message metadata (start of a turn)
        ContentBlockStart,  // a new content block opened — see blockType/toolName/toolId/index
        TextDelta,          // a fragment of assistant text — `text`
        InputJsonDelta,     // a fragment of a tool_use block's input JSON — `partialJson` (accumulate per index)
        ThinkingDelta,      // a fragment of a thinking block — `text`
        ContentBlockStop,   // the block at `index` finished
        MessageDelta,       // message-level update — carries `stopReason` when present
        MessageStop,        // the turn is complete
        Ping,               // heartbeat — consumer ignores
        Error,              // mid-stream error frame — `text` is the message
        Unknown,            // an event whose type we don't model (forward-compatible: consumer ignores)
    };

    Kind kind = Unknown;
    int index = -1;         // content-block index for Start/Delta/Stop; -1 otherwise
    QString blockType;      // ContentBlockStart: "text" | "tool_use" | "thinking"
    QString toolName;       // ContentBlockStart (tool_use): the tool name
    QString toolId;         // ContentBlockStart (tool_use): the tool_use id (echoed back in tool_result)
    QString text;           // TextDelta/ThinkingDelta: the fragment; Error: the message
    QString partialJson;    // InputJsonDelta: the JSON fragment
    QString stopReason;     // MessageDelta/MessageStop: stop_reason when present ("tool_use", "end_turn", ...)
};

class AnthropicStreamParser {
public:
    // Feed a chunk of raw SSE bytes exactly as they arrive from the socket. Returns every event that became
    // complete within this chunk (possibly zero). Bytes forming a partial trailing line are buffered until the
    // next feed() completes them, so callers may pass whatever readAll() hands them without pre-framing.
    QVector<SseEvent> feed(const QByteArray& chunk);

    // Drop the line buffer — a new turn, or an aborted stream.
    void reset();

private:
    QVector<SseEvent> parseDataLine(const QByteArray& json) const;

    QByteArray m_lineBuffer;  // bytes received since the last newline (a line split across two reads)
};

}  // namespace barista
