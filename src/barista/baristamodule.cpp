#include "baristamodule.h"

#include "assistantsettings.h"
#include "assistantorchestrator.h"
#include "assistantvoice.h"
#include "voiceinput.h"
#include "baristaknowledge.h"
#include "baristaactions.h"
#include "baristacontextbuilder.h"
#include "../controllers/maincontroller.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>

BaristaModule::BaristaModule(MainController* mainController, MachineState* machineState,
                             Settings* appSettings, QObject* parent)
    : QObject(parent)
    , m_settings(new AssistantSettings(this))
    , m_orchestrator(new AssistantOrchestrator(mainController, machineState, m_settings, this))
    , m_voice(new AssistantVoice(m_settings, appSettings, this))
    , m_voiceInput(new VoiceInput(this))
    , m_knowledge(new BaristaKnowledge(m_settings, mainController ? mainController->aiManager() : nullptr, this))
    , m_actions(new BaristaActions(appSettings, machineState, this))
    , m_contextBuilder(new BaristaContextBuilder(
          mainController ? mainController->aiManager() : nullptr,
          mainController ? mainController->beanbase() : nullptr,
          mainController ? mainController->profileManager() : nullptr,
          appSettings, this)) {
    connect(m_settings, &AssistantSettings::enabledChanged,
            this, &BaristaModule::enabledChanged);
}

bool BaristaModule::enabled() const {
    return m_settings->enabled();
}

BaristaModule* BaristaModule::install(QQmlApplicationEngine* engine,
                                      MainController* mainController,
                                      MachineState* machineState,
                                      Settings* appSettings,
                                      QObject* parent) {
    auto* module = new BaristaModule(mainController, machineState, appSettings, parent ? parent : engine);
    engine->rootContext()->setContextProperty(QStringLiteral("Barista"), module);
    return module;
}
