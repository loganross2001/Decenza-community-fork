#include "assistantorchestrator.h"

#include "assistantsettings.h"
#include "../machine/machinestate.h"
#include "../controllers/maincontroller.h"

AssistantOrchestrator::AssistantOrchestrator(MainController* mainController, MachineState* machineState,
                                             AssistantSettings* settings, QObject* parent)
    : QObject(parent)
    , m_mainController(mainController)
    , m_machineState(machineState)
    , m_settings(settings) {
    if (m_machineState) {
        connect(m_machineState, &MachineState::phaseChanged,
                this, &AssistantOrchestrator::onPhaseChanged);
        m_lastPhase = static_cast<int>(m_machineState->phase());
    }
}

QString AssistantOrchestrator::stateString() const {
    switch (m_state) {
    case State::ConfirmBean: return QStringLiteral("confirmBean");
    case State::ProposePlan: return QStringLiteral("proposePlan");
    case State::Armed:       return QStringLiteral("armed");
    case State::CloseOut:    return QStringLiteral("closeOut");
    case State::Dormant:
    default:                 return QStringLiteral("dormant");
    }
}

void AssistantOrchestrator::onPhaseChanged() {
    if (!m_machineState)
        return;
    // Phase tracking is retained for future shot-end (CloseOut) detection. The greeting itself is
    // triggered by the Espresso action (a hook in IdlePage calls wake()), NOT by machine wake.
    m_lastPhase = static_cast<int>(m_machineState->phase());
}

void AssistantOrchestrator::wake() {
    if (!m_settings || !m_settings->enabled())
        return;
    if (m_state != State::Dormant)
        return;
    // P1a: greet + immediately ask the same-bean question (rendered together as one card).
    setState(State::ConfirmBean);
}

void AssistantOrchestrator::confirmSameBean() {
    if (m_state != State::ConfirmBean)
        return;
    setState(State::ProposePlan);  // P1b fills this with a real (local + Claude) plan
}

void AssistantOrchestrator::chooseNewBean() {
    if (m_state != State::ConfirmBean)
        return;
    // P1b: open the existing bean picker and update the active bean. For now, proceed.
    setState(State::ProposePlan);
}

void AssistantOrchestrator::dismiss() {
    setState(State::Dormant);
}

void AssistantOrchestrator::handleUtterance(const QString& text) {
    // P1c: local synonym match to drive the state machine by typed (later spoken) input, so the
    // whole flow is keyboard/voice-navigable, not just tappable. Free text -> planner comes later.
    const QString t = text.trimmed().toLower();
    if (t.isEmpty())
        return;

    const auto has = [&t](std::initializer_list<const char*> words) {
        for (const char* w : words)
            if (t.contains(QString::fromLatin1(w)))
                return true;
        return false;
    };

    // Explicit dismissal works from any state.
    if (has({"dismiss", "go away", "never mind", "nevermind", "bye", "cancel"})) {
        dismiss();
        return;
    }

    switch (m_state) {
    case State::ConfirmBean:
        if (has({"yes", "yep", "yeah", "same", "correct", "right", "sure"}))
            confirmSameBean();
        else if (has({"no", "new", "different", "change", "another", "other"}))
            chooseNewBean();
        break;
    case State::ProposePlan:
        if (has({"apply", "do it", "load", "use it", "go ahead", "yes", "sure", "please"}))
            emit applyRequested();
        else if (has({"no", "keep", "not now", "leave"}))
            dismiss();
        break;
    case State::Dormant:
        wake();
        break;
    default:
        break;
    }
}

void AssistantOrchestrator::setState(State s) {
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}
