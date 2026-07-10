#include "baristamodule.h"

#include "assistantsettings.h"
#include "assistantorchestrator.h"
#include "assistantvoice.h"
#include "voiceinput.h"
#include "baristaknowledge.h"
#include "baristaactions.h"
#include "baristacontextbuilder.h"
#include "feedbackstorage.h"
#include "tasksstorage.h"
#include "../controllers/maincontroller.h"
#include "../history/shothistorystorage.h"
#include "../ai/aimanager.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFileInfo>

BaristaModule::BaristaModule(MainController* mainController, MachineState* machineState,
                             Settings* appSettings, QObject* parent)
    : QObject(parent)
    , m_settings(new AssistantSettings(this))
    , m_orchestrator(new AssistantOrchestrator(mainController, machineState, m_settings, this))
    , m_voice(new AssistantVoice(m_settings, appSettings, AssistantVoice::Role::Barista, this))
    // [barista-fork] The coaching voice: a SECOND AssistantVoice reading the coaching-voice settings,
    // used by the live steam + espresso coaches (wired in main.cpp). Reuses all the same synth code.
    , m_coachingVoice(new AssistantVoice(m_settings, appSettings, AssistantVoice::Role::Coaching, this))
    , m_voiceInput(new VoiceInput(this))
    , m_knowledge(new BaristaKnowledge(m_settings, mainController ? mainController->aiManager() : nullptr, this))
    , m_actions(new BaristaActions(appSettings, machineState, this))
    , m_contextBuilder(new BaristaContextBuilder(
          mainController ? mainController->aiManager() : nullptr,
          mainController ? mainController->beanbase() : nullptr,
          mainController ? mainController->profileManager() : nullptr,
          appSettings, this))
    , m_feedbackStorage(new FeedbackStorage(this))
    , m_tasksStorage(new TasksStorage(this)) {
    connect(m_settings, &AssistantSettings::enabledChanged,
            this, &BaristaModule::enabledChanged);

    // [barista-fork] Verbal-feedback KB wiring. assistant.db lives in the SAME app-data directory as shots.db
    // (derived from its path), so it self-relocates with the shot DB and never collides with shots.db's
    // migration chain. Initialize it and hand it to AIManager for the log_tasting_feedback write tool and the
    // proactive "recent feedback on this bean" context block. AIManager reads it lazily, so ordering is fine.
    if (mainController) {
        if (ShotHistoryStorage* sh = mainController->shotHistory(); sh && !sh->databasePath().isEmpty()) {
            const QString dir = QFileInfo(sh->databasePath()).absolutePath();
            const QString assistantDb = dir + QStringLiteral("/assistant.db");
            m_feedbackStorage->initialize(assistantDb);
            // [barista-fork] Reminders + maintenance share the SAME assistant.db file (each store's
            // ensureSchema is idempotent and touches only its own tables).
            m_tasksStorage->initialize(assistantDb);
        }
        if (AIManager* ai = mainController->aiManager()) {
            ai->setFeedbackStorage(m_feedbackStorage);
            ai->setTasksStorage(m_tasksStorage);   // [barista-fork] task tools + dueItems context block
            // [barista-fork] apply_dial_change → applyFromNext, via a std::function seam (keeps BaristaActions
            // out of the AI TUs / DB-only tests). m_actions outlives AIManager (both parented under the module).
            BaristaActions* actions = m_actions;
            ai->setApplyDialHandler([actions](const QVariantMap& next, qint64 anchorId) {
                return actions->applyFromNext(next, anchorId);
            });
            // [barista-fork] end_conversation → orchestrator.requestDismiss(), which emits dismissRequested()
            // for the overlay to act on AFTER the sign-off is spoken (never cuts it off). The tool executor runs
            // synchronously inside the provider's network-reply slot (main thread), so this is a plain main-thread
            // call — no cross-thread marshalling needed. m_orchestrator outlives AIManager (both parented under
            // the module).
            AssistantOrchestrator* orch = m_orchestrator;
            ai->setEndConversationHandler([orch]() {
                orch->requestDismiss();
            });
        }
    }
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
