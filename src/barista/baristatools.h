#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

class ShotHistoryStorage;

// [barista-fork] The barista's PRIVATE client-side AI tools — query_shots, get_shot_detail, compare_shots,
// get_bean_profile, detect_grind_drift. This module owns BOTH halves of each tool:
//   - the JSON tool DEFINITIONS (name/description/input_schema) sent to the model, and
//   - the EXECUTOR that runs them against the local shot DB.
// It is deliberately extracted OUT of the shared, actively-developed AI files (aiprovider/aimanager), which now
// expose only a GENERIC seam (AnthropicProvider::setClientTools). That keeps those upstream-tracking files free
// of barista specifics so they stop conflicting on every re-base. Wired in ONE call from AIManager.
class BaristaTools {
public:
    // The 5 tool JSON definitions, appended to the request's "tools" array when the barista opts in
    // (RequestOptions.clientTools). Byte-identical to the definitions that previously lived inline in
    // AnthropicProvider::analyzeConversation.
    static QJsonArray toolDefinitions();

    // Run ONE client tool against the local shot DB OFF the main thread and deliver the JSON result via `done`
    // (invoked back on the main thread). `shotHistory` supplies the DB path (read lazily by the caller, so it
    // may have been wired after construction); a null pointer yields an error result. Mirrors the original
    // AIManager::executeBaristaTool verbatim — same SQL, same withTempDb background-thread + callback pattern.
    static void executeTool(ShotHistoryStorage* shotHistory, const QString& name, const QJsonObject& input,
                            std::function<void(QJsonValue)> done);
};
