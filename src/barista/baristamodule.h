#pragma once

#include <QObject>

class QQmlApplicationEngine;
class AssistantSettings;

// [barista-fork] Facade for the proactive barista assistant. The ENTIRE feature hangs off this
// one object, exposed to QML as the "Barista" context property. `install()` is the single C++
// integration point into upstream (one call in main.cpp) — everything else lives under src/barista/.
//
// P0: minimal shell (enabled flag + own settings). P1+ will add the orchestrator, voice engine,
// and knowledge store as members, all constructed here and reached via Barista.* from QML.
class BaristaModule : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    Q_PROPERTY(AssistantSettings* settings READ settings CONSTANT)

public:
    // Single upstream hook: construct the module, wire its settings, register the "Barista"
    // context property. Returned module is owned by `parent` (or the engine if null).
    static BaristaModule* install(QQmlApplicationEngine* engine, QObject* parent = nullptr);

    bool enabled() const;
    AssistantSettings* settings() const { return m_settings; }

signals:
    void enabledChanged();

private:
    explicit BaristaModule(QObject* parent);

    AssistantSettings* m_settings = nullptr;
};
