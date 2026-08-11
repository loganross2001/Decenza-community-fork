#include <QTest>
#include <QByteArray>
#include <QVector>
#include <cmath>

#include "barista/mfccembedder.h"
#include "barista/speakerembedder.h"

// [barista-fork] Voice-ID Increment 1 — validates the MFCC embedder DSP (FFT/mel/DCT + aggregation) purely in
// software, no device/mic. Deterministic synthetic PCM: same input → identical vector; a self-match scores ~1;
// two DIFFERENT tones are LESS similar than a tone vs. itself-with-a-little-noise; vectors are finite + fixed.

namespace {
constexpr int kSr = 16000;
constexpr double kPi = 3.14159265358979323846;

// Deterministic small PRNG (fixed seed) so "noise" is reproducible across runs/platforms.
struct Lcg { quint32 s; double next() { s = s * 1664525u + 1013904223u; return (s >> 8) / double(1u << 24) * 2.0 - 1.0; } };

// `seconds` of a pure sine at `freq` Hz, int16 mono LE, plus optional deterministic noise at `noiseAmp`.
QByteArray tone(double freq, double seconds, double amp, double noiseAmp = 0.0, quint32 seed = 1) {
    const int n = int(seconds * kSr);
    QByteArray pcm(n * 2, Qt::Uninitialized);
    auto* out = reinterpret_cast<qint16*>(pcm.data());
    Lcg rng{seed};
    for (int i = 0; i < n; ++i) {
        double s = amp * std::sin(2.0 * kPi * freq * i / kSr);
        if (noiseAmp > 0.0) s += noiseAmp * rng.next();
        s = qBound(-1.0, s, 1.0);
        out[i] = qint16(std::lround(s * 32767.0));
    }
    return pcm;
}

// A crude VOWEL-like signal: harmonics of pitch `f0` weighted by a Gaussian "formant" envelope centered at
// `formantHz`. Two different (f0, formant) pairs stand in for two different SPEAKERS — a broadband, harmonic
// signal is a fair proxy for speech (unlike a pure tone, which is degenerate for MFCCs). Small `noiseAmp`
// stands in for the same speaker on a slightly different take.
QByteArray voiceLike(double f0, double formantHz, double seconds, double amp,
                     double noiseAmp = 0.0, quint32 seed = 7) {
    const int n = int(seconds * kSr);
    QByteArray pcm(n * 2, Qt::Uninitialized);
    auto* out = reinterpret_cast<qint16*>(pcm.data());
    Lcg rng{seed};
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int h = 1; h * f0 < kSr / 2.0; ++h) {
            const double fh = h * f0;
            const double w = std::exp(-0.5 * std::pow((fh - formantHz) / 350.0, 2.0));
            s += w * std::sin(2.0 * kPi * fh * i / kSr);
        }
        s *= amp / 4.0;   // many harmonics summed → scale down
        if (noiseAmp > 0.0) s += noiseAmp * rng.next();
        s = qBound(-1.0, s, 1.0);
        out[i] = qint16(std::lround(s * 32767.0));
    }
    return pcm;
}
}  // namespace

class TstSpeakerEmbedder : public QObject {
    Q_OBJECT
private slots:
    void init() { QTest::failOnWarning(); }
    void deterministic();
    void selfMatchIsHigh();
    void differentTonesLessSimilarThanNoisyCopy();
    void fixedLengthAndFinite();
    void emptyInputSafe();
};

void TstSpeakerEmbedder::deterministic() {
    MfccEmbedder e;
    const QByteArray pcm = tone(220.0, 1.0, 0.6);
    const QVector<float> a = e.embed(pcm, kSr);
    const QVector<float> b = e.embed(pcm, kSr);
    QVERIFY(!a.isEmpty());
    QCOMPARE(a, b);   // same PCM → byte-identical vector (enrollment ↔ match must be comparable)
}

void TstSpeakerEmbedder::selfMatchIsHigh() {
    MfccEmbedder e;
    const QVector<float> v = e.embed(tone(220.0, 1.0, 0.6), kSr);
    QVERIFY(!v.isEmpty());
    QVERIFY(cosineSimilarity(v, v) > 0.999f);
}

void TstSpeakerEmbedder::differentTonesLessSimilarThanNoisyCopy() {
    MfccEmbedder e;
    // Two distinct "speakers": different pitch AND formant. Same-speaker = speaker A with a little noise.
    const QVector<float> a      = e.embed(voiceLike(120.0, 700.0,  1.0, 0.9), kSr);
    const QVector<float> b      = e.embed(voiceLike(190.0, 1700.0, 1.0, 0.9), kSr);
    const QVector<float> aNoisy = e.embed(voiceLike(120.0, 700.0,  1.0, 0.9, 0.01), kSr);
    QVERIFY(!a.isEmpty() && !b.isEmpty() && !aNoisy.isEmpty());
    const float simSame = cosineSimilarity(a, aNoisy);   // same speaker, different take
    const float simDiff = cosineSimilarity(a, b);        // different speaker
    QVERIFY2(simSame > simDiff,
             qPrintable(QStringLiteral("expected same-speaker sim %1 > different-speaker sim %2")
                        .arg(simSame).arg(simDiff)));
}

void TstSpeakerEmbedder::fixedLengthAndFinite() {
    MfccEmbedder e;
    const QVector<float> v = e.embed(tone(300.0, 1.0, 0.5), kSr);
    QCOMPARE(v.size(), MfccEmbedder::kDims);
    for (float f : v)
        QVERIFY(std::isfinite(f));
}

void TstSpeakerEmbedder::emptyInputSafe() {
    MfccEmbedder e;
    QVERIFY(e.embed(QByteArray(), kSr).isEmpty());
    QVERIFY(e.embed(tone(200.0, 0.001, 0.5), kSr).isEmpty());   // shorter than one frame
    QCOMPARE(cosineSimilarity(QVector<float>(), QVector<float>()), 0.0f);
}

QTEST_MAIN(TstSpeakerEmbedder)
#include "tst_speakerembedder.moc"
