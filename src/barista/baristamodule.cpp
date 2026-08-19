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
#include "baristavoiceid.h"       // [barista-fork] voice-ID enrollment + probe coordinator
#include "coachphrasebook.h"      // [barista-fork] live-coach model-generated phrasing + gameplan
#include "baristawebtools.h"      // [barista-fork] fast-path web tools (weather / stock / local news)
#include "baristacloudtools.h"    // [barista-fork] coffee cloud tools (Visualizer shots + canonical bean lookup)
#include "../core/settings.h"        // [barista-fork] app Settings → dye()->dyeBarista() for the active user
#include "../core/settings_dye.h"
#include "../controllers/maincontroller.h"
#include "../controllers/profilemanager.h"   // [barista-fork] activate_recipe pre-flight (findProfileByTitle)
#include "../history/shothistorystorage.h"
#include "../history/recipestorage.h"         // [barista-fork] Recipes 2.0 activate pre-flight + load
#include "../history/coffeebagstorage.h"      // [barista-fork] bagOp seam: coffee-bag CRUD
#include "../network/beanbase_blob.h"         // [barista-fork] bagOp: merge bean-detail edits into the blob
#include "../core/dbutils.h"                  // [barista-fork] withTempDb for the off-main recipe pre-flight
#include <QDate>                              // [barista-fork] bagOp list: days-off-roast freshness
#include "../ai/aimanager.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QPermissions>          // [barista-fork] QCameraPermission for the add-a-bean-from-a-photo viewfinder
#include <QCoreApplication>      // [barista-fork] qApp->checkPermission / requestPermission
#include <QCoreApplication>
#include <memory>
#include <initializer_list>

namespace {

// [barista-fork] create_related_profile handler body. Copy/adjust a pressure-flow PROFILE for a coaching move.
// SAFE BY CONSTRUCTION: the only write is duplicateProfile() → a NEW file (originals are read-only resources and
// untouchable), UNLESS mode was edit_in_place AND the base is one the user authored — the app refuses in-place on
// any built-in/read-only profile, so we detect that and fall back to a copy with a note. Adjusts advanced
// (settings_2c) profiles by editing named frames (Infuse / a decline frame / the pressure peak); declines
// gracefully when the role frame is absent. All values clamped. Reuses ProfileManager's tested primitives only.
QJsonObject profErr(const QString& detail) {
    return QJsonObject{{QStringLiteral("success"), false}, {QStringLiteral("detail"), detail}};
}

QJsonObject baristaAdjustRelatedProfile(ProfileManager* pm, const QVariantMap& args) {
    if (!pm) return profErr(QStringLiteral("Profiles are unavailable."));
    const QString adjustment = args.value(QStringLiteral("adjustment")).toString();
    const QString direction  = args.value(QStringLiteral("direction")).toString();
    const QString newName     = args.value(QStringLiteral("name")).toString().trimmed();
    QString baseTitle         = args.value(QStringLiteral("base")).toString().trimmed();
    bool wantInPlace          = args.value(QStringLiteral("mode")).toString() == QLatin1String("edit_in_place");

    // Resolve base → canonical title + filename (default: the current active profile).
    if (baseTitle.isEmpty()) baseTitle = pm->currentProfileName();
    const QString canonical = pm->resolveProfileTitle(baseTitle);
    if (!canonical.isEmpty()) baseTitle = canonical;
    const QString baseFilename = pm->findProfileByTitle(baseTitle);
    if (baseFilename.isEmpty())
        return profErr(QStringLiteral("I couldn't find a profile called '%1'. Which one should I base it on?").arg(baseTitle));

    // Safety: in-place ONLY on a user-authored (non-built-in, non-read-only) profile; otherwise copy, with a note.
    QString note;
    if (wantInPlace && pm->isBuiltInFilename(baseFilename)) {
        wantInPlace = false;
        note = QStringLiteral("'%1' is a built-in profile, so I made a copy instead of changing it.").arg(baseTitle);
    }

    QString targetTitle, targetFilename;
    auto makeCopy = [&]() -> QJsonObject {
        if (newName.isEmpty()) return profErr(QStringLiteral("What would you like to name the new profile?"));
        if (!pm->duplicateProfile(baseFilename, newName))
            return profErr(QStringLiteral("I couldn't create '%1' — that name may already be taken. Pick another?").arg(newName));
        targetTitle = newName;
        pm->loadProfile(newName);                       // make the copy current so frame edits land on it
        targetFilename = pm->findProfileByTitle(newName);
        return QJsonObject{};                            // empty = ok
    };

    if (!wantInPlace) {
        const QJsonObject e = makeCopy();
        if (!e.isEmpty()) return e;
    } else {
        pm->loadProfile(baseTitle);
        if (pm->isCurrentProfileReadOnly()) {           // downloaded read-only → can't edit in place; copy instead
            note = QStringLiteral("'%1' is read-only, so I made a copy instead of changing it.").arg(baseTitle);
            const QJsonObject e = makeCopy();
            if (!e.isEmpty()) return e;
            wantInPlace = false;
        } else {
            targetTitle = baseTitle;
            targetFilename = baseFilename;
        }
    }
    if (targetFilename.isEmpty())
        return profErr(QStringLiteral("I created the profile but lost track of the file — please check your profiles."));

    // Gather frames from the now-current target.
    QList<QVariantMap> frames;
    for (int i = 0; i <= 40; ++i) { const QVariantMap f = pm->getFrameAt(i); if (f.isEmpty()) break; frames.append(f); }
    if (frames.isEmpty()) return profErr(QStringLiteral("That profile has no editable frames."));

    auto frameByName = [&](std::initializer_list<const char*> keys) -> int {
        for (int i = 0; i < frames.size(); ++i) {
            const QString n = frames[i].value(QStringLiteral("name")).toString().toLower();
            for (const char* k : keys) if (n.contains(QLatin1String(k))) return i;
        }
        return -1;
    };
    auto clampd = [](double v, double lo, double hi) { return qBound(lo, v, hi); };

    QString changed;
    if (adjustment == QLatin1String("preinfusion")) {
        const int i = frameByName({"infuse", "soak", "bloom", "preinfus"});
        if (i < 0) return profErr(QStringLiteral("I couldn't find a pre-infusion phase in this profile to adjust."));
        const double sec = frames[i].value(QStringLiteral("seconds")).toDouble();
        const double delta = (direction == QLatin1String("less") || direction == QLatin1String("shorter")) ? -3.0 : 5.0;
        const double newSec = clampd(sec + delta, 1.0, 45.0);
        pm->setFrameProperty(i, QStringLiteral("seconds"), newSec);
        changed = QStringLiteral("pre-infusion %1→%2 s").arg(sec, 0, 'f', 0).arg(newSec, 0, 'f', 0);
        const double pr = frames[i].value(QStringLiteral("pressure")).toDouble();
        if (direction == QLatin1String("softer") && pr > 1.5) {
            const double np = clampd(pr - 1.0, 1.0, pr);
            pm->setFrameProperty(i, QStringLiteral("pressure"), np);
            changed += QStringLiteral(", pressure %1→%2 bar").arg(pr, 0, 'f', 1).arg(np, 0, 'f', 1);
        }
    } else if (adjustment == QLatin1String("declining_tail")) {
        const int i = frameByName({"decline", "declin", "ramp down", "rampdown", "fall"});
        if (i < 0) return profErr(QStringLiteral("This profile has no declining phase to deepen (a flat pressure profile has none). Want a different adjustment?"));
        const double pr = frames[i].value(QStringLiteral("pressure")).toDouble();
        const double np = clampd(pr - (direction == QLatin1String("less") ? -1.0 : 1.0), 3.0, 10.0);
        pm->setFrameProperty(i, QStringLiteral("pressure"), np);
        changed = QStringLiteral("decline end %1→%2 bar").arg(pr, 0, 'f', 1).arg(np, 0, 'f', 1);
    } else if (adjustment == QLatin1String("cap_spike")) {
        int i = -1; double mx = -1.0;
        for (int k = 0; k < frames.size(); ++k) {
            const double p = frames[k].value(QStringLiteral("pressure")).toDouble();
            if (p > mx) { mx = p; i = k; }
        }
        if (i < 0 || mx <= 0.0) return profErr(QStringLiteral("I couldn't find a pressure peak to cap in this profile."));
        const double np = clampd(mx - 1.0, 4.0, mx);
        pm->setFrameProperty(i, QStringLiteral("pressure"), np);
        changed = QStringLiteral("peak %1→%2 bar (%3)").arg(mx, 0, 'f', 1).arg(np, 0, 'f', 1)
                      .arg(frames[i].value(QStringLiteral("name")).toString());
        const double exOver = frames[i].value(QStringLiteral("exitPressureOver")).toDouble();
        if (exOver > np) pm->setFrameProperty(i, QStringLiteral("exitPressureOver"), np);   // keep the frame able to exit
    } else {
        return profErr(QStringLiteral("I don't know the adjustment '%1'.").arg(adjustment));
    }

    // Persist + activate (uploadCurrentProfile is phase-guarded — it won't touch the machine mid-shot).
    if (!pm->saveProfile(targetFilename))
        return profErr(QStringLiteral("I adjusted the curve but couldn't save '%1'.").arg(targetTitle));
    pm->uploadCurrentProfile();

    QJsonObject out;
    out[QStringLiteral("success")]   = true;
    out[QStringLiteral("profile")]   = targetTitle;
    out[QStringLiteral("base")]      = baseTitle;
    out[QStringLiteral("mode")]      = wantInPlace ? QStringLiteral("edited_in_place") : QStringLiteral("created_copy");
    out[QStringLiteral("change")]    = changed;
    out[QStringLiteral("activated")] = true;
    if (!note.isEmpty()) out[QStringLiteral("note")] = note;
    return out;
}

} // namespace

BaristaModule::BaristaModule(MainController* mainController, MachineState* machineState,
                             Settings* appSettings, QObject* parent)
    : QObject(parent)
    , m_settings(new AssistantSettings(this))
    , m_orchestrator(new AssistantOrchestrator(mainController, machineState, m_settings, this))
    , m_voice(new AssistantVoice(m_settings, appSettings, AssistantVoice::Role::Barista, this))
    // [barista-fork] The coaching voice: a SECOND AssistantVoice reading the coaching-voice settings,
    // used by the live steam + espresso coaches (wired in main.cpp). Reuses all the same synth code.
    , m_coachingVoice(new AssistantVoice(m_settings, appSettings, AssistantVoice::Role::Coaching, this))
    // [barista-fork] Model-generated varied phrasing + pre-shot gameplan for the live coaches (one bracketing
    // AI call, cached in assistant.db). Initialized with the db path in the same block as feedback/tasks below.
    , m_coachPhrasebook(new CoachPhrasebook(mainController ? mainController->aiManager() : nullptr, this))
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
    // [barista-fork] Coffee cloud tools reuse the web QNAM and read the app's stored Visualizer login.
    , m_cloudTools(new BaristaCloudTools(m_webNetwork, appSettings, this))
    // [barista-fork] Diagnostic recorder. Constructed FIRST-class here so its static record() has a live
    // instance for the whole session; sets BaristaDiagnostics::s_instance in its ctor.
    , m_diagnostics(new BaristaDiagnostics(this))
    // [barista-fork] Independent 10-day KB backup; initialized with the assistant.db path below.
    , m_backup(new BaristaBackup(this))
    // [barista-fork] Voice-ID (Increment 1): on-device speaker enrollment + probe; voiceprints.db path below.
    , m_voiceId(new BaristaVoiceId(this)) {
    connect(m_settings, &AssistantSettings::enabledChanged,
            this, &BaristaModule::enabledChanged);

    // [barista-fork] Two-way-comms redesign: construct the new conversation state machine (in the ctor BODY so
    // the voices + voiceInput are fully built, and to avoid an init-list reorder warning). It owns the
    // SpeakerGate + self-wires the conversational voice. Feed it the mic's finished utterances — harmless while
    // the useNewConversation flag is off, since the controller ignores finalText until QML engages it (it stays
    // Idle). The rest of the wiring (AI dispatch + QML delegation behind the flag) is the next increment.
    m_conversation = new BaristaConversation(m_voice, m_coachingVoice, m_voiceInput, this);
    connect(m_voiceInput, &VoiceInput::finalText, m_conversation, &BaristaConversation::onFinalText);
    // [barista-fork] VoiceInput.error fires only on a genuinely exhausted recogniser (it retries transient
    // errors internally). Route it to the controller → NeedsTap (a visible "tap to talk"), so the new path
    // never silently re-listens. Harmless while the flag is off (controller Idle → onSttError returns).
    connect(m_voiceInput, &VoiceInput::error, m_conversation, &BaristaConversation::onSttError);

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
            // [barista-fork] Coach phrasebook persists its model-generated cue pools in assistant.db (rides the backup).
            m_coachPhrasebook->initialize(assistantDb);
            // [barista-fork] Start the independent KB backup now that assistant.db's path is known — a
            // startup backup runs if today's set is missing, then a 6h re-check keeps the 10-day history.
            m_backup->initialize(assistantDb);
            // [barista-fork] Voice-ID: voiceprints.db is its OWN file beside assistant.db — deliberately NOT
            // assistant.db (biometric data must stay off the KB backup, which VACUUMs only assistant.db).
            m_voiceId->initialize(dir + QStringLiteral("/voiceprints.db"));
            // Active user for enrollment = the roster active user (dyeBarista) → owner name (userName) → "".
            AssistantSettings* bset = m_settings;
            m_voiceId->setActiveUserProvider([appSettings, bset]() -> QString {
                const QString dye = (appSettings && appSettings->dye())
                                    ? appSettings->dye()->dyeBarista().trimmed() : QString();
                if (!dye.isEmpty()) return dye;
                return bset ? bset->userName().trimmed() : QString();
            });
            // [barista-fork] Increment 2: a confident voice match sets the active user via the SAME Phase-1
            // path the set_active_user tool uses (→ dyeBarista) — voice-ID is an INPUT to identity, not new.
            m_voiceId->setActiveUserSeam([mainController](const QString& name) {
                if (mainController) mainController->setActiveBaristaUser(name);
            });
            // [barista-fork] Owner-tunable match thresholds → BaristaVoiceId (initial + live on change).
            BaristaVoiceId* vid = m_voiceId;
            auto applyThresholds = [vid, bset]() {
                if (vid && bset) vid->setThresholds(bset->voiceIdConfidence(), bset->voiceIdMargin(), bset->voiceIdMaybe());
            };
            applyThresholds();
            connect(m_settings, &AssistantSettings::voiceIdConfidenceChanged, m_voiceId, applyThresholds);
            connect(m_settings, &AssistantSettings::voiceIdMarginChanged,     m_voiceId, applyThresholds);
            connect(m_settings, &AssistantSettings::voiceIdMaybeChanged,      m_voiceId, applyThresholds);
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
            // [barista-fork] open_bag_camera → orchestrator.requestOpenBagCamera(), which emits
            // openBagCameraRequested() on the main thread; the overlay opens BagCameraCapture.
            ai->setOpenBagCameraHandler([orch]() {
                orch->requestOpenBagCamera();
            });
            // [barista-fork] Fast-path web tools. The module lambda owns the app-side glue the pure
            // BaristaWebTools service shouldn't: (1) the homeLocation no-city fallback for weather/news, and
            // (2) building the Google-News query from topic/location. Then it forwards to the async getter,
            // which resolves `done` on the main thread. m_webTools + m_settings outlive AIManager (all parented
            // under the module).
            BaristaWebTools* web = m_webTools;
            BaristaCloudTools* cloud = m_cloudTools;
            AssistantSettings* settings = m_settings;
            ai->setWebToolsHandler([web, cloud, settings](const QString& name, const QJsonObject& input,
                                                          std::function<void(QJsonValue)> done) {
                // [barista-fork] Coffee cloud tools share this seam (same internet gate); dispatch them first.
                if (name == QLatin1String("get_visualizer_shot")) {
                    cloud->getVisualizerShot(input.value(QStringLiteral("shot")).toString(), std::move(done));
                    return;
                }
                if (name == QLatin1String("search_visualizer_shots")) {
                    cloud->searchVisualizerShots(input, std::move(done));
                    return;
                }
                if (name == QLatin1String("look_up_bean")) {
                    cloud->lookUpBean(input.value(QStringLiteral("query")).toString(), std::move(done));
                    return;
                }
                if (name == QLatin1String("fetch_bag_page")) {
                    cloud->fetchBagPage(input.value(QStringLiteral("url")).toString(), std::move(done));
                    return;
                }
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
                if (!mc) return QVariantMap{};
                QVariantMap r = mc->activeRecipe();
                // [barista-fork] Report the ACTUAL brew temperature, never the profile-relative offset — the
                // barista and user speak in real degrees. actual = profile baseline + stored offset.
                if (r.contains(QStringLiteral("tempOffsetC"))) {
                    const double offset = r.take(QStringLiteral("tempOffsetC")).toDouble();
                    const double baseline = mc->profileManager()
                        ? mc->profileManager()->profileBaselineTempC(r.value(QStringLiteral("profileTitle")).toString())
                        : 0.0;
                    if (baseline > 0)
                        r.insert(QStringLiteral("temperatureC"), baseline + offset);
                }
                return r;
            });
            // [barista-fork] Phase 1 identity: set_active_user → set the active roster user (dyeBarista). The
            // executor did the roster match/insert off-main and hands us the canonical name on the main thread.
            ai->setSetActiveUserHandler([mc](const QString& name) {
                if (mc) mc->setActiveBaristaUser(name);
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
                                    // [barista-fork] Yield is now a spec (add-yield-ratio-anchor): report the
                                    // value + mode ("absolute" = grams, "ratio" = multiple of the dose, "none").
                                    ro[QStringLiteral("yield_value")] = rec.yieldValue;
                                    ro[QStringLiteral("yield_mode")] = rec.yieldMode;
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
            // [barista-fork] update_recipe seam: mutate a saved recipe's fields (no machine mutation — the
            // storage does its own background work). One-shot recipeUpdated correlation + 10s safety timeout,
            // mirroring the MCP recipe_update handler.
            ai->setUpdateRecipeHandler([mc](qint64 recipeId, const QVariantMap& fields,
                                            std::function<void(QJsonObject)> reply) {
                RecipeStorage* rs = mc ? mc->recipeStorage() : nullptr;
                if (!rs) {
                    reply(QJsonObject{{QStringLiteral("success"), false},
                                      {QStringLiteral("failure_reason"), QStringLiteral("unavailable")},
                                      {QStringLiteral("detail"), QStringLiteral("Recipe editing is unavailable.")}});
                    return;
                }
                // [barista-fork] Validate a profile change BEFORE writing — a misheard/unknown profile would leave
                // the recipe pointing at a missing curve (activation would then fail). ProfileManager is main-thread
                // and we're on the main thread here. Normalize to the profile's canonical title on a match.
                QVariantMap f = fields;
                const QString newProfile = f.value(QStringLiteral("profileTitle")).toString().trimmed();
                if (!newProfile.isEmpty()) {
                    // resolveProfileTitle returns the canonical TITLE (exact/case-insensitive), empty on no match.
                    const QString canonical = (mc && mc->profileManager())
                        ? mc->profileManager()->resolveProfileTitle(newProfile) : QString();
                    if (canonical.isEmpty()) {
                        reply(QJsonObject{{QStringLiteral("updated"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("profile_not_found")},
                            {QStringLiteral("detail"), QStringLiteral(
                                "No profile matches '%1' — nothing changed. Call list_profiles to find the exact "
                                "title, then try again.").arg(newProfile)}});
                        return;
                    }
                    f.insert(QStringLiteral("profileTitle"), canonical);
                }
                // The actual update (correlate recipeUpdated + 10s timeout, then requestUpdateRecipe).
                auto runUpdate = [rs, recipeId, reply](QVariantMap ff) {
                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(rs);
                    timer->setSingleShot(true);
                    auto finish = [done, conn, timer, reply](QJsonObject r) {
                        if (*done) return;
                        *done = true;
                        if (*conn) QObject::disconnect(*conn);
                        timer->stop(); timer->deleteLater();
                        reply(r);
                    };
                    *conn = QObject::connect(rs, &RecipeStorage::recipeUpdated, rs,
                        [finish, recipeId](qint64 updatedId, bool success) {
                            if (updatedId != recipeId) return;   // someone else's update
                            if (success)
                                finish(QJsonObject{{QStringLiteral("updated"), true},
                                                   {QStringLiteral("recipe_id"), static_cast<double>(recipeId)}});
                            else
                                finish(QJsonObject{{QStringLiteral("updated"), false},
                                    {QStringLiteral("failure_reason"), QStringLiteral("not_found_or_failed")},
                                    {QStringLiteral("detail"), QStringLiteral("That recipe could not be updated.")}});
                        });
                    QObject::connect(timer, &QTimer::timeout, rs, [finish]() {
                        finish(QJsonObject{{QStringLiteral("updated"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("timeout")},
                            {QStringLiteral("detail"), QStringLiteral("No confirmation the recipe saved within 10s.")}});
                    });
                    timer->start(10000);
                    rs->requestUpdateRecipe(recipeId, ff);
                };

                // [barista-fork] The barista speaks ACTUAL brew temps; store them as the profile-relative offset
                // (offset = desired - profile baseline). The profile is the one being set here, else the recipe's
                // current one (from the active recipe if it's that, else a quick off-thread load).
                if (!f.contains(QStringLiteral("_desiredBrewTempC"))) {
                    runUpdate(f);
                    return;
                }
                const double desired = f.take(QStringLiteral("_desiredBrewTempC")).toDouble();
                ProfileManager* pm = mc ? mc->profileManager() : nullptr;
                auto applyTemp = [desired, pm](QVariantMap ff, const QString& title) {
                    const double baseline = (pm && !title.isEmpty()) ? pm->profileBaselineTempC(title) : 0.0;
                    if (baseline > 0)
                        ff.insert(QStringLiteral("tempOffsetC"), desired - baseline);
                    // baseline unstated (tea/pour-over profile) → leave temp untouched
                    return ff;
                };
                QString profTitle = f.value(QStringLiteral("profileTitle")).toString();
                if (profTitle.isEmpty() && mc) {
                    const QVariantMap active = mc->activeRecipe();
                    if (active.value(QStringLiteral("id")).toLongLong() == recipeId)
                        profTitle = active.value(QStringLiteral("profileTitle")).toString();
                }
                if (!profTitle.isEmpty()) {
                    runUpdate(applyTemp(f, profTitle));
                    return;
                }
                // Other recipe, no profile in the edit → load its profile off the main thread, then update.
                const QString dbPath = (mc && mc->shotHistory()) ? mc->shotHistory()->databasePath() : QString();
                if (dbPath.isEmpty()) { runUpdate(f); return; }   // can't convert; save the rest
                QThread* thread = QThread::create([dbPath, recipeId, f, applyTemp, runUpdate]() {
                    QString title;
                    withTempDb(dbPath, "barista_recipe_temp", [&](QSqlDatabase& db) {
                        title = RecipeStorage::loadRecipeStatic(db, recipeId).profileTitle;
                    });
                    QMetaObject::invokeMethod(qApp, [f, applyTemp, runUpdate, title]() {
                        runUpdate(applyTemp(f, title));
                    }, Qt::QueuedConnection);
                });
                QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
                thread->start();
            });
            // [barista-fork] recipeOp seam: app-side recipe operations needing RecipeStorage + ProfileManager.
            // Stage 1 handles "create" (build the recipe, resolve/validate the profile, optionally inherit the
            // active recipe's beans, correlate recipeCreated with a 10s timeout). clone/archive/delete land next.
            ai->setRecipeOpHandler([mc](const QString& op, const QVariantMap& args,
                                        std::function<void(QJsonObject)> reply) {
                // [barista-fork] create_related_profile rides this seam too (it needs ProfileManager, not
                // RecipeStorage) — handle it before the RecipeStorage guard below.
                if (op == QLatin1String("create_related_profile")) {
                    reply(baristaAdjustRelatedProfile(mc ? mc->profileManager() : nullptr, args));
                    return;
                }
                RecipeStorage* rs = mc ? mc->recipeStorage() : nullptr;
                if (!rs) {
                    reply(QJsonObject{{QStringLiteral("success"), false},
                                      {QStringLiteral("failure_reason"), QStringLiteral("unavailable")},
                                      {QStringLiteral("detail"), QStringLiteral("Recipe operations are unavailable.")}});
                    return;
                }
                if (op == QLatin1String("create")) {
                    QVariantMap recipe = args;
                    const bool copyBeans = recipe.take(QStringLiteral("copyBeansFromActive")).toBool();
                    const QString nm = recipe.value(QStringLiteral("name")).toString().trimmed();
                    // Profile is required here (hot-water-only creation isn't exposed to voice yet). Resolve +
                    // validate against installed profiles; normalize to the canonical title.
                    const QString profTitle = recipe.value(QStringLiteral("profileTitle")).toString().trimmed();
                    if (profTitle.isEmpty()) {
                        reply(QJsonObject{{QStringLiteral("created"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("profile_required")},
                            {QStringLiteral("detail"), QStringLiteral(
                                "A new recipe needs a profile — ask the user which profile to use.")}});
                        return;
                    }
                    const QString canonical = (mc && mc->profileManager())
                        ? mc->profileManager()->resolveProfileTitle(profTitle) : QString();
                    if (canonical.isEmpty()) {
                        reply(QJsonObject{{QStringLiteral("created"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("profile_not_found")},
                            {QStringLiteral("detail"), QStringLiteral(
                                "No profile matches '%1'. Call list_profiles to find the exact title, then try again.")
                                .arg(profTitle)}});
                        return;
                    }
                    recipe.insert(QStringLiteral("profileTitle"), canonical);
                    // [barista-fork] The barista speaks ACTUAL brew temps → store as the profile-relative offset.
                    if (recipe.contains(QStringLiteral("_desiredBrewTempC"))) {
                        const double desired = recipe.take(QStringLiteral("_desiredBrewTempC")).toDouble();
                        const double baseline = mc->profileManager()->profileBaselineTempC(canonical);
                        if (baseline > 0)
                            recipe.insert(QStringLiteral("tempOffsetC"), desired - baseline);
                        // baseline unstated (tea/pour-over) → leave temp unset
                    }
                    // "Same beans, different profile" — inherit bean identity from the active recipe when the model
                    // asked and didn't pass beans explicitly.
                    if (copyBeans && mc) {
                        const QVariantMap active = mc->activeRecipe();
                        for (const char* k : {"roasterName", "coffeeName", "beanBaseId", "bagId"}) {
                            const QString key = QLatin1String(k);
                            if (recipe.value(key).toString().isEmpty() && !active.value(key).toString().isEmpty())
                                recipe.insert(key, active.value(key));
                        }
                    }
                    if (!rs->isSaveValid(nm, recipe.value(QStringLiteral("profileTitle")).toString(),
                                         recipe.value(QStringLiteral("hotWaterJson")).toString())) {
                        reply(QJsonObject{{QStringLiteral("created"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("invalid")},
                            {QStringLiteral("detail"), QStringLiteral(
                                "The recipe needs at least a name and a valid profile.")}});
                        return;
                    }
                    // recipeCreated is a broadcast — correlate by the echoed name (active-name uniqueness makes
                    // this safe), with a 10s safety timeout and a shared-flag guard against a double reply.
                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(rs);
                    timer->setSingleShot(true);
                    auto finish = [done, conn, timer, reply](QJsonObject r) {
                        if (*done) return;
                        *done = true;
                        if (*conn) QObject::disconnect(*conn);
                        timer->stop(); timer->deleteLater();
                        reply(r);
                    };
                    // Success echoes the full recipe map (name present → match by it). Failure echoes NO name
                    // (just error/requestToken), so attribute a failure broadcast to us — barista creates are
                    // serialized, so a -1 while we wait is ours. This turns nameInUse into a fast, correct error
                    // instead of a 10s timeout.
                    *conn = QObject::connect(rs, &RecipeStorage::recipeCreated, rs,
                        [finish, nm](qint64 newId, const QVariantMap& created) {
                            if (newId > 0) {
                                if (created.value(QStringLiteral("name")).toString() != nm) return;   // another create
                                finish(QJsonObject{{QStringLiteral("created"), true},
                                                   {QStringLiteral("recipe_id"), static_cast<double>(newId)},
                                                   {QStringLiteral("name"), nm}});
                            } else {
                                const QString err = created.value(QStringLiteral("error")).toString();
                                finish(QJsonObject{{QStringLiteral("created"), false},
                                    {QStringLiteral("failure_reason"), err.isEmpty() ? QStringLiteral("create_failed") : err},
                                    {QStringLiteral("detail"), err == QLatin1String("nameInUse")
                                        ? QStringLiteral("A recipe with that name already exists — choose a different name.")
                                        : QStringLiteral("The recipe could not be saved.")}});
                            }
                        });
                    QObject::connect(timer, &QTimer::timeout, rs, [finish]() {
                        finish(QJsonObject{{QStringLiteral("created"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("timeout")},
                            {QStringLiteral("detail"), QStringLiteral("No confirmation the recipe was created within 10s.")}});
                    });
                    timer->start(10000);
                    rs->requestCreateRecipe(recipe);
                    return;
                }
                // Shared one-shot correlation scaffold (10s timeout + double-reply guard) for the remaining ops.
                auto makeFinish = [reply](std::shared_ptr<QMetaObject::Connection> conn, QTimer* timer,
                                          std::shared_ptr<bool> done) {
                    return [reply, conn, timer, done](QJsonObject r) {
                        if (*done) return;
                        *done = true;
                        if (*conn) QObject::disconnect(*conn);
                        timer->stop(); timer->deleteLater();
                        reply(r);
                    };
                };
                if (op == QLatin1String("clone")) {
                    const qint64 sourceId = args.value(QStringLiteral("sourceId")).toLongLong();
                    const QString newName = args.value(QStringLiteral("newName")).toString().trimmed();
                    if (sourceId <= 0 || newName.isEmpty()) {
                        reply(QJsonObject{{QStringLiteral("created"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("bad_args")},
                            {QStringLiteral("detail"), QStringLiteral("Cloning needs a source recipe and a new name.")}});
                        return;
                    }
                    // requestToken correlates the clone's recipeCreated broadcast unambiguously — echoed in BOTH
                    // the success and failure maps (unlike name, which failure omits).
                    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(rs); timer->setSingleShot(true);
                    auto finish = makeFinish(conn, timer, done);
                    *conn = QObject::connect(rs, &RecipeStorage::recipeCreated, rs,
                        [finish, token, newName](qint64 newId, const QVariantMap& created) {
                            if (created.value(QStringLiteral("requestToken")).toString() != token) return;   // not our clone
                            if (newId > 0)
                                finish(QJsonObject{{QStringLiteral("created"), true},
                                    {QStringLiteral("recipe_id"), static_cast<double>(newId)},
                                    {QStringLiteral("name"), newName}});
                            else {
                                const QString err = created.value(QStringLiteral("error")).toString();
                                finish(QJsonObject{{QStringLiteral("created"), false},
                                    {QStringLiteral("failure_reason"), err.isEmpty() ? QStringLiteral("clone_failed") : err},
                                    {QStringLiteral("detail"), err == QLatin1String("nameInUse")
                                        ? QStringLiteral("A recipe with that name already exists — choose a different name.")
                                        : QStringLiteral("The recipe could not be cloned.")}});
                            }
                        });
                    QObject::connect(timer, &QTimer::timeout, rs, [finish]() {
                        finish(QJsonObject{{QStringLiteral("created"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("timeout")},
                            {QStringLiteral("detail"), QStringLiteral("No confirmation the clone was created within 10s.")}});
                    });
                    timer->start(10000);
                    rs->requestCloneRecipe(sourceId, newName, token);
                    return;
                }
                if (op == QLatin1String("archive")) {
                    const qint64 recipeId = args.value(QStringLiteral("recipeId")).toLongLong();
                    const bool archived = args.value(QStringLiteral("archived"), true).toBool();
                    if (recipeId <= 0) {
                        reply(QJsonObject{{QStringLiteral("updated"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("bad_args")}});
                        return;
                    }
                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(rs); timer->setSingleShot(true);
                    auto finish = makeFinish(conn, timer, done);
                    *conn = QObject::connect(rs, &RecipeStorage::recipeUpdated, rs,
                        [finish, recipeId, archived](qint64 id, bool success) {
                            if (id != recipeId) return;
                            if (success)
                                finish(QJsonObject{{QStringLiteral("updated"), true},
                                    {QStringLiteral("recipe_id"), static_cast<double>(recipeId)},
                                    {QStringLiteral("archived"), archived}});
                            else
                                finish(QJsonObject{{QStringLiteral("updated"), false},
                                    {QStringLiteral("failure_reason"), QStringLiteral("not_found_or_failed")}});
                        });
                    QObject::connect(timer, &QTimer::timeout, rs, [finish]() {
                        finish(QJsonObject{{QStringLiteral("updated"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("timeout")}});
                    });
                    timer->start(10000);
                    if (archived) rs->requestArchiveRecipe(recipeId);
                    else          rs->requestUnarchiveRecipe(recipeId);
                    return;
                }
                if (op == QLatin1String("delete")) {
                    const qint64 recipeId = args.value(QStringLiteral("recipeId")).toLongLong();
                    if (recipeId <= 0) {
                        reply(QJsonObject{{QStringLiteral("deleted"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("bad_args")}});
                        return;
                    }
                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(rs); timer->setSingleShot(true);
                    auto finish = makeFinish(conn, timer, done);
                    *conn = QObject::connect(rs, &RecipeStorage::recipeDeleted, rs,
                        [finish, recipeId](qint64 id, bool success) {
                            if (id != recipeId) return;
                            if (success)
                                finish(QJsonObject{{QStringLiteral("deleted"), true},
                                    {QStringLiteral("recipe_id"), static_cast<double>(recipeId)}});
                            else
                                finish(QJsonObject{{QStringLiteral("deleted"), false},
                                    {QStringLiteral("failure_reason"), QStringLiteral("has_history")},
                                    {QStringLiteral("detail"), QStringLiteral(
                                        "This recipe has shot history, so it can't be deleted — offer to archive it instead.")}});
                        });
                    QObject::connect(timer, &QTimer::timeout, rs, [finish]() {
                        finish(QJsonObject{{QStringLiteral("deleted"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("timeout")}});
                    });
                    timer->start(10000);
                    rs->requestDeleteRecipe(recipeId);
                    return;
                }
                reply(QJsonObject{{QStringLiteral("success"), false},
                    {QStringLiteral("failure_reason"), QStringLiteral("unsupported_op")},
                    {QStringLiteral("detail"), QStringLiteral("That recipe operation isn't supported yet.")}});
            });
            // [barista-fork] bagOp seam: coffee-bag management (list/create/update/mark_empty/delete) against
            // CoffeeBagStorage. Same shape as recipeOp — a shared one-shot correlation scaffold (10s timeout +
            // double-reply guard) bridges each async storage signal to the reply. Bean-detail edits (origin,
            // process, tastingNotes, ...) live in the beanBaseData blob, folded via BeanBaseBlob::mergeBeanDetails
            // exactly as the in-app bag editor and the MCP `bag` tool do.
            // [fork-index] seam=bagOp | domain=bean | change=-
            //   what: bridges barista bag tools (add/update/finish/delete/list) to storage
            //   refs: CoffeeBagStorage::requestCreateBag
            ai->setBagOpHandler([mc](const QString& op, const QVariantMap& args,
                                     std::function<void(QJsonObject)> reply) {
                CoffeeBagStorage* bags = mc ? mc->bagStorage() : nullptr;
                if (!bags) {
                    reply(QJsonObject{{QStringLiteral("success"), false},
                                      {QStringLiteral("failure_reason"), QStringLiteral("unavailable")},
                                      {QStringLiteral("detail"), QStringLiteral("Bag management is unavailable.")}});
                    return;
                }
                auto makeFinish = [reply](std::shared_ptr<QMetaObject::Connection> conn, QTimer* timer,
                                          std::shared_ptr<bool> done) {
                    return [reply, conn, timer, done](QJsonObject r) {
                        if (*done) return;
                        *done = true;
                        if (*conn) QObject::disconnect(*conn);
                        timer->stop(); timer->deleteLater();
                        reply(r);
                    };
                };

                // The bean-detail blob vocabulary (mirrors the MCP `bag` tool's kBlobKeys). These do NOT map to
                // columns; they're merged into beanBaseData. Kept here (app side) so baristatools.cpp needs no
                // BeanBaseBlob dependency.
                static const QStringList kBlobKeys = {
                    QStringLiteral("origin"), QStringLiteral("region"), QStringLiteral("producer"),
                    QStringLiteral("variety"), QStringLiteral("process"), QStringLiteral("tastingNotes"),
                    QStringLiteral("link")};

                if (op == QLatin1String("list")) {
                    // requestInventory returns only in-inventory bags, MRU order (finished bags are out of scope
                    // for a "which bag do you mean" lookup); includeFinished is accepted but can't surface them.
                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(bags); timer->setSingleShot(true);
                    auto finish = makeFinish(conn, timer, done);
                    *conn = QObject::connect(bags, &CoffeeBagStorage::inventoryReady, bags,
                        [finish](const QVariantList& list) {
                            QJsonArray arr;
                            for (const QVariant& v : list) {
                                const CoffeeBag b = CoffeeBag::fromVariantMap(v.toMap());
                                QJsonObject o;
                                o[QStringLiteral("bagId")]   = static_cast<double>(b.id);
                                o[QStringLiteral("roaster")] = b.roasterName;
                                o[QStringLiteral("coffee")]  = b.coffeeName;
                                if (!b.roastDate.isEmpty()) o[QStringLiteral("roastDate")] = b.roastDate;
                                // Freshness = days since the beans last met air: the defrost date if the bag was
                                // frozen and thawed, otherwise the roast date.
                                const QString ref = !b.defrostDate.isEmpty() ? b.defrostDate : b.roastDate;
                                const QDate rd = QDate::fromString(ref, Qt::ISODate);
                                if (rd.isValid())
                                    o[QStringLiteral("freshnessDays")] = static_cast<double>(rd.daysTo(QDate::currentDate()));
                                o[QStringLiteral("inInventory")] = b.inInventory;
                                arr.append(o);
                            }
                            finish(QJsonObject{{QStringLiteral("bags"), arr}});
                        });
                    QObject::connect(timer, &QTimer::timeout, bags, [finish]() {
                        finish(QJsonObject{{QStringLiteral("error"), QStringLiteral("No inventory returned within 10s.")}});
                    });
                    timer->start(10000);
                    bags->requestInventory();
                    return;
                }

                if (op == QLatin1String("create")) {
                    const QString roaster = args.value(QStringLiteral("roasterName")).toString().trimmed();
                    const QString coffee  = args.value(QStringLiteral("coffeeName")).toString().trimmed();
                    if (roaster.isEmpty() || coffee.isEmpty()) {
                        reply(QJsonObject{{QStringLiteral("success"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("bad_args")},
                            {QStringLiteral("detail"), QStringLiteral("A new bag needs a roaster and a coffee name.")}});
                        return;
                    }
                    const QString kind = (args.value(QStringLiteral("kind")).toString() == QLatin1String("tea"))
                                         ? QStringLiteral("tea") : QStringLiteral("coffee");
                    QVariantMap bag;
                    bag.insert(QStringLiteral("roasterName"), roaster);
                    bag.insert(QStringLiteral("coffeeName"), coffee);
                    bag.insert(QStringLiteral("kind"), kind);
                    bag.insert(QStringLiteral("inInventory"), true);
                    for (const char* k : {"roastDate", "roastLevel", "grinderSetting", "yieldMode"}) {
                        const QString key = QLatin1String(k);
                        if (args.contains(key)) bag.insert(key, args.value(key).toString());
                    }
                    if (args.contains(QStringLiteral("doseWeightG")))
                        bag.insert(QStringLiteral("doseWeightG"), args.value(QStringLiteral("doseWeightG")).toDouble());
                    if (args.contains(QStringLiteral("rpm")))
                        bag.insert(QStringLiteral("rpm"), args.value(QStringLiteral("rpm")).toInt());
                    if (args.contains(QStringLiteral("yieldValue")))
                        bag.insert(QStringLiteral("yieldValue"), args.value(QStringLiteral("yieldValue")).toDouble());
                    QVariantMap blobEdits;
                    for (const QString& key : kBlobKeys)
                        if (args.contains(key)) blobEdits.insert(key, args.value(key));
                    if (!blobEdits.isEmpty())
                        bag.insert(QStringLiteral("beanBaseData"),
                                   BeanBaseBlob::mergeBeanDetails(QString(), blobEdits));

                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(bags); timer->setSingleShot(true);
                    auto finish = makeFinish(conn, timer, done);
                    // bagCreated is a broadcast with no token — correlate on the submitted identity (roaster +
                    // coffee + kind), exactly like the MCP bag create. bagId<=0 is a failure and is still ours.
                    *conn = QObject::connect(bags, &CoffeeBagStorage::bagCreated, bags,
                        [finish, roaster, coffee, kind](qint64 bagId, const QVariantMap& created) {
                            if (bagId > 0
                                && (created.value(QStringLiteral("roasterName")).toString() != roaster
                                    || created.value(QStringLiteral("coffeeName")).toString() != coffee
                                    || created.value(QStringLiteral("kind")).toString() != kind))
                                return;  // someone else's concurrent create
                            if (bagId <= 0) {
                                finish(QJsonObject{{QStringLiteral("success"), false},
                                    {QStringLiteral("failure_reason"), QStringLiteral("create_failed")},
                                    {QStringLiteral("detail"), QStringLiteral("The bag could not be created.")}});
                                return;
                            }
                            finish(QJsonObject{{QStringLiteral("success"), true},
                                {QStringLiteral("bagId"), static_cast<double>(bagId)},
                                {QStringLiteral("roaster"), roaster},
                                {QStringLiteral("coffee"), coffee}});
                        });
                    QObject::connect(timer, &QTimer::timeout, bags, [finish]() {
                        finish(QJsonObject{{QStringLiteral("success"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("timeout")},
                            {QStringLiteral("detail"), QStringLiteral("No confirmation the bag was created within 10s.")}});
                    });
                    timer->start(10000);
                    bags->requestCreateBag(bag);
                    return;
                }

                // update / mark_empty / delete all key on a bagId.
                const qint64 bagId = args.value(QStringLiteral("bagId")).toLongLong();
                if (bagId <= 0) {
                    reply(QJsonObject{{QStringLiteral("success"), false},
                        {QStringLiteral("failure_reason"), QStringLiteral("bad_args")},
                        {QStringLiteral("detail"), QStringLiteral("That needs a valid bagId — use list_bags first.")}});
                    return;
                }

                if (op == QLatin1String("delete")) {
                    auto done = std::make_shared<bool>(false);
                    auto conn = std::make_shared<QMetaObject::Connection>();
                    QTimer* timer = new QTimer(bags); timer->setSingleShot(true);
                    auto finish = makeFinish(conn, timer, done);
                    *conn = QObject::connect(bags, &CoffeeBagStorage::bagDeleted, bags,
                        [finish, bagId](qint64 id, bool success) {
                            if (id != bagId) return;
                            if (success)
                                finish(QJsonObject{{QStringLiteral("success"), true},
                                    {QStringLiteral("bagId"), static_cast<double>(bagId)}});
                            else
                                finish(QJsonObject{{QStringLiteral("success"), false},
                                    {QStringLiteral("failure_reason"), QStringLiteral("has_history")},
                                    {QStringLiteral("detail"), QStringLiteral(
                                        "This bag has shot history, so it can't be deleted — offer to finish it "
                                        "(mark it empty) instead.")}});
                        });
                    QObject::connect(timer, &QTimer::timeout, bags, [finish]() {
                        finish(QJsonObject{{QStringLiteral("success"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("timeout")}});
                    });
                    timer->start(10000);
                    bags->requestDeleteBag(bagId);
                    return;
                }

                // update / mark_empty both resolve on bagUpdated(id, success).
                QVariantMap fields;
                if (op == QLatin1String("update")) {
                    for (const char* k : {"roastDate", "roastLevel", "grinderSetting", "yieldMode"}) {
                        const QString key = QLatin1String(k);
                        if (args.contains(key)) fields.insert(key, args.value(key).toString());
                    }
                    if (args.contains(QStringLiteral("doseWeightG")))
                        fields.insert(QStringLiteral("doseWeightG"), args.value(QStringLiteral("doseWeightG")).toDouble());
                    if (args.contains(QStringLiteral("rpm")))
                        fields.insert(QStringLiteral("rpm"), args.value(QStringLiteral("rpm")).toInt());
                    if (args.contains(QStringLiteral("yieldValue")))
                        fields.insert(QStringLiteral("yieldValue"), args.value(QStringLiteral("yieldValue")).toDouble());
                    QVariantMap blobEdits;
                    for (const QString& key : kBlobKeys)
                        if (args.contains(key)) blobEdits.insert(key, args.value(key));
                    if (!blobEdits.isEmpty()) {
                        // Merge into the CURRENT blob (a bounded single-row read on a discrete voice action),
                        // so an edit to one detail doesn't wipe the rest of the canonical snapshot.
                        QString existing;
                        withTempDb(bags->databasePath(), QStringLiteral("barista_bagupd_read"),
                                   [&](QSqlDatabase& db) {
                                       existing = CoffeeBagStorage::loadBagStatic(db, bagId).beanBaseData;
                                   });
                        fields.insert(QStringLiteral("beanBaseData"),
                                      BeanBaseBlob::mergeBeanDetails(existing, blobEdits));
                    }
                    if (fields.isEmpty()) {
                        reply(QJsonObject{{QStringLiteral("success"), false},
                            {QStringLiteral("failure_reason"), QStringLiteral("bad_args")},
                            {QStringLiteral("detail"), QStringLiteral("Nothing to change on that bag.")}});
                        return;
                    }
                }

                auto done = std::make_shared<bool>(false);
                auto conn = std::make_shared<QMetaObject::Connection>();
                QTimer* timer = new QTimer(bags); timer->setSingleShot(true);
                auto finish = makeFinish(conn, timer, done);
                const bool isFinish = (op == QLatin1String("mark_empty"));
                *conn = QObject::connect(bags, &CoffeeBagStorage::bagUpdated, bags,
                    [finish, bagId, isFinish](qint64 id, bool success) {
                        if (id != bagId) return;
                        if (success)
                            finish(QJsonObject{{QStringLiteral("success"), true},
                                {QStringLiteral("bagId"), static_cast<double>(bagId)},
                                {QStringLiteral("finished"), isFinish}});
                        else
                            finish(QJsonObject{{QStringLiteral("success"), false},
                                {QStringLiteral("failure_reason"), QStringLiteral("not_found_or_failed")},
                                {QStringLiteral("detail"), QStringLiteral("That bag couldn't be found or updated.")}});
                    });
                QObject::connect(timer, &QTimer::timeout, bags, [finish]() {
                    finish(QJsonObject{{QStringLiteral("success"), false},
                        {QStringLiteral("failure_reason"), QStringLiteral("timeout")}});
                });
                timer->start(10000);
                if (isFinish) bags->requestMarkEmpty(bagId);
                else          bags->requestUpdateBag(bagId, fields);
            });
            // [barista-fork] list_profiles seam: hand back the app's usable profiles (main-thread ProfileManager
            // read). The barista uses this to show what's available AND to resolve a spoken profile name to its
            // exact title for the recipe tools.
            ai->setListProfilesHandler([mc](const QString& query) -> QJsonArray {
                ProfileManager* pm = mc ? mc->profileManager() : nullptr;
                return pm ? pm->profileListForBarista(query) : QJsonArray{};
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

// [barista-fork] Camera permission for the add-a-bean-from-a-photo viewfinder. Mirrors BLEManager's QPermission
// flow: resolve immediately when already Granted/Denied, else prompt and report the outcome via the signal.
void BaristaModule::requestCameraPermission()
{
    QCameraPermission perm;
    switch (qApp->checkPermission(perm)) {
    case Qt::PermissionStatus::Granted:
        emit cameraPermissionResult(true);
        return;
    case Qt::PermissionStatus::Denied:
        emit cameraPermissionResult(false);
        return;
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(perm, this, [this](const QPermission& p) {
            emit cameraPermissionResult(p.status() == Qt::PermissionStatus::Granted);
        });
        return;
    }
}
