#include <QtTest>
#include <QSignalSpy>

#include "barista/baristaconversation.h"

using State = BaristaConversation::State;

// [barista-fork] Headless unit tests for the barista voice-conversation state machine (the two-way-comms
// redesign controller, BARISTA_TwoWay_Comms_Redesign.md §13). This is the code that has been "on-device only"
// all along — it drives the live voice conversation and lives one layer under the unlinted QML overlay. But the
// controller borrows its actuators (AssistantVoice / VoiceInput) and null-guards every call to them, and its
// inputs are the public transition-table slots (onFinalText / onModelFinal / onCloseRequested / …), so it can
// be constructed with NULL actuators and stepped exactly the way the design's transition table describes —
// asserting state, micLive, and the outbound signals (turnRequested / closingConfirmed / continuationRequested)
// with no mic, no TTS, no network, no device.
//
// Coverage centres on the two bugs the redesign exists to kill by construction: "won't close on a farewell"
// and "the mic loops / stall drops to Listening with nothing said". Both are pinned here.
//
// One seam worth naming: with a null voice, AssistantVoice::speakingChanged never fires, so the Speaking→…
// drain can't arrive on its own. tap() while Speaking is the tap-to-skip barge-in path and calls the same
// onVoiceSpeakingChanged() dispatch — so the tests use tap() to stand in for "the reply finished speaking".
class TestBaristaConversation : public QObject {
    Q_OBJECT

private:
    // The common entry: engage, context assembled → Listening (mic hot, since the gate is quiet with no TTS).
    static void toListening(BaristaConversation& c) {
        c.tap();               // Idle → Priming
        c.onContextReady();    // Priming → Listening
    }

private slots:
    void startsIdle() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        QCOMPARE(c.state(), State::Idle);
        QVERIFY(!c.active());
        QCOMPARE(c.stateName(), QStringLiteral("idle"));
        QVERIFY(!c.micLive());
    }

    // tap() from Idle engages: → Priming, requests context, becomes active.
    void engageRequestsContext() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        QSignalSpy ctx(&c, &BaristaConversation::contextRequested);
        QSignalSpy activeSpy(&c, &BaristaConversation::activeChanged);
        c.tap();
        QCOMPARE(c.state(), State::Priming);
        QVERIFY(c.active());
        QCOMPARE(ctx.count(), 1);
        QCOMPARE(activeSpy.count(), 1);
    }

    void contextReadyOpensListeningAndMic() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        c.tap();
        c.onContextReady();
        QCOMPARE(c.state(), State::Listening);
        QVERIFY2(c.micLive(), "mic is hot in Listening when the speaker gate is quiet");
    }

    // onContextReady is inert outside Priming (a late/duplicate context callback can't jump the machine).
    void contextReadyIgnoredWhenNotPriming() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        c.onContextReady();
        QCOMPARE(c.state(), State::Idle);
    }

    // A normal utterance dispatches a model turn: Listening → Thinking, turnRequested carries the text.
    void utteranceStartsTurn() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        QSignalSpy turn(&c, &BaristaConversation::turnRequested);
        c.onFinalText(QStringLiteral("how is my extraction going"));
        QCOMPARE(c.state(), State::Thinking);
        QVERIFY(!c.micLive());   // mic is off outside Listening
        QCOMPARE(turn.count(), 1);
        QCOMPARE(turn.first().first().toString(), QStringLiteral("how is my extraction going"));
    }

    // The mic shouldn't be hot outside Listening, so an utterance arriving in Idle is ignored (no turn).
    void utteranceIgnoredWhenNotListening() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        QSignalSpy turn(&c, &BaristaConversation::turnRequested);
        c.onFinalText(QStringLiteral("hello"));
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(turn.count(), 0);
    }

    // A pure arithmetic question is answered locally and spoken — no model turn dispatched.
    void quickMathSkipsTheModel() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        QSignalSpy turn(&c, &BaristaConversation::turnRequested);
        c.onFinalText(QStringLiteral("1:2 with 18g"));
        QCOMPARE(c.state(), State::Speaking);
        QVERIFY2(c.displayText().contains(QStringLiteral("36 grams out")), qPrintable(c.displayText()));
        QCOMPARE(turn.count(), 0);
    }

    // Bug A, by construction: a farewell arms closing locally, and once the reply finishes the machine reaches
    // Closing deterministically — no dependency on the model calling end_conversation.
    void localCloseIntentClosesAfterReply() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        QSignalSpy closed(&c, &BaristaConversation::closingConfirmed);
        c.onFinalText(QStringLiteral("thanks, that'll be all"));   // looksLikeClose → closingArmed
        QCOMPARE(c.state(), State::Thinking);
        c.onModelFinal(QStringLiteral("Okay, talk soon."), /*endConversation=*/false);
        QCOMPARE(c.state(), State::Speaking);
        c.tap();                                                   // reply finished speaking (tap-to-skip drain)
        QCOMPARE(c.state(), State::Closing);
        QCOMPARE(closed.count(), 1);
    }

    // The forced per-turn close bit reaches Closing even when the local matcher never fired.
    void modelEndConversationFlagCloses() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onFinalText(QStringLiteral("should I go finer"));        // NOT a farewell
        QCOMPARE(c.state(), State::Thinking);
        c.onModelFinal(QStringLiteral("Go a touch finer. Bye now."), /*endConversation=*/true);
        QCOMPARE(c.state(), State::Speaking);
        c.tap();
        QCOMPARE(c.state(), State::Closing);
    }

    // The end_conversation tool from an idling Listening state closes immediately.
    void closeRequestedFromListeningClosesNow() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        QSignalSpy closed(&c, &BaristaConversation::closingConfirmed);
        c.onCloseRequested();
        QCOMPARE(c.state(), State::Closing);
        QCOMPARE(closed.count(), 1);
    }

    void closeRequestedIgnoredWhenIdle() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        c.onCloseRequested();
        QCOMPARE(c.state(), State::Idle);
    }

    // A normal turn plays a lead-in then the answer, and drains back to Listening.
    void normalTurnReturnsToListening() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onFinalText(QStringLiteral("how is my extraction going"));
        c.onModelSpeakable(QStringLiteral("One moment."));         // lead-in
        QCOMPARE(c.state(), State::Speaking);
        c.onModelFinal(QStringLiteral("It looks nicely balanced."), false);
        QCOMPARE(c.state(), State::Speaking);
        c.tap();                                                   // reply finished
        QCOMPARE(c.state(), State::Listening);
        QVERIFY(c.micLive());
    }

    // Bug B, by construction: a bare "let me check on that" is treated as a lead-in — spoken, turn kept in
    // flight, and ONE continuation requested — instead of dropping silently to Listening.
    void stallSpeaksLeadInAndContinues() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onFinalText(QStringLiteral("should I go finer"));
        QSignalSpy cont(&c, &BaristaConversation::continuationRequested);
        c.onModelFinal(QStringLiteral("let me check on that"), false);   // a stall, not an answer
        QCOMPARE(cont.count(), 1);
        QCOMPARE(c.state(), State::Speaking);                             // spoke the lead-in, still working
        // The real answer arrives on the continuation and drains normally.
        c.onModelFinal(QStringLiteral("Yes — go two clicks finer."), false);
        c.tap();
        QCOMPARE(c.state(), State::Listening);
        QCOMPARE(cont.count(), 1);                                        // did not re-fire
    }

    // A fatal STT error surfaces a tappable prompt instead of a silent re-listen.
    void sttErrorGoesToNeedsTap() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onSttError(QStringLiteral("Microphone unavailable"));
        QCOMPARE(c.state(), State::NeedsTap);
        QCOMPARE(c.message(), QStringLiteral("Microphone unavailable"));
        QVERIFY(!c.micLive());
    }

    // Tapping the "tap to talk" prompt resumes Listening and clears the message.
    void tapFromNeedsTapResumesListening() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onSttError(QStringLiteral("Didn't catch that"));
        QCOMPARE(c.state(), State::NeedsTap);
        c.tap();
        QCOMPARE(c.state(), State::Listening);
        QVERIFY(c.message().isEmpty());
    }

    // The "Stop" tap while Listening actually stops the mic (goes to NeedsTap) — the reported no-op bug.
    void tapWhileListeningStops() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        QVERIFY(c.micLive());
        c.tap();
        QCOMPARE(c.state(), State::NeedsTap);
        QVERIFY(!c.micLive());
    }

    // dismiss() tears down from any state.
    void dismissClosesFromThinking() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onFinalText(QStringLiteral("how is my extraction going"));
        QCOMPARE(c.state(), State::Thinking);
        c.dismiss();
        QCOMPARE(c.state(), State::Idle);
        QVERIFY(!c.active());
    }

    // A model error returns to Listening with an actionable message (not a dead end).
    void modelErrorReturnsToListening() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onFinalText(QStringLiteral("how is my extraction going"));
        c.onModelError(QStringLiteral("HTTP 429 rate limit exceeded"));
        QCOMPARE(c.state(), State::Listening);
        QVERIFY2(!c.message().isEmpty(), "a model error must leave an actionable message");
    }

    // A model error during a close turn still tears down (doesn't strand the dock open).
    void modelErrorDuringCloseStillCloses() {
        BaristaConversation c(nullptr, nullptr, nullptr);
        toListening(c);
        c.onFinalText(QStringLiteral("that'll be all"));   // arms closing
        c.onModelError(QStringLiteral("network dropped"));
        QCOMPARE(c.state(), State::Closing);
    }
};

QTEST_GUILESS_MAIN(TestBaristaConversation)
#include "tst_baristaconversation.moc"
