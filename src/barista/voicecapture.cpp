#include "voicecapture.h"
#include "baristadiagnostics.h"

#include <QAudioSource>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QIODevice>
#include <QTimer>
#include <cmath>

VoiceCapture::VoiceCapture(QObject* parent) : QObject(parent) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &VoiceCapture::stopCapture);
}

VoiceCapture::~VoiceCapture() {
    if (m_source)
        finish();
}

void VoiceCapture::startCapture(int maxMs, const QString& label) {
    if (m_capturing)
        return;
    m_label = label;
    m_buffer.clear();

    const QAudioDevice dev = QMediaDevices::defaultAudioInput();
    if (dev.isNull()) {
        BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("capture_no_input_device"),
                                   {{QStringLiteral("label"), label}});
        emit captured(QByteArray(), kSampleRate, label);   // never leave a caller hanging
        return;
    }

    QAudioFormat fmt;
    fmt.setSampleRate(kSampleRate);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);
    if (!dev.isFormatSupported(fmt))
        fmt = dev.preferredFormat();   // fall back; embed() reads whatever rate we report on stop

    m_source = new QAudioSource(dev, fmt, this);
    m_io = m_source->start();   // pull mode: read from the returned QIODevice on readyRead
    if (!m_io) {
        BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("capture_open_failed"),
                                   {{QStringLiteral("label"), label},
                                    {QStringLiteral("state"), int(m_source->state())}});
        delete m_source; m_source = nullptr;
        emit captured(QByteArray(), kSampleRate, label);
        return;
    }
    connect(m_io, &QIODevice::readyRead, this, &VoiceCapture::onReadyRead);
    m_capturing = true;
    emit capturingChanged();
    if (maxMs > 0)
        m_timer->start(maxMs);
    BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("capture_open"),
                               {{QStringLiteral("label"), label},
                                {QStringLiteral("state"), int(m_source->state())}});
}

void VoiceCapture::onReadyRead() {
    if (m_io)
        m_buffer.append(m_io->readAll());
}

void VoiceCapture::stopCapture() {
    if (!m_capturing)
        return;
    finish();
}

void VoiceCapture::finish() {
    m_timer->stop();
    if (m_io) {
        m_buffer.append(m_io->readAll());   // drain the tail
        disconnect(m_io, nullptr, this, nullptr);
        m_io = nullptr;
    }
    const int actualRate = m_source ? m_source->format().sampleRate() : kSampleRate;
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }

    // RMS over int16 samples so a dead / muted mic (all-zero, RMS≈0) is visible in the log.
    double rms = 0.0;
    const int n = m_buffer.size() / 2;
    if (n > 0) {
        const auto* s = reinterpret_cast<const qint16*>(m_buffer.constData());
        double acc = 0.0;
        for (int i = 0; i < n; ++i) acc += double(s[i]) * s[i];
        rms = std::sqrt(acc / n);
    }
    BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("capture_closed"),
                               {{QStringLiteral("label"), m_label},
                                {QStringLiteral("bytes"), m_buffer.size()},
                                {QStringLiteral("rms"), QString::number(rms, 'f', 1)}});

    const QByteArray pcm = m_buffer;
    m_buffer.clear();
    m_capturing = false;
    emit capturingChanged();
    emit captured(pcm, actualRate, m_label);
}
