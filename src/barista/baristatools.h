#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVariantMap>
#include <functional>

class ShotHistoryStorage;
class FeedbackStorage;
class TasksStorage;   // [barista-fork] reminders + maintenance (assistant.db)

// [barista-fork] The barista's PRIVATE client-side AI tools — query_shots, get_shot_detail, compare_shots,
// get_bean_profile, detect_grind_drift (READ, shots.db) plus log_tasting_feedback / search_tasting_feedback
// (the verbal-feedback KB, assistant.db). This module owns BOTH halves of each tool:
//   - the JSON tool DEFINITIONS (name/description/input_schema) sent to the model, and
//   - the EXECUTOR that runs them against the local databases.
// It is deliberately extracted OUT of the shared, actively-developed AI files (aiprovider/aimanager), which now
// expose only a GENERIC seam (AnthropicProvider::setClientTools). That keeps those upstream-tracking files free
// of barista specifics so they stop conflicting on every re-base. Wired in ONE call from AIManager.
class BaristaTools {
public:
    // The tool JSON definitions, appended to the request's "tools" array when the barista opts in
    // (RequestOptions.clientTools).
    static QJsonArray toolDefinitions();

    // [barista-fork] The FAST-PATH web tool JSON definitions (get_weather / get_stock_quote / get_local_news).
    // Registered SEPARATELY from toolDefinitions() and appended to the request ONLY when RequestOptions.webSearch
    // is set — i.e. gated by the SAME umbrella "barista may reach the internet" toggle (webSearchEnabled) as
    // Anthropic web search, not by the clientTools/query_shots gate. They resolve via the `webTools` seam in
    // executeTool below (async single GET each) — so the model prefers these fast keyless endpoints over the
    // ~10s web-search round-trip for weather / stock prices / local news.
    static QJsonArray webToolDefinitions();

    // Run ONE client tool OFF the main thread and deliver the JSON result via `done` (invoked back on the main
    // thread). `shotHistory` supplies the shots.db path; `feedback` supplies the assistant.db path for the two
    // tasting-feedback tools (both read lazily by the caller — may have been wired after construction; a null
    // pointer yields an error result for the tools that need it). `anchorSnapshot` carries the app-side
    // provenance for a log_tasting_feedback write — the CURRENT bean/type/profile + the anchor shot's id and
    // dial (dose/yield/grind/temp), stamped by AIManager (NEVER from the model). Keys (all optional):
    //   shotId (qint64), beanBrand, beanType, profile (QString), doseG, yieldG (double), grind (QString),
    //   tempC (double), source (QString). shotId 0 = a bean-general note.
    // [barista-fork] `applyDial` runs apply_dial_change (write the next-shot dial). Kept as a std::function
    // seam (not a BaristaActions* pointer) so this TU never names BaristaActions::applyFromNext — that would
    // drag the machine-source chain into DB-only tests that compile baristatools.cpp. The app wires it to
    // BaristaActions::applyFromNext (see baristamodule.cpp); it mutates Settings, so it MUST run on the main
    // thread — executeTool is invoked there (only DB reads are threaded), so it applies synchronously. An empty
    // std::function yields an error result for that tool only. Signature mirrors applyFromNext(next, anchorId).
    // [barista-fork] `endConversation` runs end_conversation (the barista dismisses itself on a spoken goodbye).
    // Same std::function-seam rationale as applyDial: this TU never names AssistantOrchestrator, so DB-only tests
    // that compile baristatools.cpp don't drag in the orchestrator/QML chain. The app wires it to
    // AssistantOrchestrator::requestDismiss (see baristamodule.cpp), which merely emits a main-thread signal the
    // overlay acts on AFTER the sign-off is spoken — the tool itself never tears down UI on this thread. An empty
    // std::function yields an error result for that tool only.
    // [barista-fork] `tasks` supplies the assistant.db reminders + maintenance store for the four
    // task tools (create_reminder / list_due_reminders / complete_reminder / log_maintenance). Read
    // lazily like `feedback`; a null pointer yields an error result for those tools only.
    // [barista-fork] `webTools` runs the three FAST-PATH web tools (get_weather / get_stock_quote /
    // get_local_news). Kept as a std::function seam (not a BaristaWebTools* pointer) for the SAME reason as
    // applyDial/endConversation: this TU never names BaristaWebTools, so DB-only tests that compile
    // baristatools.cpp don't drag in QtNetwork. The app wires it to BaristaWebTools's async getters (see
    // baristamodule.cpp); each resolves `done(result)` on completion. An empty std::function yields an error
    // result for those three tools only. Signature mirrors the generic tool executor:
    // (toolName, input, done).
    // [barista-fork] `getActiveRecipe` / `activateRecipe` / `deactivateRecipe` are the Recipes 2.0 seams (same
    // std::function-seam rationale as applyDial — this TU never names MainController). getActiveRecipe returns
    // the active recipe map (or {} = none), sync on the main thread. deactivateRecipe returns {was_active,name},
    // sync. activateRecipe is ASYNC + MACHINE-MUTATING: the app handler does the pre-flight (recipe exists +
    // profile resolvable), the activateRecipe() call, the recipeActivated correlation, and a 10s timeout, then
    // resolves its reply with a result JSON — the executor forwards that straight to `done`. Empty seams yield
    // an unavailable/error result for the corresponding tool only.
    static void executeTool(ShotHistoryStorage* shotHistory, FeedbackStorage* feedback,
                            TasksStorage* tasks,
                            const std::function<QVariantMap(const QVariantMap&, qint64)>& applyDial,
                            const std::function<void()>& endConversation,
                            const std::function<void(const QString&, const QJsonObject&,
                                                     std::function<void(QJsonValue)>)>& webTools,
                            const std::function<QVariantMap()>& getActiveRecipe,
                            const std::function<void(qint64, std::function<void(QJsonObject)>)>& activateRecipe,
                            const std::function<QVariantMap()>& deactivateRecipe,
                            // [barista-fork] update_recipe seam: mutate a recipe's fields (dose/grind/temp/yield
                            // spec), completing `done` on the storage's recipeUpdated. Empty = tool unavailable.
                            const std::function<void(qint64, const QVariantMap&,
                                                     std::function<void(QJsonObject)>)>& updateRecipe,
                            // [barista-fork] recipeOp seam: the app-side recipe operations that need
                            // RecipeStorage + ProfileManager (create/clone/archive/delete). One generic
                            // (op, args, done) seam instead of four params; baristatools.cpp names no app
                            // types, so DB-only tests still compile it. Empty = those tools unavailable.
                            const std::function<void(const QString&, const QVariantMap&,
                                                     std::function<void(QJsonObject)>)>& recipeOp,
                            // [barista-fork] bagOp seam: the app-side coffee-bag operations that need
                            // CoffeeBagStorage (list/create/update/mark_empty/delete). One generic (op, args,
                            // done) seam — baristatools.cpp names no app types, so DB-only tests still compile it.
                            // Empty = the bag tools are unavailable.
                            const std::function<void(const QString&, const QVariantMap&,
                                                     std::function<void(QJsonObject)>)>& bagOp,
                            // [barista-fork] list_profiles seam: return the app's usable profiles ([{title,editor,
                            // drink}], optional title substring filter). Sync, main-thread ProfileManager read.
                            const std::function<QJsonArray(const QString&)>& listProfiles,
                            const std::function<void(const QString&)>& setActiveUser,
                            const QVariantMap& anchorSnapshot,
                            const QString& name, const QJsonObject& input,
                            std::function<void(QJsonValue)> done);
};
