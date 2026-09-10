#pragma once

#include <QString>

#include <utility>

// [barista-fork] Pure incremental extractor for the spoken text carried in a streamed `respond` tool call
// (BARISTA_VOICE_STREAMING_DESIGN.md §1.2, corrected). The design assumed the model's reply streams as
// top-level `text_delta` blocks that SpeechChunker splits directly. It does NOT for this fork: every barista
// turn runs with `forceRespond` (aiprovider.cpp appends a `respond` tool + `tool_choice:{type:"any"}`), so the
// model is forced straight into a tool call with NO leading `text_delta`, and the spoken answer is the
// `respond` tool's `input.text`. Under Anthropic streaming a tool_use block's input arrives as
// `input_json_delta` fragments whose `partial_json` concatenates to the block's input JSON — e.g.
// `{"text":"Sure, one moment.\nLet me check."}` split at arbitrary byte boundaries (mid-key, mid-escape,
// mid-`\uXXXX`). This class turns that fragment stream into the incrementally-decoded value of the `text`
// key, which THEN feeds SpeechChunker exactly as `text_delta` text would have.
//
// Deliberately pure: QtCore only (QString), no QObject, no network, no signals, so the escape/split hazards
// (a `\` ending one fragment and `"` starting the next, a `\uXXXX` whose hex digits straddle two feeds, a
// surrogate pair spread across two `\u` escapes) unit-test against recorded fragment sequences the way
// speechchunker and anthropicstreamparser do. The consumer (aiprovider.cpp) owns the block gating — it feeds
// ONLY the `respond` tool_use block's partial_json here, identified by its ContentBlockStart name/index — so
// this class never has to know which content block it is decoding.
//
// Assumption (true for the `respond` schema, whose sole property is `text`): the extracted field is a JSON
// string value keyed by a known name (default "text"). It is located by matching `"<key>"` then the next `:`
// then the opening quote; the object may carry whitespace but is not expected to carry other keys before it.
namespace barista {

class RespondTextExtractor {
public:
    RespondTextExtractor() = default;
    // The JSON key whose string value carries the spoken text. Defaults to the `respond` tool's `text` field.
    explicit RespondTextExtractor(QString key) : m_key(std::move(key)) {}

    // Feed one `partial_json` fragment exactly as it arrives (the parser's SseEvent::partialJson). Returns the
    // text newly DECODED by this fragment — zero or more characters of the reply, ready to hand to
    // SpeechChunker::feed(). Bytes forming an incomplete escape (`\` or a short `\uXXXX`) or a not-yet-arrived
    // key are held until a later feed() completes them, so callers may pass fragments verbatim.
    QString feed(const QString& partialJson);

    void reset();

    QString text() const { return m_text; }  // everything decoded so far (inspection / final flush)
    bool done() const { return m_done; }      // the value's closing quote has been seen

private:
    QString m_key = QStringLiteral("text");
    QString m_buf;              // all partial_json seen so far (contiguous, so cross-fragment splits just work)
    QString m_text;            // decoded value so far
    qsizetype m_pos = 0;        // decode cursor into m_buf (points past the last fully-decoded char / escape)
    bool m_inValue = false;    // located the opening quote of the value → now decoding
    bool m_done = false;       // hit the closing quote (or a non-string value we can't decode)
};

}  // namespace barista
