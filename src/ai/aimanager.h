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
        qint64 timestamp;

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
    AIConversation* conversation() const { return m_conversation; }
    bool hasAnyConversation() const { return !m_conversationIndex.isEmpty(); }
    QList<ConversationEntry> conversationIndex() const { return m_conversationIndex; }

    // Conversation routing
    Q_INVOKABLE QString switchConversation(const QString& beanBrand, const QString& beanType, const QString& profileName);
    Q_INVOKABLE void loadMostRecentConversation();
    Q_INVOKABLE void clearCurrentConversation();
    // Accepts QVariant (not const ShotProjection&) so QML can pass either a
    // real ShotProjection or the plain-JS edit clone from
    // PostShotReviewPage.clonePersistedShot — the latter can't bind to a
    // ShotProjection parameter and threw at runtime (#1298). Coerced via
    // coerceShot() in the .cpp.
    Q_INVOKABLE bool isMistakeShot(const QVariant& shotData) const;
    Q_INVOKABLE bool isSupportedBeverageType(const QString& beverageType) const;
    static QString conversationKey(const QString& beanBrand, const QString& beanType, const QString& profileName);

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

    // Merge the four dialing-context blocks into a user-prompt envelope.
    // Both the in-app advisor and `ai_advisor_invoke` call this on the
    // main-thread continuation of their bg-thread DB closures, after they
    // produce `dialInSessions` / `bestRecentShot` / `grinderContext` from
    // their own DB connections. The SAW block is built here (it touches
    // `Settings::calibration()` and `ProfileManager`, both main-thread
    // only). Empty blocks are suppressed — no key, no null placeholder.
    //
    // Single source of truth for the merge step, so the in-app and MCP
    // surfaces cannot drift on which blocks land where.
    void enrichUserPromptObject(QJsonObject& payload,
                                const ShotProjection& shotData,
                                const QJsonArray& dialInSessions,
                                const QJsonObject& bestRecentShot,
                                const QJsonObject& grinderContext,
                                const QJsonArray& recentAdvice = QJsonArray(),
                                const QJsonObject& grinderCalibration = QJsonObject(),
                                const QJsonObject& beanBestShot = QJsonObject()) const;

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
    Q_INVOKABLE void requestRecentShotContext(const QString& beanBrand, const QString& beanType, const QString& profileName, int excludeShotId);

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

    // Multi-turn conversation - sends system prompt and full message array to current provider.
    // [barista-fork] clientTools enables the barista's registered client-side tools (default off — advisor
    // unaffected). The tool definitions + executor live in src/barista/baristatools.{h,cpp}.
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                             bool webSearch = false, bool clientTools = false);

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
    void phrasebookFailed(const QString& requestToken, const QString& error);
    void testResultChanged();
    void ollamaModelsChanged();
    void conversationIndexChanged();
    void recentShotContextReady(const QString& context);
    void baristaContextReady(const QString& dataBlock);   // [barista-fork]
    void conversationResponseReceived(const QString& response);
    // [barista-fork] Interim (pre-tool) prose for a conversation turn — the provider's lead-in ("let me pull
    // that up") emitted BEFORE a tool/search runs, so the barista can speak it right away instead of sitting
    // silent. Only re-emitted for a conversation request (mirrors conversationResponseReceived gating).
    void conversationInterimText(const QString& text);
    void conversationErrorOccurred(const QString& error);
    // [barista-fork] A tiny model-generated "give me a sec" filler, produced by the dedicated Haiku provider
    // in parallel with the main turn so it can be spoken ~2s sooner. The overlay gates it (only speaks if the
    // real answer hasn't already landed) — see AssistantOverlay onQuickFillerReady.
    void quickFillerReady(const QString& text);

private slots:
    void onAnalysisComplete(const QString& response);
    void onInterimText(const QString& text);   // [barista-fork] route provider interimText → conversation
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
    // [barista-fork] fast-path web-tool handler → BaristaWebTools async getters (std::function seam; see setter).
    std::function<void(const QString&, const QJsonObject&, std::function<void(QJsonValue)>)> m_webToolsHandler;
    // [barista-fork] Recipes 2.0 tool handlers → MainController (std::function seams; see setters).
    std::function<QVariantMap()> m_getActiveRecipeHandler;
    std::function<QVariantMap()> m_deactivateRecipeHandler;
    std::function<void(qint64, std::function<void(QJsonObject)>)> m_activateRecipeHandler;
    std::function<void(const QString&)> m_setActiveUserHandler;   // [barista-fork] Phase 1 set_active_user seam
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
    // is no Anthropic key (silent no-op → current behavior). m_fillerInFlightGen guards a superseded filler
    // from being spoken after a newer requestQuickFiller() bumps m_fillerGen.
    std::unique_ptr<AIProvider> m_fillerProvider;
    int m_fillerGen = 0;
    int m_fillerInFlightGen = -1;

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

    // Render the recent-shot-context prose from already-loaded data and
    // emit `recentShotContextReady` (or an empty string when stale).
    // `requestRecentShotContext`'s main-thread lambda calls this helper
    // after the background DB work resolves. Extracted so the
    // canonical-source separation logic (Profile/Setup hoisting,
    // HistoryBlock per-shot rendering) can be exercised by tests via
    // `friend class tst_AIManager` without standing up a real DB.
    void emitRecentShotContext(
        const QList<QPair<qint64, ShotProjection>>& qualifiedShots,
        const GrinderContext& grinderCtx,
        const QString& grinderBrand,
        int serial,
        const QJsonObject& grinderCalibration = QJsonObject(),
        const QJsonArray& recentAdvice = QJsonArray());

    // Conversation for multi-turn interactions
    AIConversation* m_conversation = nullptr;
    TranslationManager* m_translationManager = nullptr;
    QList<ConversationEntry> m_conversationIndex;
    bool m_isConversationRequest = false;
    bool m_isBagExtractionRequest = false;
    QString m_bagExtractionToken;
    bool m_isCoachPhrasebookRequest = false;   // [barista-fork]
    QString m_coachPhrasebookToken;            // [barista-fork]

#ifdef DECENZA_TESTING
    friend class tst_AIManager;
#endif
};
