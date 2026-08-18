#pragma once

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <functional>

class TranslationManager;

// Abstract base class for AI providers
class AIProvider : public QObject {
    Q_OBJECT

public:
    enum class Status { Ready, Busy, Error };
    Q_ENUM(Status)

    // One selectable model offered by a provider. `id` is the wire model
    // string sent to the API; `displayName` is the short label shown in the UI.
    struct ModelOption {
        QString id;
        QString displayName;
    };

    // [barista-fork] Per-request options threaded through analyzeConversation. Only the barista sets
    // webSearch / clientTools; every other caller uses the defaults (off), so the advisor/coach are unaffected.
    struct RequestOptions {
        bool webSearch = false;
        bool clientTools = false;   // enable the registered client-side tools (see setClientTools) for this turn
        int timeoutMs = 0;          // [barista-fork] per-turn network transfer timeout; 0 → ANALYSIS_TIMEOUT_MS
        bool forceRespond = false;  // [barista-fork] Anthropic-only: tool_choice:"any" + a `respond` answer tool,
                                    // so the model can NEVER end a turn with a bare "let me check…" promise and no
                                    // tool call (the stall bug). It must call a real tool (→ loop) or `respond`
                                    // (→ the answer). Gated to the barista conversation; ignored by other providers.
        // [barista-fork] Optional image for a VISION turn (e.g. reading a coffee bag's label off a photo).
        // Rides the per-turn options, NEVER the persisted messages array: an image in history would re-bill on
        // every follow-up turn and pollute the cached first-user block. The vision-capable providers attach it
        // to the CURRENT (last user) message only; non-vision providers ignore it (gate on supportsVision()).
        QByteArray imageData;          // raw JPEG/PNG bytes; empty ⇒ no image this turn
        QString imageMediaType;        // e.g. "image/jpeg" or "image/png"; empty with data ⇒ defaults to image/jpeg
    };

    explicit AIProvider(QNetworkAccessManager* networkManager, QObject* parent = nullptr);
    virtual ~AIProvider() = default;

    // Inject the TranslationManager so user-visible error/status strings
    // localize. Set by AIManager for every provider it owns; until injected,
    // tr_() returns the English fallback.
    void setTranslationManager(TranslationManager* tm) { m_translationManager = tm; }

    virtual QString name() const = 0;
    virtual QString id() const = 0;  // "openai", "anthropic", "gemini", "ollama"
    virtual QString modelName() const = 0;
    virtual QString shortModelName() const { return modelName(); }
    virtual bool isConfigured() const = 0;
    virtual bool isLocal() const { return false; }

    // [barista-fork] Provider capability flags for the barista's provider-agnostic UI gating (AssistantOverlay).
    // supportsClientTools: the provider runs the registered client-tool function-calling loop (grind/recipe/
    // taste/memory/end_conversation) — Anthropic and Gemini. supportsWebSearch: the provider offers real
    // server-side GENERAL web search — Anthropic only (Gemini has just the keyless get_weather/stock/news tools,
    // which ship as client tools). Default false; the barista gates tools/web/persona on these, never on a
    // hardcoded provider id, so any future tool-capable provider lights up automatically.
    virtual bool supportsClientTools() const { return false; }
    virtual bool supportsWebSearch() const { return false; }

    // Models the user may pick between for this provider. Default empty = the
    // provider has a single fixed model and shows no model picker. Providers
    // override this to opt into user-selectable models; the catalog is the
    // single source of truth for both the UI list and `shortModelName()`.
    virtual QList<ModelOption> availableModels() const { return {}; }

    // One-line guidance comparing the catalog's models, shown under the model
    // picker in both the in-app AI settings tab and the ShotServer web page.
    // Lives next to availableModels() so the catalog and its guidance share a
    // single source and can't drift between UIs. Empty = no hint.
    virtual QString modelHint() const { return {}; }

    // One-line running-cost estimate for a specific model, shown under the
    // model picker. Takes the model explicitly rather than reading the current
    // selection so a caller can price a model the user has not chosen yet —
    // the ShotServer page needs the whole catalog priced up front, because it
    // switches models client-side with no round trip.
    //
    // Must depend on the model, not just the provider: the OpenAI catalog alone
    // spans 10x. The per-provider strings this replaced understated the cost by
    // 5x to 8x depending on which model was selected — Anthropic claimed
    // ~$0.01/shot against $0.056 for Sonnet 4.6 (5.6x), OpenAI claimed
    // ~$0.006/shot against $0.038 for Terra (6.3x) and $0.047 for GPT-5.4
    // (7.8x) — and claimed "under $1/month" for a combination costing about $5.
    //
    // Empty when the provider has no catalog to price (OpenRouter, Ollama).
    virtual QString costHintFor(const QString& modelId) const { Q_UNUSED(modelId); return {}; }

    // The estimate for whatever model is selected right now.
    QString costHint() const { return costHintFor(modelName()); }

    Status status() const { return m_status; }

    // Main analysis method
    virtual void analyze(const QString& systemPrompt, const QString& userPrompt) = 0;

    // Multi-turn conversation method (messages = array of {role, content} objects)
    virtual void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages);
    // [barista-fork] Options-aware overload. Base forwards to the 2-arg version (options ignored), so
    // OpenAI/Gemini/OpenRouter/Ollama gracefully no-op web search; AnthropicProvider overrides it.
    virtual void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                                     const RequestOptions& options);

    // Server-side URL retrieval (add-recipe-wizard-tea, stage-2 extraction):
    // analyze() with the provider's web-fetch tool enabled, so the PROVIDER
    // fetches a URL named in the user prompt — the fallback when the local
    // page fetch got nothing (JS-rendered shops). Anthropic (web_fetch),
    // OpenAI (Responses API web_search), and Gemini (url_context) support
    // it; providers without a server-side fetch tool (Ollama, OpenRouter)
    // keep the default (unsupported).
    virtual bool supportsUrlAnalysis() const { return false; }
    // [barista-fork] Can this provider carry an image to the model on a conversation turn (RequestOptions.
    // imageData)? Only the providers whose message-build attaches the image return true (Anthropic, Gemini).
    // A caller MUST NOT route a vision turn to a different provider because this one can't — the selected
    // provider/model is the one that runs, and the honest answer when it can't read images is to say so
    // (CLAUDE.md: never silently substitute a provider). Default false.
    virtual bool supportsVision() const { return false; }
    virtual void analyzeUrl(const QString& systemPrompt, const QString& userPrompt) {
        Q_UNUSED(systemPrompt); Q_UNUSED(userPrompt);
        emit analysisFailed(tr_("ai.error.urlNotSupported", "URL analysis not supported by this provider"));
    }

    // Test connection
    virtual void testConnection() = 0;

signals:
    void analysisComplete(const QString& response);
    void analysisFailed(const QString& error);
    // [barista-fork] Interim (pre-tool) prose. When the model writes a short lead-in ("let me pull that up")
    // BEFORE a tool_use / pause_turn, that text is emitted here immediately — so the UI can speak it while the
    // tool runs (fills the silence with the model's OWN words), instead of buffering it until the whole turn
    // completes. Emitted at most once per top-level turn's first tool round; only AnthropicProvider raises it.
    // The final analysisComplete then carries ONLY the post-tool answer (no re-fold), so nothing double-speaks.
    void interimText(const QString& text);
    void statusChanged(Status status);
    void testResult(bool success, const QString& message);

protected:
    void setStatus(Status status);

    // Map Qt network errors to user-friendly messages (localized via tr_).
    QString friendlyNetworkError(QNetworkReply* reply) const;

    // Translate a user-visible string via the injected TranslationManager,
    // falling back to the English source when none is set.
    QString tr_(const char* key, const char* fallback) const;

    // Build OpenAI-compatible messages array: system message + conversation messages
    static QJsonArray buildOpenAIMessages(const QString& systemPrompt, const QJsonArray& messages);

    // Retry support: each sendRequest() assigns m_retryFn; each onAnalysisReply() calls tryScheduleRetry()
    bool tryScheduleRetry(QNetworkReply* reply);  // returns true if retry was scheduled

    // What to do when a reply stops because it hit the output cap.
    //
    // A one-shot analysis (analyze/analyzeUrl) is parsed by machine: the caller
    // reads a trailing fenced `nextShot` JSON block (#1054, parsed by
    // AIManager::parseStructuredNext), which a cut-off reply has lost. A
    // half-parsed result is worse than no result, so those paths fail.
    //
    // A conversation turn (analyzeConversation) is prose the user reads, so
    // several thousand useful tokens beat an error — show them with a notice
    // appended. Each analyze*() sets the policy before sending; the shared
    // reply handler reads it.
    enum class TruncationPolicy { Fail, ShowPartial };
    TruncationPolicy m_truncationPolicy = TruncationPolicy::Fail;

    // User-visible error for a reply that hit MAX_OUTPUT_TOKENS. Shared across
    // providers: every one of them can truncate, and the user's remedy is the
    // same regardless of which API produced it.
    QString truncatedResponseError() const;

    // Notice appended to a partial reply under TruncationPolicy::ShowPartial.
    QString truncationNotice() const;

    // Shared exit for "the reply is empty, or was cut off, or both". Returns
    // true when it has emitted (analysisComplete for an allowed partial,
    // analysisFailed otherwise) and the caller must return; false when the
    // reply is fine and the caller should carry on to emit it.
    //
    // Factored out because all five providers need identical policy on top of
    // five different vendor spellings of "why I stopped" — the divergence
    // belongs in each handler's parsing, not in what we do about it.
    bool dispatchTruncatedOrEmpty(const QString& text, bool truncated,
                                  const QString& emptyMessage);

    // Bound an untrusted provider error body before it reaches the log.
    //
    // These logs get attached to public GitHub issues, and a provider's 4xx
    // body echoes fragments of the request back: Anthropic quotes the offending
    // field and its value, OpenAI's moderation errors quote the flagged prompt
    // text. Our prompts carry the user's shot history, bean names and tasting
    // notes (AIManager::sanitizeApiMessages). Log the machine-readable type and
    // a bounded prefix, never the whole body.
    static QString logSafeErrorBody(const QByteArray& body);

    static constexpr int ANALYSIS_TIMEOUT_MS = 60000;   // 60s for cloud AI analysis
    static constexpr int TEST_TIMEOUT_MS = 15000;        // 15s for connection tests
    static constexpr int MAX_RETRIES = 3;                // max retries for 429/502/503/504
    static constexpr int LOG_BODY_LIMIT = 200;           // chars of a provider error body we log

    // Output cap for every analysis request, on every provider.
    //
    // Was 1024, which was too tight for the job: a dial-in reply plus the
    // trailing fenced `nextShot` JSON block (#1054) lands right at that edge,
    // and no provider inspected its stop/finish reason — so a capped reply was
    // emitted as if complete, silently missing the JSON block.
    //
    // #1691 was the acute form, on Anthropic; the mechanism is recorded once at
    // disableAnthropicThinking() in aiprovider.cpp rather than restated here.
    //
    // Raising the cap is close to free: it is a ceiling, not a reservation —
    // billing is on tokens actually produced.
    static constexpr int MAX_OUTPUT_TOKENS = 4096;

    QNetworkAccessManager* m_networkManager = nullptr;
    TranslationManager* m_translationManager = nullptr;
    Status m_status = Status::Ready;
    std::function<void()> m_retryFn;  // set by each sendRequest() to re-send the pending request
    int m_retryCount = 0;             // reset to 0 before each new analyze() / analyzeConversation() call
    int m_reqGen = 0;                 // incremented on each new request; guards against stale retry timers

private:
    static bool isRetryableHttpStatus(int httpStatus, int retryCount);
    static int computeRetryDelayMs(int retryCount, QNetworkReply* reply);
};

// OpenAI provider
class OpenAIProvider : public AIProvider {
    Q_OBJECT

public:
    explicit OpenAIProvider(QNetworkAccessManager* networkManager,
                            const QString& apiKey,
                            QObject* parent = nullptr);

    QString name() const override { return "OpenAI"; }
    QString id() const override { return "openai"; }
    QString modelName() const override { return m_model; }
    QString shortModelName() const override;  // catalog display for m_model
    bool isConfigured() const override { return !m_apiKey.isEmpty(); }
    QList<ModelOption> availableModels() const override;
    QString modelHint() const override;
    QString costHintFor(const QString& modelId) const override;

    void setApiKey(const QString& key) { m_apiKey = key; }
    // empty → keeps default upstream URL
    void setBaseUrl(const QString& url) { m_baseUrl = url.endsWith(QLatin1Char('/')) ? url.chopped(1) : url; }
    // Select the wire model. Ignores empty (keeps current default) and any id
    // not in availableModels(), so a stale/unknown stored value can't break the
    // request.
    void setModel(const QString& modelId);

    void analyze(const QString& systemPrompt, const QString& userPrompt) override;
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages) override;
    // OpenAI web search on the Responses API (chat/completions has no general
    // web tool): the model can open a specific URL from the prompt via the
    // tool's open_page action. Uses reasoning effort "low" — the gpt-5.4
    // generation's floor is "none" (it dropped "minimal") and rejects
    // web_search there. The 5.6 generation accepts web_search at "none", so
    // "low" is the value valid across the whole catalog, not a universal
    // web_search requirement.
    bool supportsUrlAnalysis() const override { return true; }
    void analyzeUrl(const QString& systemPrompt, const QString& userPrompt) override;
    void testConnection() override;

private slots:
    void onAnalysisReply(QNetworkReply* reply);
    void onResponsesReply(QNetworkReply* reply);
    void onTestReply(QNetworkReply* reply);

private:
    void sendRequest(const QJsonObject& requestBody);
    void sendResponsesRequest(const QJsonObject& requestBody);

    QString m_apiKey;
    QString m_baseUrl;
    // Selected wire model. Defaulted in the constructor to the first
    // availableModels() entry (the recommended default), so the C++ default and
    // the UI's "unset → index 0" fallback reference the same fact and can't drift.
    QString m_model;
    static constexpr const char* API_URL = "https://api.openai.com/v1/chat/completions";
    static constexpr const char* RESPONSES_API_URL = "https://api.openai.com/v1/responses";
};

// Anthropic provider
class AnthropicProvider : public AIProvider {
    Q_OBJECT

public:
    explicit AnthropicProvider(QNetworkAccessManager* networkManager,
                               const QString& apiKey,
                               QObject* parent = nullptr);

    QString name() const override { return "Anthropic"; }
    QString id() const override { return "anthropic"; }
    QString modelName() const override { return m_model; }
    QString shortModelName() const override;  // catalog display for m_model
    bool isConfigured() const override { return !m_apiKey.isEmpty(); }
    bool supportsClientTools() const override { return true; }  // [barista-fork] tool_use loop
    bool supportsWebSearch() const override { return true; }    // [barista-fork] server web_search
    QList<ModelOption> availableModels() const override;
    QString modelHint() const override;
    QString costHintFor(const QString& modelId) const override;

    void setApiKey(const QString& key) { m_apiKey = key; }
    // empty → keeps default upstream URL
    void setBaseUrl(const QString& url) { m_baseUrl = url.endsWith(QLatin1Char('/')) ? url.chopped(1) : url; }
    // Select the wire model. Ignores empty (keeps current default) and any id
    // not in availableModels(), so a stale/unknown stored value can't break the
    // request.
    void setModel(const QString& modelId);
    // [barista-fork] Set the wire model bypassing the availableModels() allow-list. ONLY for internal,
    // non-user-selectable providers (e.g. the AIManager quick-filler provider pinned to Haiku) — the UI
    // model picker still routes through setModel(), so this can't surface an unlisted model to users.
    void setModelUnchecked(const QString& modelId) { m_model = modelId; }

    void analyze(const QString& systemPrompt, const QString& userPrompt) override;
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages) override;
    // [barista-fork] Options-aware overload — the barista threads per-call web-search / client-tools config.
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                             const RequestOptions& options) override;
    // Anthropic web_fetch server tool (web_fetch_20250910): the API fetches
    // the URL named in the user prompt during the request — no client-side
    // round trip. URL validation requires the URL to appear in the message,
    // which the extraction prompt guarantees.
    bool supportsUrlAnalysis() const override { return true; }
    bool supportsVision() const override { return true; }   // [barista-fork] Claude models read images
    void analyzeUrl(const QString& systemPrompt, const QString& userPrompt) override;
    void testConnection() override;

    // [barista-fork] Attach an image block to the LAST user message for a vision turn (Anthropic content-array
    // format). Pure transform, public+static so it is unit-testable without a live call. Converts a string
    // content to `[{type:text},{type:image}]`, or appends the image block to an existing content array (the
    // single-message case, where messagesWithCachedFirstUser already wrapped it). No-op if there is no user
    // message. mediaType empty ⇒ image/jpeg.
    static QJsonArray messagesWithImageOnLastUser(const QJsonArray& messages, const QByteArray& imageData,
                                                  const QString& mediaType);

    // [barista-fork] Generic client-side-tool seam. A feature module (the barista) registers BOTH the tool
    // JSON definitions and the executor here; the provider knows nothing about which tools they are. The
    // definitions are appended to the request when RequestOptions.clientTools is set, and the generic
    // tool_use loop in onAnalysisReply drives the executor. The executor runs the actual work OFF the main
    // thread and delivers the JSON result via the `done` callback (invoked back on the main thread), so the
    // tablet UI never freezes mid "thinking" animation. Set once at construction; callers that never enable
    // clientTools (advisor/coach) are entirely unaffected.
    void setClientTools(const QJsonArray& defs,
                        std::function<void(const QString&, const QJsonObject&,
                                           std::function<void(QJsonValue)>)> exec) {
        m_clientToolDefs = defs;
        m_toolExecutor = std::move(exec);
    }

    // [barista-fork] FAST-PATH web tool definitions. Registered SEPARATELY from setClientTools and appended to
    // the request ONLY when RequestOptions.webSearch is set — the SAME gate as Anthropic's web_search (both mean
    // "the barista may reach the internet"), NOT the clientTools/query_shots gate. There is NO separate executor:
    // these tools flow through the SAME m_toolExecutor / tool_use loop as the client tools (the executor
    // dispatches by tool name). That loop runs on any tool_use whenever an executor exists (see onAnalysisReply)
    // — and the QML only turns webSearch on when clientTools is also on (webOn ⇒ toolsOn), so the executor is
    // always present when these defs ship. Set once at construction; callers that never enable webSearch
    // (advisor/coach) never receive them.
    void setWebTools(const QJsonArray& defs) { m_webToolDefs = defs; }

private slots:
    void onAnalysisReply(QNetworkReply* reply);
    void onTestReply(QNetworkReply* reply);

private:
    void sendRequest(const QJsonObject& requestBody);
    static QJsonArray buildCachedSystemPrompt(const QString& systemPrompt);

    // [barista-fork] server-side web search continuation state. When the model pauses mid-turn to run a
    // search (stop_reason "pause_turn"), we re-POST the accumulated turn until it completes (bounded).
    QJsonObject m_pendingRequestBody;
    int m_continuations = 0;
    QString m_accumulatedText;
    static constexpr int MAX_CONTINUATIONS = 2;

    // [barista-fork] generic client-side tool loop. On stop_reason "tool_use" we run m_toolExecutor, append
    // the assistant tool_use turn + our tool_result, and re-POST — bounded by MAX_TOOL_ROUNDS. The tool
    // definitions + executor are supplied by a feature module via setClientTools(); only callers that enable
    // RequestOptions.clientTools send them, so this path is inert for the advisor (it never gets a tool_use stop).
    QJsonArray m_clientToolDefs;   // registered client-tool JSON defs, appended to the request when clientTools is on
    QJsonArray m_webToolDefs;      // [barista-fork] fast-path web-tool JSON defs, appended when RequestOptions.webSearch is on
    std::function<void(const QString&, const QJsonObject&, std::function<void(QJsonValue)>)> m_toolExecutor;
    int m_toolRounds = 0;
    static constexpr int MAX_TOOL_ROUNDS = 4;
    bool m_forceRespond = false;  // [barista-fork] this turn used tool_choice:"any" + the `respond` tool (see RequestOptions)
    int m_currentTimeoutMs = 0;   // [barista-fork] this turn's transfer timeout (RequestOptions.timeoutMs; 0 → default)
    qint64 m_requestSentMs = 0;   // [barista-fork] request-sent stamp for reply-latency instrumentation

    // Wrap the first user message's content in a structured block carrying
    // cache_control: ephemeral when its content is currently a plain string.
    // Multi-turn conversations on the same shot reuse the cached per-shot
    // payload across follow-up turns within the 1-hour TTL, paying the 2x
    // cache-write surcharge once and amortizing it across reads (break-even
    // is 2 reads per write). This said "5-minute TTL" and "~25% surcharge"
    // for as long as it took someone to open the .cpp: the implementation
    // has sent ttl="1h" since the switch away from the 5-minute tier, and
    // 1h writes cost 2x base, not the 1.25x the 5-minute tier charges.
    static QJsonArray messagesWithCachedFirstUser(const QJsonArray& messages);

    QString m_apiKey;
    QString m_baseUrl;
    // Selected wire model. Defaulted in the constructor to the first
    // availableModels() entry (the recommended default), so the C++ default and
    // the UI's "unset → index 0" fallback reference the same fact and can't drift.
    QString m_model;
    static constexpr const char* API_URL = "https://api.anthropic.com/v1/messages";
    // [barista-fork] web_search_20260209 requires Sonnet/Opus 4.6+. Every entry in
    // availableModels() for this provider is ≥4.6, so m_model always satisfies it;
    // if an older model is ever added to the catalog, switch the web-search tool
    // type to "web_search_20250305" (same name) in analyzeConversation().
};

// Google Gemini provider
class GeminiProvider : public AIProvider {
    Q_OBJECT

public:
    explicit GeminiProvider(QNetworkAccessManager* networkManager,
                            const QString& apiKey,
                            QObject* parent = nullptr);

    QString name() const override { return "Google Gemini"; }
    QString id() const override { return "gemini"; }
    QString modelName() const override { return m_model; }
    QString shortModelName() const override;  // catalog display for m_model
    bool isConfigured() const override { return !m_apiKey.isEmpty(); }
    bool supportsClientTools() const override { return true; }  // [barista-fork] functionCall loop
    // supportsWebSearch stays false: Gemini has no server-side GENERAL web search here, only the keyless
    // get_weather/get_stock_quote/get_local_news client tools (attached under the same webSearch gate).
    QList<ModelOption> availableModels() const override;
    QString modelHint() const override;
    QString costHintFor(const QString& modelId) const override;

    void setApiKey(const QString& key) { m_apiKey = key; }
    // empty → keeps default upstream URL. Matches OpenAI/Anthropic; exists so
    // the truncation/finish-reason handling is reachable from a test against a
    // canned-response server (the branch shipped untested and was broken).
    void setBaseUrl(const QString& url) { m_baseUrl = url.endsWith(QLatin1Char('/')) ? url.chopped(1) : url; }
    // Select the wire model. Ignores empty (keeps current default) and any id
    // not in availableModels(), so a stale/unknown stored value can't break the
    // request URL.
    void setModel(const QString& modelId);

    void analyze(const QString& systemPrompt, const QString& userPrompt) override;
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages) override;
    // [barista-fork] Options-aware overload — mirrors AnthropicProvider so the barista's per-call
    // client-tools / web config threads through when Gemini is the selected provider. Base 2-arg forwards here.
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                             const RequestOptions& options) override;
    // Gemini url_context server tool: the API fetches URLs named in the
    // prompt during generateContent (supported by every catalog model —
    // 2.5 and 3.x families).
    bool supportsUrlAnalysis() const override { return true; }
    bool supportsVision() const override { return true; }   // [barista-fork] Gemini models read images
    void analyzeUrl(const QString& systemPrompt, const QString& userPrompt) override;
    void testConnection() override;

    // [barista-fork] Append an inlineData image part to the LAST user-role entry of an already-built Gemini
    // `contents` array (Gemini part format). Pure transform, public+static for unit testing. No-op if there is
    // no user-role content. mediaType empty ⇒ image/jpeg.
    static QJsonArray contentsWithImageOnLastUser(const QJsonArray& contents, const QByteArray& imageData,
                                                  const QString& mediaType);

    // [barista-fork] Generic client-side-tool seam — the exact counterpart of AnthropicProvider::setClientTools.
    // A feature module (the barista) registers BOTH the tool JSON definitions (Anthropic {name, description,
    // input_schema} shape — converted to Gemini functionDeclarations here) and the executor. Appended to the
    // request when RequestOptions.clientTools is set; the generic functionCall loop in onAnalysisReply drives the
    // executor OFF the main thread and re-POSTs with the functionResponse. Set once at construction; callers that
    // never enable clientTools (advisor/coach) are entirely unaffected — no tools ship and the request is
    // byte-identical to the original.
    void setClientTools(const QJsonArray& defs,
                        std::function<void(const QString&, const QJsonObject&,
                                           std::function<void(QJsonValue)>)> exec) {
        m_clientToolDefs = defs;
        m_toolExecutor = std::move(exec);
    }
    // [barista-fork] Fast-path web-tool definitions (get_weather/get_stock_quote/get_local_news — keyless,
    // client-executed via the SAME executor). Registered separately and appended only when RequestOptions.webSearch
    // is on. Gemini has no server-side general web search here, so these keyless tools are the web surface it
    // offers; a broad "search the web" request degrades gracefully to whatever these cover.
    void setWebTools(const QJsonArray& defs) { m_webToolDefs = defs; }

private slots:
    void onAnalysisReply(QNetworkReply* reply);
    void onTestReply(QNetworkReply* reply);

private:
    void sendRequest(const QJsonObject& requestBody);

    // [barista-fork] Convert Anthropic-format tool defs ({name, description, input_schema}) to a Gemini
    // functionDeclarations array ({name, description, parameters}). Tools whose input_schema has no properties
    // (e.g. get_active_recipe) omit `parameters` entirely — Gemini rejects an empty properties object.
    static QJsonArray toGeminiFunctionDeclarations(const QJsonArray& defs);

    QString m_apiKey;
    QString m_baseUrl;
    // Selected wire model. Defaulted in the constructor to the first
    // availableModels() entry (the recommended default), so the C++ default and
    // the UI's "unset → index 0" fallback reference the same fact and can't drift.
    QString m_model;
    QString apiUrl() const;

    // [barista-fork] client-side tool loop state (see setClientTools). Mirrors the AnthropicProvider members.
    QJsonArray m_clientToolDefs;   // registered client-tool defs, appended (as functionDeclarations) when clientTools on
    QJsonArray m_webToolDefs;      // fast-path web-tool defs, appended when RequestOptions.webSearch on
    std::function<void(const QString&, const QJsonObject&, std::function<void(QJsonValue)>)> m_toolExecutor;
    QJsonObject m_pendingRequestBody;  // basis for a functionCall continuation re-POST
    QString m_accumulatedText;         // prose accumulated across tool rounds, prepended to the final answer
    int m_toolRounds = 0;              // reset per turn in the options-aware analyzeConversation
    int m_currentTimeoutMs = 0;        // this turn's transfer timeout (RequestOptions.timeoutMs; 0 → default)
    static constexpr int MAX_TOOL_ROUNDS = 4;
};

// OpenRouter provider (multiple models via OpenAI-compatible API)
class OpenRouterProvider : public AIProvider {
    Q_OBJECT

public:
    explicit OpenRouterProvider(QNetworkAccessManager* networkManager,
                                 const QString& apiKey,
                                 const QString& model,
                                 QObject* parent = nullptr);

    QString name() const override { return "OpenRouter"; }
    QString id() const override { return "openrouter"; }
    QString modelName() const override { return m_model; }
    QString shortModelName() const override { return "Multi"; }
    bool isConfigured() const override { return !m_apiKey.isEmpty() && !m_model.isEmpty(); }

    void setApiKey(const QString& key) { m_apiKey = key; }
    void setModel(const QString& model) { m_model = model; }
    // empty → keeps default upstream URL. See GeminiProvider::setBaseUrl for
    // why this exists.
    void setBaseUrl(const QString& url) { m_baseUrl = url.endsWith(QLatin1Char('/')) ? url.chopped(1) : url; }

    void analyze(const QString& systemPrompt, const QString& userPrompt) override;
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages) override;
    void testConnection() override;

private slots:
    void onAnalysisReply(QNetworkReply* reply);
    void onTestReply(QNetworkReply* reply);

private:
    void sendRequest(const QJsonObject& requestBody);

    QString m_apiKey;
    QString m_baseUrl;
    QString m_model;
    static constexpr const char* API_URL = "https://openrouter.ai/api/v1/chat/completions";
};

// Ollama local LLM provider
class OllamaProvider : public AIProvider {
    Q_OBJECT

public:
    explicit OllamaProvider(QNetworkAccessManager* networkManager,
                            const QString& endpoint,
                            const QString& model,
                            QObject* parent = nullptr);

    QString name() const override { return "Ollama"; }
    QString id() const override { return "ollama"; }
    QString modelName() const override { return m_model; }
    QString shortModelName() const override { return "Local"; }
    bool isConfigured() const override { return !m_endpoint.isEmpty() && !m_model.isEmpty(); }
    bool isLocal() const override { return true; }

    void setEndpoint(const QString& endpoint) { m_endpoint = endpoint; }
    void setModel(const QString& model) { m_model = model; }

    void analyze(const QString& systemPrompt, const QString& userPrompt) override;
    void analyzeConversation(const QString& systemPrompt, const QJsonArray& messages) override;
    void testConnection() override;

    // Get available models from Ollama
    void refreshModels();

signals:
    void modelsRefreshed(const QStringList& models);

private slots:
    void onAnalysisReply(QNetworkReply* reply);
    void onTestReply(QNetworkReply* reply);
    void onModelsReply(QNetworkReply* reply);

private:
    void sendRequest(const QUrl& url, const QJsonObject& requestBody);

    static constexpr int LOCAL_ANALYSIS_TIMEOUT_MS = 120000;  // 120s for local models
    QString m_endpoint;
    QString m_model;
};
