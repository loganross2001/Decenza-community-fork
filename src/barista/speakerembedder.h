#pragma once

#include <QByteArray>
#include <QVector>
#include <cmath>

// [barista-fork] Voice-ID (Phase 2) — the embedder SEAM.
//
// A speaker embedder is a pure function: a chunk of mono PCM → a fixed-length float vector ("voiceprint")
// whose cosine similarity is high for the SAME speaker and low for different speakers. Everything else in the
// voice-ID pipeline (capture, enrollment, voiceprints.db, match→confirm, auto-switch) is model-agnostic and
// talks ONLY to this interface — so the embedder is swappable:
//   * Increment 1 ships MfccEmbedder: a self-contained MFCC baseline (no model download, no ONNX runtime).
//   * A later increment can drop in an OnnxEcapaEmbedder (ECAPA-TDNN via ONNX Runtime) behind THIS same
//     interface for much higher accuracy, without touching capture / storage / UX.
//
// Deterministic: the same PCM must always yield the same vector (enrollment ↔ match must be comparable).
class SpeakerEmbedder {
public:
    virtual ~SpeakerEmbedder() = default;

    // Embed 16-bit little-endian MONO PCM (int16 samples) sampled at `sampleRate` Hz. Returns a fixed-length
    // L2-normalized vector; returns an empty vector if there isn't enough voiced audio to characterize.
    virtual QVector<float> embed(const QByteArray& pcm16leMono, int sampleRate) const = 0;

    // A stable identifier for the embedder that produced a vector (so a future model swap can refuse to
    // compare vectors from a different embedder / dimensionality). e.g. "mfcc-v1".
    virtual QString name() const = 0;
};

// Cosine similarity of two vectors in [-1, 1]. Both are expected already L2-normalized (embed() normalizes),
// so this is just the dot product — but it renormalizes defensively so callers can't get a garbage score from
// an un-normalized input. Returns 0 for empty / mismatched-length / zero-norm inputs (never NaN).
inline float cosineSimilarity(const QVector<float>& a, const QVector<float>& b) {
    if (a.isEmpty() || a.size() != b.size())
        return 0.0f;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na  += static_cast<double>(a[i]) * a[i];
        nb  += static_cast<double>(b[i]) * b[i];
    }
    if (na <= 0.0 || nb <= 0.0)
        return 0.0f;
    const double sim = dot / (std::sqrt(na) * std::sqrt(nb));
    if (!std::isfinite(sim))
        return 0.0f;
    return static_cast<float>(sim);
}
