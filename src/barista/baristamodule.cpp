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
#include "maintenancedocsync.h"
#include "baristawebtools.h"      // [barista-fork] fast-path web tools (weather / stock / local news)
#include "../controllers/maincontroller.h"
#include "../controllers/profilemanager.h"   // [barista-fork] activate_recipe pre-flight (findProfileByTitle)
#include "../history/shothistorystorage.h"
#include "../history/recipestorage.h"         // [barista-fork] Recipes 2.0 activate pre-flight + load
#include "../core/dbutils.h"                  // [barista-fork] withTempDb for the off-main recipe pre-flight
#include "../ai/aimanager.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <memory>

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
    , m_tasksStorage(new TasksStorage(this))
    // [barista-fork] Periodic Decent maintenance-docs check. Owns its own QNAM; persists via m_tasksStorage.
    , m_docSync(new MaintenanceDocSync(m_tasksStorage, this))
    // [barista-fork] Fast-path web tools get a PRIVATE QNAM (no shared cookie jar) — each tool contacts only its
    // one host with only the user's query (city/symbol/topic). See BaristaWebTools' privacy note.
    , m_webNetwork(new QNetworkAccessManager(this))
    , m_webTools(new BaristaWebTools(m_webNetwork, this))
    // [barista-fork] Diagnostic recorder. Constructed FIRST-class here so its static record() has a live
    // instance for the whole session; sets BaristaDiagnostics::s_instance in its ctor.
    , m_diagnostics(new BaristaDiagnostics(this)) {
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
            // [barista-fork] Now that assistant.db is initialized, kick the once-per-launch, rate-limited
            // Decent maintenance-docs check (~30-day window, owner-toggle-gated, single GET, nothing sent).
            m_docSync->maybeCheckOnStartup();
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
            // [barista-fork] Fast-path web tools. The module lambda owns the app-side glue the pure
            // BaristaWebTools service shouldn't: (1) the homeLocation no-city fallback for weather/news, and
            // (2) building the Google-News query from topic/location. Then it forwards to the async getter,
            // which resolves `done` on the main thread. m_webTools + m_settings outlive AIManager (all parented
            // under the module).
            BaristaWebTools* web = m_webTools;
            AssistantSettings* settings = m_settings;
            ai->setWebToolsHandler([web, settings](const QString& name, const QJsonObject& input,
                                                   std::function<void(QJsonValue)> done) {
                const QString home = settings ? settings->homeLocation().trimmed() : QString();
                if (name == QLatin1String("get_weather")) {
                    QString location = input.value(QStringLiteral("location")).toString().trimmed();
                    if (location.isEmpty()) location = home;   // "weather around here" → home location
                    web->getWeather(location, std::move(done));
                    return;
                }
                if (name == QLatin1String("get_stock_quote")) {
                    web->getStockQuote(input.value(QStringLiteral("symbol")).toString(), std::move(done));
                    return;
                }
                if (name == QLatin1String("get_local_news")) {
                    // topic wins; else "<location or home> local news"; empty when no location AND no home.
                    const QString topic    = input.value(QStringLiteral("topic")).toString().trimmed();
                    QString location = input.value(QStringLiteral("location")).toString().trimmed();
                    if (location.isEmpty()) location = home;
                    QString query = topic;
                    if (query.isEmpty() && !location.isEmpty())
                        query = location + QStringLiteral(" local news");
                    if (query.isEmpty()) {
                        done(QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("no topic or location — ask the user what news they want")}});
                        return;
                    }
                    web->getLocalNews(query, std::move(done));
                    return;
                }
                done(QJsonObject{{QStringLiteral("error"), QStringLiteral("unknown web tool: ") + name}});
            });

            // [barista-fork] Recipes 2.0 tools → MainController. get/deactivate are sync main-thread reads;
            // activate is async + MACHINE-MUTATING (pre-flight off-main → profile check + activate + terminal
            // recipeActivated correlation + 10s timeout, all main-thread). mainController outlives AIManager.
            MainController* mc = mainController;
            ai->setGetActiveRecipeHandler([mc]() -> QVariantMap {
                return mc ? mc->activeRecipe() : QVariantMap{};
            });
            ai->setDeactivateRecipeHandler([mc]() -> QVariantMap {
                QVariantMap out;
                if (!mc) { out[QStringLiteral("was_active")] = false; return out; }
                const QVariantMap active = mc->activeRecipe();
                const bool wasActive = !active.isEmpty() && active.value(QStringLiteral("id")).toLongLong() > 0;
                out[QStringLiteral("was_active")] = wasActive;
                if (wasActive)
                    out[QStringLiteral("name")] = active.value(QStringLiteral("name")).toString();
                mc->deactivateRecipe();
                return out;
            });
            ai->setActivateRecipeHandler([mc](qint64 recipeId, std::function<void(QJsonObject)> reply) {
                if (!mc || !mc->shotHistory() || mc->shotHistory()->databasePath().isEmpty()) {
                    reply(QJsonObject{{QStringLiteral("success"), false},
                                      {QStringLiteral("failure_reason"), QStringLiteral("unavailable")},
                                      {QStringLiteral("detail"), QStringLiteral("Recipe activation is unavailable.")}});
                    return;
                }
                const QString dbPath = mc->shotHistory()->databasePath();
                QThread* thread = QThread::create([mc, recipeId, dbPath, reply]() {
                    // Pre-flight the recipe row OFF the main thread (DB read).
                    Recipe rec;
                    bool found = false;
                    withTempDb(dbPath, "barista_recipe_preflight", [&](QSqlDatabase& db) {
                        rec = RecipeStorage::loadRecipeStatic(db, recipeId);
                        found = rec.isValid();
                    });
                    // Everything below mutates the machine → back on the main thread.
                    QMetaObject::invokeMethod(qApp, [mc, recipeId, rec, found, reply]() {
                        if (!found) {
                            reply(QJsonObject{{QStringLiteral("success"), false},
                                {QStringLiteral("failure_reason"), QStringLiteral("not_found")},
                                {QStringLiteral("detail"), QStringLiteral("That recipe no longer exists.")}});
                            return;
                        }
                        // Profile must resolve: title installed, OR a stored profile_json fallback, OR the recipe
                        // is legitimately profile-less (hot-water-only). Else fail without touching the machine.
                        const bool hotWaterOnly = rec.profileTitle.trimmed().isEmpty()
                                                  && Recipe::hotWaterActive(rec.hotWaterJson);
                        bool profileOk = hotWaterOnly;
                        if (!profileOk && !rec.profileTitle.trimmed().isEmpty()) {
                            const bool titleResolves = mc->profileManager()
                                && !mc->profileManager()->findProfileByTitle(rec.profileTitle).isEmpty();
                            profileOk = titleResolves || !rec.profileJson.trimmed().isEmpty();
                        }
                        if (!profileOk) {
                            reply(QJsonObject{{QStringLiteral("success"), false},
                                {QStringLiteral("failure_reason"), QStringLiteral("profile_missing")},
                                {QStringLiteral("detail"),
                                 QStringLiteral("Profile '%1' referenced by this recipe was not found. Nothing changed.")
                                     .arg(rec.profileTitle)}});
                            return;
                        }
                        bool hasMilk = false;
                        if (!rec.steamJson.isEmpty())
                            hasMilk = QJsonDocument::fromJson(rec.steamJson.toUtf8())
                                          .object().value(QStringLiteral("hasMilk")).toBool();
                        // Correlate the terminal recipeActivated(id,success) with a 10s timeout. A shared flag
                        // guards against a double reply (signal vs timeout); whichever fires first tears both down.
                        auto replied = std::make_shared<bool>(false);
                        auto conn = std::make_shared<QMetaObject::Connection>();
                        QTimer* timer = new QTimer(mc);
                        timer->setSingleShot(true);
                        auto finish = [replied, conn, timer, reply](const QJsonObject& r) {
                            if (*replied) return;
                            *replied = true;
                            QObject::disconnect(*conn);
                            timer->stop();
                            timer->deleteLater();
                            reply(r);
                        };
                        *conn = QObject::connect(mc, &MainController::recipeActivated, mc,
                            [recipeId, rec, hasMilk, finish](qint64 id, bool success) {
                                if (id != recipeId) return;   // not our activation
                                QJsonObject r;
                                r[QStringLiteral("success")] = success;
                                if (success) {
                                    QJsonObject ro;
                                    ro[QStringLiteral("id")] = static_cast<double>(rec.id);
                                    ro[QStringLiteral("name")] = rec.name;
                                    ro[QStringLiteral("profile")] = rec.profileTitle;
                                    ro[QStringLiteral("dose_g")] = rec.doseG;
                                    ro[QStringLiteral("yield_g")] = rec.yieldG;
                                    ro[QStringLiteral("has_milk")] = hasMilk;
                                    ro[QStringLiteral("steam_heater_started")] = hasMilk;
                                    r[QStringLiteral("recipe")] = ro;
                                } else {
                                    r[QStringLiteral("failure_reason")] = QStringLiteral("activation_failed");
                                    r[QStringLiteral("detail")] = QStringLiteral(
                                        "The machine did not accept the recipe (its profile may be missing). Nothing changed.");
                                }
                                finish(r);
                            });
                        QObject::connect(timer, &QTimer::timeout, mc, [finish]() {
                            finish(QJsonObject{{QStringLiteral("success"), false},
                                {QStringLiteral("failure_reason"), QStringLiteral("timeout")},
                                {QStringLiteral("detail"),
                                 QStringLiteral("No confirmation from the machine within 10s; state unknown. "
                                                "Tell the user to check the screen.")}});
                        });
                        timer->start(10000);
                        mc->activateRecipe(recipeId);
                    }, Qt::QueuedConnection);
                });
                QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
                thread->start();
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
