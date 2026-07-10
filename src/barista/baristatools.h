#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVariantMap>
#include <functional>

class ShotHistoryStorage;
class FeedbackStorage;

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
    static void executeTool(ShotHistoryStorage* shotHistory, FeedbackStorage* feedback,
                            const std::function<QVariantMap(const QVariantMap&, qint64)>& applyDial,
                            const std::function<void()>& endConversation,
                            const QVariantMap& anchorSnapshot,
                            const QString& name, const QJsonObject& input,
                            std::function<void(QJsonValue)> done);
};
