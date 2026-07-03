#include "baristamodule.h"

#include "assistantsettings.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>

BaristaModule::BaristaModule(QObject* parent)
    : QObject(parent)
    , m_settings(new AssistantSettings(this)) {
    connect(m_settings, &AssistantSettings::enabledChanged,
            this, &BaristaModule::enabledChanged);
}

bool BaristaModule::enabled() const {
    return m_settings->enabled();
}

BaristaModule* BaristaModule::install(QQmlApplicationEngine* engine, QObject* parent) {
    auto* module = new BaristaModule(parent ? parent : engine);
    engine->rootContext()->setContextProperty(QStringLiteral("Barista"), module);
    return module;
}
