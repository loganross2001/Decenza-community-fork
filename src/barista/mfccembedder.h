#pragma once

#include "speakerembedder.h"

#include <QVector>

// [barista-fork] Voice-ID Increment 1 — self-contained MFCC baseline embedder (no model download, no ONNX).
//
// Pipeline (classic speaker-verification front end): int16 mono PCM → pre-emphasis → 25ms/10ms framing →
// Hamming window → 512-pt radix-2 FFT → power spectrum → 26-band mel filterbank → log → DCT-II → 13 MFCCs
// per frame. Near-silent frames are dropped (energy gate). The utterance voiceprint is the per-coefficient
// MEAN and STDDEV across the voiced frames (captures a speaker's average timbre AND its variability),
// concatenated to a fixed 26-dim vector and L2-normalized.
//
// This is a BASELINE: robust enough to tell a few enrolled speakers apart on one machine with match-then-
// confirm, but not channel/noise-robust like ECAPA. It exists to prove the capture/enroll/store/UX pipeline
// end-to-end with ZERO external dependencies; a deep embedder can replace it behind SpeakerEmbedder later.
class MfccEmbedder : public SpeakerEmbedder {
public:
    MfccEmbedder();

    QVector<float> embed(const QByteArray& pcm16leMono, int sampleRate) const override;
    QString name() const override { return QStringLiteral("mfcc-v1"); }

    // Exposed for the unit test (deterministic building blocks).
    static constexpr int kNumCepstra = 13;   // MFCCs kept per frame (c0..c12)
    static constexpr int kNumFilters = 26;   // mel filterbank bands
    static constexpr int kFftSize = 512;     // power-of-two FFT (covers a 25ms frame @16k = 400 samples)
    // Voiceprint dimensionality = mean(13) ++ stddev(13).
    static constexpr int kDims = kNumCepstra * 2;

private:
    // Build the mel filterbank triangle weights for a given sample rate (cached per rate on first use).
    QVector<QVector<float>> melFilterbank(int sampleRate) const;

    // In-place iterative radix-2 Cooley-Tukey FFT on interleaved re/im (size kFftSize). Deterministic.
    static void fftRadix2(QVector<float>& re, QVector<float>& im);
};
