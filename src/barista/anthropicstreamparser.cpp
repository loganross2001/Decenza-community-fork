#include "anthropicstreamparser.h"

#include <QJsonDocument>
#include <QJsonObject>

// [barista-fork] See anthropicstreamparser.h. The whole class is a line splitter feeding a per-line JSON
// decoder. SSE frames are `event: <type>` + `data: <json>` + a blank line; because every Anthropic event's
// `data:` JSON carries its own `type`, we dispatch off the JSON and treat the `event:` line as redundant — this
// is more robust than tracking event/data pairing across the blank-line boundary, and it means a ping or an
// unmodelled event type costs nothing.

namespace barista {

QVector<SseEvent> AnthropicStreamParser::feed(const QByteArray& chunk) {
    QVector<SseEvent> out;
    m_lineBuffer += chunk;

    // Process only complete lines (terminated by '\n'); a trailing partial line stays buffered for next feed().
    qsizetype nl;
    while ((nl = m_lineBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_lineBuffer.left(nl);
        m_lineBuffer.remove(0, nl + 1);
        if (line.endsWith('\r')) {
            line.chop(1);  // tolerate CRLF
        }
        if (line.isEmpty() || line.startsWith("event:") || line.startsWith(":")) {
            // Blank line ends a frame; `event:` is redundant with the JSON `type`; `:` is an SSE comment.
            continue;
        }
        if (line.startsWith("data:")) {
            QByteArray json = line.mid(5);       // strip "data:"
            if (json.startsWith(' ')) {
                json.remove(0, 1);               // and one optional leading space
            }
            out += parseDataLine(json);
        }
        // Any other line shape is ignored (SSE `id:`/`retry:` fields Anthropic doesn't send).
    }
    return out;
}

QVector<SseEvent> AnthropicStreamParser::parseDataLine(const QByteArray& json) const {
    QVector<SseEvent> out;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // A `data:` line that isn't valid JSON is malformed framing, not a modelled event — skip it rather than
        // guess. (The whole-line buffering above means we never see a *truncated* JSON here.)
        return out;
    }
    const QJsonObject o = doc.object();
    const QString type = o.value(QStringLiteral("type")).toString();

    SseEvent ev;
    if (type == QLatin1String("message_start")) {
        ev.kind = SseEvent::MessageStart;
    } else if (type == QLatin1String("content_block_start")) {
        ev.kind = SseEvent::ContentBlockStart;
        ev.index = o.value(QStringLiteral("index")).toInt(-1);
        const QJsonObject block = o.value(QStringLiteral("content_block")).toObject();
        ev.blockType = block.value(QStringLiteral("type")).toString();
        ev.toolName = block.value(QStringLiteral("name")).toString();
        ev.toolId = block.value(QStringLiteral("id")).toString();
    } else if (type == QLatin1String("content_block_delta")) {
        ev.index = o.value(QStringLiteral("index")).toInt(-1);
        const QJsonObject delta = o.value(QStringLiteral("delta")).toObject();
        const QString dtype = delta.value(QStringLiteral("type")).toString();
        if (dtype == QLatin1String("text_delta")) {
            ev.kind = SseEvent::TextDelta;
            ev.text = delta.value(QStringLiteral("text")).toString();
        } else if (dtype == QLatin1String("input_json_delta")) {
            ev.kind = SseEvent::InputJsonDelta;
            ev.partialJson = delta.value(QStringLiteral("partial_json")).toString();
        } else if (dtype == QLatin1String("thinking_delta")) {
            ev.kind = SseEvent::ThinkingDelta;
            ev.text = delta.value(QStringLiteral("thinking")).toString();
        } else {
            ev.kind = SseEvent::Unknown;  // e.g. signature_delta — modelled as Unknown, consumer ignores
        }
    } else if (type == QLatin1String("content_block_stop")) {
        ev.kind = SseEvent::ContentBlockStop;
        ev.index = o.value(QStringLiteral("index")).toInt(-1);
    } else if (type == QLatin1String("message_delta")) {
        ev.kind = SseEvent::MessageDelta;
        ev.stopReason = o.value(QStringLiteral("delta")).toObject()
                            .value(QStringLiteral("stop_reason")).toString();
    } else if (type == QLatin1String("message_stop")) {
        ev.kind = SseEvent::MessageStop;
    } else if (type == QLatin1String("ping")) {
        ev.kind = SseEvent::Ping;
    } else if (type == QLatin1String("error")) {
        ev.kind = SseEvent::Error;
        ev.text = o.value(QStringLiteral("error")).toObject()
                      .value(QStringLiteral("message")).toString();
    } else {
        ev.kind = SseEvent::Unknown;
    }

    out.append(ev);
    return out;
}

void AnthropicStreamParser::reset() {
    m_lineBuffer.clear();
}

}  // namespace barista
