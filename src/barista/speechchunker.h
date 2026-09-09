#pragma once

#include <QString>
#include <QStringList>

// [barista-fork] Pure text → speakable-chunk splitter for the streaming voice pipeline
// (BARISTA_VOICE_STREAMING_DESIGN.md §1.2). The model streams tokens; this turns the growing prose into
// sentence/clause-sized units so TTS can start on the first clause (~1 s) instead of waiting for the whole
// reply. Kept deliberately pure — QtCore only, no QObject, no timer, no mic/AI/voice deps — so it unit-tests
// in isolation (tests/tst_speechchunker.cpp) the way closeintent does. See speechchunker.cpp for the boundary
// rules and their rationale.
//
// The one thing this class deliberately does NOT own is the clock. The design's first-chunk rule is "emit at
// the first boundary OR 60 chars OR 1.2 s, whichever first" — the 1.2 s is a latency floor that belongs to the
// consumer (a QTimer there), not here, or the class would be untestable and would smuggle in the timer-as-guard
// pattern CLAUDE.md bans. The caller passes the elapsed-deadline as a bool into feed(); the char/boundary halves
// are pure and fully tested.
namespace barista {

class SpeechChunker {
public:
    struct Config {
        int minChunkChars = 25;    // don't emit a chunk shorter than this (after the first) — avoids choppy TTS
        int maxChunkChars = 220;   // force a word/comma split by here even with no sentence boundary
        int firstChunkChars = 60;  // the first chunk force-splits earlier, so the lead-in lands fast
    };

    SpeechChunker() = default;
    explicit SpeechChunker(Config cfg) : m_cfg(cfg) {}

    // Feed a delta of streamed text; returns zero or more chunks that are speakable NOW (front of the buffer),
    // leaving any trailing partial sentence buffered. `firstChunkDeadlinePassed` is the caller's signal that the
    // ~1.2 s first-chunk latency floor has elapsed — when true and nothing has been emitted yet, the buffered
    // text is released even without a boundary so the lead-in is not late. Ignored once a chunk has been emitted.
    QStringList feed(const QString& delta, bool firstChunkDeadlinePassed = false);

    // Emit whatever remains as a final chunk (on content_block_stop / turn complete / block end). "" if empty.
    QString flush();

    // Drop all buffered state — barge-in, or the start of a new turn.
    void reset();

    bool hasEmitted() const { return m_emittedAny; }
    QString buffer() const { return m_buffer; }  // inspection / tests

private:
    // Exclusive end index of the next chunk in m_buffer, or -1 if none is ready yet.
    qsizetype findChunkEnd(bool firstChunk, bool deadlinePassed) const;
    bool isBoundary(qsizetype i) const;               // is m_buffer[i] a real sentence/clause boundary?
    bool prevWordIsAbbrev(qsizetype dotIndex) const;  // is the word ending at this '.' a known abbreviation?

    Config m_cfg;
    QString m_buffer;
    bool m_emittedAny = false;
};

}  // namespace barista
