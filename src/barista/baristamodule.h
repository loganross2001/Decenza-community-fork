#pragma once

#include <QObject>

#include "voiceinput.h"            // complete type needed for the Q_PROPERTY(VoiceInput*) metatype
#include "baristaknowledge.h"      // ditto for Q_PROPERTY(BaristaKnowledge*)
#include "baristaactions.h"        // ditto for Q_PROPERTY(BaristaActions*)
#include "baristacontextbuilder.h" // ditto for Q_PROPERTY(BaristaContextBuilder*)

class QQmlApplicationEngine;
class MainController;
class MachineState;
class Settings;
class AssistantSettings;
class AssistantOrchestrator;
class AssistantVoice;
class FeedbackStorage;   // [barista-fork] verbal-feedback KB

// [barista-fork] Facade for the proactive barista assistant. The ENTIRE feature hangs off this
// one object, exposed to QML as the "Barista" context property. `install()` is the single C++
// integration point into upstream (one call in main.cpp) — everything else lives under src/barista/.
class BaristaModule : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    Q_PROPERTY(AssistantSettings* settings READ settings CONSTANT)
    Q_PROPERTY(AssistantOrchestrator* orchestrator READ orchestrator CONSTANT)
    Q_PROPERTY(AssistantVoice* voice READ voice CONSTANT)
    Q_PROPERTY(AssistantVoice* coachingVoice READ coachingVoice CONSTANT)
    Q_PROPERTY(VoiceInput* voiceInput READ voiceInput CONSTANT)
    Q_PROPERTY(BaristaKnowledge* knowledge READ knowledge CONSTANT)
    Q_PROPERTY(BaristaActions* actions READ actions CONSTANT)
    Q_PROPERTY(BaristaContextBuilder* contextBuilder READ contextBuilder CONSTANT)

public:
    // Single upstream hook: construct the module (settings + orchestrator), register the
    // "Barista" context property. Deps are borrowed pointers owned by main(). Returned module
    // is owned by `parent` (or the engine if null).
    static BaristaModule* install(QQmlApplicationEngine* engine,
                                  MainController* mainController,
                                  MachineState* machineState,
                                  Settings* appSettings,
                                  QObject* parent = nullptr);

    bool enabled() const;
    AssistantSettings* settings() const { return m_settings; }
    AssistantOrchestrator* orchestrator() const { return m_orchestrator; }
    AssistantVoice* voice() const { return m_voice; }
    AssistantVoice* coachingVoice() const { return m_coachingVoice; }
    VoiceInput* voiceInput() const { return m_voiceInput; }
    BaristaKnowledge* knowledge() const { return m_knowledge; }
    BaristaActions* actions() const { return m_actions; }
    BaristaContextBuilder* contextBuilder() const { return m_contextBuilder; }

signals:
    void enabledChanged();

private:
    BaristaModule(MainController* mainController, MachineState* machineState,
                  Settings* appSettings, QObject* parent);

    AssistantSettings* m_settings = nullptr;
    AssistantOrchestrator* m_orchestrator = nullptr;
    AssistantVoice* m_voice = nullptr;
    AssistantVoice* m_coachingVoice = nullptr;   // [barista-fork] separate voice for the live coaches
    VoiceInput* m_voiceInput = nullptr;
    BaristaKnowledge* m_knowledge = nullptr;
    BaristaActions* m_actions = nullptr;
    BaristaContextBuilder* m_contextBuilder = nullptr;
    // [barista-fork] Verbal-feedback KB (assistant.db). Constructed here (the module owns it), initialized with
    // a path derived beside shots.db, and handed to AIManager for the write tool + proactive context block.
    FeedbackStorage* m_feedbackStorage = nullptr;
};
