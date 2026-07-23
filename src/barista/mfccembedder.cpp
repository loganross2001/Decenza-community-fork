#include "mfccembedder.h"

#include <QtGlobal>
#include <cmath>
#include <utility>   // std::swap

namespace {
constexpr double kPi = 3.14159265358979323846;

// Hz ↔ mel (O'Shaughnessy / HTK form).
inline double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
inline double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }
}  // namespace

MfccEmbedder::MfccEmbedder() = default;

// Iterative radix-2 Cooley-Tukey FFT (in-place, decimation-in-time). re/im are length kFftSize (power of two).
void MfccEmbedder::fftRadix2(QVector<float>& re, QVector<float>& im) {
    const int n = static_cast<int>(re.size());  // length is kFftSize (power of two), fits int
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    // Butterflies.
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / len;
        const double wRe = std::cos(ang), wIm = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            double curRe = 1.0, curIm = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const double reB = re[b] * curRe - im[b] * curIm;
                const double imB = re[b] * curIm + im[b] * curRe;
                re[b] = static_cast<float>(re[a] - reB);
                im[b] = static_cast<float>(im[a] - imB);
                re[a] = static_cast<float>(re[a] + reB);
                im[a] = static_cast<float>(im[a] + imB);
                const double nextRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nextRe;
            }
        }
    }
}

// 26 triangular mel filters spanning 20 Hz .. Nyquist, over the kFftSize/2+1 power-spectrum bins.
QVector<QVector<float>> MfccEmbedder::melFilterbank(int sampleRate) const {
    const int nBins = kFftSize / 2 + 1;
    const double lowHz = 20.0;
    const double highHz = sampleRate / 2.0;
    const double lowMel = hzToMel(lowHz);
    const double highMel = hzToMel(highHz);

    // kNumFilters+2 equally-spaced mel points → FFT bin indices.
    QVector<int> binPoints(kNumFilters + 2);
    for (int i = 0; i < kNumFilters + 2; ++i) {
        const double mel = lowMel + (highMel - lowMel) * i / (kNumFilters + 1);
        const double hz = melToHz(mel);
        binPoints[i] = static_cast<int>(std::floor((kFftSize + 1) * hz / sampleRate));
        binPoints[i] = qBound(0, binPoints[i], nBins - 1);
    }

    QVector<QVector<float>> fb(kNumFilters, QVector<float>(nBins, 0.0f));
    for (int m = 1; m <= kNumFilters; ++m) {
        const int left = binPoints[m - 1], center = binPoints[m], right = binPoints[m + 1];
        for (int k = left; k < center; ++k)
            if (center > left) fb[m - 1][k] = float(k - left) / float(center - left);
        for (int k = center; k < right; ++k)
            if (right > center) fb[m - 1][k] = float(right - k) / float(right - center);
    }
    return fb;
}

QVector<float> MfccEmbedder::embed(const QByteArray& pcm16leMono, int sampleRate) const {
    if (pcm16leMono.size() < 2 || sampleRate <= 0)
        return {};

    // int16 LE → double samples in [-1, 1].
    const int nSamples = static_cast<int>(pcm16leMono.size() / 2);
    const auto* raw = reinterpret_cast<const qint16*>(pcm16leMono.constData());
    QVector<double> x(nSamples);
    for (int i = 0; i < nSamples; ++i)
        x[i] = raw[i] / 32768.0;

    // Pre-emphasis (0.97): boosts high frequencies, standard MFCC front end.
    for (int i = nSamples - 1; i > 0; --i)
        x[i] -= 0.97 * x[i - 1];

    const int frameLen = qMax(1, static_cast<int>(std::lround(0.025 * sampleRate)));  // 25 ms
    const int hop = qMax(1, static_cast<int>(std::lround(0.010 * sampleRate)));       // 10 ms
    if (nSamples < frameLen)
        return {};

    // Hamming window (precomputed for frameLen).
    QVector<double> window(frameLen);
    for (int i = 0; i < frameLen; ++i)
        window[i] = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (frameLen - 1));

    const QVector<QVector<float>> fb = melFilterbank(sampleRate);
    const int nBins = kFftSize / 2 + 1;

    // Per-frame MFCCs + frame log-energy (for the voiced-frame gate).
    QVector<QVector<double>> frames;   // each = kNumCepstra MFCCs
    QVector<double> frameEnergies;
    frames.reserve(nSamples / hop + 1);

    for (int start = 0; start + frameLen <= nSamples; start += hop) {
        // Windowed frame zero-padded into a kFftSize FFT buffer.
        QVector<float> re(kFftSize, 0.0f), im(kFftSize, 0.0f);
        double energy = 0.0;
        for (int i = 0; i < frameLen; ++i) {
            const double s = x[start + i] * window[i];
            re[i] = static_cast<float>(s);
            energy += s * s;
        }
        frameEnergies.append(energy);

        fftRadix2(re, im);

        // Power spectrum → mel energies → log.
        QVector<double> melLog(kNumFilters);
        for (int m = 0; m < kNumFilters; ++m) {
            double acc = 0.0;
            for (int k = 0; k < nBins; ++k) {
                const double power = static_cast<double>(re[k]) * re[k] + static_cast<double>(im[k]) * im[k];
                acc += power * fb[m][k];
            }
            melLog[m] = std::log(acc + 1e-10);   // floor to avoid log(0)
        }

        // DCT-II → first kNumCepstra cepstra.
        QVector<double> mfcc(kNumCepstra);
        for (int c = 0; c < kNumCepstra; ++c) {
            double acc = 0.0;
            for (int m = 0; m < kNumFilters; ++m)
                acc += melLog[m] * std::cos(kPi * c * (m + 0.5) / kNumFilters);
            mfcc[c] = acc;
        }
        frames.append(mfcc);
    }

    if (frames.isEmpty())
        return {};

    // Voiced-frame gate: keep frames whose energy is within 30 dB of the loudest frame (drops silence/noise
    // floor). Falls back to ALL frames if the gate would leave too few (very short / quiet clip).
    double maxE = 0.0;
    for (double e : frameEnergies) maxE = qMax(maxE, e);
    const double gate = maxE * 1e-3;   // -30 dB
    QVector<int> voiced;
    for (int f = 0; f < frames.size(); ++f)
        if (frameEnergies[f] >= gate) voiced.append(f);
    if (voiced.size() < 3)
        for (int f = 0; f < frames.size(); ++f) voiced.append(f);   // too quiet to gate → use everything

    // Aggregate: per-coefficient mean and (population) stddev across voiced frames.
    QVector<double> mean(kNumCepstra, 0.0), var(kNumCepstra, 0.0);
    for (int f : voiced)
        for (int c = 0; c < kNumCepstra; ++c)
            mean[c] += frames[f][c];
    for (int c = 0; c < kNumCepstra; ++c)
        mean[c] /= voiced.size();
    for (int f : voiced)
        for (int c = 0; c < kNumCepstra; ++c) {
            const double d = frames[f][c] - mean[c];
            var[c] += d * d;
        }
    for (int c = 0; c < kNumCepstra; ++c)
        var[c] /= voiced.size();

    // Concatenate mean ++ stddev → kDims, then L2-normalize.
    QVector<float> vec(kDims);
    for (int c = 0; c < kNumCepstra; ++c) {
        vec[c] = static_cast<float>(mean[c]);
        vec[kNumCepstra + c] = static_cast<float>(std::sqrt(var[c]));
    }
    double norm = 0.0;
    for (float v : vec) norm += static_cast<double>(v) * v;
    norm = std::sqrt(norm);
    if (norm <= 0.0)
        return {};
    for (float& v : vec)
        v = static_cast<float>(v / norm);
    return vec;
}
