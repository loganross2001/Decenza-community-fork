#pragma once

#include <QObject>
#include <QByteArray>

class QAudioSource;
class QIODevice;
class QTimer;

// [barista-fork] Voice-ID Increment 1 — raw mic capture for enrollment + the concurrent-capture probe.
//
// Wraps QAudioSource at 16 kHz / mono / int16 (the input the SpeakerEmbedder expects) and accumulates PCM into
// a buffer. Separate from the STT path (DecenzaSpeech, which returns TEXT only) — this reads the waveform
// directly. Instrumented via BaristaDiagnostics so a device log shows whether the mic actually opened and got
// non-zero samples (the load-bearing unknown: can a 2nd QAudioSource read WHILE Android SpeechRecognizer holds
// the mic? guaranteed only API 29+, Samsung policy unknown → the owner's log answers it).
class VoiceCapture : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool capturing READ capturing NOTIFY capturingChanged)
public:
    explicit VoiceCapture(QObject* parent = nullptr);
    ~VoiceCapture() override;

    static constexpr int kSampleRate = 16000;

    bool capturing() const { return m_capturing; }

    // Start capturing. Auto-stops after `maxMs` (0 = no auto-stop). `label` tags the diagnostic events
    // (e.g. "enroll" vs "probe") so the log distinguishes them.
    Q_INVOKABLE void startCapture(int maxMs = 12000, const QString& label = QStringLiteral("capture"));
    Q_INVOKABLE void stopCapture();

signals:
    void capturingChanged();
    // Emitted once on stop with the accumulated PCM (16 kHz int16 mono LE) + its sample rate. `pcm` is empty
    // if the mic never opened / produced nothing.
    void captured(const QByteArray& pcm, int sampleRate, const QString& label);

private:
    void onReadyRead();
    void finish();   // stop the source, compute RMS, emit + record, reset

    QAudioSource* m_source = nullptr;
    QIODevice* m_io = nullptr;     // pull-mode device owned by m_source
    QTimer* m_timer = nullptr;     // max-duration auto-stop
    QByteArray m_buffer;
    QString m_label;
    bool m_capturing = false;
};
