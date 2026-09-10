#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariantMap>
#include <QVariantList>
#include <QPair>
#include <memory>
#include <optional>
#include <functional>

#include "../history/shotprojection.h"
#include "../history/shothistory_types.h"
#include "dialing_blocks.h"  // AdvisorContextBlocks — cached between the context request and the send

#include <QtQml/qqmlregistration.h>
class QNetworkAccessManager;
class AIProvider;
class AIConversation;
class ShotSummarizer;
class ShotDataModel;
class Profile;
class Settings;
class ShotHistoryStorage;
class FeedbackStorage;   // [barista-fork]
class TasksStorage;      // [barista-fork] reminders + maintenance (assistant.db)
class TranslationManager;
class ProfileManager;

class AIManager : public QObject {
    Q_OBJECT

    // Compile-time QML registration, so qmllint, qmlcachegen and the language server can
    // follow MainController's property through to this class. A runtime qmlRegister* call is
    // invisible to all three. Full rationale in src/controllers/maincontroller.h.
    QML_ELEMENT
    QML_UNCREATABLE("AIManager is created in C++ and reached via MainController")

    Q_PROPERTY(QString selectedProvider READ selectedProvider WRITE setSelectedProvider NOTIFY providerChanged)
    Q_PROPERTY(QStringList availableProviders READ availableProviders CONSTANT)
    Q_PROPERTY(bool isConfigured READ isConfigured NOTIFY configurationChanged)
    Q_PROPERTY(bool isAnalyzing READ isAnalyzing NOTIFY analyzingChanged)
    Q_PROPERTY(QString lastRecommendation READ lastRecommendation NOTIFY recommendationReceived)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorOccurred)
    Q_PROPERTY(QString lastTestResult READ lastTestResult NOTIFY testResultChanged)
    Q_PROPERTY(bool lastTestSuccess READ lastTestSuccess NOTIFY testResultChanged)
    Q_PROPERTY(QStringList ollamaModels READ ollamaModels NOTIFY ollamaModelsChanged)
    Q_PROPERTY(QString currentModelName READ currentModelName NOTIFY providerChanged)
    Q_PROPERTY(AIConversation* conversation READ conversation CONSTANT)
    Q_PROPERTY(bool hasAnyConversation READ hasAnyConversation NOTIFY conversationIndexChanged)

public:
    explicit AIManager(QNetworkAccessManager* networkManager, Settings* settings, QObject* parent = nullptr);
    ~AIManager();

    // [barista-fork] was 5 — too small once the barista keeps a persisted per-bean thread; evicting the
    // 6th bean's whole discussion contradicts "recall prior advice even after a 2-month gap".
    static constexpr int MAX_CONVERSATIONS = 100;

    struct ConversationEntry {
        QString key;
        QString beanBrand;
        QString beanType;
        QString profileName;
        // Snapshot of the package the key was derived from, not a live
        // reference: two threads for one bean differ only by equipment, so
        // without this they are indistinguishable in every list that shows them.
        QString equipmentLabel;
        qint64 equipmentId = 0;
        qint64 timestamp = 0;

        // The one display join. Every surface that names a conversation calls
        // this rather than reassembling the fields with its own separator.
        QString label() const;

        QJsonObject toJson() const;
        static ConversationEntry fromJson(const QJsonObject& obj);
    };

    // Properties
    QString selectedProvider() const;
    void setSelectedProvider(const QString& provider);
    QStringList availableProviders() const;
    bool isConfigured() const;
    bool isAnalyzing() const { return m_analyzing; }
    QString lastRecommendation() const { return m_lastRecommendation; }
    QString lastError() const { return m_lastError; }
    QString lastTestResult() const { return m_lastTestResult; }
    bool lastTestSuccess() const { return m_lastTestSuccess; }
    QStringList ollamaModels() const { return m_ollamaModels; }
    QString currentModelName() const;
    Q_INVOKABLE QString modelDisplayName(const QString& providerId) const;
    // Selectable models for a provider as a list of { "id", "name" } maps, in UI
    // order (first = recommended default). Empty when the provider has a single
    // fixed model — the UI hides the model picker in that case.
    Q_INVOKABLE QVariantList availableModels(const QString& providerId) const;
    // One-line guidance comparing the provider's catalog models (see
    // AIProvider::modelHint). Empty when the provider has no hint.
    Q_INVOKABLE QString modelHint(const QString& providerId) const;
    // [barista-fork] Capability of the CURRENTLY selected provider — the barista UI (AssistantOverlay) gates
    // client tools / web on these instead of a hardcoded "anthropic" id, so Gemini (and any future tool-capable
    // provider) drives grind/recipe/taste/memory the same as Claude. See AIProvider::supportsClientTools/WebSearch.
    Q_INVOKABLE bool currentProviderSupportsTools() const;
    Q_INVOKABLE bool currentProviderSupportsWebSearch() const;
    // [barista-fork] Can the selected provider/model read an image on a conversation turn? Gates the barista's
    // "add a bean from a photo" flow — the UI hides/disables the photo affordance and the flow fails honestly
    // when false, rather than silently rerouting to a vision-capable provider (CLAUDE.md provider rule).
    Q_INVOKABLE bool currentProviderSupportsVision() const;
    // [barista-fork] Stage an image (already normalized JPEG/PNG bytes) to ride the NEXT conversation turn's
    // RequestOptions. Consumed and cleared inside analyzeConversation — one turn only, never persisted.
    void stagePendingImage(const QByteArray& data, const QString& mediaType);
    // [barista-fork] Stage the stable-core prefix length for the NEXT turn (Anthropic cache breakpoint). QML sets
    // this to the tailored prompt's core length just before dispatching the turn. One turn only (cleared below).
    Q_INVOKABLE void stageCachePrefixLen(int coreLen) { m_pendingCachePrefixLen = coreLen; }
    // Running-cost estimate (see AIProvider::costHintFor). Pass a modelId to
    // price a specific model, or leave it empty for the provider's current
    // selection. Depends on the model, so re-read it when the selection changes
    // — a per-provider figure is wrong across a catalog that spans 10x. Empty
    // when the provider has no catalog to price.
    Q_INVOKABLE QString costHint(const QString& providerId,
                                 const QString& modelId = QString()) const;
    AIConversation* conversation() const { return m_conversation; }
    bool hasAnyConversation() const { return !m_conversationIndex.isEmpty(); }
    QList<ConversationEntry> conversationIndex() const { return m_conversationIndex; }

    // Index lookup by storage key. Returns a default entry when the key has no
    // index record — legacy or MCP-written conversations are valid without one.
    ConversationEntry conversationEntry(const QString& key) const;

    // Conversation routing
    // Takes the shot, not its fields: the key is derived in exactly one place so
    // the MCP tools and the in-app overlay — which SHARE one conversation —
    // cannot drift into writing to different threads. QVariant for the same
    // reason as isMistakeShot below.
    Q_INVOKABLE QString switchConversation(const QVariant& shotData);
    Q_INVOKABLE void loadMostRecentConversation();
    Q_INVOKABLE void clearCurrentConversation();
    // Accepts QVariant (not const ShotProjection&) so QML can pass either a
    // real ShotProjection or the plain-JS edit clone from
    // PostShotReviewPage.clonePersistedShot — the latter can't bind to a
    // ShotProjection parameter and threw at runtime (#1298). Coerced via
    // coerceShot() in the .cpp.
    Q_INVOKABLE bool isMistakeShot(const QVariant& shotData) const;
    Q_INVOKABLE bool isSupportedBeverageType(const QString& beverageType) const;
    // The one place a conversation key is derived. Identity includes the
    // equipment package: a saved thread replays its turns, so a thread must
    // describe one equipment set for its whole life.
    static QString conversationKey(const ShotProjection& shot);

    // Builds the AI user-prompt envelope for a finished / historical shot,
    // returned as a `QJsonObject` so DB-scoped callers (`ai_advisor_invoke`'s
    // bg-thread closure) can append the four dialing-context blocks before
    // serializing. Returns an empty object when summarization fails. The live
    // advisor + conversation flows summarize from the ShotProjection directly
    // (this and buildShotAnalysisProseForShot below) — there is no separate
    // QVariantMap/ShotMetadata analyze path.
    QJsonObject buildUserPromptObjectForShot(const ShotProjection& shotData);

    // Prose-only shot analysis — no JSON envelope, no double-shipped
    // structured fields. Used by `dialing_get_context` to populate
    // `result.shotAnalysis` (the structured fields already live at the
    // top level of the response), and by the in-app conversation
    // overlay's QML to seed change-detection prose for the AI Advice
    // button (qml/components/ConversationOverlay.qml). The prose is
    // identical to the `shotAnalysis` field inside
    // `buildUserPromptObjectForShot(shot)` when that envelope is built
    // in `Standalone` mode — both paths call
    // `ShotSummarizer::renderShotAnalysisProse` with `RenderMode::Standalone`.
    // QVariant param (not const ShotProjection&) — same reason as
    // isMistakeShot above: QML may pass the plain-JS edit clone. C++ callers
    // (e.g. mcptools_dialing) wrap with QVariant::fromValue(shot).
    Q_INVOKABLE QString buildShotAnalysisProseForShot(const QVariant& shotData);

    // The whole user turn, as one JSON object: the shot's own payload
    // (`shot` / `shotAnalysis` / `currentBean` / `profile` / `tastingFeedback` /
    // `sawPrediction`), the context blocks the last `requestRecentShotContext`
    // resolved, and the user's `question` and `shotLabel` as their own fields.
    //
    // One assembler for both surfaces: `ai_advisor_invoke` builds the same object
    // from the same helpers. The question is a FIELD rather than text
    // concatenated around the object, so reading it back is a field read — the
    // wrapped shape is what forced `getConversationText`'s recovery heuristic and
    // `extractShotFields`' hand-written brace scanner, both of which now serve
    // stored history only.
    Q_INVOKABLE QString buildConversationUserPrompt(const QVariant& shotData,
                                                    const QString& question,
                                                    const QString& shotLabel);

    // Merge the DB-derived context blocks into a user-prompt envelope, and add
    // the one block that cannot come from the database pass: `sawPrediction`
    // touches `Settings::calibration()` and `ProfileManager`, both main-thread
    // only. Both the in-app advisor and `ai_advisor_invoke` call this on the
    // main-thread continuation of their background DB closures. Empty blocks are
    // suppressed — no key, no null placeholder.
    //
    // Single source of truth for the merge step, so the in-app and MCP surfaces
    // cannot drift on which blocks land where.
    //
    // Takes the struct rather than one argument per block, so that a block added
    // to AdvisorContextBlocks reaches both surfaces by construction. With one
    // argument per block, a new block means editing every call site, and a
    // surface that misses the edit silently sends one block fewer.
    void enrichUserPromptObject(QJsonObject& payload,
                                const ShotProjection& shotData,
                                const DialingBlocks::AdvisorContextBlocks& blocks) const;

    // Shot history access for contextual recommendations
    void setShotHistoryStorage(ShotHistoryStorage* storage);
    // [barista-fork] Verbal-feedback KB (assistant.db). Read lazily by the barista client-tool executor and
    // by requestBaristaContext's proactive bean-feedback block — safe to wire after construction.
    void setFeedbackStorage(FeedbackStorage* storage) { m_feedbackStorage = storage; }
    FeedbackStorage* feedbackStorage() const { return m_feedbackStorage; }
    // [barista-fork] Reminders + maintenance store (assistant.db). Read lazily by the barista task tools and
    // by requestBaristaContext's proactive dueItems block — safe to wire after construction.
    void setTasksStorage(TasksStorage* storage) { m_tasksStorage = storage; }
    TasksStorage* tasksStorage() const { return m_tasksStorage; }
    // [barista-fork] Dial-apply handler for the apply_dial_change write tool (approve-then-apply). A std::function
    // seam (not a BaristaActions* member) so this header/TU never names BaristaActions — keeps the machine-source
    // chain out of DB-only tests. Wired from BaristaModule to BaristaActions::applyFromNext.
    void setApplyDialHandler(std::function<QVariantMap(const QVariantMap&, qint64)> handler) {
        m_applyDialHandler = std::move(handler);
    }
    // [barista-fork] end_conversation handler → AssistantOrchestrator::requestDismiss (std::function seam; same
    // rationale as setApplyDialHandler — this header/TU never names AssistantOrchestrator). Wired from
    // BaristaModule; it emits a main-thread signal the overlay acts on AFTER the sign-off is spoken.
    void setEndConversationHandler(std::function<void()> handler) {
        m_endConversationHandler = std::move(handler);
    }
    // [barista-fork] open_bag_camera seam — fired when the model calls the tool to open the in-app camera. Wired
    // from BaristaModule to the orchestrator; emits a main-thread signal the overlay acts on (opens BagCameraCapture).
    void setOpenBagCameraHandler(std::function<void()> handler) {
        m_openBagCameraHandler = std::move(handler);
    }
    // [barista-fork] FAST-PATH web-tool handler for get_weather / get_stock_quote / get_local_news. A
    // std::function seam (not a BaristaWebTools* member) so this header/TU never names BaristaWebTools — keeps
    // QtNetwork's web-tool service out of the DB-only tests. Wired from BaristaModule to BaristaWebTools's async
    // getters (with the homeLocation fallback + query building done there). Signature mirrors the generic client
    // tool executor: (toolName, input, done). Unset → those three tools return an error result.
    void setWebToolsHandler(std::function<void(const QString&, const QJsonObject&,
                                               std::function<void(QJsonValue)>)> handler) {
        m_webToolsHandler = std::move(handler);
    }
    // [barista-fork] Recipes 2.0 barista tools. std::function seams (this header/TU never names MainController)
    // wired from BaristaModule to MainController. getActiveRecipe → the active recipe map or {} (main-thread,
    // sync). deactivateRecipe → {was_active, name} (main-thread, sync). activateRecipe is ASYNC and
    // MACHINE-MUTATING: the handler does the pre-flight (recipe exists + profile resolvable), the
    // MainController::activateRecipe() call, the recipeActivated(id,success) correlation, and a 10s timeout,
    // then replies with a result JSON. Unset → the corresponding tool returns an error/unavailable result.
    void setGetActiveRecipeHandler(std::function<QVariantMap()> handler) {
        m_getActiveRecipeHandler = std::move(handler);
    }
    void setDeactivateRecipeHandler(std::function<QVariantMap()> handler) {
        m_deactivateRecipeHandler = std::move(handler);
    }
    void setActivateRecipeHandler(std::function<void(qint64, std::function<void(QJsonObject)>)> handler) {
        m_activateRecipeHandler = std::move(handler);
    }
    // [barista-fork] update_recipe's seam — mutate a saved recipe's fields, resolve on recipeUpdated.
    void setUpdateRecipeHandler(std::function<void(qint64, const QVariantMap&,
                                                   std::function<void(QJsonObject)>)> handler) {
        m_updateRecipeHandler = std::move(handler);
    }
    // [barista-fork] recipeOp seam: create/clone/archive/delete a recipe (op, args, reply). App-side (needs
    // RecipeStorage + ProfileManager); wired in baristamodule.cpp.
    void setRecipeOpHandler(std::function<void(const QString&, const QVariantMap&,
                                               std::function<void(QJsonObject)>)> handler) {
        m_recipeOpHandler = std::move(handler);
    }
    // [barista-fork] bagOp seam: coffee-bag management (list/create/update/mark_empty/delete). App-side (needs
    // CoffeeBagStorage); one generic (op, args, reply) seam wired in baristamodule.cpp.
    void setBagOpHandler(std::function<void(const QString&, const QVariantMap&,
                                            std::function<void(QJsonObject)>)> handler) {
        m_bagOpHandler = std::move(handler);
    }
    // [barista-fork] list_profiles seam: return the app's usable profiles (query = optional title filter).
    void setListProfilesHandler(std::function<QJsonArray(const QString&)> handler) {
        m_listProfilesHandler = std::move(handler);
    }
    // [barista-fork] Phase 1 identity: set_active_user's seam — sets the active roster user (dyeBarista) on main.
    void setSetActiveUserHandler(std::function<void(const QString&)> handler) {
        m_setActiveUserHandler = std::move(handler);
    }
    // [barista-fork] The app-side provenance snapshot the write tool stamps (see m_lastBaristaAnchorSnapshot).
    QVariantMap lastBaristaAnchorSnapshot() const { return m_lastBaristaAnchorSnapshot; }
    // [barista-fork] Closed-loop bridge for the apply_dial_change WRITE tool (issue #1053 regression). When the
    // model APPLIES a dial change by CALLING apply_dial_change instead of emitting a fenced ```json structuredNext
    // block, the assistant turn would otherwise finalize with structuredNext=nullopt and silently drop out of the
    // recentAdvice audit. The client-tool lambda in createProviders() captures the tool INPUT (grinderSetting/
    // doseG/targetWeightG/ratio/temperatureC — same field names the fenced structuredNext audit reads) into
    // m_pendingToolStructuredNext for the CURRENT turn; AIConversation::onAnalysisComplete take()s it at
    // finalization and, when NO fenced block was emitted, records it as the turn's structuredNext so a tool-applied
    // change is audited exactly like a fenced one. Returns the captured object AND clears it in one call so it can
    // never leak into a later turn — even when a fenced block wins and the taken value is discarded.
    QJsonObject takePendingToolStructuredNext() {
        QJsonObject out = m_pendingToolStructuredNext;
        m_pendingToolStructuredNext = QJsonObject{};
        return out;
    }
    // Null until wired (and in tests that never wire it) — callers must check.
    // True while a metadata write THIS class started is awaiting its outcome.
    //
    // main.qml uses it to suppress ShotHistoryStorage's generic "please try
    // again" toast, which is emitted immediately before shotMetadataCaptureFailed
    // and would otherwise be ANNOUNCED in full to a screen-reader user before the
    // advisor-specific message replaced it on screen. Retrying cannot help when
    // the shot does not exist, so the generic advice is worse than silence.
    //
    // Approximate by design: errorOccurred carries no shot id, so a different
    // subsystem's failure arriving while one of ours is in flight is suppressed
    // too. The window is one queued block wide.
    Q_INVOKABLE bool hasPendingShotMetadataWrite() const { return !m_pendingMetadataWrites.isEmpty(); }

    ShotHistoryStorage* shotHistoryStorage() const { return m_shotHistory; }

    // Inject the TranslationManager so user-visible error strings localize.
    // Forwards to every owned provider and the conversation. Wired directly in
    // main.cpp (after MainController::setAiManager, since MainController's own
    // setTranslationManager runs before the AIManager is attached).
    void setTranslationManager(TranslationManager* tm);
    // ProfileManager hookup for the SAW prediction block (needs
    // baseProfileName + profile target metadata at user-prompt enrichment
    // time). Wired from MainController::setAiManager. Optional — falls
    // back to omitting the SAW block when null.
    void setProfileManager(ProfileManager* profileManager) { m_profileManager = profileManager; }
    // `contextShotId` is the shot to BUILD the context for — the worker loads it
    // whole. It was called `excludeShotId` while it meant only "leave this one
    // out of the history", which stopped being true when the worker started
    // reading the shot from it; qint64 because that is what a shot id is
    // everywhere else.
    Q_INVOKABLE void requestRecentShotContext(const QVariant& shotData, qint64 contextShotId);

    // [barista-fork] Assemble the FULL dialing context (the same rich blocks the advisor uses:
    // dial-in sessions, best recent shot, bean best shot, grinder context/calibration, closed-loop
    // recent advice) anchored on the latest shot for the current bean — falling back to the latest
    // shot overall when the bean name doesn't match. Emits baristaContextReady() on the main thread.
    Q_INVOKABLE void requestBaristaContext(const QString& beanBrand, const QString& beanType, const QString& profileName);
    // [barista-fork] Fire the parallel quick-filler (dedicated Haiku provider) the instant the user's turn is
    // dispatched, so a short spoken "one sec" can play ~2s sooner than the main turn's own lead-in. Silent
    // no-op when there is no Anthropic key. Emits quickFillerReady() on completion (overlay gates whether it
    // actually speaks). `utterance` is the user's words, used only to make the acknowledgement contextual.
    Q_INVOKABLE void requestQuickFiller(const QString& utterance);
    // Anchor shot id resolved by the last requestBaristaContext (0 if none) — for closed-loop turn stamping.
    Q_INVOKABLE qint64 lastBaristaAnchorId() const { return m_lastBaristaAnchorId; }

    // Provider testing
    Q_INVOKABLE void testConnection();

    // Generic analysis - sends system prompt and user prompt to current provider
    Q_INVOKABLE void analyze(const QString& systemPrompt, const QString& userPrompt);

    // Extract structured coffee-bag details from a roaster product page's
    // plain text (add-bag-detail-editing "Get info"). Same provider plumbing
    // as analyze(), but completes via bagDetailsExtracted / -Failed so the
    // advisor's recommendationReceived listeners never see extraction JSON.
    // requestToken (the page URL) is echoed on both completion signals so the
    // caller can discard a stale extraction — an LLM call takes long enough
    // that the user may have moved on to a different bag by the time it lands.
    // Guard failures use stable codes ("busy", "notConfigured", "unreadable")
    // the QML layer translates; provider errors pass through as text.
    // `kind` selects the extraction vocabulary: "coffee" (default) or "tea"
    // (add-recipe-wizard-tea) — tea pages yield teaType/garden/cultivar/flush
    // plus structured brewing fields (brewTempC normalized to Celsius,
    // leafGramsPer100Ml normalized from per-cup wordings, steepTime).
    Q_INVOKABLE void extractCoffeeBagDetails(const QString& requestToken, const QString& pageText,
                                             const QString& kind = QStringLiteral("coffee"));
    // Stage-2 extraction fallback: the local page fetch got nothing (a
    // JS-rendered shop), so the provider fetches the URL itself via its
    // server-side web tool (Anthropic web_fetch, OpenAI Responses web_search,
    // Gemini url_context; Ollama/OpenRouter report "urlFetchUnsupported").
    // Same signals + JSON contract as stage 1, plus an imageUrl key (SPA
    // pages have no og:image for the photo pipeline). Gate calls on
    // supportsUrlExtraction().
    Q_INVOKABLE bool supportsUrlExtraction() const;
    Q_INVOKABLE void extractCoffeeBagDetailsFromUrl(const QString& requestToken, const QString& url,
                                                    const QString& kind = QStringLiteral("coffee"));
    // [barista-fork] Live-coaching phrasebook: ONE bracketing AI call returning model-generated VARIED phrasings
    // per cue id + a pre-shot gameplan (the live coaches' no-canned-strings rule). Own token + signals — never
    // routed to the advisor/conversation. See CoachPhrasebook.
    Q_INVOKABLE void requestCoachPhrasebook(const QString& requestToken, const QString& contextBlock);
    // [barista-fork] Cross-session rolling summary (turn-cost architecture, Step 6). At session close the barista
    // asks the model for 1-2 sentences of DURABLE context worth remembering next time (preferences/plans, NOT
    // shot data). Runs as a separate one-shot call (own flag/token, never the conversation path); the result
    // returns via sessionSummaryReady and QML persists it per user. Own token = the active user to key it by.
    Q_INVOKABLE void requestSessionSummary(const QString& userToken, const QString& conversationText);
    // Response JSON -> whitelisted blob-vocabulary fields (coffee: origin,
    // region, farm, producer, variety, elevation, process, harvest,
    // roastLevel, tastingNotes; tea adds teaType, garden, cultivar, flush,
    // brewTempC, leafGramsPer100Ml, steepTime). Tolerates markdown fences;
    // string-array values are joined ", "; object values are skipped;
    // values capped at 500 chars.
    // ok=false when nothing parses OR the object had content but no usable
    // whitelisted values ("couldn't read it" is distinct from the honest
    // empty-object "the page states nothing"). Static + public for tests.
    static QVariantMap parseBagExtraction(const QString& response, bool* ok = nullptr);

    // Last rung of the photo/details ladder: ask the SELECTED provider (never
    // a substitute) to find a vendor's product page for a named product, using
    // its own web tool. Completes via productPageFound / -Failed. The result
    // is a suggestion the caller must have confirmed before storing — see the
    // bag-detail-editing spec.
    Q_INVOKABLE void findProductPage(const QString& requestToken, const QString& roaster,
                                     const QString& coffee, const QString& kind);
    // Whether the SELECTED provider can search the web (Anthropic, OpenAI and
    // Gemini all can, each with its own tool). Distinct from
    // supportsUrlExtraction(), which is about fetching a URL already known.
    Q_INVOKABLE bool supportsProductPageSearch() const;
    // Which condition declined the AUTOMATIC search. Every gate used to return
    // in silence, so a submitted log could not answer "did the app try?".
    Q_INVOKABLE void logProductPageSearchDeclined(const QString& reason) const;
    // The URL out of that reply's JSON, or empty when the model found none or
    // answered with something that is not an https URL. Static + public for tests.
    static QString parseProductPageUrl(const QString& response);

    // Multi-turn conversation - sends system prompt and full message array to current provider.
    // [barista-fork] webSearch/clientTools ride RequestOptions (default off — advisor unaffected). The
    // barista's client tool definitions + executor live in src/barista/baristatools.{h,cpp}. The default-arg
    // form also covers upstream's plain analyzeConversation(systemPrompt, messages) call sites.
    // [barista-fork] `streaming` = the barista's voiceStreaming flag. The stream is gated OFF whenever web search
    // is on for the turn (a paused server-tool re-POST can't be rebuilt from the stream), so the effective
    // RequestOptions.streaming = streaming && !webSearch — see analyzeConversation.
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                             bool webSearch = false, bool clientTools = false, bool streaming = false);

    // Extract the trailing fenced ```json block from an assistant message.
    // The shot-analysis system prompt asks the model to append a `nextShot`
    // JSON object at end-of-message when its response makes a concrete
    // parameter recommendation. Returns the parsed object when found,
    // std::nullopt when absent or unparseable. Mid-message fenced blocks
    // are intentionally ignored — only a block whose closing ``` is the
    // last non-whitespace content qualifies.
    //
    // Pure / static so callers without an AIManager (test harnesses,
    // ai_advisor_invoke before the provider hop) can use it.
    static std::optional<QJsonObject> parseStructuredNext(const QString& assistantMessage);

    // Strip Decenza-internal per-turn keys (shotId, structuredNext) from a
    // stored conversation, leaving only the {role, content} pair every
    // chat-completion provider accepts. Applied before dispatching to a
    // provider so internal bookkeeping never leaks into the API request —
    // the Anthropic Messages API 400s on unknown per-message fields. Pure /
    // static so the test harness can assert the invariant directly.
    static QJsonArray sanitizeApiMessages(const QJsonArray& messages);

    // Parsed numeric score + remaining notes from a user's conversational
    // reply (issue #1055 Layer 1). When the advisor asks "how did this
    // taste?" and the user answers with a number 1-100, we persist the
    // score back to ShotProjection.enjoyment0to100 + remaining text to
    // espressoNotes — closes the rating loop without forcing the user
    // into the metadata editor.
    struct UserRatingReply {
        int score = 0;     // 1-100
        QString notes;     // remaining text after the score token, trimmed
    };

    // Permissive but conservative parser. A bare integer in [1, 100] is
    // a score; optional suffixes `/100`, `out of 100`, `%` are consumed.
    // Decimal scores round to nearest int. Non-numeric replies ("really
    // good") do NOT yield a score. Multiple numeric tokens → first
    // in-range wins. Static + pure for test isolation.
    static std::optional<UserRatingReply> parseUserRatingReply(const QString& reply);

    // Issue #1055 Layer 1: when the advisor's prior assistant message
    // asked about taste AND the user's reply contains a parseable score,
    // persist the rating + remaining-text notes back to the shot via
    // ShotHistoryStorage. No-op when ANY of:
    //   - shotId is 0 (no shot is paired with the turn — typical for a
    //     legacy conversation or a free-form follow-up),
    //   - m_shotHistory is unset (no DB wired),
    //   - priorAssistantMessage doesn't contain a taste-question marker
    //     (the model wasn't asking; rating writeback would be spurious),
    //   - parseUserRatingReply returns std::nullopt (the user replied
    //     in prose without a numeric score).
    // Called by AIConversation::followUp before the request is dispatched.
    void maybePersistRatingFromReply(const QString& userReply,
                                     const QString& priorAssistantMessage,
                                     qint64 shotId);

    // Conversational bean-metadata corrections (capability
    // shot-metadata-capture). When the recorded bean info on a shot is
    // wrong (e.g. roastLevel saved as "Medium-Dark" but the user clarifies
    // mid-conversation that the coffee is dark), the parser pulls the
    // correction out of the user's reply so the app can write it back to
    // ShotProjection. Sparse: only fields the user explicitly corrected
    // are populated.
    struct BeanCorrection {
        std::optional<QString> roastLevel;  // canonical: Light/Medium-Light/Medium/Medium-Dark/Dark
        std::optional<QString> beanBrand;
        std::optional<QString> roastDate;   // ISO yyyy-MM-dd
        bool isEmpty() const {
            return !roastLevel && !beanBrand && !roastDate;
        }
    };

    // Conservative parser. Returns std::nullopt when the reply contains no
    // recognisable bean-correction patterns. Compound phrases like "dark
    // chocolate notes" or "light citrus" do NOT yield a roastLevel; the
    // parser requires a context word ("the coffee/bean/roast is X",
    // "actually X") to bind the adjective to roast level. Static + pure
    // for test isolation.
    static std::optional<BeanCorrection> parseBeanCorrectionsFromReply(const QString& reply);

    // Persist a parsed BeanCorrection back to the anchored shot via
    // ShotHistoryStorage. No-op when ANY of:
    //   - shotId is 0,
    //   - m_shotHistory is unset,
    //   - parser returns std::nullopt,
    //   - neither the user reply carries explicit corrective phrasing
    //     ("actually...", "the coffee/bean/roast is...") NOR the prior
    //     assistant message asked about beans (the gating mirrors
    //     maybePersistRatingFromReply's "must be answering a question"
    //     stance to keep false positives low).
    // Called by AIConversation::followUp alongside the rating-write hook.
    void maybePersistBeanCorrectionFromReply(const QString& userReply,
                                              const QString& priorAssistantMessage,
                                              qint64 shotId);

    // Ollama-specific
    Q_INVOKABLE void refreshOllamaModels();

signals:
    void providerChanged();
    void configurationChanged();
    void analyzingChanged();
    void recommendationReceived(const QString& recommendation);
    void errorOccurred(const QString& error);
    // "Get info" extraction results (never routed to the advisor signals).
    // requestToken = the value passed to extractCoffeeBagDetails.
    void bagDetailsExtracted(const QString& requestToken, const QVariantMap& fields);
    void bagDetailsExtractionFailed(const QString& requestToken, const QString& error);
    // [barista-fork] Coach-phrasebook results (own signals; never the advisor path).
    void phrasebookReady(const QString& requestToken, const QString& json);
    void sessionSummaryReady(const QString& userToken, const QString& summary);   // [barista-fork] Step 6
    void phrasebookFailed(const QString& requestToken, const QString& error);

    // findProductPage outcome. `url` is a SUGGESTION, not a link: it is probed
    // and confirmed by the user before anything stores it.
    void productPageFound(const QString& requestToken, const QString& url);
    void productPageSearchFailed(const QString& requestToken, const QString& error);
    // A metadata write this class made — capturing something the user told the
    // advisor — landed on a shot id that does not exist, so the value was
    // discarded. Emitted so the failure is addressable instead of vanishing
    // into a log line. What to DO about it (re-ask, retry, tell the user) is
    // deliberately not decided here.
    void shotMetadataCaptureFailed(qint64 shotId);
    void testResultChanged();
    void ollamaModelsChanged();
    void conversationIndexChanged();
    // Bare notification: the context blocks for the pending shot have resolved
    // and are cached. Carries no payload — see emitRecentShotContext.
    void recentShotContextReady();
    void baristaContextReady(const QString& dataBlock);   // [barista-fork]
    void conversationResponseReceived(const QString& response);
    // [barista-fork] Interim (pre-tool) prose for a conversation turn — the provider's lead-in ("let me pull
    // that up") emitted BEFORE a tool/search runs, so the barista can speak it right away instead of sitting
    // silent. Only re-emitted for a conversation request (mirrors conversationResponseReceived gating).
    void conversationInterimText(const QString& text);
    // [barista-fork] Streaming voice: the provider's streamTextDelta / streamTextEnd, re-emitted for a live
    // conversation turn only (mirrors conversationInterimText gating). The module wires these C++-direct to the
    // voice layer, bypassing QML, so streamed chunks speak as they arrive without the overlay also speaking the
    // final answer (double-speak). Fire only on a streaming turn (the provider raises them only then).
    void conversationStreamText(const QString& text);
    void conversationStreamEnd();
    void conversationErrorOccurred(const QString& error);
    // [barista-fork] A tiny model-generated "give me a sec" filler, produced by the dedicated Haiku provider
    // in parallel with the main turn so it can be spoken ~2s sooner. The overlay gates it (only speaks if the
    // real answer hasn't already landed) — see AssistantOverlay onQuickFillerReady.
    void quickFillerReady(const QString& text);

private slots:
    void onAnalysisComplete(const QString& response);
    void onInterimText(const QString& text);   // [barista-fork] route provider interimText → conversation
    void onStreamText(const QString& text);     // [barista-fork] route provider streamTextDelta → conversation (streaming)
    void onStreamEnd();                          // [barista-fork] route provider streamTextEnd → conversation (streaming)
    void onQuickFillerReady(const QString& text);   // [barista-fork] filler provider analysisComplete → quickFillerReady
    void onAnalysisFailed(const QString& error);
    void onTestResult(bool success, const QString& message);
    void onOllamaModelsRefreshed(const QStringList& models);
    void onSettingsChanged();

private:
    void createProviders();
    // Translate a user-visible string via the injected TranslationManager,
    // falling back to the English source when none is set.
    QString tr_(const char* key, const char* fallback) const;
    AIProvider* providerById(const QString& providerId) const;
    AIProvider* currentProvider() const;

    // Logging
    QString logPath() const;
    void logPrompt(const QString& provider, const QString& systemPrompt, const QString& userPrompt);
    void logResponse(const QString& provider, const QString& response, bool success);

    Settings* m_settings = nullptr;
    QNetworkAccessManager* m_networkManager = nullptr;
    std::unique_ptr<ShotSummarizer> m_summarizer;
    ShotHistoryStorage* m_shotHistory = nullptr;
    FeedbackStorage* m_feedbackStorage = nullptr;   // [barista-fork] verbal-feedback KB (assistant.db)
    TasksStorage* m_tasksStorage = nullptr;         // [barista-fork] reminders + maintenance (assistant.db)
    // [barista-fork] apply_dial_change handler → BaristaActions::applyFromNext (std::function seam; see setter).
    std::function<QVariantMap(const QVariantMap&, qint64)> m_applyDialHandler;
    // [barista-fork] end_conversation handler → AssistantOrchestrator::requestDismiss (std::function seam; see setter).
    std::function<void()> m_endConversationHandler;
    std::function<void()> m_openBagCameraHandler;   // [barista-fork] open_bag_camera seam
    // [barista-fork] fast-path web-tool handler → BaristaWebTools async getters (std::function seam; see setter).
    std::function<void(const QString&, const QJsonObject&, std::function<void(QJsonValue)>)> m_webToolsHandler;
    // [barista-fork] Recipes 2.0 tool handlers → MainController (std::function seams; see setters).
    std::function<QVariantMap()> m_getActiveRecipeHandler;
    std::function<QVariantMap()> m_deactivateRecipeHandler;
    std::function<void(qint64, std::function<void(QJsonObject)>)> m_activateRecipeHandler;
    std::function<void(qint64, const QVariantMap&, std::function<void(QJsonObject)>)> m_updateRecipeHandler;
    std::function<void(const QString&, const QVariantMap&, std::function<void(QJsonObject)>)> m_recipeOpHandler;
    std::function<void(const QString&, const QVariantMap&, std::function<void(QJsonObject)>)> m_bagOpHandler;
    std::function<QJsonArray(const QString&)> m_listProfilesHandler;
    std::function<void(const QString&)> m_setActiveUserHandler;   // [barista-fork] Phase 1 set_active_user seam
    // Shot ids this class has a metadata write in flight for, REFCOUNTED: one
    // reply can drive two writes for the same shot, and shotMetadataUpdated
    // carries every subsystem's writes, so without this we would report other
    // people's failures as ours. See setShotHistoryStorage.
    QHash<qint64, int> m_pendingMetadataWrites;
    ProfileManager* m_profileManager = nullptr;

    // Providers
    std::unique_ptr<AIProvider> m_openaiProvider;
    std::unique_ptr<AIProvider> m_anthropicProvider;
    std::unique_ptr<AIProvider> m_geminiProvider;
    std::unique_ptr<AIProvider> m_openrouterProvider;
    std::unique_ptr<AIProvider> m_ollamaProvider;
    // [barista-fork] Dedicated Haiku provider for the parallel "quick filler" — a tiny, fast "one sec" the
    // barista can speak ~2s sooner than the main turn's own lead-in. Kept OUT of providerById/currentProvider
    // so it is never selected as the main provider; only ever driven by requestQuickFiller(). Null when there
    // is no Anthropic key (silent no-op → current behavior). Each requestQuickFiller() captures its own gen in a
    // per-request lambda (m_fillerConn, reconnected each call); a completion whose captured gen != m_fillerGen is a
    // superseded turn and is dropped. This is the REAL staleness guard — the provider does not abort in-flight
    // requests and analysisComplete carries no gen, so a plain slot could speak an old turn's filler over a new one.
    std::unique_ptr<AIProvider> m_fillerProvider;
    int m_fillerGen = 0;
    QMetaObject::Connection m_fillerConn;

    // State
    bool m_analyzing = false;
    QString m_lastRecommendation;
    QString m_lastError;
    QString m_lastTestResult;
    bool m_lastTestSuccess = false;
    QStringList m_ollamaModels;

    // For logging - store last prompts to pair with response
    QString m_lastSystemPrompt;
    QString m_lastUserPrompt;

    // Serial counter for requestRecentShotContext (discard stale results)
    int m_contextSerial = 0;

    // [barista-fork] anchor shot id from the last requestBaristaContext, so the overlay can stamp the
    // barista's advice turns (setShotIdForCurrentTurn) into the recentAdvice closed loop.
    qint64 m_lastBaristaAnchorId = 0;
    // [barista-fork] App-side provenance snapshot for a log_tasting_feedback write, resolved by the last
    // requestBaristaContext: the CURRENT bean/type/profile + the anchor shot's id and dial (dose/yield/
    // grind/temp). The write executor merges the model's validated taste fields on top of this — shot_id and
    // dial are NEVER taken from the model. shotId 0 when the current bean has no matching shot (bean-general).
    QVariantMap m_lastBaristaAnchorSnapshot;
    // [barista-fork] Pending tool-applied structuredNext for the CURRENT turn (issue #1053 regression). Set by the
    // apply_dial_change branch of the client-tool lambda in createProviders(); take()n + cleared at conversation
    // finalization (AIConversation::onAnalysisComplete) and again unconditionally at the top of analyzeConversation
    // (the single choke point for every turn) so a failed / superseded tool-turn can't leak into the next turn.
    QJsonObject m_pendingToolStructuredNext;
    // Dedicated serial for requestBaristaContext — must NOT share m_contextSerial with
    // requestRecentShotContext, or one silently invalidates the other's callback (S1).
    int m_baristaContextSerial = 0;

public:
    void reloadConversations() { loadConversationIndex(); }
private:
    void loadConversationIndex();
    void saveConversationIndex();
    void touchConversationEntry(const QString& key);
    void evictOldestConversation();
    void migrateFromLegacyConversation();
    // One-shot conversation wipe keyed by a migration id. Fires once per
    // device; subsequent launches are no-ops. Call before loadConversationIndex.
    static void clearAllConversationsOnce(const QString& migrationId);

    // Cache the resolved context blocks and emit `recentShotContextReady`
    // (empty when stale). `requestRecentShotContext`'s main-thread lambda calls
    // this after the background DB work resolves; the cached blocks are what
    // `buildConversationUserPrompt` folds into the turn's payload.
    void emitRecentShotContext(const DialingBlocks::AdvisorContextBlocks& blocks,
                               qint64 contextShotId,
                               int serial);

    // The blocks the last completed context request resolved, and the shot they
    // were resolved for. Guarded by the id so a payload can never be built from
    // another shot's context — the request is asynchronous and the user can open
    // a different shot while one is in flight.
    DialingBlocks::AdvisorContextBlocks m_contextBlocks;
    qint64 m_contextBlocksShotId = 0;

    // Conversation for multi-turn interactions
    AIConversation* m_conversation = nullptr;
    TranslationManager* m_translationManager = nullptr;
    QList<ConversationEntry> m_conversationIndex;
    bool m_isConversationRequest = false;
    bool m_isBagExtractionRequest = false;
    QString m_bagExtractionToken;
    bool m_isCoachPhrasebookRequest = false;   // [barista-fork]
    QString m_coachPhrasebookToken;            // [barista-fork]
    bool m_isSessionSummaryRequest = false;    // [barista-fork] Step 6 rolling summary
    QString m_sessionSummaryUser;              // [barista-fork] the user this summary is keyed to
    // [barista-fork] Image staged for the NEXT conversation turn (add-a-bean-from-a-photo). Consumed+cleared in
    // analyzeConversation, so it rides exactly one turn and never enters the persisted message history.
    QByteArray m_pendingImageData;
    QString m_pendingImageMediaType;
    // [barista-fork] Length (chars) of the stable core prefix of the NEXT turn's system prompt, so Anthropic can
    // cache the core across the per-question tailoring's varying suffix. -1 ⇒ cache the whole prompt (untailored).
    // Consumed+cleared in analyzeConversation → rides exactly one turn.
    int m_pendingCachePrefixLen = -1;
    bool m_isProductPageSearch = false;
    QString m_productPageToken;

#ifdef DECENZA_TESTING
    friend class tst_AIManager;
#endif
};
