#include "aiprovider.h"
#include <QDateTime>
#include <QDebug>
#include "../core/translationmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVector>
#include <memory>

namespace {
// [barista-fork] AI-turn instrumentation. Emitted via the "[BaristaDiag]" qDebug tag (which lands in debug.log)
// rather than BaristaDiagnostics::record — the standalone AI test targets compile aiprovider.cpp WITHOUT the
// barista diagnostics object, so a direct call breaks their link. Greppable in debug.log for tool-vs-model timing.
inline void aiDiag(const QString& event, const QString& detail = QString()) {
    qDebug().noquote() << (QStringLiteral("[BaristaDiag] ai ") + event
                           + (detail.isEmpty() ? QString() : (QLatin1Char(' ') + detail)));
}
}  // namespace

// ============================================================================
// AIProvider base class
// ============================================================================

AIProvider::AIProvider(QNetworkAccessManager* networkManager, QObject* parent)
    : QObject(parent)
    , m_networkManager(networkManager)
{
}

void AIProvider::setStatus(Status status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged(status);
    }
}

QString AIProvider::tr_(const char* key, const char* fallback) const
{
    if (m_translationManager)
        return m_translationManager->translate(QString::fromUtf8(key),
                                               QString::fromUtf8(fallback));
    return QString::fromUtf8(fallback);
}

QString AIProvider::friendlyNetworkError(QNetworkReply* reply) const
{
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
        return tr_("ai.error.noConnection", "Could not connect to the AI service. Check your internet connection.");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return tr_("ai.error.timeout", "Request timed out. The AI service may be slow — please try again.");
    case QNetworkReply::AuthenticationRequiredError:
        return tr_("ai.error.authFailed", "Authentication failed. Please check your API key in Settings.");
    case QNetworkReply::ContentAccessDenied:
        return tr_("ai.error.accessDenied", "Access denied. Your API key may not have permission for this model.");
    default:
        return tr_("ai.error.requestFailed", "Request failed: %1").arg(reply->errorString());
    }
}

QJsonArray AIProvider::buildOpenAIMessages(const QString& systemPrompt, const QJsonArray& messages)
{
    QJsonArray apiMessages;
    QJsonObject sysMsg;
    sysMsg["role"] = QString("system");
    sysMsg["content"] = systemPrompt;
    apiMessages.append(sysMsg);
    for (const auto& msg : messages) {
        apiMessages.append(msg);
    }
    return apiMessages;
}

bool AIProvider::isRetryableHttpStatus(int httpStatus, int retryCount)
{
    // Primary transient codes: retry up to MAX_RETRIES times
    if (httpStatus == 429 || httpStatus == 502 || httpStatus == 503 || httpStatus == 504)
        return retryCount < MAX_RETRIES;
    // Other 5xx (e.g. 500 internal server error): retry once only
    if (httpStatus >= 500 && httpStatus < 600)
        return retryCount < 1;
    return false;
}

int AIProvider::computeRetryDelayMs(int retryCount, QNetworkReply* reply)
{
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus == 429) {
        const QByteArray retryAfter = reply->rawHeader("Retry-After");
        if (!retryAfter.isEmpty()) {
            bool ok;
            const int seconds = retryAfter.toInt(&ok);
            if (ok && seconds > 0)
                return qMin(seconds * 1000, 30000);
        }
    }
    return 1000 << (retryCount - 1);  // 1s, 2s, 4s for retries 1, 2, 3
}

bool AIProvider::tryScheduleRetry(QNetworkReply* reply)
{
    if (!m_retryFn) return false;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!isRetryableHttpStatus(status, m_retryCount)) return false;
    const int delay = computeRetryDelayMs(++m_retryCount, reply);
    const QByteArray body = reply->readAll();
    qWarning() << name() << "HTTP" << status << "- retry" << m_retryCount
               << "in" << delay << "ms" << (body.isEmpty() ? QByteArray() : ("- " + body.left(200)));
    // QTimer::singleShot is intentional: the server signalled a transient error and we must
    // wait before retrying (rate-limit or overload backoff). This is a server-driven delay,
    // not a heuristic guard. The generation counter prevents stale timers from firing if a
    // new analyze() call arrives before this one fires.
    const int gen = m_reqGen;
    QTimer::singleShot(delay, this, [this, gen]() {
        if (gen == m_reqGen) m_retryFn();
    });
    return true;
}

void AIProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    // Default fallback: flatten messages into a single string and call analyze()
    // This loses multi-turn context — providers should override for native support
    qWarning() << "AIProvider::analyzeConversation: Using flatten fallback for provider"
               << name() << "- consider implementing native multi-turn support";
    QString flatPrompt;
    for (int i = 0; i < messages.size(); i++) {
        QJsonObject msg = messages[i].toObject();
        QString role = msg["role"].toString();
        QString content = msg["content"].toString();

        if (role == "user") {
            if (i > 0) flatPrompt += "\n\n[User follow-up]:\n";
            flatPrompt += content;
        } else if (role == "assistant") {
            flatPrompt += "\n\n[Your previous response]:\n" + content;
        }
    }
    analyze(systemPrompt, flatPrompt);
}

// [barista-fork] Options-aware overload — base ignores options (no web search) and forwards. Only
// AnthropicProvider overrides this; all other providers no-op web search here.
void AIProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                                     const RequestOptions& /*options*/)
{
    analyzeConversation(systemPrompt, messages);
}

// ============================================================================
// OpenAI Provider
// ============================================================================

OpenAIProvider::OpenAIProvider(QNetworkAccessManager* networkManager,
                               const QString& apiKey,
                               QObject* parent)
    : AIProvider(networkManager, parent)
    , m_apiKey(apiKey)
{
    // Default to the recommended model = first catalog entry. Keeps the default
    // a single source of truth (no parallel DEFAULT_MODEL constant to keep in
    // sync with the list order). availableModels() dispatches to this class
    // since the object under construction is an OpenAIProvider.
    const QList<ModelOption> models = availableModels();
    if (!models.isEmpty())
        m_model = models.first().id;
}

QList<AIProvider::ModelOption> OpenAIProvider::availableModels() const
{
    // Order = UI order; first entry is the recommended default. GPT-5.4 leads
    // as the default for shot analysis quality (mini measurably misses
    // multi-shot trends and taste-feedback gating in real dial-in testing);
    // GPT-5.4 mini is the same-family cheaper/faster opt-in for cost-conscious
    // users. Pricing figures and why the other tiers (GPT-5.5, GPT-5.4 nano)
    // are omitted live in docs/CLAUDE_MD/AI_ADVISOR.md so they don't rot in
    // code. Revisit as models land.
    return {
        { "gpt-5.4", "GPT-5.4" },
        { "gpt-5.4-mini", "GPT-5.4 mini" },
    };
}

QString OpenAIProvider::modelHint() const
{
    return QStringLiteral("GPT-5.4 mini is cheaper and faster, but gives weaker dial-in advice. GPT-5.4 is recommended.");
}

void OpenAIProvider::setModel(const QString& modelId)
{
    if (modelId.isEmpty())
        return;  // unset → keep the current default
    for (const ModelOption& opt : availableModels()) {
        if (opt.id == modelId) {
            m_model = modelId;
            return;
        }
    }
    qWarning() << "OpenAIProvider::setModel ignoring unknown model id:" << modelId;
}

QString OpenAIProvider::shortModelName() const
{
    for (const ModelOption& opt : availableModels()) {
        if (opt.id == m_model)
            return opt.displayName;
    }
    return m_model;
}

void OpenAIProvider::sendRequest(const QJsonObject& requestBody)
{
    QUrl url(QString::fromLatin1(API_URL));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
    req.setTransferTimeout(ANALYSIS_TIMEOUT_MS);

    m_retryFn = [this, requestBody]() { sendRequest(requestBody); };

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onAnalysisReply(reply);
    });
}

void OpenAIProvider::analyze(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.openai.keyMissing", "OpenAI API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    QJsonArray messages;
    QJsonObject sysMsg;
    sysMsg["role"] = QString("system");
    sysMsg["content"] = systemPrompt;
    messages.append(sysMsg);
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = userPrompt;
    messages.append(userMsg);
    requestBody["messages"] = messages;
    // gpt-5-family reasoning models REJECT the legacy max_tokens parameter on
    // chat/completions ("Unsupported parameter") — max_completion_tokens is
    // the accepted cap. Live-caught July 2026: stage-1 extraction and the
    // advisor both 400'd on gpt-5.4/gpt-5.4-mini.
    requestBody["max_completion_tokens"] = 1024;
    // GPT-5 family are reasoning models; keep reasoning off so hidden
    // reasoning tokens don't count against the 1024-token output cap (which would
    // risk truncating the trailing nextShot JSON block) and to keep latency/cost
    // low. Dial-in advice needs little chain-of-thought. Mirrors Gemini's
    // thinking=off. The 5.4 generation REPLACED the value "minimal" with "none"
    // (live-caught 400: supported = none/low/medium/high). INVARIANT: assumes
    // every availableModels() entry is a reasoning model that accepts
    // reasoning_effort "none" — guard/branch here if the catalog ever gains
    // one that doesn't.
    requestBody["reasoning_effort"] = "none";

    sendRequest(requestBody);
}

void OpenAIProvider::analyzeUrl(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.openai.keyMissing", "OpenAI API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    // The web_search tool lives on the Responses API, not chat/completions.
    // Its open_page action lets the model retrieve the specific URL named in
    // the user prompt (add-recipe-wizard-tea stage-2 extraction).
    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["instructions"] = systemPrompt;
    requestBody["input"] = userPrompt;
    QJsonObject searchTool;
    searchTool["type"] = QString("web_search");
    requestBody["tools"] = QJsonArray{searchTool};
    // Reasoning "low", not the "none" floor: the gpt-5.4 generation rejects
    // web_search below "low". max_output_tokens covers reasoning + the JSON answer.
    QJsonObject reasoning;
    reasoning["effort"] = QString("low");
    requestBody["reasoning"] = reasoning;
    requestBody["max_output_tokens"] = 2048;

    sendResponsesRequest(requestBody);
}

void OpenAIProvider::sendResponsesRequest(const QJsonObject& requestBody)
{
    QUrl url(QString::fromLatin1(RESPONSES_API_URL));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
    req.setTransferTimeout(ANALYSIS_TIMEOUT_MS);

    m_retryFn = [this, requestBody]() { sendResponsesRequest(requestBody); };

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onResponsesReply(reply);
    });
}

void OpenAIProvider::onResponsesReply(QNetworkReply* reply)
{
    if (tryScheduleRetry(reply)) { reply->deleteLater(); return; }
    reply->deleteLater();
    setStatus(Status::Ready);

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        if (!body.isEmpty()) {
            QJsonDocument bodyDoc = QJsonDocument::fromJson(body);
            QString apiError = bodyDoc.object()["error"].toObject()["message"].toString();
            if (!apiError.isEmpty()) {
                int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                qWarning() << "OpenAI Responses API error" << status << "-" << apiError;
                emit analysisFailed(tr_("ai.openai.error", "OpenAI error: %1").arg(apiError));
                return;
            }
            qWarning() << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << body;
        }
        emit analysisFailed(friendlyNetworkError(reply));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject root = doc.object();

    if (root.contains("error") && root["error"].isObject()) {
        QString errorMsg = root["error"].toObject()["message"].toString();
        if (!errorMsg.isEmpty()) {
            emit analysisFailed(tr_("ai.openai.error", "OpenAI error: %1").arg(errorMsg));
            return;
        }
    }

    // The Responses output array interleaves reasoning/web_search_call items
    // with message items; the answer is the message items' output_text parts.
    QString text;
    const QJsonArray output = root["output"].toArray();
    for (const QJsonValue& itemVal : output) {
        const QJsonObject item = itemVal.toObject();
        if (item["type"].toString() != QLatin1String("message"))
            continue;
        const QJsonArray content = item["content"].toArray();
        for (const QJsonValue& partVal : content) {
            const QJsonObject part = partVal.toObject();
            if (part["type"].toString() == QLatin1String("output_text"))
                text += part["text"].toString();
        }
    }
    if (text.isEmpty()) {
        qWarning() << "OpenAI Responses: no output_text (status"
                   << root["status"].toString() << ")";
        emit analysisFailed(tr_("ai.openai.emptyContent", "OpenAI returned empty response content"));
        return;
    }
    emit analysisComplete(text);
}

void OpenAIProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.openai.keyMissing", "OpenAI API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["messages"] = buildOpenAIMessages(systemPrompt, messages);
    // [barista-fork] Voice path: GPT-5 needs max_completion_tokens (not the legacy
    // max_tokens — see analyze()), but keep the barista's larger 4096 cap so spoken
    // replies aren't truncated mid-sentence. reasoning_effort=none keeps hidden
    // reasoning tokens from eating that cap.
    requestBody["max_completion_tokens"] = 4096;
    requestBody["reasoning_effort"] = "none";  // see analyze(): 5.4 generation dropped "minimal"

    sendRequest(requestBody);
}

void OpenAIProvider::onAnalysisReply(QNetworkReply* reply)
{
    if (tryScheduleRetry(reply)) { reply->deleteLater(); return; }
    reply->deleteLater();
    setStatus(Status::Ready);

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        if (!body.isEmpty()) {
            QJsonDocument bodyDoc = QJsonDocument::fromJson(body);
            QString apiError = bodyDoc.object()["error"].toObject()["message"].toString();
            if (!apiError.isEmpty()) {
                int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                qWarning() << "OpenAI API error" << status << "-" << apiError;
                emit analysisFailed(tr_("ai.openai.error", "OpenAI error: %1").arg(apiError));
                return;
            }
            qWarning() << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << body;
        }
        emit analysisFailed(friendlyNetworkError(reply));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject root = doc.object();

    if (root.contains("error")) {
        QString errorMsg = root["error"].toObject()["message"].toString();
        emit analysisFailed(tr_("ai.openai.error", "OpenAI error: %1").arg(errorMsg));
        return;
    }

    QJsonArray choices = root["choices"].toArray();
    if (choices.isEmpty()) {
        emit analysisFailed(tr_("ai.openai.noResponse", "OpenAI returned no response"));
        return;
    }

    QString content = choices[0].toObject()["message"].toObject()["content"].toString();
    if (content.isEmpty()) {
        emit analysisFailed(tr_("ai.openai.emptyContent", "OpenAI returned empty response content"));
        return;
    }
    emit analysisComplete(content);
}

void OpenAIProvider::testConnection()
{
    if (!isConfigured()) {
        emit testResult(false, tr_("ai.test.keyNotConfigured", "API key not configured"));
        return;
    }

    // Simple test: list models
    QUrl url(QString("https://api.openai.com/v1/models"));
    QNetworkRequest req;
    req.setUrl(url);
    req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
    req.setTransferTimeout(TEST_TIMEOUT_MS);
    // Disable HTTP/2 -- Qt's HTTP/2 layer intercepts 401 as an auth challenge
    // instead of passing the response body through, breaking custom auth schemes
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply* reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onTestReply(reply);
    });
}

void OpenAIProvider::onTestReply(QNetworkReply* reply)
{
    reply->deleteLater();

    QByteArray responseBody = reply->readAll();
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Handle errors in priority order: explicit 401 with response body context,
    // then network errors with JSON error parsing, then success-with-error-body,
    // then fall back to Qt's generic error string.
    if (httpStatus == 401) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.authFailed", "Authentication failed: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.invalidKey", "Invalid API key"));
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.connectionFailed", "Connection failed: %1").arg(reply->errorString()));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseBody);
    if (doc.object().contains("error")) {
        QJsonValue errVal = doc.object()["error"];
        QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
        if (errorMsg.isEmpty())
            errorMsg = tr_("ai.test.unknownError", "Unknown API error");
        emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
        return;
    }

    emit testResult(true, tr_("ai.openai.connected", "Connected to OpenAI successfully"));
}

// ============================================================================
// Anthropic Provider
// ============================================================================

AnthropicProvider::AnthropicProvider(QNetworkAccessManager* networkManager,
                                     const QString& apiKey,
                                     QObject* parent)
    : AIProvider(networkManager, parent)
    , m_apiKey(apiKey)
{
    // Default to the recommended model = first catalog entry. Keeps the default
    // a single source of truth (no parallel DEFAULT_MODEL constant to keep in
    // sync with the list order). availableModels() dispatches to this class
    // since the object under construction is an AnthropicProvider.
    const QList<ModelOption> models = availableModels();
    if (!models.isEmpty())
        m_model = models.first().id;
}

QList<AIProvider::ModelOption> AnthropicProvider::availableModels() const
{
    // Order = UI order; first entry is the recommended default. Sonnet 4.6 leads
    // as the established default so upgrading users keep their current behavior;
    // Sonnet 5 is the opt-in "more capable" choice. Revisit as new models land.
    return {
        { "claude-sonnet-4-6", "Sonnet 4.6" },
        { "claude-sonnet-5", "Sonnet 5" },
    };
}

QString AnthropicProvider::modelHint() const
{
    return QStringLiteral("Sonnet 5 is the most capable. Sonnet 4.6 is the established default.");
}

void AnthropicProvider::setModel(const QString& modelId)
{
    if (modelId.isEmpty())
        return;  // unset → keep the current default
    for (const ModelOption& opt : availableModels()) {
        if (opt.id == modelId) {
            m_model = modelId;
            return;
        }
    }
    qWarning() << "AnthropicProvider::setModel ignoring unknown model id:" << modelId;
}

QString AnthropicProvider::shortModelName() const
{
    for (const ModelOption& opt : availableModels()) {
        if (opt.id == m_model)
            return opt.displayName;
    }
    return m_model;
}

void AnthropicProvider::sendRequest(const QJsonObject& requestBody)
{
    QUrl url(QString::fromLatin1(API_URL));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("x-api-key", m_apiKey.toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");
    // 1-hour cache TTL is set on each cache_control block in the request
    // body (see buildCachedSystemPrompt + messagesWithCachedFirstUser).
    // The 1-hour TTL tier is GA — no beta header required. Cache writes
    // cost 2x base input (vs 1.25x for 5-min); reads stay at 0.1x.
    // Break-even is ~2 reads per write, easily met for any iterative dial-in.
    // [barista-fork] Interactive barista turns pass a shorter timeout (RequestOptions.timeoutMs, ~30s) so a
    // stalled request fails+recovers fast; deep-analysis/advisor leave it 0 → the 60s default. transferTimeout
    // is an inactivity abort, which for this non-streaming POST is effectively a total cap — safe. Applies to
    // every tool-round re-POST of this turn too, since m_currentTimeoutMs persists across the turn.
    const int reqTimeout = m_currentTimeoutMs > 0 ? m_currentTimeoutMs : ANALYSIS_TIMEOUT_MS;
    req.setTransferTimeout(reqTimeout);

    m_retryFn = [this, requestBody]() { sendRequest(requestBody); };
    m_pendingRequestBody = requestBody;   // [barista-fork] basis for a pause_turn continuation

    // [barista-fork][diag] request timing — split tool-time vs model-time when a turn feels "stuck".
    m_requestSentMs = QDateTime::currentMSecsSinceEpoch();
    aiDiag(QStringLiteral("request_sent"), QStringLiteral("round=%1 timeoutMs=%2").arg(m_toolRounds).arg(reqTimeout));

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onAnalysisReply(reply);
    });
}

void AnthropicProvider::analyze(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.anthropic.keyMissing", "Anthropic API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    m_continuations = 0;          // [barista-fork]
    m_accumulatedText.clear();    // [barista-fork]
    ++m_reqGen;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["max_tokens"] = 1024;   // advisor dial-in — short structured reply
    requestBody["system"] = buildCachedSystemPrompt(systemPrompt);
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = userPrompt;
    messages.append(userMsg);
    requestBody["messages"] = messages;

    sendRequest(requestBody);
}

void AnthropicProvider::analyzeUrl(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.anthropic.keyMissing", "Anthropic API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;
    // [barista-fork] Reset the fork's continuation/tool state, same as analyze()
    // and analyzeConversation(). This provider instance is SHARED: the barista
    // (analyzeConversation) and the recipe-wizard URL extraction (analyzeUrl) run
    // on the same AnthropicProvider, so a prior barista turn can leave m_accumulatedText
    // non-empty or m_continuations advanced — which would prepend stale prose to the
    // extraction or block a legitimate web_fetch pause_turn. Clear it before each URL turn.
    m_continuations = 0;
    m_accumulatedText.clear();
    m_toolRounds = 0;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["max_tokens"] = 1024;
    requestBody["system"] = buildCachedSystemPrompt(systemPrompt);
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = userPrompt;
    messages.append(userMsg);
    requestBody["messages"] = messages;
    // The web_fetch server tool (add-recipe-wizard-tea stage-2 extraction):
    // the API fetches the URL named in the user prompt during the request.
    // max_uses 2 allows one retry; max_content_tokens bounds the token cost
    // of a huge page (fetched content is billed as input tokens).
    QJsonObject fetchTool;
    fetchTool["type"] = QString("web_fetch_20250910");
    fetchTool["name"] = QString("web_fetch");
    fetchTool["max_uses"] = 2;
    fetchTool["max_content_tokens"] = 20000;
    requestBody["tools"] = QJsonArray{fetchTool};

    sendRequest(requestBody);
}

void AnthropicProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    analyzeConversation(systemPrompt, messages, RequestOptions{});
}

void AnthropicProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                                            const RequestOptions& options)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.anthropic.keyMissing", "Anthropic API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_currentTimeoutMs = options.timeoutMs;   // [barista-fork] this turn's transfer timeout (used in sendRequest)
    m_retryCount = 0;
    m_continuations = 0;          // [barista-fork]
    m_accumulatedText.clear();    // [barista-fork]
    m_toolRounds = 0;             // [barista-fork] reset the client-tool loop counter per turn
    ++m_reqGen;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    // [barista-fork] Voice path keeps the larger 4096 cap so spoken replies don't
    // truncate mid-sentence (advisor dial-in above stays at 1024).
    requestBody["max_tokens"] = 4096;
    requestBody["system"] = buildCachedSystemPrompt(systemPrompt);
    requestBody["messages"] = messagesWithCachedFirstUser(messages);
    // [barista-fork] Tools. web_search runs on Anthropic's side (resume on "pause_turn"); the client-side
    // tools (registered via setClientTools) are CLIENT-side (we run them and feed the tool_result back on
    // "tool_use"). Both can coexist. When neither option is set (advisor/coach, or a caller with no client
    // tools) the array is empty and "tools" is omitted entirely — byte-identical to the original request.
    QJsonArray tools;
    if (options.webSearch) {
        QJsonObject ws;
        ws["type"] = QString("web_search_20260209");
        ws["name"] = QString("web_search");
        ws["max_uses"] = 3;
        tools.append(ws);
        // [barista-fork] FAST-PATH web tools ship under the SAME webSearch gate (both = "may reach the
        // internet"). They are CLIENT-side (run via m_toolExecutor, like the client tools) but gated by
        // webSearch, not clientTools — so a fast keyless get_weather/get_stock_quote/get_local_news is offered
        // exactly when the umbrella web toggle is on. The executor runs them on tool_use regardless of which
        // gate added the def (see onAnalysisReply); webOn ⇒ clientTools-on in the QML, so it is always present.
        for (const QJsonValue& def : m_webToolDefs)
            tools.append(def);
    }
    // Client-side tools are registered by a feature module (the barista) via setClientTools(); this file has
    // no knowledge of the specific tools — it just appends the registered definitions when the caller opts in.
    // When no tools are registered (or the caller left clientTools off) the array stays empty and "tools" is
    // omitted — byte-identical to the original request. The generic tool_use loop in onAnalysisReply runs them.
    if (options.clientTools && m_toolExecutor && !m_clientToolDefs.isEmpty()) {
        for (const QJsonValue& def : m_clientToolDefs)
            tools.append(def);
    }
    if (!tools.isEmpty())
        requestBody["tools"] = tools;

    sendRequest(requestBody);
}

QJsonArray AnthropicProvider::messagesWithCachedFirstUser(const QJsonArray& messages)
{
    // The first user message carries the per-shot context, which is stable
    // across follow-up turns within the cache TTL. Wrap its content in a
    // structured block with cache_control so subsequent turns read from
    // cache instead of re-billing the per-shot payload. A 1-hour TTL covers
    // a typical iterative dial-in spread across an hour-long session.
    //
    // No-op when messages[0] isn't a plain-string user message (caller
    // pre-wrapped, or first message isn't from user) — preserves input.
    if (messages.isEmpty()) return messages;
    QJsonObject first = messages[0].toObject();
    if (first.value("role").toString() != "user") return messages;
    if (!first.value("content").isString()) return messages;

    QJsonObject cacheControl;
    cacheControl["type"] = QString("ephemeral");
    cacheControl["ttl"] = QString("1h");  // Anthropic API: Literal["5m", "1h"]

    QJsonObject block;
    block["type"] = QString("text");
    block["text"] = first.value("content").toString();
    block["cache_control"] = cacheControl;

    QJsonArray contentArr;
    contentArr.append(block);
    first["content"] = contentArr;

    QJsonArray out;
    out.append(first);
    for (qsizetype i = 1; i < messages.size(); ++i)
        out.append(messages[i]);
    return out;
}

QJsonArray AnthropicProvider::buildCachedSystemPrompt(const QString& systemPrompt)
{
    // Cache the system prompt with the 1-hour extended TTL. Anthropic
    // caches give ~90% off input cost on hits; a 1-hour TTL covers most
    // dial-in patterns (back-to-back, "let me try again in 20 minutes",
    // and the typical morning-pull-evening-pull iteration). Cache writes
    // cost 2x base for the 1-hour tier (vs 1.25x for 5-min); break-even
    // is 2 reads per write — easily met for any iterative user.
    QJsonObject cacheControl;
    cacheControl["type"] = QString("ephemeral");
    cacheControl["ttl"] = QString("1h");  // Anthropic API: Literal["5m", "1h"]

    QJsonObject block;
    block["type"] = QString("text");
    block["text"] = systemPrompt;
    block["cache_control"] = cacheControl;

    QJsonArray systemArray;
    systemArray.append(block);
    return systemArray;
}

void AnthropicProvider::onAnalysisReply(QNetworkReply* reply)
{
    if (tryScheduleRetry(reply)) { reply->deleteLater(); return; }
    reply->deleteLater();
    setStatus(Status::Ready);

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        if (!body.isEmpty()) {
            QJsonDocument bodyDoc = QJsonDocument::fromJson(body);
            QString apiError = bodyDoc.object()["error"].toObject()["message"].toString();
            if (!apiError.isEmpty()) {
                int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                qWarning() << "Anthropic API error" << status << "-" << apiError;
                emit analysisFailed(tr_("ai.anthropic.error", "Anthropic error: %1").arg(apiError));
                return;
            }
            qWarning() << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << body;
        }
        emit analysisFailed(friendlyNetworkError(reply));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject root = doc.object();

    if (root.contains("error")) {
        QString errorMsg = root["error"].toObject()["message"].toString();
        emit analysisFailed(tr_("ai.anthropic.error", "Anthropic error: %1").arg(errorMsg));
        return;
    }

    const QString stopReason = root["stop_reason"].toString();
    // [barista-fork][diag] model reply latency + stop_reason — the other half of the tool-vs-model split.
    if (m_requestSentMs != 0)
        aiDiag(QStringLiteral("reply"), QStringLiteral("ms=%1 stop_reason=%2 round=%3")
               .arg(QDateTime::currentMSecsSinceEpoch() - m_requestSentMs).arg(stopReason).arg(m_toolRounds));
    const QJsonArray content = root["content"].toArray();
    if (content.isEmpty()) {
        // [barista-fork] An empty terminal response is a GENUINE failure ONLY if this turn produced nothing
        // yet. On a goodbye turn the model replies with sign-off text + an end_conversation tool_use in the
        // SAME round; we speak the sign-off (via interimText) and run the tool → re-POST → the model has
        // already said goodbye and returns EMPTY content with stop_reason "end_turn". That is NOT an error —
        // the turn already spoke and dismissed. m_toolRounds / m_continuations are per-turn (reset in
        // analyzeConversation) and are the ONLY paths that emit interimText, so either being >0 means this
        // turn already emitted spoken text or ran a tool. Complete gracefully with the accumulated text
        // (usually empty — the sign-off was already spoken and must NOT be re-accumulated → no double-speak).
        // A first-round empty response (both counters 0) still surfaces the real error.
        if (m_toolRounds > 0 || m_continuations > 0) {
            const QString done = m_accumulatedText;
            m_accumulatedText.clear();
            emit analysisComplete(done);
            return;
        }
        emit analysisFailed(tr_("ai.anthropic.noResponse", "Anthropic returned no response"));
        return;
    }

    // [barista-fork] With web search on, content is a MULTI-block array. Concatenate every text block;
    // skip server_tool_use / web_search_tool_result (they carry no prose). Plain single-text replies are
    // unaffected. Also hardens the old content[0].text read, which broke if a tool block led the array.
    QString text;
    for (const QJsonValue& v : content) {
        const QJsonObject block = v.toObject();
        if (block["type"].toString() == QLatin1String("text"))
            text += block["text"].toString();   // SF-5: append verbatim — Anthropic splits a sentence across
                                                 // text blocks at citation boundaries; a "\n" join breaks it.
    }

    // [barista-fork] CLIENT tools: the model asked us to run one. Execute each tool_use block via the
    // registered executor (see setClientTools), append the assistant tool_use turn + a user tool_result turn,
    // and re-POST — the standard Anthropic tool loop, bounded by MAX_TOOL_ROUNDS. Only callers that enable
    // client tools carry them, so the advisor (which never sends tools) never receives a "tool_use" stop_reason
    // and this branch is inert for it. (web_search is server-side and uses "pause_turn", not "tool_use" — that
    // path below is untouched.)
    if (stopReason == QLatin1String("tool_use") && m_toolExecutor && m_toolRounds < MAX_TOOL_ROUNDS) {
        // Collect every tool_use block up front — the API may batch several parallel calls in one turn.
        QVector<QJsonObject> toolUses;
        for (const QJsonValue& v : content) {
            const QJsonObject block = v.toObject();
            if (block["type"].toString() == QLatin1String("tool_use"))
                toolUses.append(block);
        }
        if (!toolUses.isEmpty()) {
            ++m_toolRounds;
            // [barista-fork] "Don't leave the user in silence": the model often writes a short natural lead-in
            // ("let me pull that up") BEFORE the tool_use block. Emit it NOW via interimText so the overlay can
            // speak it while the tool runs, instead of buffering it until the whole turn finishes. Only the
            // FIRST tool round's lead-in is worth speaking (a second round's stray prose would talk over the
            // first). The first round's lead-in is NOT folded into m_accumulatedText — the final
            // analysisComplete carries only the post-tool answer, so the lead-in is spoken exactly once (early)
            // and never double-spoken. Later rounds keep the old buffering so their prose isn't lost.
            if (!text.isEmpty()) {
                if (m_toolRounds == 1)
                    emit interimText(text);
                else
                    m_accumulatedText += text;
            }
            setStatus(Status::Busy);           // stay Busy while the DB queries run (line 499 already set Ready)
            const int gen = m_reqGen;           // guard: a superseded turn's late callback must NOT re-POST
            auto pending = std::make_shared<int>(toolUses.size());
            auto results = std::make_shared<QJsonArray>();
            for (const QJsonObject& block : toolUses) {
                const QString id = block["id"].toString();
                const QString toolName = block["name"].toString();
                const qint64 toolT0 = QDateTime::currentMSecsSinceEpoch();
                m_toolExecutor(toolName, block["input"].toObject(),
                    [this, gen, id, toolName, toolT0, pending, results, content](QJsonValue result) {
                        if (gen != m_reqGen) return;   // a newer turn started — drop this stale result
                        // [barista-fork][diag] tool completion timing — split tool-vs-model time on a "stuck" turn.
                        aiDiag(QStringLiteral("tool_done"), QStringLiteral("name=%1 ms=%2 is_error=%3")
                               .arg(toolName).arg(QDateTime::currentMSecsSinceEpoch() - toolT0)
                               .arg(result.isObject() && result.toObject().contains(QStringLiteral("error")) ? 1 : 0));
                        // Anthropic wants tool_result.content as a string; JSON-stringify arrays/objects.
                        QString contentStr;
                        if (result.isString())      contentStr = result.toString();
                        else if (result.isArray())  contentStr = QString::fromUtf8(QJsonDocument(result.toArray()).toJson(QJsonDocument::Compact));
                        else                        contentStr = QString::fromUtf8(QJsonDocument(result.toObject()).toJson(QJsonDocument::Compact));
                        QJsonObject tr;
                        tr["type"] = QString("tool_result");
                        tr["tool_use_id"] = id;
                        tr["content"] = contentStr;
                        if (result.isObject() && result.toObject().contains(QStringLiteral("error")))
                            tr["is_error"] = true;   // documented Anthropic signal — helps the model recover
                        results->append(tr);
                        if (--(*pending) > 0)
                            return;              // wait for the remaining tool calls in this turn
                        // All results in — append the assistant tool_use turn + our tool_result turn, re-POST.
                        QJsonObject body = m_pendingRequestBody;
                        QJsonArray msgs = body["messages"].toArray();
                        QJsonObject asst;  asst["role"] = QString("assistant"); asst["content"] = content;   // tool_use turn, verbatim
                        msgs.append(asst);
                        QJsonObject usr;   usr["role"]  = QString("user");      usr["content"]  = *results;   // our results
                        msgs.append(usr);
                        body["messages"] = msgs;
                        setStatus(Status::Busy);
                        sendRequest(body);
                    });
            }
            return;   // async — the completion callback re-POSTs once every query has returned
        }
        // No tool_use blocks despite the stop_reason — fall through and emit whatever text exists.
    }

    // [barista-fork] Server-side search paused mid-turn: resume by re-POSTing the turn with the assistant
    // content appended verbatim (the API detects the trailing tool block and continues). Bounded loop.
    if (stopReason == QLatin1String("pause_turn") && m_continuations < MAX_CONTINUATIONS) {
        ++m_continuations;
        // [barista-fork] Same "don't leave the user in silence" lead-in as the tool_use path: a server-side
        // web search paused mid-turn, and any prose written before it is a natural lead-in. On the FIRST
        // continuation, emit it now via interimText so it's spoken while the search runs (not folded into
        // m_accumulatedText → not double-spoken at the end). Later continuations keep the old buffering so
        // their prose isn't lost.
        if (!text.isEmpty()) {
            if (m_continuations == 1)
                emit interimText(text);
            else
                m_accumulatedText += text;
        }
        QJsonObject body = m_pendingRequestBody;
        QJsonArray msgs = body["messages"].toArray();
        QJsonObject asst;
        asst["role"] = QString("assistant");
        asst["content"] = content;
        msgs.append(asst);
        body["messages"] = msgs;
        setStatus(Status::Busy);   // stay Busy across the continuation (line 499 already set Ready)
        sendRequest(body);
        return;
    }

    text = m_accumulatedText + text;
    m_accumulatedText.clear();
    if (text.trimmed().isEmpty()) {
        // [barista-fork] The client tool loop hit MAX_TOOL_ROUNDS (or returned no prose) — degrade to a
        // friendly message instead of surfacing an error to the user.
        if (stopReason == QLatin1String("tool_use")) {
            emit analysisComplete(QStringLiteral("I dug through your shot history but couldn't quite finish that — ask me again?"));
            return;
        }
        emit analysisFailed(tr_("ai.anthropic.emptyContent", "Anthropic returned empty response content"));
        return;
    }
    emit analysisComplete(text);
}

void AnthropicProvider::testConnection()
{
    if (!isConfigured()) {
        emit testResult(false, tr_("ai.test.keyNotConfigured", "API key not configured"));
        return;
    }

    // Send a minimal request to test the API key
    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["max_tokens"] = 10;
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = QString("Hi");
    messages.append(userMsg);
    requestBody["messages"] = messages;

    QUrl url(QString::fromLatin1(API_URL));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("x-api-key", m_apiKey.toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");
    req.setTransferTimeout(TEST_TIMEOUT_MS);
    // Disable HTTP/2 — Qt's HTTP/2 layer intercepts 401 as an auth challenge
    // instead of passing the response body through, breaking custom auth schemes
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onTestReply(reply);
    });
}

void AnthropicProvider::onTestReply(QNetworkReply* reply)
{
    reply->deleteLater();

    QByteArray responseBody = reply->readAll();
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Handle errors in priority order: explicit 401 with response body context,
    // then network errors with JSON error parsing, then success-with-error-body,
    // then fall back to Qt's generic error string.
    if (httpStatus == 401) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.authFailed", "Authentication failed: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.invalidKey", "Invalid API key"));
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.connectionFailed", "Connection failed: %1").arg(reply->errorString()));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseBody);
    if (doc.object().contains("error")) {
        QJsonValue errVal = doc.object()["error"];
        QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
        if (errorMsg.isEmpty())
            errorMsg = tr_("ai.test.unknownError", "Unknown API error");
        emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
        return;
    }

    emit testResult(true, tr_("ai.anthropic.connected", "Connected to Anthropic successfully"));
}

// ============================================================================
// Gemini Provider
// ============================================================================

GeminiProvider::GeminiProvider(QNetworkAccessManager* networkManager,
                               const QString& apiKey,
                               QObject* parent)
    : AIProvider(networkManager, parent)
    , m_apiKey(apiKey)
{
    // Default to the recommended model = first catalog entry. Keeps the default
    // a single source of truth (no parallel DEFAULT_MODEL constant to keep in
    // sync with the list order). availableModels() dispatches to this class
    // since the object under construction is a GeminiProvider.
    const QList<ModelOption> models = availableModels();
    if (!models.isEmpty())
        m_model = models.first().id;
}

QList<AIProvider::ModelOption> GeminiProvider::availableModels() const
{
    // Order = UI order; first entry is the recommended default. 2.5 Flash leads
    // as the lowest-cost sensible default for shot analysis — thinking adds
    // little here and 2.5 can disable it entirely (thinkingBudget 0), plus it
    // has more provisioned capacity (fewer 503s). 3.5 Flash is the opt-in
    // "more capable" choice. Revisit as new models / pricing land.
    return {
        { "gemini-2.5-flash", "2.5 Flash" },
        { "gemini-3.5-flash", "3.5 Flash" },
    };
}

QString GeminiProvider::modelHint() const
{
    return QStringLiteral("3.5 Flash is the most capable. 2.5 Flash is more available (fewer busy errors).");
}

void GeminiProvider::setModel(const QString& modelId)
{
    if (modelId.isEmpty())
        return;  // unset → keep the current default
    for (const ModelOption& opt : availableModels()) {
        if (opt.id == modelId) {
            m_model = modelId;
            return;
        }
    }
    qWarning() << "GeminiProvider::setModel ignoring unknown model id:" << modelId;
}

QString GeminiProvider::shortModelName() const
{
    for (const ModelOption& opt : availableModels()) {
        if (opt.id == m_model)
            return opt.displayName;
    }
    return m_model;
}

QString GeminiProvider::apiUrl() const
{
    // Use URL without key - key is passed via header for better security
    return QString("https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent")
        .arg(m_model);
}

void GeminiProvider::sendRequest(const QJsonObject& requestBody)
{
    QUrl url(apiUrl());
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("x-goog-api-key", m_apiKey.toUtf8());
    req.setTransferTimeout(ANALYSIS_TIMEOUT_MS);

    // Thinking config differs by model family: the 2.5 family uses the integer
    // thinkingBudget (0 disables thinking), while 3.x+ uses the thinkingLevel
    // enum and ignores thinkingBudget — sending the wrong knob lets thinking
    // default to "medium" (billed at the $9/MTok output rate). Pick by family
    // so each selectable model keeps thinking minimal/off.
    QJsonObject bodyWithConfig = requestBody;
    QJsonObject thinkingConfig;
    // Gate on the gemini-2.x prefix — 2.5 Flash is the only 2.x model in the
    // catalog today, so this selects it exactly. If a future gemini-2.x model
    // with different thinking semantics is added, prefer encoding the thinking
    // API in ModelOption over widening this string check.
    if (m_model.startsWith(QStringLiteral("gemini-2"))) {
        thinkingConfig["thinkingBudget"] = 0;       // 2.x: integer budget knob, 0 = off
    } else {
        thinkingConfig["thinkingLevel"] = "minimal"; // 3.x+: thinkingLevel enum
    }
    QJsonObject generationConfig;
    generationConfig["thinkingConfig"] = thinkingConfig;
    generationConfig["maxOutputTokens"] = 4096;  // [barista-fork] was 1024; also bounds thinking tokens
    bodyWithConfig["generationConfig"] = generationConfig;

    m_retryFn = [this, requestBody]() { sendRequest(requestBody); };

    QByteArray body = QJsonDocument(bodyWithConfig).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onAnalysisReply(reply);
    });
}

void GeminiProvider::analyze(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.gemini.keyMissing", "Gemini API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    // Gemini uses a different format
    QJsonObject requestBody;

    // system_instruction
    QJsonObject sysInstruction;
    QJsonArray sysParts;
    QJsonObject sysTextPart;
    sysTextPart["text"] = systemPrompt;
    sysParts.append(sysTextPart);
    sysInstruction["parts"] = sysParts;
    requestBody["system_instruction"] = sysInstruction;

    // contents
    QJsonArray contents;
    QJsonObject userContent;
    userContent["role"] = QString("user");
    QJsonArray userParts;
    QJsonObject userTextPart;
    userTextPart["text"] = userPrompt;
    userParts.append(userTextPart);
    userContent["parts"] = userParts;
    contents.append(userContent);
    requestBody["contents"] = contents;

    sendRequest(requestBody);
}

void GeminiProvider::analyzeUrl(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.gemini.keyMissing", "Gemini API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    // Same body as analyze() plus the url_context server tool: the API
    // fetches the URL named in the user prompt during generateContent
    // (add-recipe-wizard-tea stage-2 extraction).
    QJsonObject requestBody;
    QJsonObject sysInstruction;
    QJsonArray sysParts;
    QJsonObject sysTextPart;
    sysTextPart["text"] = systemPrompt;
    sysParts.append(sysTextPart);
    sysInstruction["parts"] = sysParts;
    requestBody["system_instruction"] = sysInstruction;

    QJsonArray contents;
    QJsonObject userContent;
    userContent["role"] = QString("user");
    QJsonArray userParts;
    QJsonObject userTextPart;
    userTextPart["text"] = userPrompt;
    userParts.append(userTextPart);
    userContent["parts"] = userParts;
    contents.append(userContent);
    requestBody["contents"] = contents;

    QJsonObject urlContextTool;
    urlContextTool["url_context"] = QJsonObject{};
    requestBody["tools"] = QJsonArray{urlContextTool};

    sendRequest(requestBody);
}

void GeminiProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.gemini.keyMissing", "Gemini API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    QJsonObject requestBody;

    // system_instruction
    QJsonObject sysInstruction;
    QJsonArray sysParts;
    QJsonObject sysTextPart;
    sysTextPart["text"] = systemPrompt;
    sysParts.append(sysTextPart);
    sysInstruction["parts"] = sysParts;
    requestBody["system_instruction"] = sysInstruction;

    // contents — map from OpenAI roles to Gemini roles
    QJsonArray contents;
    for (const auto& msg : messages) {
        QJsonObject m = msg.toObject();
        QString role = m["role"].toString();
        if (role != "user" && role != "assistant") {
            qWarning() << "GeminiProvider: Skipping message with unexpected role:" << role;
            continue;
        }
        QJsonObject content;
        content["role"] = (role == "assistant") ? QString("model") : role;
        QJsonArray parts;
        QJsonObject textPart;
        textPart["text"] = m["content"].toString();
        parts.append(textPart);
        content["parts"] = parts;
        contents.append(content);
    }
    requestBody["contents"] = contents;

    sendRequest(requestBody);
}

void GeminiProvider::onAnalysisReply(QNetworkReply* reply)
{
    if (tryScheduleRetry(reply)) { reply->deleteLater(); return; }
    reply->deleteLater();
    setStatus(Status::Ready);

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        if (!body.isEmpty()) {
            QJsonDocument bodyDoc = QJsonDocument::fromJson(body);
            QString apiError = bodyDoc.object()["error"].toObject()["message"].toString();
            if (!apiError.isEmpty()) {
                int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                qWarning() << "Gemini API error" << status << "-" << apiError;
                emit analysisFailed(tr_("ai.gemini.error", "Gemini error: %1").arg(apiError));
                return;
            }
            qWarning() << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << body;
        }
        emit analysisFailed(friendlyNetworkError(reply));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject root = doc.object();

    if (root.contains("error")) {
        QString errorMsg = root["error"].toObject()["message"].toString();
        emit analysisFailed(tr_("ai.gemini.error", "Gemini error: %1").arg(errorMsg));
        return;
    }

    const QJsonObject usage = root["usageMetadata"].toObject();
    qInfo() << "Gemini usage — prompt:" << usage["promptTokenCount"].toInt()
            << "thoughts:" << usage["thoughtsTokenCount"].toInt()
            << "output:" << usage["candidatesTokenCount"].toInt()
            << "total:" << usage["totalTokenCount"].toInt();

    QJsonArray candidates = root["candidates"].toArray();
    if (candidates.isEmpty()) {
        emit analysisFailed(tr_("ai.gemini.noResponse", "Gemini returned no response"));
        return;
    }

    QJsonArray parts = candidates[0].toObject()["content"].toObject()["parts"].toArray();
    if (parts.isEmpty()) {
        emit analysisFailed(tr_("ai.gemini.emptyContent2", "Gemini returned empty content"));
        return;
    }

    // Join every non-thought text part: plain replies have exactly one, but
    // a url_context response (analyzeUrl) may split the answer across parts;
    // thought parts are hidden reasoning and must not leak into the answer.
    QString text;
    for (const QJsonValue& partVal : parts) {
        const QJsonObject part = partVal.toObject();
        if (part["thought"].toBool())
            continue;
        text += part["text"].toString();
    }
    if (text.isEmpty()) {
        emit analysisFailed(tr_("ai.gemini.emptyContent", "Gemini returned empty response content"));
        return;
    }
    emit analysisComplete(text);
}

void GeminiProvider::testConnection()
{
    if (!isConfigured()) {
        emit testResult(false, tr_("ai.test.keyNotConfigured", "API key not configured"));
        return;
    }

    // Send a minimal request
    QJsonObject requestBody;
    QJsonArray contents;
    QJsonObject userContent;
    userContent["role"] = QString("user");
    QJsonArray userParts;
    QJsonObject userTextPart;
    userTextPart["text"] = QString("Hi");
    userParts.append(userTextPart);
    userContent["parts"] = userParts;
    contents.append(userContent);
    requestBody["contents"] = contents;

    QUrl url(apiUrl());
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("x-goog-api-key", m_apiKey.toUtf8());
    req.setTransferTimeout(TEST_TIMEOUT_MS);
    // Disable HTTP/2 -- Qt's HTTP/2 layer intercepts 401 as an auth challenge
    // instead of passing the response body through, breaking custom auth schemes
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onTestReply(reply);
    });
}

void GeminiProvider::onTestReply(QNetworkReply* reply)
{
    reply->deleteLater();

    QByteArray responseBody = reply->readAll();
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpStatus == 401 || httpStatus == 403) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.authFailed", "Authentication failed: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.invalidKey", "Invalid API key"));
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.connectionFailed", "Connection failed: %1").arg(reply->errorString()));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseBody);
    if (doc.object().contains("error")) {
        QJsonValue errVal = doc.object()["error"];
        QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
        if (errorMsg.isEmpty())
            errorMsg = tr_("ai.test.unknownError", "Unknown API error");
        emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
        return;
    }

    emit testResult(true, tr_("ai.gemini.connected", "Connected to Gemini successfully"));
}

// ============================================================================
// OpenRouter Provider
// ============================================================================

OpenRouterProvider::OpenRouterProvider(QNetworkAccessManager* networkManager,
                                         const QString& apiKey,
                                         const QString& model,
                                         QObject* parent)
    : AIProvider(networkManager, parent)
    , m_apiKey(apiKey)
    , m_model(model)
{
}

void OpenRouterProvider::sendRequest(const QJsonObject& requestBody)
{
    QUrl url(QString::fromLatin1(API_URL));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
    // Attribution headers for OpenRouter leaderboard
    req.setRawHeader("HTTP-Referer", "https://github.com/Kulitorum/Decenza");
    req.setRawHeader("X-Title", "Decenza");
    req.setTransferTimeout(ANALYSIS_TIMEOUT_MS);

    m_retryFn = [this, requestBody]() { sendRequest(requestBody); };

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onAnalysisReply(reply);
    });
}

void OpenRouterProvider::analyze(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.openrouter.keyOrModelMissing", "OpenRouter API key or model not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    // OpenRouter uses OpenAI-compatible format
    QJsonObject requestBody;
    requestBody["model"] = m_model;
    QJsonArray messages;
    QJsonObject sysMsg;
    sysMsg["role"] = QString("system");
    sysMsg["content"] = systemPrompt;
    messages.append(sysMsg);
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = userPrompt;
    messages.append(userMsg);
    requestBody["messages"] = messages;
    requestBody["max_tokens"] = 4096;   // [barista-fork] was 1024 — short replies were truncating mid-sentence

    sendRequest(requestBody);
}

void OpenRouterProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.openrouter.keyOrModelMissing", "OpenRouter API key or model not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["messages"] = buildOpenAIMessages(systemPrompt, messages);
    requestBody["max_tokens"] = 4096;   // [barista-fork] was 1024 — short replies were truncating mid-sentence

    sendRequest(requestBody);
}

void OpenRouterProvider::onAnalysisReply(QNetworkReply* reply)
{
    if (tryScheduleRetry(reply)) { reply->deleteLater(); return; }
    reply->deleteLater();
    setStatus(Status::Ready);

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        if (!body.isEmpty()) {
            QJsonDocument bodyDoc = QJsonDocument::fromJson(body);
            QString apiError = bodyDoc.object()["error"].toObject()["message"].toString();
            if (!apiError.isEmpty()) {
                int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                qWarning() << "OpenRouter API error" << status << "-" << apiError;
                emit analysisFailed(tr_("ai.openrouter.error", "OpenRouter error: %1").arg(apiError));
                return;
            }
            qWarning() << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << body;
        }
        emit analysisFailed(friendlyNetworkError(reply));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject root = doc.object();

    if (root.contains("error")) {
        QString errorMsg = root["error"].toObject()["message"].toString();
        emit analysisFailed(tr_("ai.openrouter.error", "OpenRouter error: %1").arg(errorMsg));
        return;
    }

    QJsonArray choices = root["choices"].toArray();
    if (choices.isEmpty()) {
        emit analysisFailed(tr_("ai.openrouter.noResponse", "OpenRouter returned no response"));
        return;
    }

    QString content = choices[0].toObject()["message"].toObject()["content"].toString();
    if (content.isEmpty()) {
        emit analysisFailed(tr_("ai.openrouter.emptyContent", "OpenRouter returned empty response content"));
        return;
    }
    emit analysisComplete(content);
}

void OpenRouterProvider::testConnection()
{
    if (!isConfigured()) {
        emit testResult(false, tr_("ai.openrouter.testKeyOrModel", "API key or model not configured"));
        return;
    }

    // Send a minimal request to test the API key and model
    QJsonObject requestBody;
    requestBody["model"] = m_model;
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = QString("Hi");
    messages.append(userMsg);
    requestBody["messages"] = messages;
    requestBody["max_tokens"] = 10;

    QUrl url(QString::fromLatin1(API_URL));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
    req.setRawHeader("HTTP-Referer", "https://github.com/Kulitorum/Decenza");
    req.setRawHeader("X-Title", "Decenza");
    req.setTransferTimeout(TEST_TIMEOUT_MS);
    // Disable HTTP/2 -- Qt's HTTP/2 layer intercepts 401 as an auth challenge
    // instead of passing the response body through, breaking custom auth schemes
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onTestReply(reply);
    });
}

void OpenRouterProvider::onTestReply(QNetworkReply* reply)
{
    reply->deleteLater();

    QByteArray responseBody = reply->readAll();
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpStatus == 401) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.authFailed", "Authentication failed: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.invalidKey", "Invalid API key"));
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject() && doc.object().contains("error")) {
            QJsonValue errVal = doc.object()["error"];
            QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
            if (!errorMsg.isEmpty()) {
                emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
                return;
            }
        }
        emit testResult(false, tr_("ai.test.connectionFailed", "Connection failed: %1").arg(reply->errorString()));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseBody);
    if (doc.object().contains("error")) {
        QJsonValue errVal = doc.object()["error"];
        QString errorMsg = errVal.isObject() ? errVal.toObject()["message"].toString() : errVal.toString();
        if (errorMsg.isEmpty())
            errorMsg = tr_("ai.test.unknownError", "Unknown API error");
        emit testResult(false, tr_("ai.test.apiError", "API error: %1").arg(errorMsg));
        return;
    }

    emit testResult(true, tr_("ai.openrouter.connected", "Connected to OpenRouter successfully"));
}

// ============================================================================
// Ollama Provider
// ============================================================================

OllamaProvider::OllamaProvider(QNetworkAccessManager* networkManager,
                               const QString& endpoint,
                               const QString& model,
                               QObject* parent)
    : AIProvider(networkManager, parent)
    , m_endpoint(endpoint)
    , m_model(model)
{
}

void OllamaProvider::sendRequest(const QUrl& url, const QJsonObject& requestBody)
{
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setTransferTimeout(LOCAL_ANALYSIS_TIMEOUT_MS);

    m_retryFn = [this, url, requestBody]() { sendRequest(url, requestBody); };

    QByteArray body = QJsonDocument(requestBody).toJson();
    QNetworkReply* reply = m_networkManager->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onAnalysisReply(reply);
    });
}

void OllamaProvider::analyze(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.ollama.notConfigured", "Ollama not configured (need endpoint and model)"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["prompt"] = userPrompt;
    requestBody["system"] = systemPrompt;
    requestBody["stream"] = false;

    QString urlStr = m_endpoint;
    if (!urlStr.endsWith(QString("/"))) urlStr += QString("/");
    urlStr += QString("api/generate");

    sendRequest(QUrl(urlStr), requestBody);
}

void OllamaProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.ollama.notConfigured", "Ollama not configured (need endpoint and model)"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;

    // Use /api/chat which supports messages array natively
    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["stream"] = false;
    requestBody["messages"] = buildOpenAIMessages(systemPrompt, messages);

    QString urlStr = m_endpoint;
    if (!urlStr.endsWith(QString("/"))) urlStr += QString("/");
    urlStr += QString("api/chat");

    sendRequest(QUrl(urlStr), requestBody);
}

void OllamaProvider::onAnalysisReply(QNetworkReply* reply)
{
    if (tryScheduleRetry(reply)) { reply->deleteLater(); return; }
    reply->deleteLater();
    setStatus(Status::Ready);

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        if (!body.isEmpty())
            qWarning() << "Ollama request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << body;
        emit analysisFailed(friendlyNetworkError(reply));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject root = doc.object();

    if (root.contains("error")) {
        emit analysisFailed(tr_("ai.ollama.error", "Ollama error: %1").arg(root["error"].toString()));
        return;
    }

    // Support both /api/chat (message.content) and /api/generate (response) formats
    QString response = root["message"].toObject()["content"].toString();
    if (response.isEmpty()) {
        response = root["response"].toString();
        if (!response.isEmpty()) {
            qDebug() << "OllamaProvider: Used /api/generate response format (fallback)";
        }
    }
    if (response.isEmpty()) {
        emit analysisFailed(tr_("ai.ollama.emptyResponse", "Ollama returned empty response"));
        return;
    }

    emit analysisComplete(response);
}

void OllamaProvider::testConnection()
{
    if (m_endpoint.isEmpty()) {
        emit testResult(false, tr_("ai.ollama.endpointMissing", "Ollama endpoint not configured"));
        return;
    }

    // Test by listing models
    refreshModels();
}

void OllamaProvider::onTestReply(QNetworkReply* reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit testResult(false, tr_("ai.ollama.cannotConnect", "Cannot connect to Ollama: %1").arg(reply->errorString()));
        return;
    }

    emit testResult(true, tr_("ai.ollama.connected", "Connected to Ollama successfully"));
}

void OllamaProvider::refreshModels()
{
    QString urlStr = m_endpoint;
    if (!urlStr.endsWith(QString("/"))) urlStr += QString("/");
    urlStr += QString("api/tags");

    QUrl url(urlStr);
    QNetworkRequest req;
    req.setUrl(url);
    req.setTransferTimeout(TEST_TIMEOUT_MS);
    QNetworkReply* reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onModelsReply(reply);
    });
}

void OllamaProvider::onModelsReply(QNetworkReply* reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit testResult(false, tr_("ai.ollama.cannotList", "Cannot list Ollama models: %1").arg(reply->errorString()));
        emit modelsRefreshed({});
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonArray models = doc.object()["models"].toArray();

    QStringList modelNames;
    for (const auto& model : models) {
        modelNames.append(model.toObject()["name"].toString());
    }

    emit modelsRefreshed(modelNames);

    if (!modelNames.isEmpty()) {
        emit testResult(true, tr_("ai.ollama.foundModels", "Found %1 Ollama model(s)").arg(modelNames.size()));
    } else {
        emit testResult(false, tr_("ai.ollama.noModels", "No models found. Run: ollama pull llama3.2"));
    }
}
