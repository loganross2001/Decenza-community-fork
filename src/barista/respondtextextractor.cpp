#include "respondtextextractor.h"

// [barista-fork] See respondtextextractor.h. Two phases over a contiguous accumulation buffer: locate the
// value's opening quote, then JSON-decode the string until its closing quote. Because m_buf accumulates every
// fragment and m_pos only advances past FULLY-resolved input, an escape or key split across two feed() calls is
// resolved the moment its remaining bytes arrive — no fragment re-framing needed at the call site.

namespace barista {

QString RespondTextExtractor::feed(const QString& partialJson) {
    if (m_done)
        return {};
    m_buf += partialJson;
    QString out;

    // Phase 1 — locate the value's opening quote: `"<key>"` then `:` then the next `"` (whitespace tolerated).
    // Re-run from the buffer start each feed (the buffer is tiny) so a key split across fragments resolves once
    // its bytes are contiguous. Nothing is emitted until the value string actually opens.
    if (!m_inValue) {
        const QString needle = QLatin1Char('"') + m_key + QLatin1Char('"');
        const qsizetype k = m_buf.indexOf(needle);
        if (k < 0)
            return {};                                   // key not fully arrived yet
        const qsizetype colon = m_buf.indexOf(QLatin1Char(':'), k + needle.size());
        if (colon < 0)
            return {};                                   // separator not arrived yet
        qsizetype q = colon + 1;
        while (q < m_buf.size() && m_buf.at(q).isSpace())
            ++q;
        if (q >= m_buf.size())
            return {};                                   // value hasn't started yet
        if (m_buf.at(q) != QLatin1Char('"')) {
            // A non-string value where a string was expected — nothing speakable to decode. Stop cleanly rather
            // than guess; the whole-body path still delivers the final answer as a fallback.
            m_done = true;
            return {};
        }
        m_pos = q + 1;                                   // first char of the value
        m_inValue = true;
    }

    // Phase 2 — decode the string value from m_pos. On an incomplete trailing escape, break WITHOUT advancing
    // m_pos past the backslash, so the next feed() resumes at it once the rest arrives.
    while (m_pos < m_buf.size()) {
        const QChar c = m_buf.at(m_pos);
        if (c == QLatin1Char('\\')) {
            if (m_pos + 1 >= m_buf.size())
                break;                                   // lone trailing '\' — wait for the escape body
            const QChar e = m_buf.at(m_pos + 1);
            if (e == QLatin1Char('u')) {
                if (m_pos + 5 >= m_buf.size())
                    break;                               // `\uXXXX` needs 6 chars — wait for the hex digits
                bool ok = false;
                const ushort code = m_buf.mid(m_pos + 2, 4).toUShort(&ok, 16);
                if (ok) {
                    out += QChar(code);                  // a surrogate half is a valid QChar; a pair reassembles
                    m_pos += 6;
                } else {
                    out += e;                            // malformed \u — best-effort, don't wedge the stream
                    m_pos += 2;
                }
                continue;
            }
            QChar decoded;
            switch (e.unicode()) {
            case '"':  decoded = QLatin1Char('"');  break;
            case '\\': decoded = QLatin1Char('\\'); break;
            case '/':  decoded = QLatin1Char('/');  break;
            case 'n':  decoded = QLatin1Char('\n'); break;
            case 't':  decoded = QLatin1Char('\t'); break;
            case 'r':  decoded = QLatin1Char('\r'); break;
            case 'b':  decoded = QChar(0x0008);     break;
            case 'f':  decoded = QChar(0x000C);     break;
            default:   decoded = e;                 break;  // unknown escape → the literal char
            }
            out += decoded;
            m_pos += 2;
            continue;
        }
        if (c == QLatin1Char('"')) {
            m_done = true;                               // closing quote — value complete
            m_pos += 1;
            break;
        }
        out += c;
        m_pos += 1;
    }

    m_text += out;
    return out;
}

void RespondTextExtractor::reset() {
    m_buf.clear();
    m_text.clear();
    m_pos = 0;
    m_inValue = false;
    m_done = false;
}

}  // namespace barista
