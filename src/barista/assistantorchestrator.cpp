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
    if (m_state != State::Dormant)   // don't restart a conversation already in progress
        return;
    setState(State::Greeting);
}

void AssistantOrchestrator::onShotSaved(qlonglong shotId) {
    // A shot just finished — open the taste close-out conversation. The card renders on the idle
    // page, so it appears once the user is back on the home screen.
    if (shotId <= 0 || !m_settings || !m_settings->enabled())
        return;
    m_lastShotId = shotId;
    emit lastShotIdChanged();
    setState(State::CloseOut);
}

void AssistantOrchestrator::dismiss() {
    setState(State::Dormant);
}

void AssistantOrchestrator::setState(State s) {
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}
