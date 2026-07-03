#pragma once

#include <QObject>
#include <QString>

class MainController;
class MachineState;
class AssistantSettings;

// [barista-fork] The conversation state machine that drives the proactive barista, for BOTH the
// typed and (later) voice front-ends. QML renders `state`; the buttons/typed input call the
// Q_INVOKABLE transitions. Kept in C++ so the wake logic (MachineState phase) and, later, the
// plan request (AIManager) live off the UI thread's binding graph.
//
// State flow (P1 = typed only):
//   Dormant --(machine wakes: Sleep->Idle, or wake())--> ConfirmBean
//   ConfirmBean --confirmSameBean()/chooseNewBean()--> ProposePlan
//   ProposePlan --(P1b: apply/keep)--> Armed --(shot ends)--> CloseOut --> Dormant
class AssistantOrchestrator : public QObject {
    Q_OBJECT
    // Exposed as a lowercase string ("dormant"/"confirmBean"/"proposePlan"/"armed"/"closeOut")
    // so QML can compare without the enum type being registered.
    Q_PROPERTY(QString state READ stateString NOTIFY stateChanged)
    // Id of the just-finished shot (for the taste close-out to write the rating to). -1 = none.
    Q_PROPERTY(qlonglong lastShotId READ lastShotId NOTIFY lastShotIdChanged)

public:
    enum class State { Dormant, ConfirmBean, ProposePlan, Armed, CloseOut };
    Q_ENUM(State)

    AssistantOrchestrator(MainController* mainController, MachineState* machineState,
                          AssistantSettings* settings, QObject* parent = nullptr);

    QString stateString() const;
    qlonglong lastShotId() const { return m_lastShotId; }

    // Interaction entry points. Buttons call these now; the voice layer (P3) will route
    // recognised phrases here too via handleUtterance().
    Q_INVOKABLE void wake();               // explicit summon (tap the assistant pill / debug)
    Q_INVOKABLE void confirmSameBean();
    Q_INVOKABLE void chooseNewBean();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void handleUtterance(const QString& text);  // typed free text (P1c stub)

signals:
    void stateChanged();
    void lastShotIdChanged();
    // "Apply" is a QML-side action (writes dial memory), so a typed/spoken "apply" in ProposePlan
    // is surfaced as a signal the overlay wires to its _applyRecipe().
    void applyRequested();

private slots:
    void onPhaseChanged();
    void onShotSaved(qlonglong shotId);   // → taste close-out

private:
    void setState(State s);

    MainController* m_mainController = nullptr;
    MachineState* m_machineState = nullptr;
    AssistantSettings* m_settings = nullptr;
    State m_state = State::Dormant;
    int m_lastPhase = -1;  // detects the Sleep->Idle wake transition
    qlonglong m_lastShotId = -1;
};
