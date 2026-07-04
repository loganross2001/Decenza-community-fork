#pragma once

#include <QObject>
#include <QString>

// [barista-fork] Speech-to-text for the assistant. A listening SESSION is opened by the Chat button
// (start()) and closed by stop() or the overlay's 20s-silence timer. While a session is open the
// engine keeps listening across utterances (restarting itself after each result) so the user can
// talk conversationally. On Android it uses the on-device android.speech.SpeechRecognizer via JNI
// (audio stays on the device — the private default); on desktop it is a no-op stub (available=false).
//
// Deliberately a thin, swappable surface: a cloud engine (e.g. Whisper) can later implement the same
// signals/slots behind a settings switch (the "pragmatic" privacy posture) without touching the UI.
class VoiceInput : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)          // STT usable on this platform
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)   // session open but not hearing (assistant busy)
    Q_PROPERTY(QString partial READ partial NOTIFY partialChanged)

public:
    explicit VoiceInput(QObject* parent = nullptr);
    ~VoiceInput() override;

    bool available() const;
    bool listening() const { return m_listening; }
    bool paused() const { return m_paused; }
    QString partial() const { return m_partial; }

    Q_INVOKABLE void start();       // open the mic (begin a listening session)
    Q_INVOKABLE void stop();        // close the mic (end the session)
    Q_INVOKABLE void pauseMic();    // stop the recogniser but keep the session (e.g. while TTS speaks)
    Q_INVOKABLE void resumeMic();   // resume after pauseMic() if the session is still open

    // Reached from the Android JNI callbacks (already hopped to the main thread).
    void handleFinal(const QString& text);
    void handlePartial(const QString& text);
    void handleError(int code);

signals:
    void listeningChanged();
    void pausedChanged();
    void partialChanged();
    void finalText(const QString& text);   // a complete utterance → send to the conversation
    void error(const QString& message);

private:
    void startRecogniser();   // (re)start the native recogniser if the session is open and not paused
    void stopRecogniser();
    void setListening(bool on);
    void setPartial(const QString& p);

    bool m_listening = false;   // session open (Chat active)
    bool m_paused = false;      // recogniser temporarily stopped (assistant is speaking)
    bool m_preferOffline = true; // try on-device first; drop to online if the model is unavailable
    int m_errorStreak = 0;      // consecutive errors — back off to avoid a restart storm
    QString m_partial;          // live partial transcription
};
