#include "baristavoiceid.h"
#include "voicecapture.h"
#include "voiceprintstore.h"
#include "baristadiagnostics.h"

#include <QPointer>
#include <cmath>

BaristaVoiceId::BaristaVoiceId(QObject* parent) : QObject(parent) {
    m_capture = new VoiceCapture(this);
    m_store = new VoiceprintStore(this);
    connect(m_capture, &VoiceCapture::captured, this, &BaristaVoiceId::onCaptured);
}

void BaristaVoiceId::initialize(const QString& voiceprintsDbPath) {
    m_store->initialize(voiceprintsDbPath);
    refreshEnrolled();
}

void BaristaVoiceId::setActiveUserProvider(std::function<QString()> provider) {
    m_activeUserProvider = std::move(provider);
    emit activeUserChanged();
}

QString BaristaVoiceId::activeUser() const {
    return m_activeUserProvider ? m_activeUserProvider() : QString();
}

void BaristaVoiceId::setStatus(const QString& s) {
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

void BaristaVoiceId::setRecording(bool r) {
    if (m_recording == r) return;
    m_recording = r;
    emit recordingChanged();
}

void BaristaVoiceId::enrollActiveUser() {
    if (m_recording)
        return;
    const QString name = activeUser().trimmed();
    if (name.isEmpty()) {
        setStatus(tr("Tell me who you are first — say \"I'm <your name>\" — then enroll."));
        return;
    }
    m_enrollingName = name;
    setRecording(true);
    setStatus(tr("Listening to %1 — keep talking for about ten seconds…").arg(name));
    BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("enroll_start"),
                               {{QStringLiteral("name"), name}});
    m_capture->startCapture(10000, QStringLiteral("enroll"));
}

void BaristaVoiceId::cancelEnroll() {
    if (!m_recording)
        return;
    m_enrollingName.clear();   // onCaptured sees an empty enrolling name → treats it as cancelled
    m_capture->stopCapture();
}

void BaristaVoiceId::onCaptured(const QByteArray& pcm, int sampleRate, const QString& label) {
    if (label == QLatin1String("probe")) {
        m_probeInFlight = false;
        BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("concurrent_capture_probe"),
                                   {{QStringLiteral("opened"), !pcm.isEmpty()},
                                    {QStringLiteral("bytes"), pcm.size()}});
        return;   // Increment 1 discards probe audio (no matching yet)
    }

    if (label == QLatin1String("identify")) {
        m_identifyInFlight = false;
        identify(pcm, sampleRate);
        return;
    }

    // [barista-fork] V1 validator — a short on-demand clip: log the match (testOnly → no switch, no hint).
    if (label == QLatin1String("engage_test")) {
        m_identifyInFlight = false;
        identify(pcm, sampleRate, /*testOnly=*/true);
        return;
    }

    // [barista-fork] V2 validator — the engage capture-then-recognizer health test: record the capture, then
    // ALWAYS signal so the overlay opens the STT mic (even on empty/failed capture — never wedge the session).
    if (label == QLatin1String("engage_v2")) {
        m_identifyInFlight = false;
        double rms = 0.0;
        const int n = static_cast<int>(pcm.size() / 2);
        if (n > 0) {
            const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
            double acc = 0.0;
            for (int i = 0; i < n; ++i) acc += double(s[i]) * s[i];
            rms = std::sqrt(acc / n);
        }
        const int durationMs = sampleRate > 0 ? (n * 1000) / sampleRate : 0;
        BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("engage_capture_done"),
                                   {{QStringLiteral("bytes"), pcm.size()},
                                    {QStringLiteral("rms"), QString::number(rms, 'f', 1)},
                                    {QStringLiteral("durationMs"), durationMs}});
        emit engageCaptureTestDone();
        return;
    }

    // enrollment
    setRecording(false);
    const QString name = m_enrollingName;
    m_enrollingName.clear();
    if (name.isEmpty()) {                 // cancelled
        setStatus(tr("Enrollment cancelled."));
        return;
    }
    if (pcm.isEmpty()) {
        setStatus(tr("The mic didn't pick anything up — check it's not muted and try again."));
        return;
    }
    const QVector<float> vec = m_embedder.embed(pcm, sampleRate);
    if (vec.isEmpty()) {
        setStatus(tr("Didn't catch enough speech — try again and keep talking."));
        return;
    }
    setStatus(tr("Saving %1's voice…").arg(name));
    QPointer<BaristaVoiceId> self(this);
    m_store->upsert(name, vec, m_embedder.name(), [self, name](bool ok) {
        if (!self) return;
        self->setStatus(ok ? tr("Got it — I'll recognize %1's voice.").arg(name)
                           : tr("Couldn't save that voiceprint — please try again."));
        self->refreshEnrolled();
    });
}

void BaristaVoiceId::deleteVoiceprint(const QString& name) {
    QPointer<BaristaVoiceId> self(this);
    m_store->remove(name, [self, name](bool ok) {
        if (!self) return;
        if (ok) self->setStatus(tr("Removed %1's voiceprint.").arg(name));
        self->refreshEnrolled();
    });
}

void BaristaVoiceId::refreshEnrolled() {
    QPointer<BaristaVoiceId> self(this);
    m_store->loadAll([self](QVector<VoiceprintStore::Voiceprint> list) {
        if (!self) return;
        self->m_prints = list;   // [barista-fork] Increment 2: cache vectors so matching is synchronous on stop
        QStringList names;
        for (const auto& vp : list) names << vp.name;
        if (self->m_enrolledNames != names) {
            self->m_enrolledNames = names;
            emit self->enrolledNamesChanged();
        }
    });
}

void BaristaVoiceId::setActiveUserSeam(std::function<void(const QString&)> seam) {
    m_setActiveUserSeam = std::move(seam);
}

// [barista-fork] Owner-tunable match thresholds (from AssistantSettings). Clamp to sane cosine ranges.
void BaristaVoiceId::setThresholds(double confident, double margin, double maybe) {
    m_confidentScore  = static_cast<float>(qBound(0.30, confident, 0.99));
    m_confidentMargin = static_cast<float>(qBound(0.00, margin,    0.50));
    m_maybeScore      = static_cast<float>(qBound(0.20, maybe,     0.98));
}

void BaristaVoiceId::consumeHeard() {
    if (m_heardConfidence == QLatin1String("none") && m_heardName.isEmpty())
        return;
    m_heardName.clear();
    m_heardConfidence = QStringLiteral("none");
    emit heardChanged();
}

// [barista-fork] Increment 2 — capture the actual utterance for identification. Started when the mic opens for
// a barista turn, stopped at the final STT result. Guarded: no-op if there's nothing enrolled or a capture is
// already busy (enroll/probe/identify share one VoiceCapture).
void BaristaVoiceId::startUtteranceCapture() {
    if (m_prints.isEmpty() || m_recording || m_probeInFlight || m_identifyInFlight)
        return;
    m_identifyInFlight = true;
    m_capture->startCapture(6000, QStringLiteral("identify"));   // ~6s cap so a long turn can't grow unbounded
}

void BaristaVoiceId::stopUtteranceCaptureAndIdentify() {
    if (!m_identifyInFlight)
        return;
    m_capture->stopCapture();   // synchronous → onCaptured("identify") → identify() before the caller's _send
}

void BaristaVoiceId::runProbe() {
    if (m_recording || m_probeInFlight || m_identifyInFlight)
        return;   // never fight enrollment / identify / double-probe
    m_probeInFlight = true;
    m_capture->startCapture(1500, QStringLiteral("probe"));   // ~1.5s, discarded on capture
}

// [barista-fork] V1 validator: a ~2s capture from the settings panel (no STT running → no contention) that logs
// the match without switching. For the owner to prove a short clip still matches before the hail UX is built.
void BaristaVoiceId::testShortIdentify() {
    if (m_prints.isEmpty()) {
        setStatus(tr("Enroll a voice first, then test."));
        return;
    }
    if (m_recording || m_probeInFlight || m_identifyInFlight)
        return;
    m_identifyInFlight = true;
    setStatus(tr("Listening for two seconds…"));
    m_capture->startCapture(2000, QStringLiteral("engage_test"));
}

// [barista-fork] V2 validator: a ~2.5s capture at engage, BEFORE the STT mic opens (the anti-probe sequencing
// test). Always emits engageCaptureTestDone() so the overlay opens the mic even if capture is busy/fails.
void BaristaVoiceId::startEngageCaptureTest() {
    if (m_recording || m_probeInFlight || m_identifyInFlight) {
        emit engageCaptureTestDone();   // busy → don't wedge; let the overlay open the mic normally
        return;
    }
    m_identifyInFlight = true;
    m_capture->startCapture(2500, QStringLiteral("engage_v2"));
}

// [barista-fork] Increment 2 decision bands — deliberately CONSERVATIVE to start, and now owner-tunable via
// AssistantSettings (m_confidentScore/m_confidentMargin/m_maybeScore, defaults 0.72/0.06/0.55). Tune from the
// owner's real `match` logs (a wrong confident switch is worse than a missed one — Phase-1 attribution rides on it).
void BaristaVoiceId::identify(const QByteArray& pcm, int sampleRate, bool testOnly) {
    // Capture-quality diagnostic (RMS so a silent/muted turn is visible; duration to spot too-short clips).
    double rms = 0.0;
    const int n = static_cast<int>(pcm.size() / 2);
    if (n > 0) {
        const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
        double acc = 0.0;
        for (int i = 0; i < n; ++i) acc += double(s[i]) * s[i];
        rms = std::sqrt(acc / n);
    }
    const int durationMs = sampleRate > 0 ? (n * 1000) / sampleRate : 0;
    BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("identify_capture"),
                               {{QStringLiteral("bytes"), pcm.size()},
                                {QStringLiteral("rms"), QString::number(rms, 'f', 1)},
                                {QStringLiteral("durationMs"), durationMs}});

    const QString active = activeUser().trimmed();
    if (pcm.isEmpty() || m_prints.isEmpty()) {
        consumeHeard();
        return;
    }
    const QVector<float> vec = m_embedder.embed(pcm, sampleRate);
    if (vec.isEmpty()) {
        BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("match"),
                                   {{QStringLiteral("decision"), QStringLiteral("none_no_speech")},
                                    {QStringLiteral("active"), active}});
        consumeHeard();
        return;
    }

    // Cosine vs every enrolled print → best + second-best (margin guards against two similar voices).
    QString bestName, secondName;
    float bestScore = -2.0f, secondScore = -2.0f;
    for (const auto& vp : m_prints) {
        const float sim = cosineSimilarity(vec, vp.vector);
        if (sim > bestScore) {
            secondScore = bestScore; secondName = bestName;
            bestScore = sim; bestName = vp.name;
        } else if (sim > secondScore) {
            secondScore = sim; secondName = vp.name;
        }
    }
    const float margin = (secondScore < -1.0f) ? bestScore : (bestScore - secondScore);

    QString decision;
    if (bestScore >= m_confidentScore && margin >= m_confidentMargin) decision = QStringLiteral("confident");
    else if (bestScore >= m_maybeScore)                               decision = QStringLiteral("maybe");
    else                                                              decision = QStringLiteral("none");

    BaristaDiagnostics::record(QStringLiteral("voiceid"), QStringLiteral("match"),
        {{QStringLiteral("best"), bestName},
         {QStringLiteral("score"), QString::number(bestScore, 'f', 3)},
         {QStringLiteral("margin"), QString::number(margin, 'f', 3)},
         {QStringLiteral("second"), secondName},
         {QStringLiteral("secondScore"), QString::number(secondScore < -1.0f ? 0.0f : secondScore, 'f', 3)},
         {QStringLiteral("decision"), decision},
         {QStringLiteral("active"), active}});

    // [barista-fork] V1 validator: log + show the score, but NEVER expose a hint or switch the active user.
    if (testOnly) {
        setStatus(tr("Match: %1 — %2 (score %3, margin %4)")
                      .arg(bestName, decision,
                           QString::number(bestScore, 'f', 2), QString::number(margin, 'f', 2)));
        return;
    }

    // Expose the hint for the model (one-shot, consumed by the overlay when it builds the turn).
    if (decision == QLatin1String("none")) {
        m_heardName.clear();
        m_heardConfidence = QStringLiteral("none");
    } else {
        m_heardName = bestName;
        m_heardConfidence = decision;   // "confident" | "maybe"
    }
    emit heardChanged();

    // CONFIDENT + a different person → app-driven switch via the Phase-1 seam (the model is told via the hint).
    // MAYBE never switches on its own — the model confirms first. Already-active confident → no-op.
    if (decision == QLatin1String("confident") && m_setActiveUserSeam
            && bestName.compare(active, Qt::CaseInsensitive) != 0) {
        m_setActiveUserSeam(bestName);
    }
}
