#include "assistantsettings.h"

AssistantSettings::AssistantSettings(QObject* parent)
    : QObject(parent) {}

bool AssistantSettings::enabled() const {
    // Default ON — but the runtime toggle is the kill-switch for a kitchen appliance:
    // a bad assistant state must never be able to block making coffee.
    return m_settings.value(QStringLiteral("barista/enabled"), true).toBool();
}

void AssistantSettings::setEnabled(bool on) {
    if (enabled() == on)
        return;
    m_settings.setValue(QStringLiteral("barista/enabled"), on);
    emit enabledChanged();
}
