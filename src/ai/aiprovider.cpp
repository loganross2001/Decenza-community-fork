#include "core/diagnosticlogging.h"

#define PROVIDER_DEBUG(tag) DECENZA_SUBSYS_VALUE_STREAM(diagnosticOwner(), tag, qDebug) << diagnosticFields()
#define PROVIDER_INFO(tag) DECENZA_SUBSYS_VALUE_STREAM(diagnosticOwner(), tag, qInfo) << diagnosticFields()
#define PROVIDER_WARN(tag) DECENZA_SUBSYS_VALUE_STREAM(diagnosticOwner(), tag, qWarning) << diagnosticFields()
#include "aiprovider.h"
#include "airequestshape.h"
#include <QDateTime>
#include <QDebug>
#include "../core/translationmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QStringList>
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

QString AIProvider::tr_(const char* key, const char* fallback) const {
    return translateOrFallback(m_translationManager, key, fallback);
}

QString AIProvider::truncatedResponseError() const
{
    // Deliberately does NOT say "ask a more specific question": analyzeUrl() is
    // the recipe-wizard URL extraction, where there is no question to narrow —
    // the user pasted a link. Keep the remedy true on every path.
    return tr_("ai.error.truncated",
               "The AI's reply was cut off before it finished. Please try again.");
}

QString AIProvider::truncationNotice() const
{
    return QStringLiteral("\n\n_") +
           tr_("ai.notice.truncated", "This reply was cut off before it finished.") +
           QStringLiteral("_");
}

bool AIProvider::dispatchTruncatedOrEmpty(const QString& text, bool truncated,
                                          const QString& emptyMessage)
{
    if (!truncated && !text.isEmpty())
        return false;  // ordinary reply — caller emits it

    if (truncated && !text.isEmpty() && m_truncationPolicy == TruncationPolicy::ShowPartial) {
        emit analysisComplete(text + truncationNotice());
        return true;
    }
    emit analysisFailed(truncated ? truncatedResponseError() : emptyMessage);
    return true;
}

QString AIProvider::logSafeErrorBody(const QByteArray& body)
{
    // Even error.type/code and HTML prefixes are remote text and may echo a
    // prompt or credentials. Shape + byte count are sufficient beside HTTP and
    // QNetworkReply's numeric status, captured on the operation itself.
    return QStringLiteral("bodyFormat=%1 bodyBytes=%2 contentOmitted")
        .arg(QJsonDocument::fromJson(body).isObject() ? QStringLiteral("json") : QStringLiteral("other"))
        .arg(body.size());
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
    if (auto operation = m_logOperation.lock())
        operation->network(QStringLiteral("provider"), reply->url().toString(),
                           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                           int(reply->error()));
    if (!m_retryFn) return false;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!isRetryableHttpStatus(status, m_retryCount)) return false;
    const int delay = computeRetryDelayMs(++m_retryCount, reply);
    const QByteArray body = reply->readAll();
    PROVIDER_DEBUG("Retry") << "HTTP" << status << "- retry" << m_retryCount
               << "in" << delay << "ms"
               << (body.isEmpty() ? QString() : QStringLiteral("- ") + logSafeErrorBody(body));
    // QTimer::singleShot is intentional: the server signalled a transient error and we must
    // wait before retrying (rate-limit or overload backoff). This is a server-driven delay,
    // not a heuristic guard. The generation counter prevents stale timers from firing if a
    // new analyze() call arrives before this one fires.
    const int gen = m_reqGen;
    QTimer::singleShot(delay, this, [this, gen]() {
        if (gen != m_reqGen) return;
        // sendRequest replaces m_retryFn. Keep this callable and its captured
        // request alive until the resend has finished reading that request.
        const auto retry = m_retryFn;
        retry();
    });
    return true;
}

void AIProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    // Default fallback: flatten messages into a single string and call analyze()
    // This loses multi-turn context — providers should override for native support
    PROVIDER_WARN("AIProvider") << "analyzeConversation: Using flatten fallback for provider"
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
    // Order = UI order; first entry is the recommended default. GPT-5.6 Terra
    // leads: it is cheaper than GPT-5.4 on both input and output AND a
    // generation newer, and in live replay testing the 5.6 pair caught a
    // 64.3g/8.5s blowout in recent history that BOTH 5.4 models missed — the
    // split was generational, not tier. Luna is the cheap opt-in and measured
    // at least as well as Terra; it is not the default only because six
    // scenarios is too thin a base to crown the smallest tier. The two 5.4
    // entries stay as known-quantity fallbacks.
    //
    // Pricing figures, the replay methodology and the per-model defects live in
    // docs/CLAUDE_MD/AI_ADVISOR.md so they don't rot in code. Revisit as models
    // land.
    return {
        { "gpt-5.6-terra", "GPT-5.6 Terra" },
        { "gpt-5.6-luna", "GPT-5.6 Luna" },
        { "gpt-5.4", "GPT-5.4" },
        { "gpt-5.4-mini", "GPT-5.4 mini" },
    };
}

// Per-shot cost estimates.
//
// Derived from a measured shot-analysis request — ~17K input tokens (the
// assembled system + user prompt) and ~300 output — priced at each model's
// published rate. Cold cache: repeat shots on the same profile cost less,
// because the system prompt is cached at ~90% off. Rounded to the cent the
// user would actually notice, except where a cent would round the figure to
// "$0.00" and say nothing at all — Luna ($0.004) and Gemini 2.5 Flash
// ($0.006) are quoted to a tenth of a cent for that reason. Monthly figures
// are the nearest nickel to the derivation table in AI_ADVISOR.md.
//
// These WILL rot. They live beside availableModels() so a catalog change puts
// the cost line in the same diff; docs/CLAUDE_MD/AI_ADVISOR.md carries the
// per-million rates they were computed from.
//
// Every catalogued model gets its OWN case and an unknown id returns nothing.
// The tempting shape — fall through to the default model's price — quietly
// promises a specific spend for a model nobody priced, and the size of that
// error is unbounded: Luna and GPT-5.4 differ by 12x inside this one catalog.
// A missing cost line is a gap the user can see; a wrong one is not.
QString OpenAIProvider::costHintFor(const QString& modelId) const
{
    if (modelId == QLatin1String("gpt-5.6-terra"))
        return tr_("ai.cost.openai.terra",
                   "About $0.04 per shot — roughly $3.40/month at 3 shots a day.");
    if (modelId == QLatin1String("gpt-5.6-luna"))
        return tr_("ai.cost.openai.luna",
                   "About $0.004 per shot — roughly $0.35/month at 3 shots a day.");
    if (modelId == QLatin1String("gpt-5.4-mini"))
        return tr_("ai.cost.openai.mini",
                   "About $0.01 per shot — roughly $1.25/month at 3 shots a day.");
    if (modelId == QLatin1String("gpt-5.4"))
        return tr_("ai.cost.openai.gpt54",
                   "About $0.05 per shot — roughly $4.25/month at 3 shots a day.");
    return {};
}

QString OpenAIProvider::modelHint() const
{
    return QStringLiteral(
        "GPT-5.6 Terra is recommended. GPT-5.6 Luna is much cheaper and did as well in testing. "
        "GPT-5.4 and GPT-5.4 mini are the older generation — both missed a failed shot the 5.6 "
        "models caught, and mini gives the weakest dial-in advice.");
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
    PROVIDER_WARN("OpenAIProvider") << "setModel ignoring unknown model id:" << modelId;
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
    QString urlStr = m_baseUrl.isEmpty()
        ? QString::fromLatin1(API_URL)
        : m_baseUrl + QStringLiteral("/v1/chat/completions");
    QUrl url(urlStr);
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
    m_truncationPolicy = TruncationPolicy::Fail;

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
    requestBody["max_completion_tokens"] = MAX_OUTPUT_TOKENS;
    // Reasoning off — rationale and INVARIANT in
    // AIRequestShape::disableOpenAIReasoning() (src/ai/airequestshape.h),
    // shared with the bulk translator so the two cannot drift.
    AIRequestShape::disableOpenAIReasoning(requestBody);

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
    m_truncationPolicy = TruncationPolicy::Fail;

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
    // web_search below "low". The 5.6 generation accepts web_search at "none"
    // (verified live 2026-07-30, all three tiers), so this is a 5.4-generation
    // floor kept because it is valid for every catalog entry — not a universal
    // web_search requirement. max_output_tokens covers reasoning + the JSON answer.
    //
    // Unlike analyze(), the effort here is NOT a nextShot-block risk: this path
    // extracts recipe JSON from a URL and never emits that block.
    QJsonObject reasoning;
    reasoning["effort"] = QString("low");
    requestBody["reasoning"] = reasoning;
    requestBody["max_output_tokens"] = MAX_OUTPUT_TOKENS;

    sendResponsesRequest(requestBody);
}

void OpenAIProvider::searchWeb(const QString& systemPrompt, const QString& userPrompt)
{
    // OpenAI is the one provider whose single Responses `web_search` tool both
    // searches and opens a page, so this request is analyzeUrl's request. An
    // alias, not a copy: two bodies that must stay identical are two bodies
    // free to drift.
    analyzeUrl(systemPrompt, userPrompt);
}


void OpenAIProvider::sendResponsesRequest(const QJsonObject& requestBody)
{
    QString urlStr = m_baseUrl.isEmpty()
        ? QString::fromLatin1(RESPONSES_API_URL)
        : m_baseUrl + QStringLiteral("/v1/responses");
    QUrl url(urlStr);
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
                PROVIDER_DEBUG("aiprovider") << "OpenAI Responses API error" << status << "remoteErrorContentOmitted";
                emit analysisFailed(tr_("ai.openai.error", "OpenAI error: %1").arg(apiError));
                return;
            }
            // Bounded/classified, never the raw body — see logSafeErrorBody().
            PROVIDER_DEBUG("aiprovider") << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << logSafeErrorBody(body);
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
    // Collect the part types too: when the answer is missing, WHAT came back
    // instead is the whole diagnosis (#1691's lesson — log shape, not content).
    QString text;
    QString refusal;
    QStringList partTypes;
    const QJsonArray output = root["output"].toArray();
    for (const QJsonValue& itemVal : output) {
        const QJsonObject item = itemVal.toObject();
        if (item["type"].toString() != QLatin1String("message"))
            continue;
        const QJsonArray content = item["content"].toArray();
        for (const QJsonValue& partVal : content) {
            const QJsonObject part = partVal.toObject();
            const QString partType = part["type"].toString();
            partTypes << partType;
            if (partType == QLatin1String("output_text"))
                text += part["text"].toString();
            else if (partType == QLatin1String("refusal"))
                refusal = part["refusal"].toString();
        }
    }

    // Gate on completion, not on one reason. `incomplete_details.reason` is
    // also "content_filter", and status can be "failed"/"cancelled" — anything
    // other than "completed" means the text in hand is not the whole answer.
    const QString status = root["status"].toString();
    const QString incompleteReason = root["incomplete_details"].toObject()["reason"].toString();
    const bool truncated = status != QLatin1String("completed");
    if (text.isEmpty() || truncated) {
        PROVIDER_DEBUG("aiprovider") << "OpenAI Responses: model" << diagnosticModel() << "status" << diagnosticCode(status)
                   << "incomplete_reason" << diagnosticCode(incompleteReason)
                   << "part count" << partTypes.size() << "text chars" << text.size();
        // A refusal explains itself; surfacing it beats the generic message,
        // which is the failure #1691 took three days to place.
        if (text.isEmpty() && !refusal.isEmpty()) {
            emit analysisFailed(tr_("ai.openai.refused", "OpenAI declined the request: %1").arg(refusal));
            return;
        }
        if (dispatchTruncatedOrEmpty(text, truncated,
                tr_("ai.openai.emptyContent", "OpenAI returned empty response content")))
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
    m_truncationPolicy = TruncationPolicy::Fail;
    // A conversation turn is prose the user reads, so a cut-off reply still has
    // value — show it with a notice rather than discarding it (see
    // TruncationPolicy). The one-shot analyze()/analyzeUrl() paths keep Fail:
    // their result is machine-parsed.
    m_truncationPolicy = TruncationPolicy::ShowPartial;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["messages"] = buildOpenAIMessages(systemPrompt, messages);
    // [barista-fork] Voice path: GPT-5 needs max_completion_tokens (not the legacy
    // max_tokens — see analyze()). MAX_OUTPUT_TOKENS (4096) keeps spoken replies from
    // truncating mid-sentence; reasoning_effort=none keeps hidden reasoning tokens from
    // eating that cap.
    requestBody["max_completion_tokens"] = MAX_OUTPUT_TOKENS;
    // Rationale and INVARIANT in AIRequestShape::disableOpenAIReasoning().
    // This is the dial-in conversation path — the one that emits the trailing
    // nextShot block that rationale is written about — so it matters most here.
    AIRequestShape::disableOpenAIReasoning(requestBody);

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
                PROVIDER_DEBUG("aiprovider") << "OpenAI API error" << status << "remoteErrorContentOmitted";
                emit analysisFailed(tr_("ai.openai.error", "OpenAI error: %1").arg(apiError));
                return;
            }
            // Bounded/classified, never the raw body — see logSafeErrorBody().
            PROVIDER_DEBUG("aiprovider") << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << logSafeErrorBody(body);
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

    const QJsonObject choice = choices[0].toObject();
    const QString finishReason = choice["finish_reason"].toString();
    const QJsonObject message = choice["message"].toObject();
    QString content = message["content"].toString();
    // "length" = hit max_completion_tokens; "content_filter" = moderation cut
    // it short. Either way the text in hand is not the whole answer.
    const bool truncated = finishReason == QLatin1String("length")
                        || finishReason == QLatin1String("content_filter");
    if (content.isEmpty() || truncated) {
        PROVIDER_DEBUG("OpenAI") << "model" << diagnosticModel() << "finish_reason" << diagnosticCode(finishReason)
                   << "content chars" << content.size()
                   << "reasoning tokens"
                   << root["usage"].toObject()["completion_tokens_details"]
                          .toObject()["reasoning_tokens"].toInt();
        // `message.refusal` carries the model's own stated reason; without this
        // it reads as a bare "empty response content".
        const QString refusal = message["refusal"].toString();
        if (content.isEmpty() && !refusal.isEmpty()) {
            emit analysisFailed(tr_("ai.openai.refused", "OpenAI declined the request: %1").arg(refusal));
            return;
        }
        if (dispatchTruncatedOrEmpty(content, truncated,
                tr_("ai.openai.emptyContent", "OpenAI returned empty response content")))
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
    QString urlStr = m_baseUrl.isEmpty()
        ? QStringLiteral("https://api.openai.com/v1/models")
        : m_baseUrl + QStringLiteral("/v1/models");
    QUrl url(urlStr);
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

// Thinking-off lives in AIRequestShape::disableAnthropicThinking()
// (src/ai/airequestshape.h) — the rationale, the #1691 mechanism and the
// INVARIANT are documented there. It is shared rather than local because the
// bulk translator builds its own Anthropic bodies and has to apply the same
// rule; it previously did not. Do not reintroduce a local copy.
using AIRequestShape::disableAnthropicThinking;

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
    // Order = UI order; first entry is the recommended default. Sonnet 5 leads:
    // it is both more capable and CHEAPER than Sonnet 4.6 at its current rate
    // ($2/$10 vs $3/$15 per 1M), so there is no longer a reason to lead with the
    // older model. See costHintFor() for why the promotional rate is treated as
    // the working number.
    //
    // Safe to default to specifically because of the #1691 mechanism: omitting
    // the `thinking` field runs ADAPTIVE thinking on Sonnet 5, which can consume
    // the whole max_tokens budget and return no text block. Every Anthropic
    // request goes through AIRequestShape::disableAnthropicThinking(), and that
    // Sonnet 5 accepts `{"type": "disabled"}` AND still returns a text block was
    // verified live (2026-07-30) rather than assumed — see
    // tools/ai_model_eval/probe_request_shape.py.
    return {
        { "claude-sonnet-5", "Sonnet 5" },
        { "claude-sonnet-4-6", "Sonnet 4.6" },
    };
}

// See the note above OpenAIProvider::costHintFor() for how these are derived.
//
// Sonnet 5 is priced here at its introductory $2/$10 per 1M rather than the
// $3/$15 list rate. That is a judgement call, not an oversight: the intro rate
// is nominally dated, but the GPT-5.6 generation reset the price floor
// underneath it, so list is treated as a ceiling that is unlikely to be
// charged. If Anthropic does revert, this number goes UP — which is the safe
// direction for a promise made to a user about spend.
QString AnthropicProvider::costHintFor(const QString& modelId) const
{
    if (modelId == QLatin1String("claude-sonnet-5"))
        return tr_("ai.cost.anthropic.sonnet5",
                   "About $0.04 per shot — roughly $3.35/month at 3 shots a day.");
    // The comparative line is why this case must be exact rather than a
    // fallthrough: "Sonnet 5 is both newer and cheaper" is a claim ABOUT
    // Sonnet 4.6, and shown against any other model it is simply false.
    if (modelId == QLatin1String("claude-sonnet-4-6"))
        return tr_("ai.cost.anthropic.sonnet46",
                   "About $0.06 per shot — roughly $5/month at 3 shots a day. "
                   "Sonnet 5 is both newer and cheaper.");
    return {};
}

QString AnthropicProvider::modelHint() const
{
    return QStringLiteral("Sonnet 5 is recommended — more capable than Sonnet 4.6 and currently cheaper. "
                          "Sonnet 4.6 is the previous generation.");
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
    PROVIDER_WARN("AnthropicProvider") << "setModel ignoring unknown model id:" << modelId;
}

QString AnthropicProvider::shortModelName() const
{
    for (const ModelOption& opt : availableModels()) {
        if (opt.id == m_model)
            return opt.displayName;
    }
    return m_model;
}

void AnthropicProvider::sendRequest(const QJsonObject& requestBody, const QByteArray& betaFeature)
{
    QString urlStr = m_baseUrl.isEmpty()
        ? QString::fromLatin1(API_URL)
        : m_baseUrl + QStringLiteral("/v1/messages");
    QUrl url(urlStr);
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    req.setRawHeader("x-api-key", m_apiKey.toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");
    // Beta opt-in, for the request bodies that carry a beta tool. Empty for
    // everything else — the header is per-feature, not a blanket flag.
    if (!betaFeature.isEmpty())
        req.setRawHeader("anthropic-beta", betaFeature);
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
    // [barista-fork] Streaming turn: parse SSE incrementally on readyRead (early speech), and finalize off the
    // accumulated events on finished. Each round of a streaming turn re-POSTs through here, so reset the
    // per-round accumulation now. The non-streaming path is untouched.
    if (m_streaming) {
        resetStreamState();
        connect(reply, &QNetworkReply::readyRead, this, [this, reply]() { onStreamReadyRead(reply); });
        connect(reply, &QNetworkReply::finished, this, [this, reply]() { onStreamReply(reply); });
    } else {
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            onAnalysisReply(reply);
        });
    }
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
    m_truncationPolicy = TruncationPolicy::Fail;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["max_tokens"] = MAX_OUTPUT_TOKENS;   // advisor dial-in — short structured reply; the cap is a ceiling
    disableAnthropicThinking(requestBody);
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
    // One-shot URL extraction is machine-parsed, so a truncated reply must fail
    // rather than surface a partial (unlike the voice conversation path).
    m_truncationPolicy = TruncationPolicy::Fail;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["max_tokens"] = MAX_OUTPUT_TOKENS;
    disableAnthropicThinking(requestBody);
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

    // web_fetch is a BETA tool: without the opt-in header the API rejects the
    // request outright, so stage-2 extraction on Anthropic could never have
    // worked. (web_search, used by searchWeb, is GA and needs no header.)
    sendRequest(requestBody, QByteArrayLiteral("web-fetch-2025-09-10"));
}

void AnthropicProvider::searchWeb(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.anthropic.keyMissing", "Anthropic API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;
    m_truncationPolicy = TruncationPolicy::Fail;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["max_tokens"] = MAX_OUTPUT_TOKENS;
    disableAnthropicThinking(requestBody);
    requestBody["system"] = buildCachedSystemPrompt(systemPrompt);
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = userPrompt;
    messages.append(userMsg);
    requestBody["messages"] = messages;
    // web_search, NOT web_fetch: this path has no URL to fetch — finding one is
    // the whole question. web_fetch validates that the URL appears in the
    // message, so it cannot serve a search at all.
    QJsonObject searchTool;
    searchTool["type"] = QString("web_search_20250305");
    searchTool["name"] = QString("web_search");
    searchTool["max_uses"] = 3;
    requestBody["tools"] = QJsonArray{searchTool};

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
    m_forceRespond = options.forceRespond;   // [barista-fork] this turn: tool_choice:"any" + `respond` (barista only)
    m_streaming = options.streaming;   // [barista-fork] this turn streams (SSE) — see resetStreamState / sendRequest
    resetStreamState();
    ++m_reqGen;
    m_truncationPolicy = TruncationPolicy::Fail;
    // A conversation turn is prose the user reads, so a cut-off reply still has
    // value — show it with a notice rather than discarding it (see
    // TruncationPolicy). The one-shot analyze()/analyzeUrl() paths keep Fail:
    // their result is machine-parsed.
    m_truncationPolicy = TruncationPolicy::ShowPartial;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    // [barista-fork] Voice path: MAX_OUTPUT_TOKENS (4096) keeps spoken replies from
    // truncating mid-sentence; disableAnthropicThinking keeps hidden reasoning from
    // eating that cap. A truncated turn still shows its partial (ShowPartial above).
    requestBody["max_tokens"] = MAX_OUTPUT_TOKENS;
    disableAnthropicThinking(requestBody);
    // [barista-fork] Cache the stable core prefix (options.cachePrefixLen) so the per-question tailoring's varying
    // module suffix doesn't cost a full cache miss every turn. -1 (advisor/coach, or untailored) = whole prompt.
    requestBody["system"] = buildCachedSystemPrompt(systemPrompt, options.cachePrefixLen);
    // [barista-fork] Cache-wrap the first user message, then (for a vision turn) attach the image to the LAST
    // user message. Order matters: the cache wrap turns the first message's content into an array, and when the
    // conversation is a single message (first == last) messagesWithImageOnLastUser then appends the image block
    // to that same array. The image rides options (per-turn), never the persisted messages — no re-billing.
    QJsonArray outMessages = messagesWithCachedFirstUser(messages);
    if (!options.imageData.isEmpty())
        outMessages = messagesWithImageOnLastUser(outMessages, options.imageData, options.imageMediaType);
    requestBody["messages"] = outMessages;
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
    // [barista-fork] Forced-respond turn protocol (the structural stall fix). With tool_choice:"any" the model
    // MUST call a tool every turn — it can no longer end a turn with a bare "let me check…" promise and stop
    // (the reported stall). The only "answer" tool is `respond`, so every reply the user hears comes through it;
    // to actually check something the model must call a real tool first (get_weather/query_shots/…), whose result
    // loops back, and then call `respond`. (We used to also set disable_parallel_tool_use to keep it to ONE tool
    // per turn, but Anthropic now rejects that alongside its server tools — see the tool_choice block below.)
    // `respond` is intercepted in onAnalysisReply — never executed — and its `text` becomes the turn's answer.
    // Anthropic-only; other providers
    // never see options.forceRespond. Fail-safe: if a turn somehow ends without `respond`, the terminal path below
    // still delivers whatever text exists, so this can't be worse than the auto/heuristic path.
    if (m_forceRespond && !tools.isEmpty()) {
        QJsonObject respondTool;
        respondTool["name"] = QString("respond");
        respondTool["description"] = QString(
            "Say something to the user. This is the ONLY way to speak to them — your spoken reply goes in `text`. "
            "Call it to answer, greet, acknowledge, or sign off. If you need to look something up first (weather, "
            "their shots, the web), call that tool FIRST; its result comes back and THEN you call respond with the "
            "answer. NEVER promise to check something without calling the tool — just call the tool.");
        QJsonObject rSchema; rSchema["type"] = QString("object");
        QJsonObject rProps;
        QJsonObject rText; rText["type"] = QString("string");
        rText["description"] = QString("Your reply to speak to the user, in natural spoken language.");
        rProps["text"] = rText;
        rSchema["properties"] = rProps;
        rSchema["required"] = QJsonArray{ QString("text") };
        respondTool["input_schema"] = rSchema;
        tools.append(respondTool);
        QJsonObject toolChoice;
        toolChoice["type"] = QString("any");
        // [barista-fork] DO NOT set disable_parallel_tool_use here. Anthropic now REJECTS the whole request with
        // "tool_choice.disable_parallel_tool_use: true cannot be used with programmatic tool calling" whenever a
        // server-side tool is present (the web_search tool this barista adds under the web toggle). That killed
        // every forced-respond turn on Claude — the user talked, the request 400'd, and the turn dropped straight
        // back to listening ("I have to ask multiple times / it never stops"). Verified in barista-diagnostics
        // 2026-08-19. Losing the single-tool guarantee only means the model MAY emit a real tool AND `respond`
        // together; `respond` is still intercepted (never executed) and the terminal path delivers its text, so
        // this is strictly safer than the request failing.
        requestBody["tool_choice"] = toolChoice;
    }
    if (!tools.isEmpty())
        requestBody["tools"] = tools;

    // [barista-fork] Stream this turn's reply when the caller asked for it (voiceStreaming && !webSearch). Set on
    // the base body here so every re-POST of this turn (tool rounds / pause_turn) inherits it via
    // m_pendingRequestBody — sendRequest wires readyRead + the streaming finished handler when m_streaming.
    if (m_streaming)
        requestBody["stream"] = true;

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

QJsonArray AnthropicProvider::messagesWithImageOnLastUser(const QJsonArray& messages,
                                                          const QByteArray& imageData, const QString& mediaType)
{
    if (imageData.isEmpty()) return messages;
    // Find the last user message — that is the CURRENT turn the image belongs to.
    qsizetype target = -1;
    for (qsizetype i = messages.size() - 1; i >= 0; --i) {
        if (messages[i].toObject().value("content").isNull()) continue;
        if (messages[i].toObject().value("role").toString() == QLatin1String("user")) { target = i; break; }
    }
    if (target < 0) return messages;   // no user message — nothing to attach to

    QJsonObject msg = messages[target].toObject();
    // Content is either a plain string (typical) or already an array (single-message case: the cache wrap ran
    // first). Normalize to an array of blocks, preserving whatever text is there, then append the image block.
    QJsonArray content;
    const QJsonValue cv = msg.value("content");
    if (cv.isArray()) {
        content = cv.toArray();
    } else {
        QJsonObject textBlock;
        textBlock["type"] = QString("text");
        textBlock["text"] = cv.toString();
        content.append(textBlock);
    }

    QJsonObject source;
    source["type"] = QString("base64");
    source["media_type"] = mediaType.isEmpty() ? QString("image/jpeg") : mediaType;
    source["data"] = QString::fromLatin1(imageData.toBase64());
    QJsonObject imageBlock;
    imageBlock["type"] = QString("image");
    imageBlock["source"] = source;
    content.append(imageBlock);

    msg["content"] = content;
    QJsonArray out = messages;
    out[target] = msg;
    return out;
}

QJsonArray AnthropicProvider::buildCachedSystemPrompt(const QString& systemPrompt, int cachePrefixLen)
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

    QJsonArray systemArray;

    // [barista-fork] Per-question tailoring appends a VARYING module suffix onto a STABLE core. Put the cache
    // breakpoint at the core boundary so the (unchanged) core is cached and reused every turn while only the
    // small suffix is reprocessed — without a breakpoint there, the whole prompt differs each turn and Anthropic
    // gets a cache MISS on every turn. Only split on a real, in-range boundary; otherwise cache the whole prompt.
    if (cachePrefixLen > 0 && cachePrefixLen < systemPrompt.size()) {
        QJsonObject core;
        core["type"] = QString("text");
        core["text"] = systemPrompt.left(cachePrefixLen);
        core["cache_control"] = cacheControl;   // cache_control caches the cumulative prefix up to here
        systemArray.append(core);
        QJsonObject suffix;
        suffix["type"] = QString("text");
        suffix["text"] = systemPrompt.mid(cachePrefixLen);   // varying modules — not cached
        systemArray.append(suffix);
        return systemArray;
    }

    QJsonObject block;
    block["type"] = QString("text");
    block["text"] = systemPrompt;
    block["cache_control"] = cacheControl;
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
                PROVIDER_DEBUG("aiprovider") << "Anthropic API error" << status << "remoteErrorContentOmitted";
                emit analysisFailed(tr_("ai.anthropic.error", "Anthropic error: %1").arg(apiError));
                return;
            }
            // Bounded/classified, never the raw body — see logSafeErrorBody().
            PROVIDER_DEBUG("aiprovider") << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << logSafeErrorBody(body);
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

    finalizeConversationResponse(root);
}

// [barista-fork] Extracted from onAnalysisReply so the (coming) SSE streaming path can drive the SAME tool
// loop / pause_turn continuation / terminal-emit logic: it assembles the streamed events into a synthetic
// `root` (assembleAnthropicResponse) and calls this, exactly as the whole-body parse does. See the header.
void AnthropicProvider::finalizeConversationResponse(const QJsonObject& root)
{
    // Read stop_reason BEFORE any content gate. An early return above it makes
    // the truncation branch unreachable for exactly the case that matters —
    // the reply that stopped with nothing to show (upstream #1691). That ordering
    // bug shipped in this file's Gemini handler and is fixed there too.
    const QString stopReason = root["stop_reason"].toString();
    // [barista-fork][diag] model reply latency + stop_reason — the other half of the tool-vs-model split.
    if (m_requestSentMs != 0)
        aiDiag(QStringLiteral("reply"), QStringLiteral("ms=%1 stop_reason=%2 round=%3")
               .arg(QDateTime::currentMSecsSinceEpoch() - m_requestSentMs).arg(stopReason).arg(m_toolRounds));
    QJsonArray content = root["content"].toArray();
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
        // Upstream #1691: distinguish a truncated empty reply (max_tokens) from a genuinely empty one.
        PROVIDER_DEBUG("Anthropic") << "model" << diagnosticModel() << "no content blocks, stop_reason" << diagnosticCode(stopReason);
        emit analysisFailed(stopReason == QLatin1String("max_tokens")
            ? truncatedResponseError()
            : tr_("ai.anthropic.noResponse", "Anthropic returned no response"));
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
        // [barista-fork] Forced-respond: the `respond` tool is the ANSWER channel, not an executable tool. Skip
        // it here (never dispatch it to the executor) and capture its text; when it's the only tool call this
        // turn, that text becomes the turn's answer via the normal terminal path below — no re-POST.
        QString respondText; bool haveRespond = false;
        for (const QJsonValue& v : content) {
            const QJsonObject block = v.toObject();
            if (block["type"].toString() != QLatin1String("tool_use"))
                continue;
            if (m_forceRespond && block["name"].toString() == QLatin1String("respond")) {
                respondText = block["input"].toObject().value(QStringLiteral("text")).toString();
                haveRespond = true;
                continue;
            }
            toolUses.append(block);
        }
        // Model answered via respond (no other tool this turn) → deliver respondText through the terminal emit.
        if (haveRespond && toolUses.isEmpty())
            text = respondText;
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

    // [barista-fork] A `tool_use` reaching the terminal means the CLIENT tool loop
    // exhausted MAX_TOOL_ROUNDS. That is not truncation — it has its own friendly
    // degrade path (below) — so it stays excluded from `unfinished`, or the
    // truncation dispatch would misclassify it as a cut-off reply.
    //
    // `pause_turn` is different and is deliberately NOT excluded: a server-side turn
    // that still says "pause_turn" after MAX_CONTINUATIONS never actually resolved,
    // and its text is partial by definition. Treating it as unfinished routes it
    // through dispatchTruncatedOrEmpty — Fail on the one-shot analyze()/analyzeUrl()
    // paths (a half-extracted recipe must not be delivered as complete), ShowPartial
    // (partial text + a "cut off" notice) on the barista conversation path.
    const bool loopExhausted = stopReason == QLatin1String("tool_use");
    // Upstream #1691: anything that is not a natural end (and not a loop we already
    // handled) is an unfinished turn — max_tokens, refusal,
    // model_context_window_exceeded. Allow-listing the good reasons fails safe as
    // Anthropic adds more; enumerating the bad ones does not.
    const bool unfinished = !loopExhausted
                         && stopReason != QLatin1String("end_turn")
                         && stopReason != QLatin1String("stop_sequence");

    // [barista-fork] The client tool loop hit MAX_TOOL_ROUNDS (or returned no prose) — degrade to a
    // friendly message instead of surfacing an error. Gated on tool_use (only the barista enables
    // client tools; analyzeUrl reaches the terminal via pause_turn and must fall through to the
    // failure path below, not speak "shot history" prose for a URL extraction).
    if (text.trimmed().isEmpty() && stopReason == QLatin1String("tool_use")) {
        emit analysisComplete(QStringLiteral("I got partway through that but ran out of steps before I finished — ask me to pick it back up?"));
        return;
    }
    if (text.isEmpty() || unfinished) {
        // Log the block types on any text-less reply: content can be non-empty
        // while carrying no text block at all (a thinking-only reply — #1691),
        // and the generic message alone made that indistinguishable from a
        // refusal. The model matters too: the #1691 root cause was that the
        // thinking default differs BETWEEN models.
        QStringList blockTypes;
        for (const QJsonValue& block : content)
            blockTypes << block.toObject()["type"].toString();
        PROVIDER_DEBUG("Anthropic") << "model" << diagnosticModel() << "stop_reason" << diagnosticCode(stopReason)
                   << "block count" << blockTypes.size() << "text chars" << text.size()
                   << "output tokens" << root["usage"].toObject()["output_tokens"].toInt();
        if (dispatchTruncatedOrEmpty(text, unfinished,
                tr_("ai.anthropic.emptyContent", "Anthropic returned empty response content")))
            return;
    }
    emit analysisComplete(text);
}

void AnthropicProvider::resetStreamState()
{
    m_streamParser.reset();
    m_streamEvents.clear();
    m_respondExtractor.reset();
    m_streamRespondIndex = -1;
    m_streamSawOtherTool = false;
}

void AnthropicProvider::onStreamReadyRead(QNetworkReply* reply)
{
    // Feed whatever bytes are available now to the SSE parser, accumulate the events for the finished-time
    // assembly, and emit early speech: only the `respond` tool's decoded text (or a non-forced turn's plain
    // text_delta), and only while no real tool has opened this round — a round that also runs a tool is not the
    // terminal answer, so its respond text (if any) must not be spoken.
    const QVector<barista::SseEvent> evs = m_streamParser.feed(reply->readAll());
    for (const barista::SseEvent& e : evs) {
        m_streamEvents.append(e);
        switch (e.kind) {
        case barista::SseEvent::ContentBlockStart:
            if (e.blockType == QLatin1String("tool_use")) {
                if (m_forceRespond && e.toolName == QLatin1String("respond")) {
                    m_streamRespondIndex = e.index;   // this block's input.text is the spoken answer
                    m_respondExtractor.reset();
                } else {
                    m_streamSawOtherTool = true;       // a real tool this round → suppress respond speech
                }
            }
            break;
        case barista::SseEvent::TextDelta:
            if (!m_streamSawOtherTool && !e.text.isEmpty())
                emit streamTextDelta(e.text);
            break;
        case barista::SseEvent::InputJsonDelta:
            if (e.index == m_streamRespondIndex && !m_streamSawOtherTool) {
                const QString spoken = m_respondExtractor.feed(e.partialJson);
                if (!spoken.isEmpty())
                    emit streamTextDelta(spoken);
            }
            break;
        default:
            break;   // MessageStart / block stop / MessageDelta / MessageStop / ping / error carry no speech
        }
    }
}

void AnthropicProvider::onStreamReply(QNetworkReply* reply)
{
    reply->deleteLater();
    setStatus(Status::Ready);

    if (reply->error() != QNetworkReply::NoError) {
        // A streamed turn that errors mid-flight: settle the voice queue (streamTextEnd), then surface the
        // failure. We deliberately do NOT retry a streaming turn — any partial reply already spoken cannot be
        // un-spoken, so a silent re-POST would double-speak. The whole feature is behind voiceStreaming, so a
        // transient failure simply ends the turn (the user can ask again).
        emit streamTextEnd();
        const QByteArray body = reply->readAll();
        const QString apiError = body.isEmpty() ? QString()
            : QJsonDocument::fromJson(body).object()["error"].toObject()["message"].toString();
        if (!apiError.isEmpty())
            emit analysisFailed(tr_("ai.anthropic.error", "Anthropic error: %1").arg(apiError));
        else
            emit analysisFailed(friendlyNetworkError(reply));
        return;
    }

    // Drain any bytes readyRead hadn't delivered before `finished` (no-op if already drained), fold the turn's
    // events into a whole-body-equivalent root, and drive the SAME finalization / tool loop the non-streaming
    // path uses. finalize re-POSTs (→ Busy) on a tool round / pause_turn and stays Ready on a terminal turn, so
    // end the spoken stream only when the turn is actually done — a re-POST keeps the queue open for next round.
    onStreamReadyRead(reply);
    const QJsonObject root = barista::assembleAnthropicResponse(m_streamEvents);
    finalizeConversationResponse(root);
    if (status() == Status::Ready)
        emit streamTextEnd();
}

void AnthropicProvider::testConnection()
{
    if (!isConfigured()) {
        emit testResult(false, tr_("ai.test.keyNotConfigured", "API key not configured"));
        return;
    }

    // Send a minimal request to test the API key. Thinking off for the same
    // reason as the analysis paths — with thinking on, a 10-token budget
    // produces a reply with no text block. This check only looks for an error,
    // so it PASSED while every real request failed (#1691): the user's key
    // tested fine and the advisor was unusable.
    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["max_tokens"] = 10;
    disableAnthropicThinking(requestBody);
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = QString("user");
    userMsg["content"] = QString("Hi");
    messages.append(userMsg);
    requestBody["messages"] = messages;

    QString urlStr = m_baseUrl.isEmpty()
        ? QString::fromLatin1(API_URL)
        : m_baseUrl + QStringLiteral("/v1/messages");
    QUrl url(urlStr);
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
    // Order = UI order; first entry is the recommended default. 3.6 Flash leads:
    // it is the current GA flagship Flash — stronger on agentic/tool-driving work
    // (which the barista's function-calling relies on) at a lower price than 3.5,
    // and it takes the 3.x thinkingLevel knob (see sendRequest). 3.5 Flash-Lite is
    // the fastest/cheapest 3.5 option; 2.5 Flash stays as the most-provisioned
    // fallback (fewer 503s, and the known-good id that predates this catalog bump —
    // if a newer id ever fails to resolve, this one still works). Revisit as new
    // models / pricing land.
    return {
        { "gemini-3.6-flash", "3.6 Flash" },
        { "gemini-3.5-flash-lite", "3.5 Flash-Lite" },
        { "gemini-2.5-flash", "2.5 Flash" },
    };
}

// See the note above OpenAIProvider::costHintFor() for how these are derived.
QString GeminiProvider::costHintFor(const QString& modelId) const
{
    // Deliberately NOT "the cheapest of the three cloud providers" — that was
    // true when Gemini's catalog was the only cheap one, and the same change
    // that wrote it added GPT-5.6 Luna at $0.004. Compare within Gemini, where
    // the claim stays true without tracking every other provider's catalog.
    //
    // [barista-fork] 3.6 Flash and 3.5 Flash-Lite are the fork's catalog entries
    // (2.5 Flash is upstream's). Their per-shot figures are rough estimates placed
    // in the ordering the availableModels() comment states — 3.6 Flash below 3.5
    // Flash, 3.5 Flash-Lite the cheapest 3.5 — i.e. an end-user budgeting hint, not
    // billed pricing; refine if Google publishes exact rates for these ids.
    if (modelId == QLatin1String("gemini-3.6-flash"))
        return tr_("ai.cost.gemini.flash36",
                   "About $0.02 per shot — roughly $1.80/month at 3 shots a day. "
                   "Gemini's newest Flash and the recommended default.");
    if (modelId == QLatin1String("gemini-3.5-flash-lite"))
        return tr_("ai.cost.gemini.flash35lite",
                   "About $0.01 per shot — roughly $0.90/month at 3 shots a day. "
                   "The fastest and cheapest 3.5 option.");
    if (modelId == QLatin1String("gemini-2.5-flash"))
        return tr_("ai.cost.gemini.flash25",
                   "About $0.006 per shot — roughly $0.55/month at 3 shots a day. "
                   "The cheapest option and the most available fallback.");
    return {};
}

QString GeminiProvider::modelHint() const
{
    return QStringLiteral("3.6 Flash is the recommended default — best at the tool-driven barista. "
                          "3.5 Flash-Lite is the fastest and cheapest. 2.5 Flash is the most available (fewer busy errors).");
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
    PROVIDER_WARN("GeminiProvider") << "setModel ignoring unknown model id:" << modelId;
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
    const QString host = m_baseUrl.isEmpty()
        ? QStringLiteral("https://generativelanguage.googleapis.com")
        : m_baseUrl;
    return host + QStringLiteral("/v1beta/models/%1:generateContent").arg(m_model);
}

void GeminiProvider::sendRequest(const QJsonObject& requestBody)
{
    QUrl url(apiUrl());
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QString("application/json")));
    // trimmed(): a key pasted on a tablet often carries a trailing newline/space; an invalid character in the
    // header value makes Qt drop the header, so Google sees NO credential and returns "unregistered callers".
    req.setRawHeader("x-goog-api-key", m_apiKey.trimmed().toUtf8());
    // Disable HTTP/2 — same fix as AnthropicProvider: Qt's HTTP/2 layer intercepts the 401 on a custom
    // auth-header scheme (here x-goog-api-key) as an auth challenge and re-drives the request WITHOUT the header,
    // so a valid key still comes back "method doesn't allow unregistered callers". HTTP/1.1 passes it through.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    // [barista-fork] Interactive barista turns pass a shorter timeout (RequestOptions.timeoutMs, ~30s) so a
    // stalled request fails+recovers fast; deep-analysis/advisor/analyzeUrl leave it 0 → the 60s default. Persists
    // across a turn's tool-round re-POSTs (m_currentTimeoutMs is set once per turn in analyzeConversation).
    req.setTransferTimeout(m_currentTimeoutMs > 0 ? m_currentTimeoutMs : ANALYSIS_TIMEOUT_MS);

    // [barista-fork] Basis for a functionCall continuation re-POST (see onAnalysisReply's tool loop). Stored
    // WITHOUT generationConfig — sendRequest re-adds it below on every call, so the loop appends to `contents`
    // and calls sendRequest() again, keeping the thinking/token config consistent across rounds.
    m_pendingRequestBody = requestBody;

    // Thinking config differs by model family: the 2.5 family uses the integer
    // thinkingBudget (0 disables thinking), while 3.x+ uses the thinkingLevel
    // enum and ignores thinkingBudget — sending the wrong knob lets thinking
    // default to "medium" (billed at the $9/MTok output rate). Pick by family
    // so each selectable model keeps thinking minimal/off.
    //
    // VERIFIED live 2026-07-30 for both catalog entries — not merely accepted,
    // but actually off: gemini-2.5-flash with thinkingBudget 0 and
    // gemini-3.5-flash with thinkingLevel "minimal" each reported
    // usageMetadata.thoughtsTokenCount == 0. Checking the status alone would
    // not have been enough; a silently ignored knob still bills thinking.
    //
    // INVARIANT for anything added later: the legal thinkingLevel values VARY
    // BY MODEL (Google's thinking docs — gemini-3-pro-preview accepts only
    // low/high, while 3.6 Flash accepts minimal/low/medium/high). "minimal" is
    // NOT a safe default for every 3.x model. Probe a new entry before adding
    // it; tools/ai_model_eval/ has the shape.
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
    generationConfig["maxOutputTokens"] = MAX_OUTPUT_TOKENS;  // [barista-fork] 4096, also bounds thinking tokens; matches other providers
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
    m_currentTimeoutMs = 0;   // [barista-fork] single-shot analyze uses the default timeout (not a barista turn)
    m_toolRounds = 0;         // [barista-fork] clear tool-loop state so a prior failed barista turn can't leak into
    m_accumulatedText.clear();// this shared onAnalysisReply path (stale text / false friendly-fallback message)
    ++m_reqGen;
    m_truncationPolicy = TruncationPolicy::Fail;

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
    m_currentTimeoutMs = 0;   // [barista-fork] URL extraction uses the default timeout (not a barista turn)
    m_toolRounds = 0;         // [barista-fork] clear tool-loop state so a prior failed barista turn can't leak into
    m_accumulatedText.clear();// this shared onAnalysisReply path (stale text / false friendly-fallback message)
    ++m_reqGen;
    m_truncationPolicy = TruncationPolicy::Fail;

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

QJsonArray GeminiProvider::contentsWithImageOnLastUser(const QJsonArray& contents,
                                                       const QByteArray& imageData, const QString& mediaType)
{
    if (imageData.isEmpty()) return contents;
    // The last user-role content is the current turn (Gemini maps assistant→"model", so user stays "user").
    qsizetype target = -1;
    for (qsizetype i = contents.size() - 1; i >= 0; --i) {
        if (contents[i].toObject().value("role").toString() == QLatin1String("user")) { target = i; break; }
    }
    if (target < 0) return contents;

    QJsonObject content = contents[target].toObject();
    QJsonArray parts = content.value("parts").toArray();
    QJsonObject inlineData;
    inlineData["mimeType"] = mediaType.isEmpty() ? QString("image/jpeg") : mediaType;
    inlineData["data"] = QString::fromLatin1(imageData.toBase64());
    QJsonObject imagePart;
    imagePart["inlineData"] = inlineData;
    parts.append(imagePart);
    content["parts"] = parts;

    QJsonArray out = contents;
    out[target] = content;
    return out;
}

void GeminiProvider::searchWeb(const QString& systemPrompt, const QString& userPrompt)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.gemini.keyMissing", "Gemini API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_retryCount = 0;
    ++m_reqGen;
    m_truncationPolicy = TruncationPolicy::Fail;

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

    // google_search grounding, NOT url_context: url_context only fetches URLs
    // the prompt already names, so asked to FIND a page the model answers from
    // memory — a hallucinated URL wearing a tool's credibility.
    QJsonObject searchTool;
    searchTool["google_search"] = QJsonObject{};
    requestBody["tools"] = QJsonArray{searchTool};

    sendRequest(requestBody);
}

void GeminiProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages)
{
    // [barista-fork] Forward to the options-aware overload with defaults (no tools) — mirrors AnthropicProvider,
    // so advisor/coach turns produce a byte-identical toolless request.
    analyzeConversation(systemPrompt, messages, RequestOptions{});
}

QJsonArray GeminiProvider::toGeminiFunctionDeclarations(const QJsonArray& defs)
{
    // Anthropic tool def: { name, description, input_schema: {type, properties, required} }
    // Gemini functionDeclaration: { name, description, parameters: <same JSON-Schema object> }.
    // The JSON-Schema subset the barista uses (object/string/integer/array + properties/required/description/items)
    // is accepted verbatim as Gemini `parameters`. The one incompatibility: Gemini rejects a parameters object
    // whose `properties` is empty, so a no-argument tool (get_active_recipe, deactivate_recipe, list_due_reminders,
    // dismiss_maintenance_doc_change) omits `parameters` entirely.
    QJsonArray out;
    for (const QJsonValue& v : defs) {
        const QJsonObject def = v.toObject();
        QJsonObject decl;
        decl["name"] = def["name"];
        decl["description"] = def["description"];
        const QJsonObject schema = def["input_schema"].toObject();
        if (!schema["properties"].toObject().isEmpty())
            decl["parameters"] = schema;
        out.append(decl);
    }
    return out;
}

void GeminiProvider::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                                         const RequestOptions& options)
{
    if (!isConfigured()) {
        emit analysisFailed(tr_("ai.gemini.keyMissing", "Gemini API key not configured"));
        return;
    }

    setStatus(Status::Busy);
    m_currentTimeoutMs = options.timeoutMs;   // [barista-fork] this turn's transfer timeout (used in sendRequest)
    m_retryCount = 0;
    m_toolRounds = 0;             // [barista-fork] reset the client-tool loop counter per turn
    m_accumulatedText.clear();    // [barista-fork]
    ++m_reqGen;
    m_truncationPolicy = TruncationPolicy::Fail;
    // A conversation turn is prose the user reads, so a cut-off reply still has
    // value — show it with a notice rather than discarding it (see
    // TruncationPolicy). The one-shot analyze()/analyzeUrl() paths keep Fail:
    // their result is machine-parsed.
    m_truncationPolicy = TruncationPolicy::ShowPartial;

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
            PROVIDER_WARN("GeminiProvider") << "Skipping message with unexpected role:" << role;
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
    // [barista-fork] Vision turn: append the image as an inlineData part on the current (last user) content.
    // Rides options (per-turn), never the persisted messages.
    if (!options.imageData.isEmpty())
        contents = contentsWithImageOnLastUser(contents, options.imageData, options.imageMediaType);
    requestBody["contents"] = contents;

    // [barista-fork] Function-calling tools. The client tools (setClientTools) and the fast-path web tools
    // (setWebTools) are BOTH client-executed via the same m_toolExecutor (dispatched by name) and the same
    // functionCall loop in onAnalysisReply — the only difference is the gate: client tools ship on
    // options.clientTools, the keyless web tools on options.webSearch (the "may reach the internet" toggle).
    // Every declaration goes into ONE tools:[{functionDeclarations:[...]}] entry, as Gemini requires. When
    // neither option is set (advisor/coach) the array stays empty and "tools" is omitted — byte-identical to the
    // original toolless request.
    QJsonArray functionDeclarations;
    if (options.clientTools && m_toolExecutor && !m_clientToolDefs.isEmpty()) {
        for (const QJsonValue& d : toGeminiFunctionDeclarations(m_clientToolDefs))
            functionDeclarations.append(d);
    }
    if (options.webSearch && m_toolExecutor && !m_webToolDefs.isEmpty()) {
        for (const QJsonValue& d : toGeminiFunctionDeclarations(m_webToolDefs))
            functionDeclarations.append(d);
    }
    if (!functionDeclarations.isEmpty()) {
        QJsonObject toolEntry;
        toolEntry["functionDeclarations"] = functionDeclarations;
        requestBody["tools"] = QJsonArray{ toolEntry };
    }

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
                PROVIDER_DEBUG("aiprovider") << "Gemini API error" << status << "remoteErrorContentOmitted";
                emit analysisFailed(tr_("ai.gemini.error", "Gemini error: %1").arg(apiError));
                return;
            }
            // Bounded/classified, never the raw body — see logSafeErrorBody().
            PROVIDER_DEBUG("aiprovider") << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << logSafeErrorBody(body);
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
    PROVIDER_DEBUG("aiprovider") << "Gemini usage — prompt:" << usage["promptTokenCount"].toInt()
            << "thoughts:" << usage["thoughtsTokenCount"].toInt()
            << "output:" << usage["candidatesTokenCount"].toInt()
            << "total:" << usage["totalTokenCount"].toInt();

    QJsonArray candidates = root["candidates"].toArray();
    if (candidates.isEmpty()) {
        // A prompt-level block returns promptFeedback.blockReason and no
        // candidates. That reason IS the explanation; discarding it left the
        // user with five generic words and the log with nothing.
        const QString blockReason = root["promptFeedback"].toObject()["blockReason"].toString();
        PROVIDER_DEBUG("Gemini") << "model" << diagnosticModel() << "no candidates, blockReason" << diagnosticCode(blockReason);
        emit analysisFailed(blockReason.isEmpty()
            ? tr_("ai.gemini.noResponse", "Gemini returned no response")
            : tr_("ai.gemini.blocked", "Gemini refused the request (%1).").arg(blockReason));
        return;
    }

    // Upstream #1691: read finishReason BEFORE the parts gate. When Gemini stops on
    // MAX_TOKENS *while still thinking* — and gemini-3.5-flash runs thinkingLevel
    // "minimal", which is on, not off — the candidate comes back carrying a
    // finishReason and NO content key at all. Reading it after a parts-empty early
    // return made that failure unreachable: budget eaten by hidden reasoning, no
    // text, opaque error. SAFETY/RECITATION/PROHIBITED_CONTENT arrive the same way.
    const QString finishReason = candidates[0].toObject()["finishReason"].toString();
    const bool truncated = finishReason == QLatin1String("MAX_TOKENS");

    // [barista-fork] Keep modelContent — the client-tool loop below appends it verbatim
    // as the model's functionCall turn (captured in the executor callback). The fork's
    // former parts-empty early return is gone: an empty-parts turn now falls through to
    // the terminal degrade/dispatch below (m_toolRounds > 0 → graceful, else empty/truncated).
    const QJsonObject modelContent = candidates[0].toObject()["content"].toObject();
    // Split the parts into prose (non-thought text) and functionCall requests. Plain replies have exactly one
    // text part; a url_context response (analyzeUrl) may split the answer across several; thought parts are
    // hidden reasoning and must not leak into the answer; functionCall parts drive the client-tool loop below.
    const QJsonArray parts = modelContent["parts"].toArray();
    QString text;
    QVector<QJsonObject> functionCalls;
    for (const QJsonValue& partVal : parts) {
        const QJsonObject part = partVal.toObject();
        if (part.contains(QLatin1String("functionCall"))) {
            functionCalls.append(part["functionCall"].toObject());
            continue;
        }
        if (part["thought"].toBool())
            continue;
        text += part["text"].toString();
    }

    // [barista-fork] CLIENT tools: the model asked us to run one or more functions. Execute each via the
    // registered executor (see setClientTools), append the model turn (verbatim) + a user turn carrying our
    // functionResponse parts, and re-POST — the standard Gemini function-calling loop, bounded by
    // MAX_TOOL_ROUNDS. Only callers that enable tools carry them, so the advisor (which never sends tools) never
    // sees a functionCall and this branch is inert for it.
    if (!functionCalls.isEmpty() && m_toolExecutor && m_toolRounds < MAX_TOOL_ROUNDS) {
        ++m_toolRounds;
        // Any prose written alongside the call is a natural lead-in; buffer it and prepend to the final answer.
        m_accumulatedText += text;
        setStatus(Status::Busy);            // stay Busy while the tools run (top of this fn set Ready)
        const int gen = m_reqGen;            // guard: a superseded turn's late callback must NOT re-POST
        auto pending = std::make_shared<int>(functionCalls.size());
        // Place each result at its call's index — Gemini matches functionResponse to functionCall by ORDER
        // (there is no tool_use_id like Anthropic), so completion-order appends would mismatch two parallel calls
        // to the same tool. Pre-sized; each callback writes its own slot.
        auto responses = std::make_shared<QVector<QJsonObject>>(functionCalls.size());
        for (qsizetype i = 0; i < functionCalls.size(); ++i) {
            const QJsonObject call = functionCalls[i];
            const QString toolName = call["name"].toString();
            const QString callId = call["id"].toString();   // present only on newer parallel-call responses
            m_toolExecutor(toolName, call["args"].toObject(),
                [this, gen, i, toolName, callId, pending, responses, modelContent](QJsonValue result) {
                    if (gen != m_reqGen) return;   // a newer turn started — drop this stale result
                    // Gemini functionResponse.response must be a JSON object; wrap non-objects under "result".
                    QJsonObject responseObj;
                    if (result.isObject())      responseObj = result.toObject();
                    else if (result.isArray())  responseObj = QJsonObject{{ QStringLiteral("result"), result.toArray() }};
                    else                        responseObj = QJsonObject{{ QStringLiteral("result"), result }};
                    QJsonObject fr;
                    fr["name"] = toolName;
                    if (!callId.isEmpty())
                        fr["id"] = callId;   // echo the id back so the API pairs it precisely (parallel calls)
                    fr["response"] = responseObj;
                    QJsonObject part;
                    part["functionResponse"] = fr;
                    (*responses)[i] = part;
                    if (--(*pending) > 0)
                        return;              // wait for the remaining calls in this turn
                    // All results in — append the model's functionCall turn + our functionResponse turn, re-POST.
                    QJsonArray responseParts;
                    for (const QJsonObject& p : *responses)
                        responseParts.append(p);
                    QJsonObject body = m_pendingRequestBody;
                    QJsonArray contents = body["contents"].toArray();
                    contents.append(modelContent);              // model turn (functionCall parts), verbatim
                    QJsonObject userTurn;
                    userTurn["role"] = QString("user");
                    userTurn["parts"] = responseParts;
                    contents.append(userTurn);
                    body["contents"] = contents;
                    setStatus(Status::Busy);
                    sendRequest(body);
                });
        }
        return;   // async — the completion callback re-POSTs once every call has returned
    }

    // Terminal turn — assemble the answer, prepending any lead-in prose buffered across prior tool rounds.
    text = m_accumulatedText + text;
    m_accumulatedText.clear();

    // [barista-fork] The tool loop hit MAX_TOOL_ROUNDS (or the model returned only a call with no prose) —
    // degrade to a friendly message instead of an error. Gated on a tool turn having run (only the barista
    // enables client tools; analyzeUrl never sets functionCalls/m_toolRounds, so it falls through to the
    // truncation/empty dispatch below rather than speaking "shot history" prose for a URL extraction).
    if (text.trimmed().isEmpty() && (!functionCalls.isEmpty() || m_toolRounds > 0)) {
        emit analysisComplete(QStringLiteral("I got partway through that but ran out of steps before I finished — ask me to pick it back up?"));
        return;
    }
    if (text.isEmpty() || truncated) {
        // thoughtsTokenCount is the field that names a thinking-ate-the-budget
        // failure on sight (upstream #1691), so log it next to the reason rather
        // than only in the qInfo line above.
        PROVIDER_DEBUG("Gemini") << "model" << diagnosticModel() << "finishReason" << diagnosticCode(finishReason)
                   << "parts" << parts.size() << "text chars" << text.size()
                   << "thought tokens" << usage["thoughtsTokenCount"].toInt()
                   << "output tokens" << usage["candidatesTokenCount"].toInt();
        // One message for both empty cases. There used to be two strings one
        // word apart ("empty content" vs "empty response content"), only one of
        // which logged — a user reporting either could not say which they hit.
        if (dispatchTruncatedOrEmpty(text, truncated,
                tr_("ai.gemini.emptyContent", "Gemini returned empty response content")))
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
    // trimmed(): a key pasted on a tablet often carries a trailing newline/space. Qt 6 refuses to set a raw
    // header whose value contains a newline, so the header is silently dropped and Google sees NO credential →
    // "method doesn't allow unregistered callers" (403), which looks like a bad key but is actually a missing one.
    req.setRawHeader("x-goog-api-key", m_apiKey.trimmed().toUtf8());
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
    QUrl url(m_baseUrl.isEmpty()
        ? QString::fromLatin1(API_URL)
        : m_baseUrl + QStringLiteral("/api/v1/chat/completions"));
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
    m_truncationPolicy = TruncationPolicy::Fail;

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
    requestBody["max_tokens"] = MAX_OUTPUT_TOKENS;   // [barista-fork] 4096 — short replies were truncating mid-sentence

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
    m_truncationPolicy = TruncationPolicy::Fail;
    // A conversation turn is prose the user reads, so a cut-off reply still has
    // value — show it with a notice rather than discarding it (see
    // TruncationPolicy). The one-shot analyze()/analyzeUrl() paths keep Fail:
    // their result is machine-parsed.
    m_truncationPolicy = TruncationPolicy::ShowPartial;

    QJsonObject requestBody;
    requestBody["model"] = m_model;
    requestBody["messages"] = buildOpenAIMessages(systemPrompt, messages);
    requestBody["max_tokens"] = MAX_OUTPUT_TOKENS;   // [barista-fork] 4096 — short replies were truncating mid-sentence

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
                PROVIDER_DEBUG("aiprovider") << "OpenRouter API error" << status << "remoteErrorContentOmitted";
                emit analysisFailed(tr_("ai.openrouter.error", "OpenRouter error: %1").arg(apiError));
                return;
            }
            // Bounded/classified, never the raw body — see logSafeErrorBody().
            PROVIDER_DEBUG("aiprovider") << "AI request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << logSafeErrorBody(body);
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

    const QJsonObject choice = choices[0].toObject();

    // OpenRouter reports an upstream provider failure as a 200 carrying an
    // error object on the CHOICE, which the top-level root["error"] check above
    // does not see.
    const QJsonObject choiceError = choice["error"].toObject();
    if (!choiceError.isEmpty()) {
        const QString message = choiceError["message"].toString();
        PROVIDER_DEBUG("OpenRouter") << "model" << diagnosticModel() << "upstream error"
                   << "remoteErrorContentOmitted";
        emit analysisFailed(tr_("ai.openrouter.error", "OpenRouter error: %1")
            .arg(message.isEmpty() ? tr_("ai.error.unknown", "unknown error") : message));
        return;
    }

    // finish_reason "length" = the answer hit max_tokens. This matters most on
    // OpenRouter: the model is a free-text user string, so it can point at a
    // reasoning model whose hidden tokens eat the cap the way #1691's did.
    // "content_filter" and "error" likewise mean the text in hand is not the
    // whole answer.
    const QString finishReason = choice["finish_reason"].toString();
    const QJsonObject message = choice["message"].toObject();
    QString content = message["content"].toString();
    const bool truncated = finishReason == QLatin1String("length")
                        || finishReason == QLatin1String("content_filter")
                        || finishReason == QLatin1String("error");
    if (content.isEmpty() || truncated) {
        PROVIDER_DEBUG("OpenRouter") << "model" << diagnosticModel() << "finish_reason" << diagnosticCode(finishReason)
                   << "content chars" << content.size();
        const QString refusal = message["refusal"].toString();
        if (content.isEmpty() && !refusal.isEmpty()) {
            emit analysisFailed(tr_("ai.openrouter.refused",
                                    "OpenRouter's model declined the request: %1").arg(refusal));
            return;
        }
        if (dispatchTruncatedOrEmpty(content, truncated,
                tr_("ai.openrouter.emptyContent", "OpenRouter returned empty response content")))
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
    m_truncationPolicy = TruncationPolicy::Fail;

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
    m_truncationPolicy = TruncationPolicy::Fail;
    // A conversation turn is prose the user reads, so a cut-off reply still has
    // value — show it with a notice rather than discarding it (see
    // TruncationPolicy). The one-shot analyze()/analyzeUrl() paths keep Fail:
    // their result is machine-parsed.
    m_truncationPolicy = TruncationPolicy::ShowPartial;

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
            PROVIDER_DEBUG("aiprovider") << "Ollama request failed"
                       << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                       << "-" << logSafeErrorBody(body);
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
    const QJsonObject message = root["message"].toObject();
    QString response = message["content"].toString();
    if (response.isEmpty()) {
        response = root["response"].toString();
        // Warn, not qDebug: taking the /api/generate shape off what may have
        // been an /api/chat request means one of the two assumptions is wrong,
        // and this fallback otherwise masks a legitimately-empty chat reply
        // (truncation, refusal, thinking-only) behind a shape probe.
        if (!response.isEmpty())
            PROVIDER_WARN("OllamaProvider") << "/api/chat message.content was empty; "
                          "fell back to the /api/generate response field";
    }

    // This app sets no token cap for Ollama, but that is not the same as "it
    // cannot truncate": num_predict and num_ctx live in the USER's Modelfile
    // and server config, and Ollama reports done_reason "length" when either
    // one stops generation. Without this check a locally-truncated reply was
    // emitted as complete — the same defect this change fixes on the four
    // cloud providers, on the fifth.
    const QString doneReason = root["done_reason"].toString();
    const bool truncated = doneReason == QLatin1String("length");

    if (response.isEmpty() || truncated) {
        // Thinking models (deepseek-r1, qwen3) return reasoning in
        // message.thinking with a possibly-empty message.content — literally
        // #1691's shape, on a local model. Say so rather than logging nothing,
        // which is what this branch did.
        PROVIDER_DEBUG("Ollama") << "model" << diagnosticModel() << "done_reason" << diagnosticCode(doneReason)
                   << "content chars" << response.size()
                   << "thinking chars" << message["thinking"].toString().size()
                   << "eval_count" << root["eval_count"].toInt();
        if (dispatchTruncatedOrEmpty(response, truncated,
                tr_("ai.ollama.emptyResponse", "Ollama returned empty response")))
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
