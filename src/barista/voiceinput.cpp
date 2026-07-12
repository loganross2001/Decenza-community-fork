#include "voiceinput.h"
#include "baristadiagnostics.h"  // [barista-fork] STT / mic timeline recorder

#include <QCoreApplication>
#include <QDateTime>

namespace {
// [barista-fork] After the barista's TTS ends and the mic resumes, drop any finalText for this long so
// the recogniser doesn't hear the tail of its own speech (or flush a stale buffered result) as a new turn.
constexpr qint64 kPostTtsIgnoreMs = 400;
// [barista-fork] Cap consecutive transient-error auto-restarts so a client/timeout/no-match/busy storm
// can't tight-loop the recogniser. Reset to 0 on any genuine final result (handleFinal).
constexpr int kMaxTransientRestarts = 3;
} // namespace

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJniEnvironment>
#include <QPointer>
#include <QPermissions>   // QMicrophonePermission + QPermission live here (no per-class header)

namespace {
constexpr const char* kSpeechClass = "io/github/kulitorum/decenza_de1/DecenzaSpeech";

// One VoiceInput at a time; JNI callbacks route back to it (on the main thread).
QPointer<VoiceInput> s_active;

QString jstringToQString(JNIEnv* env, jstring s) {
    if (!s) return QString();
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (!chars) return QString();
    QString out = QString::fromUtf8(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

// SpeechRecognizer callbacks fire on the Android main thread; hop to the Qt main thread (via qApp)
// because they touch QObject state + emit signals that drive QML.
void nativeOnFinal(JNIEnv* env, jclass, jstring textJ) {
    const QString text = jstringToQString(env, textJ);
    QMetaObject::invokeMethod(qApp, [text] {
        if (auto* v = s_active.data()) v->handleFinal(text);
    }, Qt::QueuedConnection);
}
void nativeOnPartial(JNIEnv* env, jclass, jstring textJ) {
    const QString text = jstringToQString(env, textJ);
    QMetaObject::invokeMethod(qApp, [text] {
        if (auto* v = s_active.data()) v->handlePartial(text);
    }, Qt::QueuedConnection);
}
void nativeOnError(JNIEnv*, jclass, jint code) {
    const int c = static_cast<int>(code);
    QMetaObject::invokeMethod(qApp, [c] {
        if (auto* v = s_active.data()) v->handleError(c);
    }, Qt::QueuedConnection);
}

void registerVoiceNatives() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    QJniEnvironment env;
    const JNINativeMethod methods[] = {
        {"nativeOnFinal",   "(Ljava/lang/String;)V", reinterpret_cast<void*>(nativeOnFinal)},
        {"nativeOnPartial", "(Ljava/lang/String;)V", reinterpret_cast<void*>(nativeOnPartial)},
        {"nativeOnError",   "(I)V",                  reinterpret_cast<void*>(nativeOnError)},
    };
    if (!env.registerNativeMethods(kSpeechClass, methods, 3))
        qWarning() << "VoiceInput: failed to register DecenzaSpeech native methods";
}
} // namespace
#endif // Q_OS_ANDROID

VoiceInput::VoiceInput(QObject* parent)
    : QObject(parent) {
#ifdef Q_OS_ANDROID
    s_active = this;   // native registration is done lazily in startRecogniser() (class is loaded by then)
#endif
}

VoiceInput::~VoiceInput() {
    stopRecogniser();
}

bool VoiceInput::available() const {
#ifdef Q_OS_ANDROID
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid()) return false;
    return QJniObject::callStaticMethod<jboolean>(
        "android/speech/SpeechRecognizer", "isRecognitionAvailable",
        "(Landroid/content/Context;)Z", ctx.object());
#else
    return false;   // no on-device STT on desktop; a cloud engine could fill this later
#endif
}

void VoiceInput::start() {
    const bool wasPaused = m_paused;
    m_paused = false;
    m_errorStreak = 0;
    if (wasPaused) emit pausedChanged();
#ifdef Q_OS_ANDROID
    // Ask for the microphone once, then open the session. Nothing listens until this is granted.
    QMicrophonePermission mic;
    const Qt::PermissionStatus status = qApp->checkPermission(mic);
    if (status == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(mic, this, [this](const QPermission& p) {
            if (p.status() == Qt::PermissionStatus::Granted) {
                setListening(true);
                startRecogniser();
            } else {
                emit error(QStringLiteral("microphone permission denied"));
            }
        });
        return;
    }
    if (status == Qt::PermissionStatus::Denied) {
        emit error(QStringLiteral("microphone permission denied"));
        return;
    }
#endif
    setListening(true);
    startRecogniser();
}

void VoiceInput::stop() {
    const bool wasPaused = m_paused;
    setListening(false);
    m_paused = false;
    if (wasPaused) emit pausedChanged();
    setPartial(QString());
    stopRecogniser();
}

void VoiceInput::pauseMic() {
    if (!m_listening || m_paused)
        return;
    m_paused = true;
    m_errorStreak = 0;  // S10: the cancel here can raise ERROR_CLIENT(5) — don't let it count toward the kill
    BaristaDiagnostics::record(QStringLiteral("mic"), QStringLiteral("pause"));
    emit pausedChanged();
    stopRecogniser();   // stop hearing while the assistant speaks (no echo)
}

void VoiceInput::resumeMic() {
    if (!m_listening || !m_paused)
        return;
    m_paused = false;
    m_errorStreak = 0;  // fresh listen after a pause — reset the transient-error run
    // [barista-fork] Post-TTS echo guard: the barista JUST finished speaking. Ignore any finalText for a
    // short window so the recogniser can't transcribe the tail of that speech (or deliver a stale result
    // buffered while paused) as if it were a new user turn — which is what feeds the listen/hear loop.
    m_ignoreFinalUntilMs = QDateTime::currentMSecsSinceEpoch() + kPostTtsIgnoreMs;
    BaristaDiagnostics::record(QStringLiteral("mic"), QStringLiteral("resume"),
        {{QStringLiteral("echoGuardMs"), kPostTtsIgnoreMs}});
    emit pausedChanged();
    startRecogniser();
}

void VoiceInput::startRecogniser() {
    if (!m_listening || m_paused)
        return;
#ifdef Q_OS_ANDROID
    registerVoiceNatives();   // once; the DecenzaSpeech class is loadable by now
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (ctx.isValid())
        QJniObject::callStaticMethod<void>(kSpeechClass, "start",
            "(Landroid/content/Context;Z)V", ctx.object(), static_cast<jboolean>(m_preferOffline));
#endif
}

void VoiceInput::stopRecogniser() {
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>(kSpeechClass, "stop", "()V");
#endif
}

void VoiceInput::handleFinal(const QString& text) {
    // [barista-fork] Post-TTS echo guard (event-based, cleared by the wall clock): if we're still inside
    // the window resumeMic() opened when the barista stopped speaking, this "result" is almost certainly
    // the tail of the barista's own speech or a stale result buffered while the mic was paused — drop it so
    // it doesn't become a new turn (the listen/hear loop). Clear the partial and let listening continue.
    if (m_ignoreFinalUntilMs != 0) {
        if (QDateTime::currentMSecsSinceEpoch() < m_ignoreFinalUntilMs) {
            // [barista-fork][diag] Dropped inside the post-TTS echo window (self-hearing guard).
            BaristaDiagnostics::record(QStringLiteral("stt"), QStringLiteral("final_dropped_echo_window"),
                {{QStringLiteral("heard"), text.trimmed().left(80)},
                 {QStringLiteral("action"), QStringLiteral("restart")}});
            setPartial(QString());
            // [barista-fork] The recogniser's single-utterance session ENDED when this result fired. Since we're
            // dropping it (not starting a turn), nobody else restarts it → dead mic. Restart to keep listening.
            // (Bounded: the echo window is ~400ms, so at most a couple of tail drops before it elapses.)
            startRecogniser();
            return;   // NOTE: don't touch m_errorStreak — this isn't a genuine user result
        }
        m_ignoreFinalUntilMs = 0;   // window elapsed → back to normal
    }
    m_errorStreak = 0;   // a real result → the recogniser is healthy
    setPartial(QString());
    const QString t = text.trimmed();
    if (!t.isEmpty()) {
        BaristaDiagnostics::record(QStringLiteral("stt"), QStringLiteral("final_result"),
            {{QStringLiteral("heard"), t.left(120)}});
        emit finalText(t);
        // Do NOT auto-restart here. The overlay pauses the mic while the assistant thinks/speaks and calls
        // resumeMic() when the turn is done — restarting into that pending stop is what triggers ERROR_CLIENT (5).
    } else {
        // [barista-fork] Empty final (recogniser gave up with no words): the single-utterance session ENDED but
        // no turn starts, so nothing ever calls resumeMic() → dead mic. Restart to keep listening (no finalText,
        // so no turn is dispatched — this is the "listening continues" case, unlike a real result).
        BaristaDiagnostics::record(QStringLiteral("stt"), QStringLiteral("final_empty_restart"));
        startRecogniser();
    }
}

void VoiceInput::handlePartial(const QString& text) {
    setPartial(text);
}

void VoiceInput::handleError(int code) {
    // Language unavailable (12) / not supported (13): the on-device model isn't present. Under the
    // pragmatic privacy posture, retry with ONLINE recognition rather than failing silently.
    if ((code == 12 || code == 13) && m_preferOffline) {
        m_preferOffline = false;
        // [barista-fork][diag] recogniser errors were previously INVISIBLE (nothing logged) — record every path.
        BaristaDiagnostics::record(QStringLiteral("stt"), QStringLiteral("error"),
            {{QStringLiteral("code"), code}, {QStringLiteral("action"), QStringLiteral("offline_retry")}});
        startRecogniser();
        return;
    }
    // Transient (5=client, 6=timeout, 7=no-match, 8=busy): silence / overlap / benign client hiccup —
    // keep listening quietly (no pop-up), but back off after a short run so a storm can't tight-loop.
    // The cap is small (kMaxTransientRestarts): a genuine final result resets m_errorStreak to 0
    // (handleFinal), so normal listening across utterances is unaffected — only an unbroken run of
    // errors with no real result in between can exhaust it and end the session.
    if (code == 5 || code == 6 || code == 7 || code == 8) {
        if (++m_errorStreak <= kMaxTransientRestarts) {
            BaristaDiagnostics::record(QStringLiteral("stt"), QStringLiteral("error"),
                {{QStringLiteral("code"), code}, {QStringLiteral("streak"), m_errorStreak},
                 {QStringLiteral("action"), QStringLiteral("restart")}});
            startRecogniser();
            return;
        }
    }
    // Fatal, or too many in a row — surface it loudly and end the session. (The overlay's onError turns this
    // into a visible message + a distinct mic-off earcon so a mic death isn't silent.)
    BaristaDiagnostics::record(QStringLiteral("stt"), QStringLiteral("error"),
        {{QStringLiteral("code"), code}, {QStringLiteral("streak"), m_errorStreak},
         {QStringLiteral("action"), QStringLiteral("fatal")}});
    emit error(QStringLiteral("code %1").arg(code));
    stop();
}

void VoiceInput::setListening(bool on) {
    if (m_listening == on)
        return;
    m_listening = on;
    emit listeningChanged();
}

void VoiceInput::setPartial(const QString& p) {
    if (m_partial == p)
        return;
    m_partial = p;
    emit partialChanged();
}
