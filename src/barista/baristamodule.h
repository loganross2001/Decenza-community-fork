#pragma once

#include <QObject>

#include "voiceinput.h"            // complete type needed for the Q_PROPERTY(VoiceInput*) metatype
#include "baristaconversation.h"   // [barista-fork] complete type for the Q_PROPERTY(BaristaConversation*) metatype
#include "baristaknowledge.h"      // ditto for Q_PROPERTY(BaristaKnowledge*)
#include "baristaactions.h"        // ditto for Q_PROPERTY(BaristaActions*)
#include "baristacontextbuilder.h" // ditto for Q_PROPERTY(BaristaContextBuilder*)
#include "tasksstorage.h"          // complete type needed for the Q_PROPERTY(TasksStorage*) metatype
#include "maintenancedocsync.h"    // ditto for Q_PROPERTY(MaintenanceDocSync*)
#include "baristadiagnostics.h"    // ditto for Q_PROPERTY(BaristaDiagnostics*)
#include "baristabackup.h"         // ditto for Q_PROPERTY(BaristaBackup*)
#include "baristavoiceid.h"        // ditto for Q_PROPERTY(BaristaVoiceId*)

class QQmlApplicationEngine;
class MainController;
class MachineState;
class Settings;
class AssistantSettings;
class AssistantOrchestrator;
class AssistantVoice;
class FeedbackStorage;   // [barista-fork] verbal-feedback KB
class BaristaWebTools;   // [barista-fork] fast-path web tools (weather / stock / local news)
class BaristaCloudTools; // [barista-fork] coffee cloud tools (Visualizer shots + canonical bean lookup)
class CoachPhrasebook;   // [barista-fork] model-generated live-coach phrasing
class QNetworkAccessManager;

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
    // [barista-fork] Two-way-comms redesign: the new conversation state machine (QML reaches it as
    // Barista.conversation when the useNewConversation flag is on). Present but inert while the flag is off.
    Q_PROPERTY(BaristaConversation* conversation READ conversation CONSTANT)
    Q_PROPERTY(BaristaKnowledge* knowledge READ knowledge CONSTANT)
    Q_PROPERTY(BaristaActions* actions READ actions CONSTANT)
    Q_PROPERTY(BaristaContextBuilder* contextBuilder READ contextBuilder CONSTANT)
    // [barista-fork] Reminders + maintenance store, exposed so the maintenance settings dialog can list/edit
    // tasks and mark them done directly (the barista also reaches it via the AI task tools).
    Q_PROPERTY(TasksStorage* tasks READ tasks CONSTANT)
    // [barista-fork] Periodic Decent maintenance-docs check, exposed so the maintenance settings dialog can
    // show last-checked / toggle the periodic check / run "Check now".
    Q_PROPERTY(MaintenanceDocSync* docSync READ docSync CONSTANT)
    // [barista-fork] Always-on voice/coaching diagnostic recorder, exposed so the diagnostics settings card
    // can toggle it, show the log path/count, export a snapshot, and copy recent events.
    Q_PROPERTY(BaristaDiagnostics* diagnostics READ diagnostics CONSTANT)
    // [barista-fork] Independent 10-day KB backup, exposed so the backup settings card can toggle it, show
    // status (last/count/dir), and trigger a manual "Back up now".
    Q_PROPERTY(BaristaBackup* backup READ backup CONSTANT)
    // [barista-fork] Voice-ID (Phase 2, Increment 1): on-device speaker enrollment + concurrent-capture probe.
    Q_PROPERTY(BaristaVoiceId* voiceId READ voiceId CONSTANT)

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
    BaristaConversation* conversation() const { return m_conversation; }   // [barista-fork]
    BaristaKnowledge* knowledge() const { return m_knowledge; }
    BaristaActions* actions() const { return m_actions; }
    BaristaContextBuilder* contextBuilder() const { return m_contextBuilder; }
    TasksStorage* tasks() const { return m_tasksStorage; }   // [barista-fork] reminders + maintenance
    MaintenanceDocSync* docSync() const { return m_docSync; } // [barista-fork] periodic Decent docs check
    BaristaDiagnostics* diagnostics() const { return m_diagnostics; } // [barista-fork] voice/coaching recorder
    BaristaBackup* backup() const { return m_backup; }                // [barista-fork] independent KB backup
    BaristaVoiceId* voiceId() const { return m_voiceId; }             // [barista-fork] on-device speaker enrollment
    CoachPhrasebook* coachPhrasebook() const { return m_coachPhrasebook; } // [barista-fork] live-coach varied phrasing + gameplan

signals:
    void enabledChanged();

private:
    BaristaModule(MainController* mainController, MachineState* machineState,
                  Settings* appSettings, QObject* parent);

    AssistantSettings* m_settings = nullptr;
    AssistantOrchestrator* m_orchestrator = nullptr;
    AssistantVoice* m_voice = nullptr;
    AssistantVoice* m_coachingVoice = nullptr;   // [barista-fork] separate voice for the live coaches
    CoachPhrasebook* m_coachPhrasebook = nullptr; // [barista-fork] model-generated varied cue phrasing + gameplan
    VoiceInput* m_voiceInput = nullptr;
    // [barista-fork] Two-way-comms redesign state machine. Declared AFTER the voices + voiceInput so the ctor
    // init-list can hand them to it. Inert until the useNewConversation flag engages it from QML.
    BaristaConversation* m_conversation = nullptr;
    BaristaKnowledge* m_knowledge = nullptr;
    BaristaActions* m_actions = nullptr;
    BaristaContextBuilder* m_contextBuilder = nullptr;
    // [barista-fork] Verbal-feedback KB (assistant.db). Constructed here (the module owns it), initialized with
    // a path derived beside shots.db, and handed to AIManager for the write tool + proactive context block.
    FeedbackStorage* m_feedbackStorage = nullptr;
    // [barista-fork] Reminders + maintenance store (SAME assistant.db as m_feedbackStorage). Owned here,
    // initialized with the path derived beside shots.db, handed to AIManager for the task tools + dueItems.
    TasksStorage* m_tasksStorage = nullptr;
    // [barista-fork] Periodic Decent maintenance-docs check (network + rate-limit + hash). Owned here;
    // persists its state through m_tasksStorage (assistant.db maintenance_doc_state row).
    MaintenanceDocSync* m_docSync = nullptr;
    // [barista-fork] Fast-path web tools (get_weather / get_stock_quote / get_local_news). Owns a private QNAM
    // (child of this). Wired into AIManager via setWebToolsHandler; the module lambda resolves the homeLocation
    // fallback + builds the news query before calling into it.
    QNetworkAccessManager* m_webNetwork = nullptr;
    BaristaWebTools* m_webTools = nullptr;
    // [barista-fork] Coffee CLOUD tools (get_visualizer_shot / search_visualizer_shots / look_up_bean). Reuses
    // m_webNetwork; reads the app's stored Visualizer login for authenticated pulls; dispatched via the same
    // web-tools seam by name.
    BaristaCloudTools* m_cloudTools = nullptr;
    // [barista-fork] Always-on voice/coaching timeline recorder (owned here). Its static record() is used
    // across subsystems; this instance is what backs Barista.diagnostics and the settings card.
    BaristaDiagnostics* m_diagnostics = nullptr;
    // [barista-fork] Independent 10-day rolling backup of the private KB (assistant.db + settings), initialized
    // with the assistant.db path once feedback/tasks storage are up.
    BaristaBackup* m_backup = nullptr;
    // [barista-fork] Voice-ID coordinator (enrollment + probe); initialized with the voiceprints.db path.
    BaristaVoiceId* m_voiceId = nullptr;
};
