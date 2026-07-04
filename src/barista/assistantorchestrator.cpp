#include "assistantorchestrator.h"

#include "assistantsettings.h"
#include "../controllers/maincontroller.h"
#include "../history/shothistorystorage.h"

AssistantOrchestrator::AssistantOrchestrator(MainController* mainController, MachineState* machineState,
                                             AssistantSettings* settings, QObject* parent)
    : QObject(parent)
    , m_mainController(mainController)
    , m_machineState(machineState)
    , m_settings(settings) {
    if (m_mainController && m_mainController->shotHistory()) {
        connect(m_mainController->shotHistory(), &ShotHistoryStorage::shotSaved,
                this, &AssistantOrchestrator::onShotSaved);
    }
}

QString AssistantOrchestrator::stateString() const {
    switch (m_state) {
    case State::Greeting: return QStringLiteral("greeting");
    case State::CloseOut: return QStringLiteral("closeOut");
    case State::Dormant:
    default:              return QStringLiteral("dormant");
    }
}

void AssistantOrchestrator::wake() {
    if (!m_settings || !m_settings->enabled())
        return;
    if (m_state == State::Greeting)   // already greeting — don't restart it
        return;
    // Preempt a stale, undismissed close-out (B1): otherwise the state wedges in CloseOut and greetings
    // stop working until the user taps ×. A fresh Espresso tap always means "greet me".
    setState(State::Greeting);
}

void AssistantOrchestrator::onShotSaved(qlonglong shotId) {
    // A shot just finished — open the taste close-out conversation. The card renders on the idle
    // page, so it appears once the user is back on the home screen.
    if (shotId <= 0 || !m_settings || !m_settings->enabled())
        return;
    m_lastShotId = shotId;
    emit lastShotIdChanged();
    // BL-R3-1: a second shot pulled without dismissing the card leaves state already CloseOut, so a plain
    // setState() would no-op and the new shot would get NO close-out. Force reactivation: clear the
    // session latch and re-emit stateChanged even when the state is unchanged.
    if (m_sessionStarted) {
        m_sessionStarted = false;
        emit sessionStartedChanged();
    }
    if (m_state == State::CloseOut)
        emit stateChanged();
    else
        setState(State::CloseOut);
}

void AssistantOrchestrator::dismiss() {
    setState(State::Dormant);
}

void AssistantOrchestrator::markSessionStarted() {
    if (m_sessionStarted)
        return;
    m_sessionStarted = true;
    emit sessionStartedChanged();
}

void AssistantOrchestrator::setState(State s) {
    if (m_state == s)
        return;
    m_state = s;
    if (m_sessionStarted) {   // a new activation → the overlay may start fresh (SF-3)
        m_sessionStarted = false;
        emit sessionStartedChanged();
    }
    emit stateChanged();
}
