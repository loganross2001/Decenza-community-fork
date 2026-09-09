#pragma once

// [barista-fork] Compile-time QML singleton registration for the barista facade, replacing the
// setContextProperty("Barista") that was invisible to qmllint/qmlcachegen/the language server
// (CLAUDE.md's #1 QML gotcha: a context property resolves at runtime and nowhere else, so every
// reference counts as an unqualified access indistinguishable from a typo). Mirrors
// src/core/contextsingletons_qml.h. Compiled ONLY into the Decenza target and ONLY when
// DECENZA_BARISTA is on (registered via cmake/barista.cmake), so both the QtQml dependency and the
// registration stay inside the fork module and upstream is untouched.
//
// LIFETIME (the rule from contextsingletons_qml.h): a singleton published through a static raw
// pointer is not nulled when its object dies, so the object MUST outlive the QQmlApplicationEngine.
// BaristaModule::install() parents the module to MainController, which main.cpp declares BEFORE the
// engine and therefore destroys AFTER it — so the instance outlives the engine. It is also created
// before engine.load(), so the lazy create() below always finds it.
//
// AssistantOrchestrator and AssistantSettings are registered UNCREATABLE, not singletons: QML only
// ever reaches them as Barista.orchestrator / Barista.settings, never constructs or names them at
// top level. Registering the TYPES is what lets qmllint resolve their members
// (Barista.orchestrator.lastShotId, Barista.settings.assistantName, ...).
//
// OFF build (DECENZA_BARISTA=OFF, the vanilla-bisect build): this header is not compiled, so
// `Barista` is not a registered type. QML's existing `typeof Barista !== "undefined"` guards then
// short-circuit safely — `typeof` on an unregistered name does not throw (see contextsingletons_qml.h
// on Qt's qv4runtime.cpp), so those guards stay correct in BOTH configs and need no rewrite. In the
// ON build the module instance is always present (install runs before engine.load), so the
// registered-type-with-null-instance trap that file documents does not arise here.

#include <QtQml/qqmlregistration.h>
#include <QtQml/QQmlEngine>
#include <QtQml/QJSEngine>

#include "baristamodule.h"
#include "assistantorchestrator.h"
#include "assistantsettings.h"

// The QML name `Barista` — the whole barista surface hangs off it (Barista.enabled,
// Barista.orchestrator, Barista.settings, ...). Was a context property; now a checked singleton.
struct BaristaModuleForeign
{
    Q_GADGET
    QML_FOREIGN(BaristaModule)
    QML_SINGLETON
    QML_NAMED_ELEMENT(Barista)

public:
    inline static BaristaModule* s_singletonInstance = nullptr;
    static BaristaModule* create(QQmlEngine*, QJSEngine*)
    {
        // Null only before install() or in states the QML guards already handle; return it rather
        // than assert so a Release build degrades to the guarded path instead of aborting.
        if (!s_singletonInstance)
            return nullptr;
        // CppOwnership: main() (via MainController) owns the module; the QML engine must never delete it.
        QJSEngine::setObjectOwnership(s_singletonInstance, QJSEngine::CppOwnership);
        return s_singletonInstance;
    }
};

// Reached only as Barista.orchestrator — registered so qmllint knows its members, never constructed.
struct AssistantOrchestratorForeign
{
    Q_GADGET
    QML_FOREIGN(AssistantOrchestrator)
    QML_UNCREATABLE("Reached as Barista.orchestrator; never constructed in QML.")
    QML_NAMED_ELEMENT(AssistantOrchestrator)
};

// Reached only as Barista.settings — same reasoning.
struct AssistantSettingsForeign
{
    Q_GADGET
    QML_FOREIGN(AssistantSettings)
    QML_UNCREATABLE("Reached as Barista.settings; never constructed in QML.")
    QML_NAMED_ELEMENT(AssistantSettings)
};
