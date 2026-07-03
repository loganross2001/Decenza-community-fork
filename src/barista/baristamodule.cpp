#include "baristamodule.h"

#include "assistantsettings.h"
#include "assistantorchestrator.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>

BaristaModule::BaristaModule(MainController* mainController, MachineState* machineState, QObject* parent)
    : QObject(parent)
    , m_settings(new AssistantSettings(this))
    , m_orchestrator(new AssistantOrchestrator(mainController, machineState, m_settings, this)) {
    connect(m_settings, &AssistantSettings::enabledChanged,
            this, &BaristaModule::enabledChanged);
}

bool BaristaModule::enabled() const {
    return m_settings->enabled();
}

BaristaModule* BaristaModule::install(QQmlApplicationEngine* engine,
                                      MainController* mainController,
                                      MachineState* machineState,
                                      QObject* parent) {
    auto* module = new BaristaModule(mainController, machineState, parent ? parent : engine);
    engine->rootContext()->setContextProperty(QStringLiteral("Barista"), module);
    return module;
}
