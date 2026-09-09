#include "speechchunker.h"

#include <QChar>
#include <QSet>

// [barista-fork] Streaming speech chunker — see speechchunker.h for the API and why the clock lives in the
// caller. The whole job is one decision made over and over: given the buffered prose so far, is there a
// speakable unit at the front we can hand to TTS now? The rules below are what "speakable unit" means.

namespace barista {

namespace {

// Closing punctuation that belongs WITH the sentence it ends ("done." + '"' → keep the quote on the chunk).
bool isCloser(QChar c) {
    return c == u'"' || c == u'\'' || c == u'’' /* ’ */ || c == u'”' /* ” */
        || c == u')' || c == u']';
}

// Abbreviations whose trailing '.' is NOT a sentence end even when a capital follows ("Mr. Smith", "e.g. The").
// The lowercase-follows heuristic in isBoundary() catches the common lowercase-continuation cases ("etc. and");
// this set is only the ones a capitalised word would otherwise wrongly split.
const QSet<QString>& abbreviations() {
    static const QSet<QString> kAbbrev = {
        QStringLiteral("mr"), QStringLiteral("mrs"), QStringLiteral("ms"), QStringLiteral("dr"),
        QStringLiteral("st"), QStringLiteral("sr"), QStringLiteral("jr"), QStringLiteral("prof"),
        QStringLiteral("vs"), QStringLiteral("e.g"), QStringLiteral("i.e"), QStringLiteral("etc"),
        QStringLiteral("approx"), QStringLiteral("no"), QStringLiteral("oz"), QStringLiteral("al"),
        QStringLiteral("fig"), QStringLiteral("cf"),
    };
    return kAbbrev;
}

}  // namespace

bool SpeechChunker::prevWordIsAbbrev(qsizetype dotIndex) const {
    // Walk back over the letters/dots forming the token that ends at this '.' (so "e.g" and "Mr" both resolve).
    qsizetype start = dotIndex - 1;
    while (start >= 0 && (m_buffer[start].isLetter() || m_buffer[start] == u'.')) {
        --start;
    }
    const QString word = m_buffer.mid(start + 1, dotIndex - (start + 1)).toLower();
    return !word.isEmpty() && abbreviations().contains(word);
}

bool SpeechChunker::isBoundary(qsizetype i) const {
    const qsizetype n = m_buffer.size();
    const QChar c = m_buffer[i];

    if (c == u'!' || c == u'?' || c == u';' || c == u'—' /* em-dash */) {
        return true;
    }
    if (c == u':') {
        // A clause boundary, except a clock/ratio like "3:30" or "1:2".
        if (i > 0 && i + 1 < n && m_buffer[i - 1].isDigit() && m_buffer[i + 1].isDigit()) {
            return false;
        }
        return true;
    }
    if (c == u'.') {
        // "1.5 bar" — a decimal point, not a full stop.
        if (i > 0 && i + 1 < n && m_buffer[i - 1].isDigit() && m_buffer[i + 1].isDigit()) {
            return false;
        }
        if (prevWordIsAbbrev(i)) {
            return false;
        }
        // Look at the next non-space char. If none is buffered yet, defer: we can't tell "etc." from a real
        // stop until we see what follows, and flush() will emit it at the true end regardless. If it's a
        // lowercase letter, the sentence is continuing (an abbreviation or a mid-thought dot), so not a stop.
        qsizetype j = i + 1;
        while (j < n && m_buffer[j].isSpace()) {
            ++j;
        }
        if (j >= n) {
            return false;
        }
        if (m_buffer[j].isLetter() && m_buffer[j].isLower()) {
            return false;
        }
        return true;
    }
    return false;
}

qsizetype SpeechChunker::findChunkEnd(bool firstChunk, bool deadlinePassed) const {
    const qsizetype n = m_buffer.size();
    // The first chunk accepts any boundary (effective min 1) so the lead-in lands fast; later chunks hold to
    // minChunkChars so TTS units aren't choppy.
    const qsizetype effMin = firstChunk ? 1 : m_cfg.minChunkChars;
    const qsizetype forceLimit = firstChunk ? m_cfg.firstChunkChars : m_cfg.maxChunkChars;

    for (qsizetype i = 0; i < n; ++i) {
        if (!isBoundary(i)) {
            continue;
        }
        qsizetype end = i + 1;
        while (end < n && isCloser(m_buffer[end])) {
            ++end;
        }
        if (end >= effMin) {
            return end;
        }
        // Boundary too early (short leading sentence) — keep scanning to merge it into a longer unit.
    }

    // No accepted boundary. Force a split once we're past the limit so a long clause-free run still speaks.
    if (n >= forceLimit) {
        // Prefer the last comma or space before the limit; a comma stays with the chunk, a space is dropped.
        for (qsizetype k = qMin(forceLimit, n) - 1; k > 0; --k) {
            if (m_buffer[k] == u',') {
                return k + 1;
            }
            if (m_buffer[k].isSpace()) {
                return k + 1;
            }
        }
        return qMin(forceLimit, n);  // one unbroken token longer than the limit — hard split
    }

    // First-chunk latency floor elapsed (caller's clock) and something is buffered: release it now.
    if (firstChunk && deadlinePassed && n > 0) {
        return n;
    }
    return -1;
}

QStringList SpeechChunker::feed(const QString& delta, bool firstChunkDeadlinePassed) {
    m_buffer += delta;
    QStringList out;
    // Trim leading whitespace so index positions track content length (min/force thresholds are content-based).
    while (!m_buffer.isEmpty() && m_buffer.front().isSpace()) {
        m_buffer.remove(0, 1);
    }

    for (;;) {
        const bool firstChunk = !m_emittedAny;
        const qsizetype end = findChunkEnd(firstChunk, firstChunkDeadlinePassed);
        if (end < 0) {
            break;
        }
        const QString chunk = m_buffer.left(end).trimmed();
        m_buffer.remove(0, end);
        while (!m_buffer.isEmpty() && m_buffer.front().isSpace()) {
            m_buffer.remove(0, 1);
        }
        if (!chunk.isEmpty()) {
            out << chunk;
            m_emittedAny = true;
        }
    }
    return out;
}

QString SpeechChunker::flush() {
    const QString rest = m_buffer.trimmed();
    m_buffer.clear();
    if (!rest.isEmpty()) {
        m_emittedAny = true;
    }
    return rest;
}

void SpeechChunker::reset() {
    m_buffer.clear();
    m_emittedAny = false;
}

}  // namespace barista
