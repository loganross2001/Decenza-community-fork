#pragma once

#include <QObject>
#include <QString>

class MainController;
class MachineState;
class AssistantSettings;

// [barista-fork] Lightweight lifecycle driver for the CONVERSATIONAL assistant. It only decides
// WHEN to start talking — Espresso selected → a greeting conversation; a shot finished → a taste
// close-out conversation — and exposes that as `state`. The overlay runs the actual multi-turn
// Claude conversation (AIConversation.ask/followUp), so what the assistant says is AI-generated and
// adaptive, never scripted.
class AssistantOrchestrator : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ stateString NOTIFY stateChanged)   // "dormant" | "greeting" | "closeOut"
    Q_PROPERTY(qlonglong lastShotId READ lastShotId NOTIFY lastShotIdChanged)

public:
    enum class State { Dormant, Greeting, CloseOut };
    Q_ENUM(State)

    AssistantOrchestrator(MainController* mainController, MachineState* machineState,
                          AssistantSettings* settings, QObject* parent = nullptr);

    QString stateString() const;
    qlonglong lastShotId() const { return m_lastShotId; }

    Q_INVOKABLE void wake();       // Espresso selected → start a greeting conversation
    Q_INVOKABLE void dismiss();

signals:
    void stateChanged();
    void lastShotIdChanged();

private slots:
    void onShotSaved(qlonglong shotId);   // → taste close-out conversation

private:
    void setState(State s);

    MainController* m_mainController = nullptr;
    MachineState* m_machineState = nullptr;
    AssistantSettings* m_settings = nullptr;
    State m_state = State::Dormant;
    qlonglong m_lastShotId = -1;
};
