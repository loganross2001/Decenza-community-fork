#include "voiceinput.h"

#include <QCoreApplication>

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
    emit pausedChanged();
    stopRecogniser();   // stop hearing while the assistant speaks (no echo)
}

void VoiceInput::resumeMic() {
    if (!m_listening || !m_paused)
        return;
    m_paused = false;
    m_errorStreak = 0;  // fresh listen after a pause — reset the transient-error run
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
    m_errorStreak = 0;   // a real result → the recogniser is healthy
    setPartial(QString());
    const QString t = text.trimmed();
    if (!t.isEmpty())
        emit finalText(t);
    // Do NOT auto-restart here. The overlay pauses the mic while the assistant thinks/speaks and calls
    // resumeMic() when the turn is done — restarting into that pending stop is what triggers ERROR_CLIENT (5).
}

void VoiceInput::handlePartial(const QString& text) {
    setPartial(text);
}

void VoiceInput::handleError(int code) {
    // Language unavailable (12) / not supported (13): the on-device model isn't present. Under the
    // pragmatic privacy posture, retry with ONLINE recognition rather than failing silently.
    if ((code == 12 || code == 13) && m_preferOffline) {
        m_preferOffline = false;
        startRecogniser();
        return;
    }
    // Transient (5=client, 6=timeout, 7=no-match, 8=busy): silence / overlap / benign client hiccup —
    // keep listening quietly (no pop-up), but back off after a run so a storm can't tight-loop.
    if (code == 5 || code == 6 || code == 7 || code == 8) {
        if (++m_errorStreak <= 6) {
            startRecogniser();
            return;
        }
    }
    // Fatal, or too many in a row — surface it loudly and end the session.
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
