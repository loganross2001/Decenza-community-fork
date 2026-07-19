#include "aimanager.h"
#include "aiprovider.h"
#include "aiconversation.h"
#include "shotsummarizer.h"
#include "../core/settings.h"
#include "../core/settings_ai.h"
#include "../core/settings_dye.h"          // [barista-fork] active-bag roast/freeze state for the proactive-rec block
#include "../core/drinktypes.h"            // [barista-fork] natural shot descriptor (type + beans), not a stat dump
#include "../network/roastdate.h"          // [barista-fork] locale-robust roast-date → ISO for days-off-roast
#include "../core/grinderaliases.h"
#include "../controllers/profilemanager.h"
#include "dialing_blocks.h"
#include "../models/shotdatamodel.h"
#include "../profile/profile.h"
#include "../network/visualizeruploader.h"
#include "../history/shothistorystorage.h"
#include "../history/recipestorage.h"   // [barista-fork] Recipes 2.0 proactive context block
#include "../history/baristastorage.h"  // [barista-fork] Phase 1 identity: roster for the [Who] block
#include "../barista/baristatools.h"
#include "../barista/feedbackstorage.h"   // [barista-fork] verbal-feedback KB (proactive context + write tool)
#include "../barista/tasksstorage.h"      // [barista-fork] reminders + maintenance (proactive dueItems + task tools)
#include "../core/translationmanager.h"

#include <QNetworkAccessManager>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QDate>
#include <QLocale>
#include <QDebug>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QThread>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include "../core/dbutils.h"
#include <QPointer>
#include <QCoreApplication>
#include <QRegularExpression>
#include <cmath>

namespace {
// Coerce a QML-supplied shot argument into a ShotProjection. QML hands the
// shot-taking Q_INVOKABLEs below one of two shapes:
//   - a real ShotProjection (fresh shot via onShotReady, or History reload) —
//     arrives as a QVariant wrapping ShotProjection;
//   - a plain JS object — the optimistic edit clone built by
//     PostShotReviewPage.clonePersistedShot() after an in-place edit — arrives
//     as a QVariant wrapping QVariantMap.
// A JS object does NOT auto-convert to a `const ShotProjection&` parameter on
// the QML→C++ argument-binding path in Qt 6.11 (it throws "Could not convert
// argument 0 from [object Object] to ShotProjection"), which is why the AI
// Advice / Discuss buttons died after any edit (#1298). Accepting QVariant and
// coercing explicitly here handles both shapes. fromVariantMap reconstructs a
// full ShotProjection — clonePersistedShot copies every field, including the
// curve arrays, so the summarizer gets complete data.
ShotProjection coerceShot(const QVariant& v)
{
    if (v.userType() == qMetaTypeId<ShotProjection>())
        return v.value<ShotProjection>();
    const QVariantMap m = v.toMap();
    // An empty map means QML passed null/undefined or a non-map scalar — the
    // result would be a default ShotProjection (durationSec=0), which
    // isMistakeShot reads as a mistake and silently suppresses the AI summary.
    // That's a benign degrade, but log it so a future QML arg-shape regression
    // is debuggable instead of silently flagging every shot as a mistake.
    if (m.isEmpty())
        qWarning() << "AIManager::coerceShot: empty/non-map shot arg (type"
                   << v.typeName() << ") — shot will read as a mistake";
    return ShotProjection::fromVariantMap(m);
}

// [barista-fork] The date of the Nth given weekday of a month (e.g. 4th Thursday of November = Thanksgiving).
// weekday is Qt's 1=Mon..7=Sun. Walks from the 1st to the first matching weekday, then adds whole weeks.
QDate nthWeekdayOfMonth(int year, int month, int weekday, int n)
{
    QDate d(year, month, 1);
    if (!d.isValid())
        return QDate();
    int delta = (weekday - d.dayOfWeek() + 7) % 7;
    d = d.addDays(delta + 7 * (n - 1));
    return (d.month() == month) ? d : QDate();
}

// [barista-fork] The date of the LAST given weekday of a month (e.g. last Monday of May = Memorial Day).
QDate lastWeekdayOfMonth(int year, int month, int weekday)
{
    QDate d(year, month, 1);
    if (!d.isValid())
        return QDate();
    d = d.addMonths(1).addDays(-1);   // last day of the month
    int back = (d.dayOfWeek() - weekday + 7) % 7;
    return d.addDays(-back);
}

// [barista-fork] Built-in US-holiday lookup for the barista's greeting/goodbye "todaysOccasion". Returns the
// holiday name for `date` or an empty string. Covers the fixed-date holidays for sure plus the common floating
// ones computed with QDate (no external table). Easter is deliberately skipped (hard to compute; the task
// permits skipping it). Purely presentational — a warm "Happy Thanksgiving!" — never a factual claim.
QString usHolidayForDate(const QDate& date)
{
    if (!date.isValid())
        return QString();
    const int y = date.year();
    const int m = date.month();
    const int d = date.day();

    // Fixed-date holidays.
    if (m == 1  && d == 1)  return QStringLiteral("New Year's Day");
    if (m == 2  && d == 14) return QStringLiteral("Valentine's Day");
    if (m == 3  && d == 17) return QStringLiteral("St. Patrick's Day");
    if (m == 6  && d == 19) return QStringLiteral("Juneteenth");
    if (m == 7  && d == 4)  return QStringLiteral("Independence Day");
    if (m == 10 && d == 31) return QStringLiteral("Halloween");
    if (m == 11 && d == 11) return QStringLiteral("Veterans Day");
    if (m == 12 && d == 24) return QStringLiteral("Christmas Eve");
    if (m == 12 && d == 25) return QStringLiteral("Christmas");
    if (m == 12 && d == 31) return QStringLiteral("New Year's Eve");

    // Floating holidays (Nth/last weekday; Qt weekday 1=Mon..7=Sun).
    if (m == 1  && date == nthWeekdayOfMonth(y, 1, 1, 3))  return QStringLiteral("Martin Luther King Jr. Day");
    if (m == 2  && date == nthWeekdayOfMonth(y, 2, 1, 3))  return QStringLiteral("Presidents' Day");
    if (m == 5  && date == lastWeekdayOfMonth(y, 5, 1))    return QStringLiteral("Memorial Day");
    if (m == 9  && date == nthWeekdayOfMonth(y, 9, 1, 1))  return QStringLiteral("Labor Day");
    if (m == 10 && date == nthWeekdayOfMonth(y, 10, 1, 2)) return QStringLiteral("Indigenous Peoples' Day / Columbus Day");
    if (m == 11 && date == nthWeekdayOfMonth(y, 11, 4, 4)) return QStringLiteral("Thanksgiving");

    return QString();
}

// [barista-fork] PROACTIVE-REC INPUTS. A compact, current-state block the barista reads at engage time to
// DECIDE whether a recipe tweak is worth offering on its first reply — bean age (days off roast + a plain
// freshness read), and storage state (frozen / days out of the freezer / the user's bag notes). The shot
// OUTCOME half of a proactive rec (best/most-recent shot on this bean, and the user's own taste words) is
// already carried by the block's beanBestShot + recentTastingFeedbackOnThisBean fields, so this adds ONLY the
// bean-condition signals those blocks lack. Everything here is computed FACT (dates, day counts) — no
// fabricated dial numbers (the persona's no-invention guard still owns those). Reads live SettingsDye, so it
// must run on the main thread. Returns an empty object when there's nothing worth saying (no roast date, no
// freeze/notes) so the caller can skip the field entirely.
//
// Freshness windows (arabica espresso rule-of-thumb, off roast):
//   < 5 days  → very fresh / still degassing (runs fast + unstable; do NOT chase it finer yet)
//   5–10      → opening up
//   10–21     → peak / dialed-in window
//   21–35     → mature (expect to grind a touch finer as it fades)
//   > 35      → fading / stale (grind finer, shorten ratio, or move on)
QJsonObject buildProactiveRecBlock(SettingsDye* dye)
{
    if (!dye)
        return {};
    QJsonObject o;
    const QDate today = QDate::currentDate();

    // --- Bean age off roast ------------------------------------------------
    const QString rawRoast = dye->dyeRoastDate();
    if (!rawRoast.isEmpty()) {
        // RoastDate::toIso is locale-robust (handles US/EU numeric + ISO); fall back to a couple of
        // explicit formats for anything it passes through unchanged.
        QDate roast = QDate::fromString(RoastDate::toIso(rawRoast).left(10), Qt::ISODate);
        if (!roast.isValid()) roast = QDate::fromString(rawRoast, QStringLiteral("yyyy-MM-dd"));
        if (roast.isValid()) {
            const qint64 days = roast.daysTo(today);
            if (days >= 0 && days < 3650) {   // sane range only; a future/typo'd date is left out
                o.insert(QStringLiteral("daysOffRoast"), static_cast<int>(days));
                QString read;
                if (days < 5)       read = QStringLiteral("very fresh / still degassing (runs fast + unstable — do not chase it finer yet)");
                else if (days < 10) read = QStringLiteral("opening up (approaching its window)");
                else if (days < 21) read = QStringLiteral("in its peak window");
                else if (days < 35) read = QStringLiteral("maturing (expect to grind a touch finer as it fades)");
                else                read = QStringLiteral("fading / past peak (grind finer or shorten the ratio, or consider a fresher bag)");
                o.insert(QStringLiteral("freshnessRead"), read);
            }
        }
    }

    // --- Storage state (frozen / defrosted) --------------------------------
    // These describe the ACTIVE bag's thermal history. A just-thawed bag behaves like a young bag for a
    // day or two (offgassing resumes), which is a legitimate "hold off / re-dial" signal.
    const QString frozen  = dye->activeBagFrozenDate();
    const QString defrost = dye->activeBagDefrostDate();
    if (!frozen.isEmpty() || !defrost.isEmpty()) {
        QJsonObject storage;
        const QDate def = QDate::fromString(defrost.left(10), Qt::ISODate);
        if (def.isValid()) {
            const qint64 daysOut = def.daysTo(today);
            if (daysOut >= 0 && daysOut < 3650) {
                storage.insert(QStringLiteral("daysOutOfFreezer"), static_cast<int>(daysOut));
                storage.insert(QStringLiteral("note"), daysOut <= 2
                    ? QStringLiteral("recently defrosted — beans may still be settling; hold a big grind move for a shot or two")
                    : QStringLiteral("defrosted and settled"));
            }
        } else if (!frozen.isEmpty()) {
            // Frozen with no defrost date recorded → treat as currently frozen (informational only).
            storage.insert(QStringLiteral("state"), QStringLiteral("frozen (no defrost date recorded)"));
        }
        if (!storage.isEmpty())
            o.insert(QStringLiteral("storage"), storage);
    }

    // --- The user's own current-shot note ----------------------------------
    // dyeShotNotes is the CURRENT-SHOT note (dye/shotNotes, per-shot — NOT written through to the bag),
    // so surface it as recentShotNote, not bag guidance. Still worth honoring when the user jotted
    // something about this setup ("tastes flat", "trying 1:2.2"); trimmed + capped.
    const QString notes = dye->dyeShotNotes().trimmed();
    if (!notes.isEmpty())
        o.insert(QStringLiteral("recentShotNote"), notes.left(240));

    return o;
}
}

AIManager::AIManager(QNetworkAccessManager* networkManager, Settings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_networkManager(networkManager)
    , m_summarizer(std::make_unique<ShotSummarizer>(this))
{
    Q_ASSERT(networkManager);
    createProviders();

    // Create conversation handler for multi-turn interactions
    m_conversation = new AIConversation(this, this);

    // One-time clear: grinder calibration (v1.7.2) changed what the advisor
    // knows per shot; old conversations anchored on "I don't have LRV3 data"
    // are misleading. Fires once per device, then becomes a no-op.
    clearAllConversationsOnce(QStringLiteral("grinder_calibration_v1.7.2"));

    // Migrate legacy single-conversation storage if needed
    migrateFromLegacyConversation();

    // Load conversation index and restore most recent conversation
    loadConversationIndex();
    loadMostRecentConversation();

    // Connect to settings changes
    connect(m_settings->ai(), &SettingsAI::configurationChanged, this, &AIManager::onSettingsChanged);
}

AIManager::~AIManager() = default;

void AIManager::createProviders()
{
    // Create OpenAI provider
    QString openaiKey = m_settings->ai()->openaiApiKey();
    auto* openai = new OpenAIProvider(m_networkManager, openaiKey, this);
    openai->setModel(m_settings->ai()->providerModel("openai"));  // empty → keeps default
    connect(openai, &AIProvider::analysisComplete, this, &AIManager::onAnalysisComplete);
    connect(openai, &AIProvider::analysisFailed, this, &AIManager::onAnalysisFailed);
    connect(openai, &AIProvider::testResult, this, &AIManager::onTestResult);
    m_openaiProvider.reset(openai);

    // Create Anthropic provider
    QString anthropicKey = m_settings->ai()->anthropicApiKey();
    auto* anthropic = new AnthropicProvider(m_networkManager, anthropicKey, this);
    anthropic->setModel(m_settings->ai()->providerModel("anthropic"));  // empty → keeps default
    connect(anthropic, &AIProvider::analysisComplete, this, &AIManager::onAnalysisComplete);
    connect(anthropic, &AIProvider::analysisFailed, this, &AIManager::onAnalysisFailed);
    connect(anthropic, &AIProvider::testResult, this, &AIManager::onTestResult);
    // [barista-fork] Interim pre-tool lead-in (only Anthropic emits it — the tool_use/pause_turn paths).
    connect(anthropic, &AIProvider::interimText, this, &AIManager::onInterimText);
    // [barista-fork] Register the barista's private client-side tools (definitions + executor) behind the
    // provider's generic seam. The executor reads m_shotHistory lazily (it's wired after construction via
    // setShotHistoryStorage), so capturing `this` and forwarding at call time preserves the original behavior.
    anthropic->setClientTools(BaristaTools::toolDefinitions(),
        [this](const QString& name, const QJsonObject& input, std::function<void(QJsonValue)> done) {
            // [barista-fork] Closed-loop bridge (issue #1053 regression): when the model APPLIES a dial change by
            // CALLING apply_dial_change (instead of emitting a fenced ```json structuredNext block), capture the
            // applied dial fields as a pending structuredNext for the CURRENT turn so it still enters the
            // recentAdvice audit. The field names (grinderSetting/doseG/targetWeightG/ratio/temperatureC) already
            // match what the fenced-block audit reads (computeAdherence keys on grinderSetting/doseG), so the
            // tool-applied object is auditable the same way. AIConversation::onAnalysisComplete take()s this at
            // finalization and uses it ONLY when no fenced block was emitted (fenced wins → no double-count).
            // Captures the raw tool INPUT, not the executor's applied/queued/rejected result — see the
            // known-limitation note in this fix's report.
            if (name == QLatin1String("apply_dial_change")) {
                QJsonObject applied;
                for (const char* k : {"grinderSetting", "doseG", "targetWeightG", "ratio", "temperatureC"}) {
                    if (input.contains(QLatin1String(k)))
                        applied.insert(QLatin1String(k), input.value(QLatin1String(k)));
                }
                if (!applied.isEmpty())
                    m_pendingToolStructuredNext = applied;
            }
            // [barista-fork] Thread the feedback KB + the app-side anchor/dial snapshot into the executor so
            // log_tasting_feedback stamps shot_id + bean/profile/dial itself (never from the model). The
            // m_webToolsHandler seam runs the fast-path web tools (get_weather/get_stock_quote/get_local_news).
            // [barista-fork] Stamp the FRESH active roster user into the snapshot so remember_fact/forget_fact
            // scope facts to whoever the barista is talking to RIGHT NOW (survives a mid-session set_active_user
            // switch, unlike the snapshot which is frozen at context-assembly time). Never trusted from the model.
            QVariantMap anchor = m_lastBaristaAnchorSnapshot;
            if (m_settings && m_settings->dye())
                anchor.insert(QStringLiteral("activeUser"), m_settings->dye()->dyeBarista());
            BaristaTools::executeTool(m_shotHistory, m_feedbackStorage, m_tasksStorage, m_applyDialHandler,
                                      m_endConversationHandler, m_webToolsHandler,
                                      m_getActiveRecipeHandler, m_activateRecipeHandler, m_deactivateRecipeHandler,
                                      m_updateRecipeHandler,
                                      m_setActiveUserHandler,
                                      anchor,
                                      name, input, std::move(done));
        });
    // [barista-fork] Register the fast-path web-tool DEFINITIONS separately. They ship under the webSearch gate
    // (umbrella "may reach the internet" toggle), not the clientTools gate — but flow through the SAME executor
    // above (dispatched by tool name). See AnthropicProvider::setWebTools / analyzeConversation.
    anthropic->setWebTools(BaristaTools::webToolDefinitions());
    m_anthropicProvider.reset(anthropic);

    // [barista-fork] Dedicated Haiku provider for the parallel quick-filler (requestQuickFiller). Plain
    // single-shot analyze() path — NO client tools / web tools / interim wiring, and deliberately kept out of
    // providerById()/currentProvider() so it can never serve a real turn. Pinned to Haiku via setModelUnchecked
    // (Haiku isn't in the user-facing model list). Only built when an Anthropic key exists; otherwise the
    // filler is a silent no-op and behavior is unchanged. A failed filler is swallowed — it must never surface.
    if (!anthropicKey.isEmpty()) {
        auto* filler = new AnthropicProvider(m_networkManager, anthropicKey, this);
        filler->setModelUnchecked(QStringLiteral("claude-haiku-4-5"));
        // NOTE: analysisComplete is NOT statically connected here — requestQuickFiller() makes a per-request
        // gen-capturing connection so a superseded turn's completion can be dropped (see m_fillerConn).
        connect(filler, &AIProvider::analysisFailed, this, [](const QString&) { /* filler failure: silent */ });
        m_fillerProvider.reset(filler);
    }

    // Create Gemini provider
    QString geminiKey = m_settings->ai()->geminiApiKey();
    auto* gemini = new GeminiProvider(m_networkManager, geminiKey, this);
    gemini->setModel(m_settings->ai()->providerModel("gemini"));  // empty → keeps default
    connect(gemini, &AIProvider::analysisComplete, this, &AIManager::onAnalysisComplete);
    connect(gemini, &AIProvider::analysisFailed, this, &AIManager::onAnalysisFailed);
    connect(gemini, &AIProvider::testResult, this, &AIManager::onTestResult);
    m_geminiProvider.reset(gemini);

    // Create OpenRouter provider
    QString openrouterKey = m_settings->ai()->openrouterApiKey();
    QString openrouterModel = m_settings->ai()->openrouterModel();
    auto* openrouter = new OpenRouterProvider(m_networkManager, openrouterKey, openrouterModel, this);
    connect(openrouter, &AIProvider::analysisComplete, this, &AIManager::onAnalysisComplete);
    connect(openrouter, &AIProvider::analysisFailed, this, &AIManager::onAnalysisFailed);
    connect(openrouter, &AIProvider::testResult, this, &AIManager::onTestResult);
    m_openrouterProvider.reset(openrouter);

    // Create Ollama provider
    QString ollamaEndpoint = m_settings->ai()->ollamaEndpoint();
    QString ollamaModel = m_settings->ai()->ollamaModel();
    auto* ollama = new OllamaProvider(m_networkManager, ollamaEndpoint, ollamaModel, this);
    connect(ollama, &AIProvider::analysisComplete, this, &AIManager::onAnalysisComplete);
    connect(ollama, &AIProvider::analysisFailed, this, &AIManager::onAnalysisFailed);
    connect(ollama, &AIProvider::testResult, this, &AIManager::onTestResult);
    connect(ollama, &OllamaProvider::modelsRefreshed, this, &AIManager::onOllamaModelsRefreshed);
    m_ollamaProvider.reset(ollama);
}

void AIManager::setTranslationManager(TranslationManager* tm)
{
    m_translationManager = tm;
    // Fan out to every owned provider (created in the ctor, before this runs)
    // and the conversation, so their user-visible error strings localize too.
    for (AIProvider* p : { m_openaiProvider.get(), m_anthropicProvider.get(),
                           m_geminiProvider.get(), m_openrouterProvider.get(),
                           m_ollamaProvider.get() }) {
        if (p) p->setTranslationManager(tm);
    }
    if (m_conversation) m_conversation->setTranslationManager(tm);
}

QString AIManager::tr_(const char* key, const char* fallback) const
{
    if (m_translationManager)
        return m_translationManager->translateString(QString::fromUtf8(key),
                                               QString::fromUtf8(fallback));
    return QString::fromUtf8(fallback);
}

QString AIManager::selectedProvider() const
{
    return m_settings->ai()->aiProvider();
}

QString AIManager::currentModelName() const
{
    AIProvider* provider = currentProvider();
    return provider ? provider->modelName() : QString();
}

QString AIManager::modelDisplayName(const QString& providerId) const
{
    AIProvider* provider = providerById(providerId);
    return provider ? provider->shortModelName() : QString();
}

QVariantList AIManager::availableModels(const QString& providerId) const
{
    QVariantList out;
    AIProvider* provider = providerById(providerId);
    if (!provider) return out;
    const QList<AIProvider::ModelOption> models = provider->availableModels();
    for (const AIProvider::ModelOption& opt : models) {
        // Keys are a contract with SettingsAITab.qml and the ShotServer
        // settings page JS (served as providerModelCatalogs): "name" is the
        // display label, "id" is read back on selection. Keep both in sync
        // with those consumers.
        QVariantMap entry;
        entry["id"] = opt.id;
        entry["name"] = opt.displayName;
        out.append(entry);
    }
    return out;
}

QString AIManager::modelHint(const QString& providerId) const
{
    AIProvider* provider = providerById(providerId);
    return provider ? provider->modelHint() : QString();
}

void AIManager::setSelectedProvider(const QString& provider)
{
    if (selectedProvider() != provider) {
        m_settings->ai()->setAiProvider(provider);
        emit providerChanged();
        emit configurationChanged();
    }
}

QStringList AIManager::availableProviders() const
{
    return {"openai", "anthropic", "gemini", "openrouter", "ollama"};
}

bool AIManager::isConfigured() const
{
    AIProvider* provider = currentProvider();
    return provider && provider->isConfigured();
}

AIProvider* AIManager::providerById(const QString& providerId) const
{
    if (providerId == "openai") return m_openaiProvider.get();
    if (providerId == "anthropic") return m_anthropicProvider.get();
    if (providerId == "gemini") return m_geminiProvider.get();
    if (providerId == "openrouter") return m_openrouterProvider.get();
    if (providerId == "ollama") return m_ollamaProvider.get();
    return nullptr;
}

AIProvider* AIManager::currentProvider() const
{
    AIProvider* provider = providerById(selectedProvider());
    return provider ? provider : m_openaiProvider.get();  // Default
}

std::optional<QJsonObject> AIManager::parseStructuredNext(const QString& assistantMessage)
{
    // Locate the LAST fenced ```json ... ``` block whose closing fence is
    // the final non-whitespace content in the message. Mid-message blocks
    // (e.g., the model echoing a snippet from the user) MUST be ignored —
    // the recommendation block always trails the prose.
    //
    // Strategy: walk all ``` fence positions, pair them as opener/closer,
    // and check the LAST pair. If that pair's opener is tagged `json`
    // (case-insensitive) and its closer is followed only by whitespace,
    // parse the inner body. Anything else → std::nullopt.
    if (assistantMessage.isEmpty()) return std::nullopt;

    QList<qsizetype> fenceStarts;
    fenceStarts.reserve(8);
    qsizetype searchFrom = 0;
    while (true) {
        const qsizetype pos = assistantMessage.indexOf(QStringLiteral("```"), searchFrom);
        if (pos < 0) break;
        fenceStarts.append(pos);
        searchFrom = pos + 3;
    }
    if (fenceStarts.size() < 2) return std::nullopt;

    // Take the last two fences unconditionally — odd total counts (a
    // stray ``` somewhere earlier in the prose) MUST NOT silently drop a
    // structurally valid trailing block. The closer-followed-only-by-
    // whitespace check below is what actually enforces "this is the
    // trailing block."
    const qsizetype openerStart = fenceStarts.at(fenceStarts.size() - 2);
    const qsizetype closerStart = fenceStarts.at(fenceStarts.size() - 1);

    // Closer must be followed only by whitespace.
    const qsizetype closerEnd = closerStart + 3;
    for (qsizetype i = closerEnd; i < assistantMessage.size(); ++i) {
        if (!assistantMessage[i].isSpace()) return std::nullopt;
    }

    // Tag: characters between opener fence and the next newline.
    const qsizetype tagStart = openerStart + 3;
    const qsizetype newlineAfterOpener = assistantMessage.indexOf(QLatin1Char('\n'), tagStart);
    if (newlineAfterOpener < 0 || newlineAfterOpener >= closerStart) return std::nullopt;
    const QString tag = assistantMessage.mid(tagStart, newlineAfterOpener - tagStart).trimmed();
    if (tag.compare(QStringLiteral("json"), Qt::CaseInsensitive) != 0) return std::nullopt;

    const QString inner = assistantMessage.mid(newlineAfterOpener + 1, closerStart - newlineAfterOpener - 1).trimmed();
    if (inner.isEmpty()) return std::nullopt;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(inner.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "AIManager::parseStructuredNext: structuredNext parse failed —" << err.errorString();
        return std::nullopt;
    }
    if (!doc.isObject()) return std::nullopt;
    return doc.object();
}

QJsonArray AIManager::sanitizeApiMessages(const QJsonArray& messages)
{
    // AIConversation stores each turn as {role, content[, shotId][, structuredNext]}.
    // shotId (issue #1053 shot latching) and structuredNext are Decenza-internal
    // bookkeeping and must never reach a provider. The Anthropic Messages API
    // rejects unknown per-message keys with HTTP 400 ("messages.N.shotId: Extra
    // inputs are not permitted"), which killed every dial-in conversation request.
    // Whitelist role + content only; content is copied verbatim (a plain string
    // here — providers do their own cache-block wrapping downstream).
    QJsonArray out;
    for (const QJsonValue& v : messages) {
        const QJsonObject msg = v.toObject();
        QJsonObject clean;
        clean["role"] = msg.value("role");
        clean["content"] = msg.value("content");
        out.append(clean);
    }
    return out;
}

// Heuristic for "the prior assistant message asked the user about
// taste". Conservative: a false negative just means we don't auto-
// persist — the user can still rate via the editor or the rating slider.
// False positives are the dangerous mode (a recommendation reply
// mentioning a "score from 75" past tense triggers a writeback from
// the user's next prose number). To minimize false positives:
//   1. The marker list is tight — bare "score" / "how did" are too
//      common in advisor recommendation prose. Require taste-specific
//      phrasings.
//   2. The message must end in a question — the assistant has to
//      actually be asking, not just discussing scores in passing.
static bool priorAssistantAskedAboutTaste(const QString& priorAssistantMessage)
{
    if (priorAssistantMessage.isEmpty()) return false;
    // Must end in a question mark (allow trailing whitespace and the
    // structuredNext fenced block from #1054).
    QString trimmed = priorAssistantMessage.trimmed();
    if (trimmed.endsWith(QStringLiteral("```"))) {
        // Strip the trailing fenced JSON block before testing for a
        // question-mark suffix on the prose body.
        const qsizetype openerStart = trimmed.lastIndexOf(QStringLiteral("```"),
            trimmed.length() - 4);
        if (openerStart > 0) trimmed = trimmed.left(openerStart).trimmed();
    }
    if (!trimmed.endsWith(QLatin1Char('?'))) return false;

    const QString lc = priorAssistantMessage.toLower();
    static const QStringList markers{
        QStringLiteral("how did it taste"),
        QStringLiteral("how did this taste"),
        QStringLiteral("how did this shot taste"),
        QStringLiteral("how does it taste"),
        QStringLiteral("how does this taste"),
        QStringLiteral("how would you rate"),
        QStringLiteral("rate this shot"),
        QStringLiteral("rate the shot"),
        QStringLiteral("score from 1"),
        QStringLiteral("score 1-100"),
        QStringLiteral("score 1 to 100"),
        QStringLiteral("tasting notes"),
        QStringLiteral("what did you think of the taste"),
    };
    for (const QString& m : markers) {
        if (lc.contains(m)) return true;
    }
    return false;
}

void AIManager::maybePersistRatingFromReply(const QString& userReply,
                                             const QString& priorAssistantMessage,
                                             qint64 shotId)
{
    if (shotId <= 0) return;
    if (!m_shotHistory) return;
    if (!priorAssistantAskedAboutTaste(priorAssistantMessage)) return;

    const auto parsed = parseUserRatingReply(userReply);
    if (!parsed.has_value()) return;

    QVariantMap metadata;
    metadata.insert(QStringLiteral("enjoyment"), parsed->score);
    if (!parsed->notes.isEmpty()) {
        metadata.insert(QStringLiteral("espressoNotes"), parsed->notes);
    }
    qDebug() << "AIManager: conversational rating capture — writing"
             << parsed->score << "to shot" << shotId
             << "(notes" << (parsed->notes.isEmpty() ? "absent" : "present") << ")";
    m_shotHistory->requestUpdateShotMetadata(shotId, metadata);
}

// shot-metadata-capture: did the prior assistant turn ask the user about
// beans (roast level, brand, type, date)? Mirrors priorAssistantAskedAboutTaste
// — message must end in a question mark (allow trailing structuredNext fence)
// and contain at least one bean-asking marker.
static bool priorAssistantAskedAboutBean(const QString& priorAssistantMessage)
{
    if (priorAssistantMessage.isEmpty()) return false;
    QString trimmed = priorAssistantMessage.trimmed();
    if (trimmed.endsWith(QStringLiteral("```"))) {
        const qsizetype openerStart = trimmed.lastIndexOf(QStringLiteral("```"),
            trimmed.length() - 4);
        if (openerStart > 0) trimmed = trimmed.left(openerStart).trimmed();
    }
    if (!trimmed.endsWith(QLatin1Char('?'))) return false;

    const QString lc = priorAssistantMessage.toLower();
    static const QStringList markers{
        QStringLiteral("roast level"),
        QStringLiteral("how dark"),
        QStringLiteral("how light"),
        QStringLiteral("light or dark"),
        QStringLiteral("light, medium"),
        QStringLiteral("medium or dark"),
        QStringLiteral("what kind of bean"),
        QStringLiteral("what bean"),
        QStringLiteral("which bean"),
        QStringLiteral("describe the bean"),
        QStringLiteral("tell me about the bean"),
        QStringLiteral("what roaster"),
        QStringLiteral("which roaster"),
        QStringLiteral("when was it roasted"),
        QStringLiteral("when were they roasted"),
        QStringLiteral("roast date"),
    };
    for (const QString& m : markers) {
        if (lc.contains(m)) return true;
    }
    return false;
}

// Does the user's reply carry an explicit corrective phrasing? This is the
// SECOND gate (alongside priorAssistantAskedAboutBean) — either the model
// asked about beans OR the user volunteered a correction. Markers are
// deliberately conservative: only phrasings that strongly imply a metadata
// correction qualify ("actually...", "the coffee/bean/roast is/was...",
// "the roaster is...", "roasted on/<ISO date>"). Common conversational
// openers like "this is a great shot" or "it's really good" do NOT
// qualify — those would create false-positive writes when paired with the
// parser's roast-level patterns.
static bool userReplyVolunteersBeanCorrection(const QString& reply)
{
    const QString lc = reply.toLower();
    static const QStringList markers{
        QStringLiteral("actually it"),
        QStringLiteral("actually this"),
        QStringLiteral("actually the"),
        QStringLiteral("actually, it"),
        QStringLiteral("actually, this"),
        QStringLiteral("actually, the"),
        QStringLiteral("the coffee is"),
        QStringLiteral("the coffee was"),
        QStringLiteral("the bean is"),
        QStringLiteral("the bean was"),
        QStringLiteral("the beans are"),
        QStringLiteral("the beans were"),
        QStringLiteral("the roast is"),
        QStringLiteral("the roast was"),
        QStringLiteral("the roaster is"),
        QStringLiteral("the roaster was"),
        QStringLiteral("roasted on "),
    };
    for (const QString& m : markers) {
        if (lc.contains(m)) return true;
    }
    // "roasted YYYY-MM-DD" without "on" — match a digit immediately
    // following "roasted ". Tighter than a bare "roasted " contains check
    // (which would fire on "roasted chocolate notes").
    static const QRegularExpression rxRoastedDate(
        QStringLiteral("roasted\\s+\\d{4}-\\d{2}-\\d{2}"),
        QRegularExpression::CaseInsensitiveOption);
    return rxRoastedDate.match(reply).hasMatch();
}

// Canonicalise a free-form roast-level token into the app's stored form.
// Returns an empty string when the token doesn't match any known level.
static QString canonicalRoastLevel(const QString& raw)
{
    QString collapsed = raw.toLower().trimmed();
    collapsed.replace(QRegularExpression(QStringLiteral("[\\s\\-]+")), QString());
    if (collapsed == QLatin1String("light")) return QStringLiteral("Light");
    if (collapsed == QLatin1String("mediumlight") || collapsed == QLatin1String("lightmedium"))
        return QStringLiteral("Medium-Light");
    if (collapsed == QLatin1String("medium")) return QStringLiteral("Medium");
    if (collapsed == QLatin1String("mediumdark") || collapsed == QLatin1String("darkmedium"))
        return QStringLiteral("Medium-Dark");
    if (collapsed == QLatin1String("dark")) return QStringLiteral("Dark");
    return QString();
}

std::optional<AIManager::BeanCorrection>
AIManager::parseBeanCorrectionsFromReply(const QString& reply)
{
    if (reply.trimmed().isEmpty()) return std::nullopt;

    BeanCorrection out;

    // --- roastLevel ---------------------------------------------------------
    // Require a context word that binds the adjective to roast level so
    // tasting phrases ("dark chocolate notes", "light citrus", "this is a
    // dark crema") don't fire the parser. Two regexes; the loosest branch
    // ("(this|it|that) is a X") is split out and requires a mandatory
    // "roast" suffix so "this is a dark crema" is rejected while "this is
    // a dark roast" matches.
    //
    // Patterns covered:
    //   - "the (coffee|bean|roast|roast level) is X (roast)?"
    //   - "actually it's/this is/the X is X (roast)?"
    //   - "roast level is X"
    //   - "(this|it|that) is/was a X roast"  ← roast suffix MANDATORY
    static const QRegularExpression rxRoastStrong(
        QStringLiteral(
            // Group 1 = the level adjective (incl. medium-light / medium-dark)
            "(?:"
              "the\\s+(?:coffee|bean|beans|roast|roastlevel|roast\\s+level)\\s+(?:is|was|are|were)\\s+"
              "(?:a\\s+|really\\s+|very\\s+|pretty\\s+|quite\\s+){0,2}"
            "|"
              "actually[,\\s]+(?:it'?s|this\\s+is|the\\s+coffee\\s+is|the\\s+bean\\s+is|the\\s+roast\\s+is)\\s+"
              "(?:a\\s+|really\\s+|very\\s+|pretty\\s+|quite\\s+){0,2}"
            "|"
              "roast\\s+level\\s+is\\s+"
            ")"
            "(light|medium[\\s\\-]?light|light[\\s\\-]?medium|medium[\\s\\-]?dark|dark[\\s\\-]?medium|medium|dark)"
            "(?:\\s+roast)?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rxRoastLooseRequiresRoastSuffix(
        QStringLiteral(
            "(?:this|it|it's|its|that)\\s+(?:is|was)\\s+"
            "(?:a\\s+|really\\s+|very\\s+|pretty\\s+|quite\\s+){1,3}"
            "(light|medium[\\s\\-]?light|light[\\s\\-]?medium|medium[\\s\\-]?dark|dark[\\s\\-]?medium|medium|dark)"
            "\\s+roast\\b"),
        QRegularExpression::CaseInsensitiveOption);
    {
        QRegularExpressionMatch m = rxRoastStrong.match(reply);
        if (!m.hasMatch()) m = rxRoastLooseRequiresRoastSuffix.match(reply);
        if (m.hasMatch()) {
            const QString canon = canonicalRoastLevel(m.captured(1));
            if (!canon.isEmpty()) out.roastLevel = canon;
        }
    }

    // --- beanBrand ----------------------------------------------------------
    // Patterns:
    //   "from <Brand>" preceded by a corrective lead-in (actually / it's /
    //     this is / the coffee is / the bean is)
    //   "the (roaster|brand) is <Brand>"
    // Brand capture is bounded to 1-4 word tokens (each starting with a
    // word character) so prose replies like "the roaster is having problems
    // with the new burr today" don't get captured as a brand. The captured
    // brand must also begin with an UPPERCASE letter — brand names in
    // conversational English are essentially always capitalised, and the
    // uppercase check rejects sentences that begin with lowercase verbs
    // ("having", "starting", "working") even when they happen to fit the
    // word-count window.
    static const QRegularExpression rxBrand(
        QStringLiteral(
            "(?:"
              "(?:actually[,\\s]+)?(?:it'?s|this\\s+is|the\\s+coffee\\s+is|the\\s+bean\\s+is|the\\s+beans\\s+are)\\s+from\\s+"
            "|"
              "the\\s+(?:roaster|brand)\\s+(?:is|was)\\s+"
            ")"
            "(\\w[\\w&'.\\-]{0,30}(?:\\s+\\w[\\w&'.\\-]{0,30}){0,3})"),
        QRegularExpression::CaseInsensitiveOption);
    {
        const QRegularExpressionMatch m = rxBrand.match(reply);
        if (m.hasMatch()) {
            QString brand = m.captured(1).trimmed();
            // Strip a trailing " roast" / " coffee" suffix the regex may have
            // grabbed on its way to the natural terminator.
            static const QRegularExpression suffix(
                QStringLiteral("\\s+(?:roast|coffee|beans?|espresso)\\s*$"),
                QRegularExpression::CaseInsensitiveOption);
            brand.replace(suffix, QString());
            // Require Title Case on the first character — rejects prose
            // continuations ("having problems...") that the lead-in would
            // otherwise gate through.
            if (!brand.isEmpty() && brand.at(0).isUpper()) out.beanBrand = brand;
        }
    }

    // --- roastDate ----------------------------------------------------------
    // Patterns:
    //   "roasted (on)? YYYY-MM-DD"
    //   "roasted (on)? <Month> <Day>(,? YYYY)?"
    // ISO form takes precedence. Natural-language form defaults year to the
    // current year when omitted.
    static const QRegularExpression rxIso(
        QStringLiteral("roasted\\s+(?:on\\s+)?(\\d{4}-\\d{2}-\\d{2})"),
        QRegularExpression::CaseInsensitiveOption);
    {
        const QRegularExpressionMatch m = rxIso.match(reply);
        if (m.hasMatch()) {
            const QDate d = QDate::fromString(m.captured(1), QStringLiteral("yyyy-MM-dd"));
            if (d.isValid()) out.roastDate = d.toString(QStringLiteral("yyyy-MM-dd"));
        }
    }
    if (!out.roastDate) {
        static const QRegularExpression rxNatural(
            QStringLiteral(
                "roasted\\s+(?:on\\s+)?"
                "(january|february|march|april|may|june|july|august|september|october|november|december|"
                 "jan|feb|mar|apr|jun|jul|aug|sep|sept|oct|nov|dec)\\s+"
                "(\\d{1,2})"
                "(?:[,\\s]+(\\d{4}))?"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = rxNatural.match(reply);
        if (m.hasMatch()) {
            const QString monthRaw = m.captured(1).toLower();
            const int day = m.captured(2).toInt();
            int year = m.captured(3).toInt();
            if (year < 1900) year = QDate::currentDate().year();
            static const QHash<QString, int> monthMap{
                {QStringLiteral("january"), 1}, {QStringLiteral("jan"), 1},
                {QStringLiteral("february"), 2}, {QStringLiteral("feb"), 2},
                {QStringLiteral("march"), 3}, {QStringLiteral("mar"), 3},
                {QStringLiteral("april"), 4}, {QStringLiteral("apr"), 4},
                {QStringLiteral("may"), 5},
                {QStringLiteral("june"), 6}, {QStringLiteral("jun"), 6},
                {QStringLiteral("july"), 7}, {QStringLiteral("jul"), 7},
                {QStringLiteral("august"), 8}, {QStringLiteral("aug"), 8},
                {QStringLiteral("september"), 9}, {QStringLiteral("sep"), 9},
                {QStringLiteral("sept"), 9},
                {QStringLiteral("october"), 10}, {QStringLiteral("oct"), 10},
                {QStringLiteral("november"), 11}, {QStringLiteral("nov"), 11},
                {QStringLiteral("december"), 12}, {QStringLiteral("dec"), 12},
            };
            const int month = monthMap.value(monthRaw, 0);
            if (month > 0) {
                const QDate d(year, month, day);
                if (d.isValid()) out.roastDate = d.toString(QStringLiteral("yyyy-MM-dd"));
            }
        }
    }

    if (out.isEmpty()) return std::nullopt;
    return out;
}

void AIManager::maybePersistBeanCorrectionFromReply(const QString& userReply,
                                                     const QString& priorAssistantMessage,
                                                     qint64 shotId)
{
    if (shotId <= 0) return;
    if (!m_shotHistory) return;

    const auto parsed = parseBeanCorrectionsFromReply(userReply);
    if (!parsed.has_value()) return;

    // Two-gate write: either the model asked about beans last turn OR the
    // user volunteered the correction with explicit corrective phrasing.
    const bool gated = priorAssistantAskedAboutBean(priorAssistantMessage)
                    || userReplyVolunteersBeanCorrection(userReply);
    if (!gated) return;

    QVariantMap metadata;
    if (parsed->roastLevel) metadata.insert(QStringLiteral("roastLevel"), *parsed->roastLevel);
    if (parsed->beanBrand)  metadata.insert(QStringLiteral("beanBrand"),  *parsed->beanBrand);
    if (parsed->roastDate)  metadata.insert(QStringLiteral("roastDate"),  *parsed->roastDate);
    if (metadata.isEmpty()) return;

    qDebug() << "AIManager: conversational bean-metadata capture — writing"
             << metadata.keys() << "to shot" << shotId;
    m_shotHistory->requestUpdateShotMetadata(shotId, metadata);
}

std::optional<AIManager::UserRatingReply> AIManager::parseUserRatingReply(const QString& reply)
{
    // A number 1-100 in the user's reply counts as a score ONLY when
    // one of these strong signals is present:
    //   (a) the number is followed by a `/100`, `out of 100`, or `%`
    //       suffix (unambiguous score notation), OR
    //   (b) the number is the first non-whitespace token in the reply
    //       (the user's reply leads with a score, optionally followed
    //       by notes — the conversational pattern "82, balanced and
    //       sweet" or "82").
    // Numbers that appear elsewhere in prose ("I dosed 18 grams",
    // "30-day-old roast") MUST NOT be picked up as ratings. Out-of-range
    // numbers, negatives, and non-numeric replies all return nullopt.
    if (reply.trimmed().isEmpty()) return std::nullopt;

    static const QRegularExpression rx(QStringLiteral(
        "(\\d+(?:\\.\\d+)?)\\s*"
        "(/\\s*100|out\\s*of\\s*100|%)?"),
        QRegularExpression::CaseInsensitiveOption);

    // Where does the first non-whitespace character sit? The "leading
    // token" gate compares each match's start against this anchor.
    qsizetype firstNonWs = 0;
    while (firstNonWs < reply.size() && reply.at(firstNonWs).isSpace()) ++firstNonWs;

    QRegularExpressionMatchIterator it = rx.globalMatch(reply);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        bool ok = false;
        const double raw = m.captured(1).toDouble(&ok);
        if (!ok) continue;
        const int rounded = static_cast<int>(std::round(raw));
        if (rounded < 1 || rounded > 100) continue;

        // Reject negatives: the regex captures digits without the minus,
        // but if the preceding character is `-` or U+2212 the user wrote
        // a negative.
        const qsizetype matchStartCheck = m.capturedStart(1);
        if (matchStartCheck > 0) {
            const QChar prev = reply.at(matchStartCheck - 1);
            if (prev == QLatin1Char('-') || prev == QChar(0x2212)) continue;
        }

        const bool hasSuffix = !m.captured(2).isEmpty();
        const bool isLeadingToken = m.capturedStart(0) == firstNonWs;
        if (!hasSuffix && !isLeadingToken) continue;  // weak anchor — skip

        UserRatingReply out;
        out.score = rounded;
        const qsizetype matchStart = m.capturedStart(0);
        const qsizetype matchEnd = m.capturedEnd(0);
        QString remaining = reply.left(matchStart) + reply.mid(matchEnd);
        static const QRegularExpression edgeTrim(QStringLiteral(
            "^[\\s,;:\\-—.!?]+|[\\s,;:\\-—.!?]+$"));
        remaining.replace(edgeTrim, QString());
        out.notes = remaining.trimmed();
        return out;
    }
    return std::nullopt;
}

QJsonObject AIManager::buildUserPromptObjectForShot(const ShotProjection& shotData)
{
    ShotSummary summary = m_summarizer->summarizeFromHistory(shotData);
    return m_summarizer->buildUserPromptObject(summary);
}

QString AIManager::buildShotAnalysisProseForShot(const QVariant& shotVariant)
{
    const ShotProjection shotData = coerceShot(shotVariant);
    ShotSummary summary = m_summarizer->summarizeFromHistory(shotData);
    return m_summarizer->buildShotAnalysisProse(summary);
}

void AIManager::enrichUserPromptObject(QJsonObject& payload,
                                       const ShotProjection& shotData,
                                       const QJsonArray& dialInSessions,
                                       const QJsonObject& bestRecentShot,
                                       const QJsonObject& grinderContext,
                                       const QJsonArray& recentAdvice,
                                       const QJsonObject& grinderCalibration,
                                       const QJsonObject& beanBestShot) const
{
    if (!dialInSessions.isEmpty())
        payload["dialInSessions"] = dialInSessions;
    if (!bestRecentShot.isEmpty())
        payload["bestRecentShot"] = bestRecentShot;
    // Bean memory: the user's best rated shot for the CURRENT bean on this
    // profile (issue: bean-memory). Anchors advice to "your best on THIS
    // bean", not just the profile. Empty → key omitted (no placeholder).
    if (!beanBestShot.isEmpty())
        payload["beanBestShot"] = beanBestShot;
    if (!grinderContext.isEmpty())
        payload["grinderContext"] = grinderContext;
    if (!grinderCalibration.isEmpty())
        payload["grinderCalibration"] = grinderCalibration;
    // Closed-loop coaching: prior advisor turns paired with the user's
    // actual next shots (issue #1053). Empty array (no qualifying turns
    // yet) → key omitted; never `recentAdvice: []` placeholder.
    if (!recentAdvice.isEmpty())
        payload["recentAdvice"] = recentAdvice;
    if (shotData.isValid()) {
        const QJsonObject sawPrediction = DialingBlocks::buildSawPredictionBlock(
            m_settings, m_profileManager, shotData);
        if (!sawPrediction.isEmpty())
            payload["sawPrediction"] = sawPrediction;
    }
}

void AIManager::setShotHistoryStorage(ShotHistoryStorage* storage)
{
    m_shotHistory = storage;
}

// File-scope helper: runs on a background thread with its own SQLite connection.
// Returns (timestamp, fullShot) pairs. Extracted from requestRecentShotContext
// to reduce lambda nesting. NOT safe to call from the main thread (would conflict
// with the primary DB connection).
static QList<QPair<qint64, ShotProjection>> loadQualifiedShots(
    const QString& dbPath,
    const QString& beanBrand, const QString& beanType,
    const QString& profileName, int excludeShotId)
{
    QList<QPair<qint64, ShotProjection>> qualifiedShots;

    withTempDb(dbPath, "ai_context", [&](QSqlDatabase& db) {
        // 1. Look up the current shot's timestamp
        qint64 shotTimestamp = 0;
        {
            QSqlQuery q(db);
            q.prepare("SELECT timestamp FROM shots WHERE id = ?");
            q.bindValue(0, static_cast<qint64>(excludeShotId));
            if (!q.exec()) {
                qWarning() << "AIManager::requestRecentShotContext: timestamp query failed:" << q.lastError().text();
            } else if (q.next()) {
                shotTimestamp = q.value(0).toLongLong();
            } else {
                qDebug() << "AIManager::requestRecentShotContext: no shot found for excludeShotId=" << excludeShotId;
            }
        }

        if (shotTimestamp <= 0) return;

        // 2. Query candidates: same bean/profile, up to 3 weeks before this shot
        qint64 dateFrom = shotTimestamp - 21 * 24 * 3600;
        QStringList conditions;
        QVariantList bindValues;
        if (!beanBrand.isEmpty()) { conditions << "bean_brand = ?"; bindValues << beanBrand; }
        if (!beanType.isEmpty()) { conditions << "bean_type = ?"; bindValues << beanType; }
        if (!profileName.isEmpty()) { conditions << "profile_name = ?"; bindValues << profileName; }
        conditions << "timestamp >= ?" << "timestamp <= ?";
        bindValues << dateFrom << shotTimestamp;

        QString sql = "SELECT id, timestamp, profile_name, duration_seconds, final_weight "
                      "FROM shots WHERE " + conditions.join(" AND ") +
                      " ORDER BY timestamp DESC LIMIT 6";

        QSqlQuery q(db);
        q.prepare(sql);
        for (int i = 0; i < bindValues.size(); ++i)
            q.bindValue(i, bindValues[i]);

        struct Candidate { qint64 id; qint64 timestamp; QString profileName; double duration; double finalWeight; };
        QList<Candidate> candidates;
        if (q.exec()) {
            while (q.next()) {
                candidates.append({q.value(0).toLongLong(), q.value(1).toLongLong(),
                                   q.value(2).toString(), q.value(3).toDouble(), q.value(4).toDouble()});
            }
        } else {
            qWarning() << "AIManager::requestRecentShotContext: candidate query failed:" << q.lastError().text();
        }

        qDebug() << "AIManager::requestRecentShotContext: excludeShotId=" << excludeShotId
                 << "shotTimestamp=" << QDateTime::fromSecsSinceEpoch(shotTimestamp).toString("yyyy-MM-dd HH:mm")
                 << "filter: bean=" << beanBrand << beanType << "profile=" << profileName
                 << "candidates=" << candidates.size();

        // 3. Filter and load full records for up to 3 qualifying shots
        int included = 0;
        for (const auto& c : candidates) {
            if (included >= 3) break;

            if (c.id == excludeShotId) {
                qDebug() << "  Shot id=" << c.id << "-> SKIPPED (current shot)";
                continue;
            }

            // Lightweight mistake check (duration < 10s or weight < 5g)
            if (c.duration < 10.0 || c.finalWeight < 5.0) {
                qDebug() << "  Shot id=" << c.id << "-> SKIPPED (mistake)";
                continue;
            }

            ShotProjection fullShot;
            try {
                ShotRecord record = ShotHistoryStorage::loadShotRecordStatic(db, c.id);
                fullShot = ShotHistoryStorage::convertShotRecord(record);
            } catch (const std::exception& e) {
                qWarning() << "  Shot id=" << c.id << "-> SKIPPED (exception:" << e.what() << ")";
                continue;
            }
            if (!fullShot.isValid()) {
                qWarning() << "  Shot id=" << c.id << "-> SKIPPED (convertShotRecord returned empty)";
                continue;
            }

            // Check targetWeight-based mistake filter (needs full record)
            if (fullShot.targetWeightG > 0.0 && c.finalWeight < fullShot.targetWeightG / 3.0) {
                qDebug() << "  Shot id=" << c.id << "-> SKIPPED (mistake, weight < 1/3 target)";
                continue;
            }

            qDebug() << "  Shot id=" << c.id << "-> INCLUDED";
            qualifiedShots.append({c.timestamp, std::move(fullShot)});
            ++included;
        }
    });
    return qualifiedShots;
}

// File-scope helper: render one `recentAdvice` entry (see
// DialingBlocks::buildRecentAdviceBlock) as a markdown block. Every other
// section of the in-app historicalContext is hand-rendered prose, not a
// JSON blob, so this keeps the format consistent — the underlying data is
// identical to what the MCP `recentAdvice` JSON array carries for the same
// inputs (turnsAgo/recommendation/structuredNext/userResponse).
static QString renderRecentAdviceEntry(const QJsonObject& entry)
{
    const int turnsAgo = entry.value("turnsAgo").toInt();
    const QString recommendation = entry.value("recommendation").toString();
    const QJsonObject sn = entry.value("structuredNext").toObject();
    const QJsonObject resp = entry.value("userResponse").toObject();

    QString out = QStringLiteral("### %1 shot%2 ago\n\n")
        .arg(turnsAgo).arg(turnsAgo == 1 ? "" : "s");
    if (!recommendation.isEmpty())
        out += QStringLiteral("**You recommended**: %1\n\n").arg(recommendation);

    const DialingBlocks::StructuredNextSummary snSummary = DialingBlocks::summarizeStructuredNext(sn);
    if (!snSummary.predictedParts.isEmpty())
        out += QStringLiteral("- Predicted: %1\n").arg(snSummary.predictedParts.join(QStringLiteral(", ")));
    if (!snSummary.expectedParts.isEmpty())
        out += QStringLiteral("- Expected: %1\n").arg(snSummary.expectedParts.join(QStringLiteral(", ")));

    QStringList actual;
    const QString actualGrinder = resp.value("grinderSetting").toString();
    if (!actualGrinder.isEmpty())
        actual << QStringLiteral("grinder %1").arg(actualGrinder);
    const int actualRpm = resp.value("rpm").toInt();
    if (actualRpm > 0)
        actual << QStringLiteral("%1 RPM").arg(actualRpm);
    const double actualDose = resp.value("doseG").toDouble();
    if (actualDose > 0)
        actual << QStringLiteral("dose %1g").arg(actualDose, 0, 'f', 1);
    out += QStringLiteral("- Your next shot: %1 — adherence: **%2**\n")
        .arg(actual.isEmpty() ? QStringLiteral("(no change recorded)") : actual.join(QStringLiteral(", ")))
        .arg(resp.value("adherence").toString());

    if (resp.contains(QStringLiteral("outcomeRating0to100"))) {
        QString ratingLine = QStringLiteral("- Score: %1/100").arg(resp.value("outcomeRating0to100").toInt());
        const QString notes = resp.value("outcomeNotes").toString();
        if (!notes.isEmpty())
            ratingLine += QStringLiteral(" (\"%1\")").arg(notes);
        out += ratingLine + QStringLiteral("\n");
    } else if (resp.contains(QStringLiteral("outcomeNotes"))) {
        out += QStringLiteral("- Notes: \"%1\"\n").arg(resp.value("outcomeNotes").toString());
    }

    const QJsonObject inRange = resp.value(QStringLiteral("outcomeInPredictedRange")).toObject();
    if (!inRange.isEmpty()) {
        QStringList rangeParts;
        if (inRange.contains(QStringLiteral("duration")))
            rangeParts << QStringLiteral("duration %1").arg(inRange.value("duration").toBool() ? "in range" : "out of range");
        if (inRange.contains(QStringLiteral("flow")))
            rangeParts << QStringLiteral("flow %1").arg(inRange.value("flow").toBool() ? "in range" : "out of range");
        if (inRange.contains(QStringLiteral("pressure")))
            rangeParts << QStringLiteral("pressure %1").arg(inRange.value("pressure").toBool() ? "in range" : "out of range");
        if (!rangeParts.isEmpty())
            out += QStringLiteral("- Outcome vs prediction: %1\n").arg(rangeParts.join(QStringLiteral(", ")));
    }

    return out;
}

void AIManager::requestRecentShotContext(const QString& beanBrand, const QString& beanType, const QString& profileName, int excludeShotId)
{
    if (!m_shotHistory || (beanBrand.isEmpty() && profileName.isEmpty())) {
        emit recentShotContextReady(QString());
        return;
    }

    const QString dbPath = m_shotHistory->databasePath();
    QPointer<AIManager> self(this);
    ++m_contextSerial;
    int serial = m_contextSerial;

    // NOTE: QPointer is NOT thread-safe — it tracks QObject destruction via the main
    // event loop. The background thread captures `self` by value but MUST NOT dereference
    // it. All dereferences occur inside the QueuedConnection callback, which runs on the
    // main thread where QPointer's tracking is valid.
    QThread* thread = QThread::create([self, dbPath, beanBrand, beanType, profileName, excludeShotId, serial]() {
        auto qualifiedShots = loadQualifiedShots(dbPath, beanBrand, beanType, profileName, excludeShotId);

        GrinderContext grinderCtx;
        QString grinderBrand;
        QJsonObject grinderCalibration;
        QJsonArray recentAdvice;
        withTempDb(dbPath, "ai_grinder_ctx", [&](QSqlDatabase& db) {
            QSqlQuery q(db);
            // Grinder identity resolves through the shot's equipment_id pointer
            // (the per-shot grinder_brand/model/burrs columns are dropped in
            // migration 23, add-equipment-packages task 4.1). burrs is in the
            // grinder item's attrs JSON blob. profile_kb_id is pulled here too
            // (not a separate query) so the recentAdvice build below can
            // cross-profile-filter without another round-trip.
            q.prepare("SELECT eg.brand, eg.model, json_extract(eg.attrs, '$.burrs'), s.beverage_type, s.profile_kb_id "
                      "FROM shots s "
                      "LEFT JOIN equipment_items eg ON eg.package_id = s.equipment_id AND eg.kind = 'grinder' "
                      "WHERE s.id = ?");
            q.bindValue(0, static_cast<qint64>(excludeShotId));
            QString profileKbId;
            if (!q.exec()) {
                qWarning() << "AIManager::requestRecentShotContext: grinder ctx query failed:"
                           << q.lastError().text();
            } else if (q.next()) {
                grinderBrand = q.value(0).toString();
                QString model = q.value(1).toString();
                QString burrs = q.value(2).toString();
                QString bev = q.value(3).toString();
                profileKbId = q.value(4).toString();
                if (!model.isEmpty()) {
                    grinderCtx = ShotHistoryStorage::queryGrinderContext(db, model, bev);
                    grinderCalibration = DialingBlocks::buildGrinderCalibrationBlock(
                        db, model, burrs, bev, excludeShotId);
                }
            }

            // Closed-loop recentAdvice (issue #1053) — same pattern
            // ai_advisor_invoke uses (mcptools_ai.cpp), so the in-app
            // advisor's historicalContext carries the same tracking data
            // the MCP path already does.
            if (!profileKbId.isEmpty()) {
                const QString convKey = AIManager::conversationKey(beanBrand, beanType, profileName);
                const auto turns = AIConversation::loadRecentAssistantTurnsForKey(convKey, 3);
                if (!turns.isEmpty()) {
                    DialingBlocks::RecentAdviceInputs in;
                    in.turns = turns;
                    in.currentProfileKbId = profileKbId;
                    in.currentShotId = excludeShotId;
                    recentAdvice = DialingBlocks::buildRecentAdviceBlock(db, in);
                }
            }
        });

        // Summarization runs on main thread (ShotSummarizer is owned by AIManager).
        // The render+emit work is in `emitRecentShotContext` so the
        // canonical-source separation logic can be exercised by tests
        // (`friend class tst_AIManager`) without standing up a real DB.
        QMetaObject::invokeMethod(qApp, [self, serial, qualifiedShots = std::move(qualifiedShots),
                                         grinderCtx = std::move(grinderCtx),
                                         grinderBrand = std::move(grinderBrand),
                                         grinderCalibration = std::move(grinderCalibration),
                                         recentAdvice = std::move(recentAdvice)]() mutable {
            if (!self) return;
            self->emitRecentShotContext(qualifiedShots, grinderCtx, grinderBrand, serial, grinderCalibration, recentAdvice);
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void AIManager::emitRecentShotContext(
    const QList<QPair<qint64, ShotProjection>>& qualifiedShots,
    const GrinderContext& grinderCtx,
    const QString& grinderBrand,
    int serial,
    const QJsonObject& grinderCalibration,
    const QJsonArray& recentAdvice)
{
    if (serial != m_contextSerial) {
        // Stale request superseded by a newer one — emit empty so QML clears contextLoading.
        emit recentShotContextReady(QString());
        return;
    }

    QString result;

    // Per openspec optimize-dialing-context-payload (task 10.3):
    // hoist profile + setup constants to a single header at the
    // top of the history section, then render each shot in
    // `HistoryBlock` mode so the per-shot blocks carry shot-
    // variable data only. Saves ~5,400 chars across a 4-shot
    // history (Northbound 80's Espresso baseline) by killing
    // N× repetition of profile intent + recipe + grinder/bean
    // identity.
    QString profileTitle, profileIntent, profileRecipe;
    QString setupGrinderBrand, setupGrinderModel, setupGrinderBurrs;
    QString setupBeanBrand, setupBeanType, setupRoastLevel, setupRoastDate;
    // Empty fields read as "unrecorded, inherit" — not "different."
    // Older shots predating DYE recording have empty grinder/bean
    // strings; treating those as a mismatch would suppress the
    // hoisted Setup header for any history that mixes
    // pre-DYE shots with post-DYE shots. Only flip setupShared
    // false when both sides are non-empty AND differ. The shared
    // values are populated lazily via firstNonEmpty so a recorded
    // value seeds the canonical even if shot[0] was unrecorded.
    bool setupShared = !qualifiedShots.isEmpty();
    auto seedOrCompare = [&setupShared](QString& canonical, const QString& v) {
        if (canonical.isEmpty()) {
            canonical = v;
        } else if (!v.isEmpty() && v != canonical) {
            setupShared = false;
        }
    };
    for (const auto& qs : qualifiedShots) {
        const ShotProjection& s = qs.second;
        seedOrCompare(setupGrinderBrand, s.grinderBrand);
        seedOrCompare(setupGrinderModel, s.grinderModel);
        seedOrCompare(setupGrinderBurrs, s.grinderBurrs);
        seedOrCompare(setupBeanBrand, s.beanBrand);
        seedOrCompare(setupBeanType, s.beanType);
        seedOrCompare(setupRoastLevel, s.roastLevel);
        seedOrCompare(setupRoastDate, s.roastDate);
        if (profileTitle.isEmpty() && !s.profileName.isEmpty())
            profileTitle = s.profileName;
        if (profileIntent.isEmpty() && !s.profileNotes.isEmpty())
            profileIntent = s.profileNotes;
        if (profileRecipe.isEmpty() && !s.profileJson.isEmpty())
            profileRecipe = Profile::describeFramesFromJson(s.profileJson);
    }

    QStringList shotSections;
    for (const auto& qs : qualifiedShots) {
        ShotSummary summary = m_summarizer->summarizeFromHistory(qs.second);
        QString summaryText = m_summarizer->buildUserPrompt(
            summary, ShotSummarizer::RenderMode::HistoryBlock);
        if (summaryText.isEmpty()) continue;

        static const bool use12h = QLocale::system().timeFormat(QLocale::ShortFormat).contains("AP", Qt::CaseInsensitive);
        QString dateStr = QDateTime::fromSecsSinceEpoch(qs.first).toString(use12h ? "MMM d, h:mm AP" : "MMM d, HH:mm");
        shotSections.prepend(QString("### Shot (%1)\n\n%2").arg(dateStr).arg(summaryText));
    }

    if (!shotSections.isEmpty()) {
        result = "## Previous Shots with This Bean & Profile\n\n"
                 "All shots below use the same profile as the current shot. "
                 "Do not comment on frame-level recipe details unless they changed between shots. "
                 "Focus on what the user changed (grind, dose, temperature) and how it affected the outcome.\n\n";

        if (!profileTitle.isEmpty()) {
            result += "### Profile: " + profileTitle + "\n";
            if (!profileIntent.isEmpty())
                result += profileIntent + "\n";
            if (!profileRecipe.isEmpty())
                result += profileRecipe;
            result += "\n";
        }

        if (setupShared && (!setupGrinderBrand.isEmpty() || !setupGrinderModel.isEmpty()
                            || !setupBeanBrand.isEmpty() || !setupBeanType.isEmpty())) {
            // Build each segment as a complete fragment, then join with " "
            // — that way no segment owns a leading space, and absent fields
            // don't produce double-space artifacts (e.g. burrs without a
            // grinder brand+model used to render "### Setup:  with 63mm").
            QStringList parts;
            QString grinderName;
            if (!setupGrinderBrand.isEmpty()) grinderName = setupGrinderBrand;
            if (!setupGrinderModel.isEmpty()) {
                if (!grinderName.isEmpty()) grinderName += " ";
                grinderName += setupGrinderModel;
            }
            if (!setupGrinderBurrs.isEmpty()) {
                grinderName += grinderName.isEmpty()
                    ? setupGrinderBurrs
                    : " with " + setupGrinderBurrs;
            }
            if (!grinderName.isEmpty()) parts << grinderName;

            QString beanName;
            if (!setupBeanBrand.isEmpty() && !setupBeanType.isEmpty())
                beanName = setupBeanBrand + " - " + setupBeanType;
            else if (!setupBeanBrand.isEmpty())
                beanName = setupBeanBrand;
            else if (!setupBeanType.isEmpty())
                beanName = setupBeanType;
            if (!beanName.isEmpty()) {
                QString beanFull = beanName;
                if (!setupRoastLevel.isEmpty()) beanFull += " (" + setupRoastLevel + ")";
                if (!setupRoastDate.isEmpty()) beanFull += ", roasted " + setupRoastDate;
                parts << (parts.isEmpty() ? beanFull : "on " + beanFull);
            }

            result += "### Setup: " + parts.join(" ") + "\n\n";
        }

        result += shotSections.join("\n\n");
    }

    // Append grinder context if available (observed settings range and step size)
    if (!grinderCtx.settingsObserved.isEmpty()) {
        QString section = "\n\n## Grinder Context\n\n"
            "From the user's own shot history with this grinder:\n\n";
        section += "- **Model**: " + grinderCtx.model + "\n";

        // Burr specs are already shown per-shot in buildUserPrompt().
        // Only add swappability here — it's grinder-level info not in per-shot data.
        if (GrinderAliases::isBurrSwappable(grinderBrand, grinderCtx.model))
            section += "- **Burr-swappable**: yes (aftermarket burrs available for this grinder)\n";

        section += "- **Settings used for " + grinderCtx.beverageType + "**: "
                 + grinderCtx.settingsObserved.join(", ") + "\n";
        if (grinderCtx.allNumeric && grinderCtx.maxSetting > grinderCtx.minSetting) {
            section += "- **Range explored**: " + QString::number(grinderCtx.minSetting) + " \u2013 "
                     + QString::number(grinderCtx.maxSetting) + "\n";
        }
        // Noise-filtered typical dial increment (mirrors grinderContext.stepSize
        // in the MCP payload). Decoupled from the range gate so it shows for a
        // mixed-notation grinder too.
        if (grinderCtx.stepSize > 0) {
            section += "- **Typical step**: " + QString::number(grinderCtx.stepSize) + "\n";
        }
        // RPM axis (variable-RPM grinders): mirrors grinderContext.rpmsObserved /
        // observedMin/MaxRpm / rpmStepSize in the MCP payload.
        if (!grinderCtx.rpmsObserved.isEmpty()) {
            QStringList rpmStrs;
            for (int r : grinderCtx.rpmsObserved)
                rpmStrs << QString::number(r);
            section += "- **RPMs used**: " + rpmStrs.join(", ") + "\n";
            if (grinderCtx.rpmMax > grinderCtx.rpmMin) {
                section += "- **RPM range**: " + QString::number(grinderCtx.rpmMin) + " – "
                         + QString::number(grinderCtx.rpmMax) + "\n";
            }
            if (grinderCtx.rpmStepSize > 0) {
                section += "- **Typical RPM step**: " + QString::number(grinderCtx.rpmStepSize) + "\n";
            }
        }
        result += section;
    }

    // Append grinder calibration. Rewritten for issue #1223
    // (openspec `fix-grinder-calibration-cross-profile`): the block is now
    // `confidence`-tagged and may be directional-only (no numbers). The
    // `usageConstraint` string is repeated verbatim so the model cannot
    // misuse UGS as click arithmetic; directional profiles get finer/
    // coarser only — never a number, never a click delta. Goes into
    // historicalContext → first user message → cached like the rest.
    if (!grinderCalibration.isEmpty()) {
        const QString model = grinderCalibration[QStringLiteral("grinderModel")].toString();
        const QString confidence = grinderCalibration[QStringLiteral("confidence")].toString();
        const QString usage = grinderCalibration[QStringLiteral("usageConstraint")].toString();
        const bool curUgsPlaced =
            grinderCalibration[QStringLiteral("currentProfileUgsPlaced")].toBool();
        const QJsonArray profiles =
            grinderCalibration[QStringLiteral("profiles")].toArray();

        QString cal = QStringLiteral("\n\n## Grinder Calibration\n\n");
        if (!usage.isEmpty())
            cal += usage + QStringLiteral("\n\n");

        if (confidence == QStringLiteral("approximate")) {
            const QJsonObject anchor =
                grinderCalibration[QStringLiteral("coffeeAnchor")].toObject();
            const QJsonArray range =
                grinderCalibration[QStringLiteral("calibratedUgsRange")].toArray();
            const double ck = grinderCalibration[QStringLiteral("conversionKey")].toDouble();
            cal += QStringLiteral(
                "Approximate calibration for your %1, anchored on your recent "
                "**%2** shot (setting %3) for the current coffee (%4). "
                "Conversion ≈ %5 grinder steps per UGS unit; numbers are "
                "valid only within UGS %6–%7. Treat as a rough starting "
                "point, not a precise dial.\n\n")
                .arg(model)
                .arg(anchor[QStringLiteral("profileName")].toString())
                .arg(anchor[QStringLiteral("setting")].toString())
                .arg(anchor[QStringLiteral("coffee")].toString())
                .arg(ck)
                .arg(range.size() == 2 ? range.at(0).toDouble() : 0.0)
                .arg(range.size() == 2 ? range.at(1).toDouble() : 0.0);
        } else {
            cal += QStringLiteral(
                "No numeric cross-profile calibration is available for the "
                "current coffee on your %1 — not enough same-batch dial-in "
                "data. Give only relative grind direction (finer/coarser) "
                "and tell the user to pull a reference shot on the target "
                "profile; do NOT quote or compute a grinder number.\n\n")
                .arg(model);
        }

        if (!curUgsPlaced) {
            cal += QStringLiteral(
                "Your current profile is not on the UGS chart, so finer/"
                "coarser ordering against it cannot be given — say so rather "
                "than guess.\n");
        } else {
            QStringList lines;
            for (const QJsonValue& v : profiles) {
                const QJsonObject p = v.toObject();
                const QString name = p[QStringLiteral("profileName")].toString();
                const double ugs = p[QStringLiteral("ugs")].toDouble();
                const QString src = p[QStringLiteral("source")].toString();
                if (src == QStringLiteral("history") || src == QStringLiteral("derived")) {
                    lines << QStringLiteral("- **%1** (UGS %2): **%3** (%4)")
                        .arg(name).arg(ugs)
                        .arg(p[QStringLiteral("rgs")].toString()).arg(src);
                } else {
                    const QString dir = p[QStringLiteral("direction")].toString();
                    lines << QStringLiteral("- **%1** (UGS %2): grind %3 — pull a reference shot")
                        .arg(name).arg(ugs)
                        .arg(dir.isEmpty()
                             ? QStringLiteral("similar; relative position unclear")
                             : dir);
                }
            }
            if (!lines.isEmpty())
                cal += QStringLiteral("Cross-profile guidance (relative to your "
                                      "current profile):\n\n") + lines.join('\n') + '\n';
        }

        result += cal;
    }

    // Recent Advice Tracking goes first — "what I told you last time"
    // should read before the raw shot-by-shot history so the model sees
    // its own prior call before re-deriving from scratch.
    if (!recentAdvice.isEmpty()) {
        QStringList entries;
        for (const QJsonValue& v : recentAdvice)
            entries << renderRecentAdviceEntry(v.toObject());
        result = QStringLiteral("## Recent Advice Tracking\n\n")
                + entries.join(QStringLiteral("\n"))
                + QStringLiteral("\n\n") + result;
    }

    emit recentShotContextReady(result);
}

// [barista-fork] Assemble the full advisor-grade dialing context for the conversational barista, anchored
// on the current bean (falling back to the latest shot overall). Mirrors the ai_advisor_invoke recipe in
// mcptools_ai.cpp: SQL/blocks on a background thread, then buildUserPromptObjectForShot + enrichUserPromptObject
// on the main thread. Stale results are dropped via the shared m_contextSerial guard.
// [barista-fork] Format the proactive [Recipes] block (Fable design spec §3): the active recipe line + up to
// 6 MRU rows, so the model can resolve "use <name>" / "this recipe" without a tool round-trip. Built off the
// main thread from the shot DB inventory; the active id is captured on the main thread and passed in.
static QString formatRecipesBlock(const QVector<InventoryRecipe>& inv, qint64 activeRecipeId)
{
    const auto milkOf = [](const Recipe& r) {
        return !r.steamJson.isEmpty()
            && QJsonDocument::fromJson(r.steamJson.toUtf8()).object().value(QStringLiteral("hasMilk")).toBool();
    };
    QString out = QStringLiteral("\n\n[Recipes]\n");
    const InventoryRecipe* active = nullptr;
    if (activeRecipeId > 0)
        for (const InventoryRecipe& ir : inv)
            if (ir.recipe.id == activeRecipeId) { active = &ir; break; }
    if (active) {
        out += QStringLiteral("Active recipe: \"%1\" (%2%3, profile: %4) — id %5\n")
                   .arg(active->recipe.name,
                        active->recipe.drinkType.isEmpty() ? QStringLiteral("drink") : active->recipe.drinkType,
                        milkOf(active->recipe) ? QStringLiteral(", milk drink") : QString(),
                        active->recipe.profileTitle.isEmpty() ? QStringLiteral("(hot water)") : active->recipe.profileTitle,
                        QString::number(active->recipe.id));
    } else {
        out += QStringLiteral("Active recipe: none\n");
    }
    if (inv.isEmpty()) {
        out += QStringLiteral("Saved recipes: none\n");
        return out;
    }
    out += QStringLiteral("Saved recipes, most recent first (%1 total):\n").arg(inv.size());
    int shown = 0;
    for (const InventoryRecipe& ir : inv) {
        if (shown >= 6) break;
        ++shown;
        const Recipe& r = ir.recipe;
        const QString bean = QString(r.roasterName + QLatin1Char(' ') + r.coffeeName).trimmed();
        out += QStringLiteral("  %1 · %2 · %3%4%5%6\n")
                   .arg(QString::number(r.id),
                        r.name,
                        r.drinkType.isEmpty() ? QStringLiteral("?") : r.drinkType,
                        bean.isEmpty() ? QString() : (QStringLiteral(" · ") + bean),
                        milkOf(r) ? QStringLiteral(" · milk") : QString(),
                        ir.stale ? QStringLiteral(" · stale:YES (bag out of inventory)") : QString());
    }
    out += QStringLiteral("Say \"use <name>\" style requests map to activate_recipe. Recipes are whole drinks; "
                          "activating one replaces the loaded profile.\n");
    return out;
}

// [barista-fork] Phase 1 identity: the [Who] block tells the model who it's talking to + who it knows, and —
// critically — that ALL the history/notes/recipes in this context belong to the OWNER of the machine, so a
// guest never gets the owner's shots attributed to them. activeUser = the roster active user (dyeBarista); the
// persona (QML) supplies the owner's actual name, so the NOTE references "the owner" generically here.
static QString formatWhoBlock(const QString& activeUser, const QVector<Barista>& roster)
{
    QString out = QStringLiteral("\n\n[Who]\n");
    out += QStringLiteral("activeUser: %1\n").arg(activeUser.isEmpty() ? QStringLiteral("unknown") : activeUser);
    if (!roster.isEmpty()) {
        QStringList names;
        for (const Barista& b : roster)
            if (!b.name.trimmed().isEmpty()) names << b.name.trimmed();
        if (!names.isEmpty())
            out += QStringLiteral("known: %1\n").arg(names.join(QStringLiteral(", ")));
    }
    out += QStringLiteral(
        "NOTE: The shot history, tasting notes, recipes, dial-in history, and past advice in this context were "
        "ALL recorded by the OWNER on this machine. If the active user IS the owner (or unknown), treat that "
        "history as theirs normally. If the active user is a DIFFERENT guest, you still have the machine's full "
        "history — but it's the owner's, so do NOT tell the guest they pulled shots, earned ratings, or have a "
        "history they don't; speak of it as the machine's or the owner's. Always attribute honestly. "
        "MAINTENANCE and REMINDERS belong to the MACHINE, not to any user — they are the same for everyone, so "
        "never reframe them as the active user's (e.g. not \"your descale\" tied to a guest — it's the machine's).\n");
    return out;
}

// [barista-fork] Parallel quick-filler: fire a tiny Haiku turn the instant the user's utterance is dispatched,
// so a short spoken acknowledgement can play ~2s sooner than the main turn's own (slower) lead-in. Model-
// generated + varied (never a hardcoded string). Silent no-op without an Anthropic key. A per-request lambda
// captures this request's gen; if a newer requestQuickFiller() bumps m_fillerGen before this one returns, the
// stale completion is dropped. The overlay does the real "is it still needed" gating.
void AIManager::requestQuickFiller(const QString& utterance)
{
    if (!m_fillerProvider)
        return;
    // Capture THIS request's gen in a per-request connection. analysisComplete carries no gen and the provider
    // does not abort an in-flight request, so a plain static slot would happily speak turn N's filler into turn
    // N+1. Reconnect each call (dropping the prior in-flight connection) and gate on the captured gen.
    const int gen = ++m_fillerGen;
    QObject::disconnect(m_fillerConn);
    m_fillerConn = connect(m_fillerProvider.get(), &AIProvider::analysisComplete, this,
        [this, gen](const QString& text) {
            if (gen != m_fillerGen)   // a newer requestQuickFiller() superseded this one → drop
                return;
            onQuickFillerReady(text);
        });
    // [barista-fork] STATIC system prompt (the utterance rides as the user message below, not embedded here) so
    // it stays cache-friendly AND — the load-bearing part — it forces VARIETY. The owner's rule: fillers must
    // sound human, never a canned catchphrase ("don't sound like an ATM"). No lead example to anchor on (an
    // "e.g. one sec" list made Haiku parrot "one sec"); instead, explicit anti-catchphrase + riff-on-their-words
    // instruction, and Anthropic's default temperature (1.0) does the rest. The model sees the actual utterance
    // as the user turn, so it naturally varies with what was asked.
    const QString sys = QStringLiteral(
        "You are a warm, quick-witted espresso barista. The user just said something to you (below). In the "
        "half-second before you answer, say ONE short, natural spoken filler — 3 to 8 words — the kind of thing a "
        "real person blurts out while their brain catches up. REACT TO THE MOMENT: a warm little response to what "
        "they actually just said, or a genuine beat of personality. This is the variety engine — because it "
        "responds to THEIR words, it should come out different every single time. "
        // The observed failure was the OPPOSITE of the old "one sec" parroting: the previous prompt over-narrowed
        // this to "acknowledge you're thinking," and the model collapsed onto a robotic "Let me think about that
        // for a second" nearly every turn. So: explicitly ban that flat phrasing and push toward reacting to the
        // actual message, which varies by input.
        "It must feel SPONTANEOUS and be different every time — never the same opener twice, and NEVER a flat "
        "\"let me think about that\" / \"let me think about that for a second\" (that is exactly the robotic tic "
        "to avoid). React to what they said instead. "
        // Still must not over-promise: it plays BEFORE the real turn decides whether to look anything up.
        "One rule: you don't yet know whether you'll look anything up, so don't promise a specific action (no "
        "\"let me get/find/check/look up/pull up\" a thing) — just a genuine human beat reacting to them. "
        "No emojis, no surrounding quotes, don't answer the question.");
    m_fillerProvider->analyze(sys, utterance);
}

void AIManager::onQuickFillerReady(const QString& text)
{
    // Staleness is already handled by the per-request gen check in requestQuickFiller's lambda; this only
    // cleans up and forwards the accepted filler.
    QString t = text.trimmed();
    // Strip a stray wrapping quote pair the model may add despite the instruction.
    if (t.size() >= 2 && ((t.startsWith(QLatin1Char('"')) && t.endsWith(QLatin1Char('"')))
                          || (t.startsWith(QLatin1Char('\'')) && t.endsWith(QLatin1Char('\'')))))
        t = t.mid(1, t.size() - 2).trimmed();
    if (t.isEmpty())
        return;
    emit quickFillerReady(t);
}

void AIManager::requestBaristaContext(const QString& beanBrand, const QString& beanType, const QString& profileName)
{
    if (!m_shotHistory) {
        emit baristaContextReady(QStringLiteral("recordedShots: 0"));
        return;
    }

    m_lastBaristaAnchorId = 0;   // clear now so a dropped/superseded callback can't leave a stale anchor (S1)
    m_lastBaristaAnchorSnapshot.clear();   // [barista-fork] same reason for the write-tool provenance snapshot
    const QString dbPath = m_shotHistory->databasePath();
    // [barista-fork] assistant.db path for the proactive feedback block (a SEPARATE DB from shots.db).
    const QString feedbackDbPath = m_feedbackStorage ? m_feedbackStorage->databasePath() : QString();
    QPointer<AIManager> self(this);
    ++m_baristaContextSerial;
    int serial = m_baristaContextSerial;
    // [barista-fork] Recipes 2.0 proactive block: read the active recipe id on the MAIN thread (live dye
    // setting); the worker loads the recipe inventory from the shot DB and formats the [Recipes] block.
    const qint64 activeRecipeId = (m_settings && m_settings->dye()) ? m_settings->dye()->activeRecipeId() : -1;
    // [barista-fork] Phase 1 identity: the active roster user (dyeBarista), read live on the main thread. The
    // worker loads the roster from the shot DB and formats the [Who] block.
    const QString activeUser = (m_settings && m_settings->dye()) ? m_settings->dye()->dyeBarista() : QString();

    // self is captured by value but ONLY dereferenced inside the main-thread callback (QPointer is
    // not thread-safe). See requestRecentShotContext for the same discipline.
    QThread* thread = QThread::create([self, dbPath, feedbackDbPath, beanBrand, beanType, profileName, serial, activeRecipeId, activeUser]() {
        qint64 anchorId = 0;
        bool beanFilterMissed = false;
        ShotProjection shot;
        QJsonArray dialInSessions;
        QJsonArray recentAdvice;
        QJsonObject bestRecentShot;
        QJsonObject beanBestShot;
        QJsonObject grinderContext;
        QJsonObject grinderCalibration;
        QJsonObject fullHistory;
        QJsonArray beanFeedback;   // [barista-fork] recent verbal tasting feedback on the CURRENT bean
        QJsonObject dueItems;      // [barista-fork] due reminders + due maintenance (assistant.db, shot-independent)
        QJsonObject docChange;     // [barista-fork] a pending Decent cleaning-guide change to OFFER (assistant.db)
        QJsonObject occasion;      // [barista-fork] today's US holiday + personal dates for the greeting/goodbye
        QJsonArray knownFacts;     // [barista-fork] durable basic facts the user told the barista (remember_fact)

        withTempDb(dbPath, "barista_ctx", [&](QSqlDatabase& db) {
            // Anchor: latest shot for the current bean; else latest overall (robust to bean-name drift).
            if (!beanBrand.isEmpty() || !beanType.isEmpty()) {
                QSqlQuery q(db);
                q.prepare("SELECT id FROM shots WHERE bean_brand = ? AND bean_type = ? "
                          "ORDER BY timestamp DESC LIMIT 1");
                q.addBindValue(beanBrand);
                q.addBindValue(beanType);
                if (q.exec () && q.next())
                    anchorId = q.value(0).toLongLong();
            }
            if (anchorId <= 0) {
                beanFilterMissed = (!beanBrand.isEmpty() || !beanType.isEmpty());
                QSqlQuery q(db);
                if (q.exec ("SELECT id FROM shots ORDER BY timestamp DESC LIMIT 1") && q.next())
                    anchorId = q.value(0).toLongLong();
            }
            if (anchorId <= 0)
                return;

            ShotRecord record = ShotHistoryStorage::loadShotRecordStatic(db, anchorId);
            shot = ShotHistoryStorage::convertShotRecord(record);
            if (!shot.isValid())
                return;

            // Same shared helpers the advisor ships (openspec add-dialing-blocks-to-advisor).
            dialInSessions = DialingBlocks::buildDialInSessionsBlock(db, shot.profileKbId, anchorId, 5);
            bestRecentShot = DialingBlocks::buildBestRecentShotBlock(db, shot.profileKbId, anchorId, shot);
            beanBestShot = DialingBlocks::buildBeanBestShotBlock(
                db, shot.profileKbId, shot.beanBrand, shot.beanType, shot.barista, anchorId, shot);
            grinderContext = DialingBlocks::buildGrinderContextBlock(
                db, shot.grinderModel, shot.beverageType, shot.beanBrand);
            grinderCalibration = DialingBlocks::buildGrinderCalibrationBlock(
                db, shot.grinderModel, shot.grinderBurrs, shot.beverageType, anchorId);
            if (!shot.profileKbId.isEmpty()) {
                const QString convKey = AIManager::conversationKey(shot.beanBrand, shot.beanType, shot.profileName);
                const auto turns = AIConversation::loadRecentAssistantTurnsForKey(convKey, 3);
                if (!turns.isEmpty()) {
                    DialingBlocks::RecentAdviceInputs in;
                    in.turns = turns;
                    in.currentProfileKbId = shot.profileKbId;
                    in.currentShotId = anchorId;
                    recentAdvice = DialingBlocks::buildRecentAdviceBlock(db, in);
                }
            }

            // [barista-fork] FULL-HISTORY AWARENESS: the true extent of the local shot DB (which holds
            // EVERY shot), as aggregates only — so the assistant knows their whole history and never
            // claims it "only has recent shots". Not every shot (token cost); the dial-in blocks above
            // already carry the detailed recent + best-shot data.
            {
                QSqlQuery q(db);
                if (q.exec("SELECT COUNT(*), MIN(timestamp), MAX(timestamp) FROM shots") && q.next()) {
                    const int total = q.value(0).toInt();
                    fullHistory["totalShots"] = total;
                    if (total > 0) {
                        fullHistory["earliest"] = QDateTime::fromSecsSinceEpoch(q.value(1).toLongLong()).toString(QStringLiteral("yyyy-MM-dd"));
                        fullHistory["latest"]   = QDateTime::fromSecsSinceEpoch(q.value(2).toLongLong()).toString(QStringLiteral("yyyy-MM-dd"));
                    }
                }
                QSqlQuery bq(db);
                if (bq.exec("SELECT bean_brand, bean_type, COUNT(*) c, MIN(timestamp) mn, MAX(timestamp) mx "
                            "FROM shots GROUP BY bean_brand, bean_type ORDER BY c DESC LIMIT 12")) {
                    QJsonArray beans;
                    while (bq.next()) {
                        QJsonObject b;
                        b["brand"]     = bq.value(0).toString();
                        b["type"]      = bq.value(1).toString();
                        b["shots"]     = bq.value(2).toInt();
                        b["firstDate"] = QDateTime::fromSecsSinceEpoch(bq.value(3).toLongLong()).toString(QStringLiteral("yyyy-MM-dd"));
                        b["lastDate"]  = QDateTime::fromSecsSinceEpoch(bq.value(4).toLongLong()).toString(QStringLiteral("yyyy-MM-dd"));
                        beans.append(b);
                    }
                    if (!beans.isEmpty())
                        fullHistory["beans"] = beans;
                }
            }
        });

        // [barista-fork] Proactive retrieval (advisor refinement #3): fold the user's recent VERBAL tasting
        // feedback on the CURRENT bean into the context block EVERY turn — far more reliable than depending on
        // the model to call search_tasting_feedback. This reads assistant.db (a DIFFERENT DB from shots.db), so
        // it opens its OWN withTempDb; the read shares FeedbackStorage::fetchFeedbackForBeanStatic with the
        // async request path + the search tool so all three read paths use one query.
        if (!feedbackDbPath.isEmpty() && (!beanBrand.isEmpty() || !beanType.isEmpty())) {
            withTempDb(feedbackDbPath, "barista_feedback_ctx", [&](QSqlDatabase& db) {
                const QVariantList rows = FeedbackStorage::fetchFeedbackForBeanStatic(
                    db, beanBrand, beanType, QString(), 5);
                for (const QVariant& r : rows) {
                    const QVariantMap m = r.toMap();
                    QJsonObject o;
                    o["date"] = QDateTime::fromSecsSinceEpoch(m.value("createdAt").toLongLong())
                                    .toString(QStringLiteral("yyyy-MM-dd"));
                    if (const int rating = m.value("rating0to100").toInt(); rating > 0)
                        o["rating0to100"] = rating;
                    if (const QString raw = m.value("rawText").toString(); !raw.isEmpty())
                        o["saidAboutTaste"] = raw.left(240);
                    if (const QString desc = m.value("descriptors").toString(); !desc.isEmpty())
                        o["descriptors"] = desc;
                    const QJsonObject sj = QJsonDocument::fromJson(
                        m.value("structuredJson").toString().toUtf8()).object();
                    if (sj.contains(QStringLiteral("suggested_adjustment")))
                        o["suggestedAdjustment"] = sj.value(QStringLiteral("suggested_adjustment"));
                    beanFeedback.append(o);
                }
            });
        }

        // [barista-fork] DUE ITEMS — reminders + maintenance that are due NOW. Read UNCONDITIONALLY (these are
        // shot-independent and bean-independent: a brand-new user with zero shots can still have set a reminder),
        // and folded into the context even when there are no shots (the no-shot early-return path below also
        // carries dueItems). Uses TasksStorage static readers so the query is the same as the async/tool path.
        if (!feedbackDbPath.isEmpty()) {
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            withTempDb(feedbackDbPath, "barista_dueitems_ctx", [&](QSqlDatabase& db) {
                TasksStorage::ensureSchemaStatic(db);   // idempotent; seeds the editable-default maintenance schedule
                QJsonArray reminders;
                for (const QVariant& r : TasksStorage::fetchDueRemindersStatic(db, now, 5)) {
                    const QVariantMap m = r.toMap();
                    QJsonObject o;
                    o["reminderId"] = m.value("id").toLongLong();
                    o["text"]       = m.value("text").toString();
                    o["due"]        = QDateTime::fromSecsSinceEpoch(m.value("dueAt").toLongLong())
                                          .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                    if (const QString phr = m.value("userPhrasing").toString(); !phr.isEmpty())
                        o["userPhrasing"] = phr;
                    if (const QString rec = m.value("recurrence").toString(); !rec.isEmpty())
                        o["recurrence"] = rec;
                    reminders.append(o);
                }
                QJsonArray maintenance;
                for (const QVariant& r : TasksStorage::fetchDueMaintenanceStatic(db, now, 5)) {
                    const QVariantMap m = r.toMap();
                    QJsonObject o;
                    o["taskKey"]      = m.value("taskKey").toString();
                    o["label"]        = m.value("label").toString();
                    o["intervalDays"] = m.value("intervalDays").toInt();
                    if (m.contains("overdueDays"))
                        o["overdueDays"] = m.value("overdueDays").toInt();
                    else
                        o["neverDone"] = true;
                    maintenance.append(o);
                }
                if (!reminders.isEmpty())
                    dueItems["reminders"] = reminders;
                if (!maintenance.isEmpty()) {
                    dueItems["maintenance"] = maintenance;
                    // Intervals follow Decent's DE1 Quickstart cleaning guide but stay user-adjustable.
                    dueItems["maintenanceNote"] = QStringLiteral(
                        "These maintenance intervals follow Decent's DE1 cleaning guide but are user-adjustable "
                        "defaults. Raise a due item as a gentle suggestion the user can confirm/adjust in settings. "
                        "For descaling, defer to the user's water (it is TDS-dependent) rather than asserting a fixed interval.");
                }

                // [barista-fork] TODAY'S OCCASION — a built-in US holiday and/or the owner's own personal dates
                // for TODAY, so the barista can warmly acknowledge it ONCE in its greeting or goodbye (never a
                // separate proactive item — see the persona rule). Computed on the DB thread beside dueItems so
                // the personal_dates read (a DIFFERENT table, same assistant.db) stays off the main thread and
                // rides the same first-reply turn including the no-shot early-return path.
                const QDate today = QDate::currentDate();
                if (const QString holiday = usHolidayForDate(today); !holiday.isEmpty())
                    occasion[QStringLiteral("holiday")] = holiday;
                QJsonArray personalDates;
                for (const QVariant& r : TasksStorage::fetchPersonalDatesForTodayStatic(
                         db, today.month(), today.day(), today.year())) {
                    const QVariantMap m = r.toMap();
                    QJsonObject o;
                    o[QStringLiteral("label")] = m.value(QStringLiteral("label")).toString();
                    personalDates.append(o);
                }
                if (!personalDates.isEmpty())
                    occasion[QStringLiteral("personalDates")] = personalDates;
                if (!occasion.isEmpty())
                    occasion[QStringLiteral("note")] = QStringLiteral(
                        "Today is a recognized occasion. If — and only if — a warm greeting or goodbye is natural "
                        "this turn (see the recency/greeting rules), acknowledge it ONCE, briefly and genuinely "
                        "('Happy Thanksgiving!', 'Happy anniversary!'). It is part of the hello or sign-off, NOT a "
                        "separate proactive item, and never mid-conversation — do not force it or repeat it.");

                // [barista-fork] KNOWN FACTS — durable basic facts the user told the barista (remember_fact),
                // scoped to the active user (+ unattributed). Read UNCONDITIONALLY like dueItems (shot-independent)
                // and capped at 30 so the injected prompt can't grow without bound. Injected as background
                // continuity the barista already knows — it should weave them in, not recite or re-ask them.
                for (const QVariant& r : TasksStorage::fetchUserFactsStatic(db, activeUser, 30)) {
                    const QVariantMap m = r.toMap();
                    QJsonObject o;
                    o[QStringLiteral("fact")] = m.value(QStringLiteral("fact")).toString();
                    const QString cat = m.value(QStringLiteral("category")).toString();
                    if (!cat.isEmpty())
                        o[QStringLiteral("category")] = cat;
                    knownFacts.append(o);
                }

                // [barista-fork] MAINTENANCE-DOC CHANGE: the periodic Decent cleaning-guide check
                // (MaintenanceDocSync) sets reviewed=0 when it detects a change the owner hasn't seen yet.
                // Fold in the fetched doc text PLUS the CURRENT still-default schedule (is_default=1 rows only —
                // owner-overridden rows are excluded so we never propose to overwrite them), so the model can
                // OFFER specific default-interval deltas ("backflush 7→5") rather than a vague "they changed
                // something". This is rare and one-time; it competes for the single proactive slot (see the
                // persona priority note echoed here). Read only when there is something pending.
                const QVariantMap docState = TasksStorage::fetchDocStateStatic(db);
                if (!docState.value(QStringLiteral("reviewed"), true).toBool()) {
                    const QString docText = docState.value(QStringLiteral("docText")).toString();
                    if (!docText.isEmpty()) {
                        docChange[QStringLiteral("changedGuideText")] = docText.left(4000);
                        // The tasks still on their seeded default — the only rows update_maintenance_default can
                        // touch. Owner-overridden tasks (is_default=0) are deliberately omitted.
                        QJsonArray defaults;
                        QSqlQuery dq(db);
                        if (dq.exec("SELECT task_key, label, interval_days FROM maintenance_tasks "
                                    "WHERE is_default = 1 ORDER BY label ASC")) {
                            while (dq.next()) {
                                QJsonObject d;
                                d[QStringLiteral("taskKey")]      = dq.value(0).toString();
                                d[QStringLiteral("label")]        = dq.value(1).toString();
                                d[QStringLiteral("intervalDays")] = dq.value(2).toInt();
                                defaults.append(d);
                            }
                        }
                        if (!defaults.isEmpty())
                            docChange[QStringLiteral("currentDefaultSchedule")] = defaults;
                        docChange[QStringLiteral("note")] = QStringLiteral(
                            "Decent's DE1 cleaning guide has changed since it was last acknowledged. In your "
                            "FIRST reply (after answering the user), you MAY offer ONE thing: mention the update "
                            "and OFFER specific default-interval changes you can infer by comparing changedGuideText "
                            "to currentDefaultSchedule (e.g. 'they now suggest backflushing every 5 days instead of "
                            "7 — want me to update that?'). It is an OFFER with an easy spoken 'no thanks', never "
                            "auto-applied. On a yes, call update_maintenance_default per accepted task (it touches "
                            "ONLY still-default tasks; owner-customised ones are left alone). On a no — or once "
                            "you've applied the accepted ones — call dismiss_maintenance_doc_change so it isn't "
                            "re-offered. This shares the ONE-proactive-thing-per-turn budget but YIELDS to a due "
                            "reminder/maintenance item (raise it only when nothing is due this turn); it still "
                            "outranks a mere recipe tweak. For descaling, still defer to the user's water — don't "
                            "assert a fixed interval.");
                    }
                }
            });
        }

        // [barista-fork] Recipes 2.0 proactive block (off-main DB read). Shot-independent like dueItems, so it
        // rides every first-reply turn including the no-shot paths below.
        QString recipesBlock;
        {
            QVector<InventoryRecipe> inv;
            withTempDb(dbPath, "barista_ctx_recipes", [&](QSqlDatabase& db) {
                inv = RecipeStorage::loadInventoryStatic(db, /*archived=*/false);
            });
            recipesBlock = formatRecipesBlock(inv, activeRecipeId);
        }

        // [barista-fork] Phase 1 identity: the [Who] block (roster loaded off-main from the shot DB).
        QString whoBlock;
        {
            QVector<Barista> roster;
            withTempDb(dbPath, "barista_ctx_who", [&](QSqlDatabase& db) {
                roster = BaristaStorage::loadRosterStatic(db);
            });
            whoBlock = formatWhoBlock(activeUser, roster);
        }

        QMetaObject::invokeMethod(qApp, [self, serial, shot, anchorId, beanFilterMissed,
                                         beanBrand, beanType, profileName, beanFeedback, dueItems, docChange,
                                         occasion, knownFacts, dialInSessions, bestRecentShot, beanBestShot, grinderContext,
                                         grinderCalibration, recentAdvice, fullHistory, recipesBlock, whoBlock]() {
            if (!self || serial != self->m_baristaContextSerial)
                return;   // stale — a newer request superseded this one
            self->m_lastBaristaAnchorId = (anchorId > 0 && shot.isValid()) ? anchorId : 0;

            // [barista-fork] Build the write-tool provenance snapshot (advisor blocker #2). bean/type/profile
            // come from the CURRENT request args — NOT the anchor shot's bean, because when beanFilterMissed the
            // anchor is the latest shot OVERALL (a different bean) and stamping its bean would mislabel feedback.
            // The dial (shot_id + dose/yield/grind/temp) is only trustworthy when the anchor actually matched
            // this bean; otherwise shot_id=0 (a bean-general note) and the dial is left empty.
            {
                QVariantMap snap;
                snap["beanBrand"] = beanBrand;
                snap["beanType"]  = beanType;
                snap["profile"]   = profileName;
                snap["source"]    = QStringLiteral("volunteered");
                if (!beanFilterMissed && anchorId > 0 && shot.isValid()) {
                    snap["shotId"] = anchorId;
                    if (shot.doseWeightG > 0)          snap["doseG"]  = shot.doseWeightG;
                    if (shot.finalWeightG > 0)         snap["yieldG"] = shot.finalWeightG;
                    if (!shot.grinderSetting.isEmpty()) snap["grind"] = shot.grinderSetting;
                    if (shot.temperatureOverrideC > 0)  snap["tempC"] = shot.temperatureOverrideC;
                    // [barista-fork] A VERBAL rating/taste lands on the SHOT record (enjoyment + a "Tasted sour"
                    // marker) via ShotHistoryStorage::requestApplyTasteToShot, which reads the shot's notes LIVE
                    // on the DB thread — so we deliberately do NOT snapshot notes here (a session-stale snapshot
                    // could clobber notes the user typed after the barista opened).
                } else {
                    snap["shotId"] = 0;   // bean-general note (no matching shot for this exact bean)
                }
                self->m_lastBaristaAnchorSnapshot = snap;
            }

            // [barista-fork] Due reminders/maintenance are shot-independent, so they must reach the prompt even
            // when the user has NO shots (a brand-new user who set a reminder). Format once as a suffix appended
            // to whichever block we emit — including the "recordedShots: 0" early-return paths below.
            QString dueSuffix;
            if (!dueItems.isEmpty())
                dueSuffix = QStringLiteral("\n\n## Due now (reminders & maintenance the user set up):\n")
                          + QString::fromUtf8(QJsonDocument(dueItems).toJson(QJsonDocument::Indented));

            // [barista-fork] A pending Decent cleaning-guide change to OFFER — shot-independent like dueItems,
            // so it rides the same first-reply turn including the no-shot paths. It YIELDS to a due
            // reminder/maintenance item: only surface the doc offer when nothing is due this turn, so a
            // rare one-off note can never starve an overdue (possibly safety-relevant) reminder. Enforced
            // here in code, not just in the persona.
            QString docSuffix;
            if (!docChange.isEmpty() && dueItems.isEmpty())
                docSuffix = QStringLiteral("\n\n## maintenanceDocChanged (Decent's cleaning guide changed — offer an update):\n")
                          + QString::fromUtf8(QJsonDocument(docChange).toJson(QJsonDocument::Indented));

            // [barista-fork] Today's occasion (holiday + personal dates) — shot-independent like dueItems, so it
            // rides every first-reply turn including the no-shot paths. It is greeting/goodbye flavor, NOT a
            // proactive item, so it never competes with (or yields to) dueItems/docChange — always included.
            QString occasionSuffix;
            if (!occasion.isEmpty())
                occasionSuffix = QStringLiteral("\n\n## todaysOccasion (acknowledge once in your greeting or goodbye, only if natural):\n")
                               + QString::fromUtf8(QJsonDocument(occasion).toJson(QJsonDocument::Indented));

            // [barista-fork] knownFacts — durable basic facts remembered about this user; background continuity,
            // NOT a proactive item. Shot-independent, so it rides every path including the no-shot early returns.
            QString factsSuffix;
            if (!knownFacts.isEmpty()) {
                QJsonObject facts;
                facts[QStringLiteral("facts")] = knownFacts;
                facts[QStringLiteral("note")] = QStringLiteral(
                    "Facts you've remembered about this user from past conversations — treat as background you "
                    "already know. Weave them in naturally when relevant; do NOT recite the list, and do NOT re-ask "
                    "what's already here. Use remember_fact to add a new durable fact, forget_fact to correct one.");
                if (knownFacts.size() >= 30)
                    facts[QStringLiteral("truncatedNote")] = QStringLiteral(
                        "Showing the 30 most recent facts; older ones may be omitted.");
                factsSuffix = QStringLiteral("\n\n## knownFacts (things you remember about this user):\n")
                            + QString::fromUtf8(QJsonDocument(facts).toJson(QJsonDocument::Indented));
            }

            if (anchorId <= 0 || !shot.isValid()) {
                emit self->baristaContextReady(QStringLiteral("recordedShots: 0") + dueSuffix + docSuffix + occasionSuffix + factsSuffix + recipesBlock + whoBlock);
                return;
            }
            QJsonObject obj = self->buildUserPromptObjectForShot(shot);
            if (obj.isEmpty()) {
                emit self->baristaContextReady(QStringLiteral("recordedShots: 0") + dueSuffix + docSuffix + occasionSuffix + factsSuffix + recipesBlock + whoBlock);
                return;
            }
            self->enrichUserPromptObject(obj, shot, dialInSessions, bestRecentShot, grinderContext,
                                         recentAdvice, grinderCalibration, beanBestShot);
            // [barista-fork] Speakable descriptor for the most-recent shot so the barista leads with
            // "that lungo espresso on the Ethiopia beans" instead of reading back the scalar analysis.
            // The numbers stay in the payload for the barista's own reasoning; the persona says not to
            // recite them. (Barista-only path — enrichUserPromptObject is shared with MCP, so this rides
            // at the call site, not in the shared builder.)
            if (shot.doseWeightG > 0 && shot.finalWeightG > 0)
                obj.insert(QStringLiteral("descriptor"), DrinkTypes::espressoShotDescriptor(
                    shot.finalWeightG / shot.doseWeightG, shot.beanBrand, shot.beanType));
            // Same for the two secondary anchor shots the barista may reference (best-recent
            // and bean-best) so NONE of the shots it sees is a bare stat bag. Both blocks carry
            // ratio + beanBrand + beanType (dialing_blocks.cpp).
            auto addShotDescriptor = [](QJsonObject& parent, const QString& key) {
                if (!parent.contains(key)) return;
                QJsonObject s = parent.value(key).toObject();
                const double r = s.value(QStringLiteral("ratio")).toDouble();
                if (r > 0 && !s.contains(QStringLiteral("descriptor"))) {
                    s.insert(QStringLiteral("descriptor"), DrinkTypes::espressoShotDescriptor(
                        r, s.value(QStringLiteral("beanBrand")).toString().trimmed(),
                        s.value(QStringLiteral("beanType")).toString().trimmed()));
                    parent[key] = s;
                }
            };
            addShotDescriptor(obj, QStringLiteral("bestRecentShot"));
            addShotDescriptor(obj, QStringLiteral("beanBestShot"));
            if (!fullHistory.isEmpty())
                obj.insert(QStringLiteral("fullHistory"), fullHistory);
            // [barista-fork] Proactive verbal-feedback retrieval: the user's own past words about how this
            // bean tasted, so the barista closes the loop ("you called this sour twice") without a tool call.
            if (!beanFeedback.isEmpty())
                obj.insert(QStringLiteral("recentTastingFeedbackOnThisBean"), beanFeedback);
            // [barista-fork] PROACTIVE-REC INPUTS: bean age + freshness read + storage state for the ACTIVE
            // bag (from live SettingsDye — safe here, this callback runs on the main thread). Lets the barista
            // decide whether to OFFER one recipe tweak on its first reply (grind finer as a bag ages, re-dial
            // a just-thawed bag) — always as an offer, never auto-applied. Complements beanBestShot +
            // recentTastingFeedbackOnThisBean (the shot-outcome half); omitted when there's nothing to say.
            if (self->m_settings && self->m_settings->dye()) {
                const QJsonObject rec = buildProactiveRecBlock(self->m_settings->dye());
                if (!rec.isEmpty())
                    obj.insert(QStringLiteral("proactiveRecInputs"), rec);
            }
            QString block;
            if (beanFilterMissed)
                block += QStringLiteral("NOTE: No shots recorded under the exact current bean name — "
                                        "the data below is the user's recent shots overall.\n\n");
            block += QStringLiteral("## The app's structured data on this user and their coffee "
                                    "(this is real — you DO have their history):\n");
            block += QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented));
            block += dueSuffix;   // [barista-fork] due reminders/maintenance ride the same first-reply turn
            block += docSuffix;   // [barista-fork] a pending Decent cleaning-guide change to offer (rare, one-time)
            block += occasionSuffix;   // [barista-fork] today's holiday/personal dates for a warm greeting/goodbye
            block += factsSuffix;      // [barista-fork] durable basic facts remembered about this user
            block += recipesBlock;     // [barista-fork] Recipes 2.0 proactive block (active recipe + MRU list)
            block += whoBlock;         // [barista-fork] Phase 1 identity: who's here + honest-attribution note
            emit self->baristaContextReady(block);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void AIManager::testConnection()
{
    AIProvider* provider = currentProvider();
    if (!provider) {
        m_lastTestResult = tr_("ai.error.noProviderSelected", "No AI provider selected");
        m_lastTestSuccess = false;
        emit testResultChanged();
        return;
    }

    provider->testConnection();
}

void AIManager::analyze(const QString& systemPrompt, const QString& userPrompt)
{
    if (m_analyzing) {
        m_lastError = tr_("ai.error.analysisInProgress", "Analysis already in progress");
        emit errorOccurred(m_lastError);
        return;
    }

    AIProvider* provider = currentProvider();
    if (!provider) {
        m_lastError = tr_("ai.error.noProviderConfigured", "No AI provider configured");
        emit errorOccurred(m_lastError);
        return;
    }

    if (!isConfigured()) {
        m_lastError = tr_("ai.error.providerNotConfigured", "AI provider not configured");
        emit errorOccurred(m_lastError);
        return;
    }

    m_analyzing = true;
    m_isConversationRequest = false;
    m_isBagExtractionRequest = false;
    m_isCoachPhrasebookRequest = false;   // [barista-fork]
    emit analyzingChanged();

    // Store for logging
    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = userPrompt;

    logPrompt(selectedProvider(), systemPrompt, userPrompt);
    provider->analyze(systemPrompt, userPrompt);
}

// [barista-fork] Live-coaching phrasebook: ONE bracketing AI call → strict JSON of varied cue phrasings +
// a gameplan. Mirrors extractCoffeeBagDetails (own flag+token; routed in onAnalysisComplete/Failed). Fails
// fast (never blocks a shot) — the coaches fall back to their deterministic lines until a pool lands.
void AIManager::requestCoachPhrasebook(const QString& requestToken, const QString& contextBlock)
{
    if (m_analyzing) { emit phrasebookFailed(requestToken, QStringLiteral("busy")); return; }
    AIProvider* provider = currentProvider();
    if (!provider || !isConfigured()) { emit phrasebookFailed(requestToken, QStringLiteral("notConfigured")); return; }

    const QString systemPrompt = QStringLiteral(
        "You write short spoken coaching cues for a home espresso machine's voice coach. Return STRICT JSON "
        "ONLY (no prose, no markdown), shape: {\"cues\": {\"<id>\": [\"line\", ...]}, \"gameplan\": \"...\"}. "
        "For EACH of these ids give 4-5 DISTINCT natural spoken variants (warm, brief, a coach beside them — "
        "never robotic): no-puck, channeling, flow-fast, flow-slow, steam-stretch, steam-roll, steam-almost, "
        "steam-done, no-coaching. Each line <=110 characters, plain spoken words, NO placeholders/%/{}, NO "
        "numbers unless natural. "
        "PROFESSIONAL AND CONCRETE — this is a barista instructor, fully professional: every line NAMES the "
        "specific equipment and action — the steam wand, the wand tip, the milk, the pitcher, the puck, the "
        "flow, the shot — in plain technical barista terms. Warm but unambiguous. NEVER use vague, open-ended, "
        "or double-meaning phrasing that could be misread out of context: no bare 'almost there', 'a little "
        "longer', 'keep going', 'nice and slow', 'hold it', 'go deeper', or 'get ready' without stating get "
        "ready TO WHAT. Always say WHAT to do to WHICH part (e.g. 'ease the wand deeper to spin the milk', not "
        "'go deeper'; 'steam's nearly done, ready to shut it off', not 'almost there'). "
        "'gameplan' = ONE <=2-sentence pre-shot plan grounded in the bean/history below. "
        "Match the app's language. Output the JSON object and nothing else.");

    m_analyzing = true;
    m_isConversationRequest = false;
    m_isBagExtractionRequest = false;
    m_isCoachPhrasebookRequest = true;
    m_coachPhrasebookToken = requestToken;
    emit analyzingChanged();
    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = QStringLiteral("[coach phrasebook]");
    logPrompt(selectedProvider(), systemPrompt, m_lastUserPrompt);
    provider->analyze(systemPrompt, contextBlock);
}

void AIManager::extractCoffeeBagDetails(const QString& requestToken, const QString& pageText,
                                        const QString& kind)
{
    if (m_analyzing) {
        emit bagDetailsExtractionFailed(requestToken, QStringLiteral("busy"));
        return;
    }
    AIProvider* provider = currentProvider();
    if (!provider || !isConfigured()) {
        emit bagDetailsExtractionFailed(requestToken, QStringLiteral("notConfigured"));
        return;
    }

    // Extraction contract mirrors Visualizer's "Get info": page text in, a
    // flat JSON object of only-what-the-page-states out. Keys = the blob
    // vocabulary so the caller can merge without remapping.
    static const QString kCoffeePrompt = QStringLiteral(
        "You extract coffee bag details from the plain text of a roaster's product page. "
        "Reply with ONLY a JSON object - no markdown, no commentary. Use exactly these keys, "
        "omitting any the page does not clearly state: origin (country), region, farm, "
        "producer (person or company that grew it), variety, elevation (display string, e.g. "
        "\"1900-2100 m\"), process (e.g. \"Washed\", \"Natural\"), harvest (e.g. \"Late 2025\"), "
        "roastLevel (one of: Light, Medium-Light, Medium, Medium-Dark, Dark - map the page's "
        "wording), tastingNotes (comma-separated flavor descriptors from the page). "
        "Never guess or infer a value the text does not state. For blends without a stated "
        "origin, leave origin out and describe the blend in variety if stated.");
    // Tea vocabulary (add-recipe-wizard-tea): descriptive keys renamed for
    // tea (garden/cultivar/flush) plus STRUCTURED brewing numbers the recipe
    // wizard seeds from — brewTempC must be Celsius (the model converts °F
    // and boiling-water wordings), leafGramsPer100Ml must be normalized from
    // per-cup dosing (1 cup = 237 ml unless the page defines one).
    static const QString kTeaPrompt = QStringLiteral(
        "You extract loose-leaf tea details from the plain text of a tea vendor's product page. "
        "Reply with ONLY a JSON object - no markdown, no commentary. Use exactly these keys, "
        "omitting any the page does not clearly state: teaType (one of: black, green, oolong, "
        "white, herbal, pu-erh - map the page's wording; a tisane or infusion is herbal), "
        "origin (country), region, garden (the estate or garden name), cultivar, flush (the "
        "harvest or flush, e.g. \"First flush 2026\", \"Spring 2026\"), tastingNotes "
        "(comma-separated flavor descriptors from the page), brewTempC (NUMBER, Celsius - "
        "convert Fahrenheit, e.g. 212 -> 100; \"boiling\" or \"freshly-boiled\" -> 100), "
        "leafGramsPer100Ml (NUMBER - normalize the stated leaf dose to grams per 100 ml of "
        "water; treat one cup as 237 ml unless the page defines a cup), steepTime (display "
        "string, e.g. \"3-5 minutes\"). "
        "Never guess or infer a value the text does not state - in particular, never invent "
        "brewing numbers the page does not give.");

    const QString& systemPrompt =
        (kind == QLatin1String("tea")) ? kTeaPrompt : kCoffeePrompt;

    m_analyzing = true;
    m_isConversationRequest = false;
    m_isBagExtractionRequest = true;
    m_bagExtractionToken = requestToken;
    emit analyzingChanged();

    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = QStringLiteral("[Bag page text from %1, %2 chars]")
                           .arg(requestToken).arg(pageText.size());
    logPrompt(selectedProvider(), systemPrompt, m_lastUserPrompt);
    provider->analyze(systemPrompt, pageText);
}

bool AIManager::supportsUrlExtraction() const
{
    AIProvider* provider = const_cast<AIManager*>(this)->currentProvider();
    return provider && provider->supportsUrlAnalysis();
}

void AIManager::extractCoffeeBagDetailsFromUrl(const QString& requestToken, const QString& url,
                                               const QString& kind)
{
    // Stage-2 extraction (add-recipe-wizard-tea): the local page fetch got
    // nothing (JS-rendered shop), so the PROVIDER fetches the URL itself via
    // its server-side web-fetch tool. Same JSON contract as stage 1 plus one
    // extra key: imageUrl (the main product photo) — SPA pages have no
    // og:image for the normal photo pipeline to find.
    if (m_analyzing) {
        emit bagDetailsExtractionFailed(requestToken, QStringLiteral("busy"));
        return;
    }
    AIProvider* provider = currentProvider();
    if (!provider || !isConfigured()) {
        emit bagDetailsExtractionFailed(requestToken, QStringLiteral("notConfigured"));
        return;
    }
    if (!provider->supportsUrlAnalysis()) {
        emit bagDetailsExtractionFailed(requestToken, QStringLiteral("urlFetchUnsupported"));
        return;
    }

    // Re-run extractCoffeeBagDetails' prompt selection with the stage-2
    // addendum. The prompts are function-local statics there; keep this
    // addendum in sync with the keys documented on parseBagExtraction.
    const QString base = (kind == QLatin1String("tea"))
        ? QStringLiteral(
            "You extract loose-leaf tea details from a tea vendor's product page. "
            "Reply with ONLY a JSON object - no markdown, no commentary. Use exactly these keys, "
            "omitting any the page does not clearly state: teaType (one of: black, green, oolong, "
            "white, herbal, pu-erh - map the page's wording; a tisane or infusion is herbal), "
            "origin (country), region, garden (the estate or garden name), cultivar, flush (the "
            "harvest or flush, e.g. \"First flush 2026\", \"Spring 2026\"), tastingNotes "
            "(comma-separated flavor descriptors from the page), brewTempC (NUMBER, Celsius - "
            "convert Fahrenheit, e.g. 212 -> 100; \"boiling\" or \"freshly-boiled\" -> 100), "
            "leafGramsPer100Ml (NUMBER - normalize the stated leaf dose to grams per 100 ml of "
            "water; treat one cup as 237 ml unless the page defines a cup), steepTime (display "
            "string, e.g. \"3-5 minutes\"). "
            "Never guess or infer a value the text does not state - in particular, never invent "
            "brewing numbers the page does not give.")
        : QStringLiteral(
            "You extract coffee bag details from a roaster's product page. "
            "Reply with ONLY a JSON object - no markdown, no commentary. Use exactly these keys, "
            "omitting any the page does not clearly state: origin (country), region, farm, "
            "producer (person or company that grew it), variety, elevation (display string, e.g. "
            "\"1900-2100 m\"), process (e.g. \"Washed\", \"Natural\"), harvest (e.g. \"Late 2025\"), "
            "roastLevel (one of: Light, Medium-Light, Medium, Medium-Dark, Dark - map the page's "
            "wording), tastingNotes (comma-separated flavor descriptors from the page). "
            "Never guess or infer a value the text does not state. For blends without a stated "
            "origin, leave origin out and describe the blend in variety if stated.");
    const QString systemPrompt = base + QStringLiteral(
        " Retrieve the product page URL in the user message yourself with your web tool first. "
        "Additionally include the key imageUrl (the MAIN product photo's absolute URL from the "
        "fetched page, when one is shown - an image file URL, never the page URL itself, and "
        "never a logo or banner).");
    const QString userPrompt = QStringLiteral(
        "Fetch this product page and extract the details: %1").arg(url);

    m_analyzing = true;
    m_isConversationRequest = false;
    m_isBagExtractionRequest = true;
    m_bagExtractionToken = requestToken;
    emit analyzingChanged();

    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = userPrompt;
    logPrompt(selectedProvider(), systemPrompt, userPrompt);
    provider->analyzeUrl(systemPrompt, userPrompt);
}

// static
QVariantMap AIManager::parseBagExtraction(const QString& response, bool* ok)
{
    if (ok)
        *ok = false;
    // Tolerate markdown fences / prose around the object: parse the first
    // '{' .. last '}' span.
    const qsizetype start = response.indexOf(QLatin1Char('{'));
    const qsizetype end = response.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end <= start)
        return {};
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(
        response.mid(start, end - start + 1).toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return {};

    static const QStringList kKeys{
        QStringLiteral("origin"), QStringLiteral("region"), QStringLiteral("farm"),
        QStringLiteral("producer"), QStringLiteral("variety"), QStringLiteral("elevation"),
        QStringLiteral("process"), QStringLiteral("harvest"), QStringLiteral("roastLevel"),
        QStringLiteral("tastingNotes"),
        // Tea vocabulary (add-recipe-wizard-tea). Union whitelist: the prompt
        // selected by bag kind controls which keys come back; this filter just
        // has to let both vocabularies through.
        QStringLiteral("teaType"), QStringLiteral("garden"), QStringLiteral("cultivar"),
        QStringLiteral("flush"), QStringLiteral("brewTempC"),
        QStringLiteral("leafGramsPer100Ml"), QStringLiteral("steepTime"),
        // Stage-2 only (extractCoffeeBagDetailsFromUrl): the product photo's
        // URL, consumed by the image cache — never a form field.
        QStringLiteral("imageUrl")};
    const QJsonObject obj = doc.object();
    QVariantMap fields;
    for (const QString& key : kKeys) {
        const QJsonValue raw = obj.value(key);
        QString value;
        if (raw.isArray()) {
            // Models frequently return tasting notes as an array despite the
            // prompt — join the scalar elements rather than dropping them.
            QStringList parts;
            const QJsonArray arr = raw.toArray();
            for (const QJsonValue& v : arr) {
                const QString part = v.toVariant().toString().trimmed();
                if (!part.isEmpty())
                    parts << part;
            }
            value = parts.join(QStringLiteral(", "));
        } else if (raw.isObject()) {
            qWarning() << "AIManager: bag extraction returned an object for" << key << "- skipped";
        } else {
            value = raw.toVariant().toString().trimmed();
        }
        // Cap per value: a prompt-injected page must not push multi-KB text
        // through a form field into the DB blob.
        if (!value.isEmpty())
            fields.insert(key, value.left(500));
    }
    // A non-empty object that yielded nothing usable is a response we could
    // not read, NOT an honest "the page states nothing" ({} stays a success).
    if (fields.isEmpty() && !obj.isEmpty())
        return {};
    if (ok)
        *ok = true;
    return fields;
}

void AIManager::analyzeConversation(const QString& systemPrompt, const QJsonArray& messages,
                                    bool webSearch, bool clientTools)
{
    if (m_analyzing) {
        emit conversationErrorOccurred(tr_("ai.error.analysisInProgress", "Analysis already in progress"));
        return;
    }

    AIProvider* provider = currentProvider();
    if (!provider) {
        m_lastError = tr_("ai.error.noProviderConfigured", "No AI provider configured");
        emit conversationErrorOccurred(m_lastError);
        return;
    }

    if (!isConfigured()) {
        m_lastError = tr_("ai.error.providerNotConfigured", "AI provider not configured");
        emit conversationErrorOccurred(m_lastError);
        return;
    }

    m_analyzing = true;
    m_isConversationRequest = true;
    m_isBagExtractionRequest = false;
    m_isCoachPhrasebookRequest = false;   // [barista-fork]
    // [barista-fork] Closed-loop safety net (issue #1053 regression): clear any stale tool-applied structuredNext
    // at the single choke point every conversation turn passes through, so a prior turn's apply_dial_change capture
    // (e.g. one whose turn failed, or that was superseded) can NEVER leak into this turn's finalization. Unconditional
    // — a no-op for the advisor path (no client tools) and for turns that don't call the write tool.
    m_pendingToolStructuredNext = QJsonObject{};
    emit analyzingChanged();

    // Strip internal-only per-turn keys (shotId / structuredNext) that providers reject as extra inputs
    // (Anthropic 400: "messages.N.shotId: Extra inputs are not permitted"). Whitelist role+content only.
    // (Upstream #1545 factored this into sanitizeApiMessages(); the fork's old inline copy is retired here.)
    const QJsonArray apiMessages = sanitizeApiMessages(messages);

    // Store for logging — flatten for the log file
    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = QString("[Conversation with %1 messages]").arg(apiMessages.size());

    logPrompt(selectedProvider(), systemPrompt, m_lastUserPrompt);
    // [barista-fork] Interactive conversation turns get a ~30s per-request timeout (vs the 60s deep-analysis
    // default) so a stalled request fails+recovers fast instead of a long freeze. transferTimeout is per-request
    // inactivity, so each tool-round leg gets its own 30s — a healthy leg completes in seconds.
    provider->analyzeConversation(systemPrompt, apiMessages, AIProvider::RequestOptions{webSearch, clientTools, 30000});
}

void AIManager::refreshOllamaModels()
{
    auto* ollama = dynamic_cast<OllamaProvider*>(m_ollamaProvider.get());
    if (ollama) {
        ollama->refreshModels();
    }
}

void AIManager::onAnalysisComplete(const QString& response)
{
    m_analyzing = false;
    m_lastRecommendation = response;
    m_lastError.clear();

    // Log the successful response
    logResponse(selectedProvider(), response, true);

    emit analyzingChanged();

    // Emit to the appropriate listener based on request type
    if (m_isCoachPhrasebookRequest) {   // [barista-fork]
        m_isCoachPhrasebookRequest = false;
        const QString token = m_coachPhrasebookToken;
        m_coachPhrasebookToken.clear();
        emit phrasebookReady(token, response);
    } else if (m_isBagExtractionRequest) {
        m_isBagExtractionRequest = false;
        const QString token = m_bagExtractionToken;
        m_bagExtractionToken.clear();
        bool parsed = false;
        const QVariantMap fields = parseBagExtraction(response, &parsed);
        if (parsed)
            emit bagDetailsExtracted(token, fields);
        else
            emit bagDetailsExtractionFailed(token, QStringLiteral("unreadable"));
    } else if (m_isConversationRequest) {
        emit conversationResponseReceived(response);
    } else {
        emit recommendationReceived(response);
    }
}

void AIManager::onInterimText(const QString& text)
{
    // [barista-fork] Pre-tool lead-in. Route it ONLY for a live conversation turn (mirrors the
    // conversationResponseReceived gating) — a recommendation/extraction turn has no spoken overlay to fill.
    // This is NOT a turn completion: m_analyzing stays true, nothing is finalized; it's a "speak this now" nudge.
    if (m_isConversationRequest)
        emit conversationInterimText(text);
}

void AIManager::onAnalysisFailed(const QString& error)
{
    m_analyzing = false;
    m_lastError = error;

    // Log the failed response
    logResponse(selectedProvider(), error, false);

    emit analyzingChanged();

    // Emit to the appropriate listener based on request type
    if (m_isCoachPhrasebookRequest) {   // [barista-fork]
        m_isCoachPhrasebookRequest = false;
        const QString token = m_coachPhrasebookToken;
        m_coachPhrasebookToken.clear();
        emit phrasebookFailed(token, error);
    } else if (m_isBagExtractionRequest) {
        m_isBagExtractionRequest = false;
        const QString token = m_bagExtractionToken;
        m_bagExtractionToken.clear();
        emit bagDetailsExtractionFailed(token, error);
    } else if (m_isConversationRequest) {
        emit conversationErrorOccurred(error);
    } else {
        emit errorOccurred(error);
    }
}

void AIManager::onTestResult(bool success, const QString& message)
{
    m_lastTestSuccess = success;
    m_lastTestResult = message;
    emit testResultChanged();
}

void AIManager::onOllamaModelsRefreshed(const QStringList& models)
{
    m_ollamaModels = models;
    emit ollamaModelsChanged();
}

void AIManager::onSettingsChanged()
{
    // Update providers with new settings
    auto* openai = dynamic_cast<OpenAIProvider*>(m_openaiProvider.get());
    if (openai) {
        openai->setApiKey(m_settings->ai()->openaiApiKey());
        openai->setModel(m_settings->ai()->providerModel("openai"));  // empty → keeps default
    }

    auto* anthropic = dynamic_cast<AnthropicProvider*>(m_anthropicProvider.get());
    if (anthropic) {
        anthropic->setApiKey(m_settings->ai()->anthropicApiKey());
        anthropic->setModel(m_settings->ai()->providerModel("anthropic"));  // empty → keeps default
    }

    auto* gemini = dynamic_cast<GeminiProvider*>(m_geminiProvider.get());
    if (gemini) {
        gemini->setApiKey(m_settings->ai()->geminiApiKey());
        gemini->setModel(m_settings->ai()->providerModel("gemini"));  // empty → keeps default
    }

    auto* openrouter = dynamic_cast<OpenRouterProvider*>(m_openrouterProvider.get());
    if (openrouter) {
        openrouter->setApiKey(m_settings->ai()->openrouterApiKey());
        openrouter->setModel(m_settings->ai()->openrouterModel());
    }

    auto* ollama = dynamic_cast<OllamaProvider*>(m_ollamaProvider.get());
    if (ollama) {
        ollama->setEndpoint(m_settings->ai()->ollamaEndpoint());
        ollama->setModel(m_settings->ai()->ollamaModel());
    }

    emit configurationChanged();
}

// ============================================================================
// Conversation Routing
// ============================================================================

QJsonObject AIManager::ConversationEntry::toJson() const
{
    QJsonObject obj;
    obj["key"] = key;
    obj["beanBrand"] = beanBrand;
    obj["beanType"] = beanType;
    obj["profileName"] = profileName;
    obj["timestamp"] = timestamp;
    return obj;
}

AIManager::ConversationEntry AIManager::ConversationEntry::fromJson(const QJsonObject& obj)
{
    ConversationEntry entry;
    entry.key = obj["key"].toString();
    entry.beanBrand = obj["beanBrand"].toString();
    entry.beanType = obj["beanType"].toString();
    entry.profileName = obj["profileName"].toString();
    entry.timestamp = obj["timestamp"].toVariant().toLongLong();
    return entry;
}

QString AIManager::conversationKey(const QString& beanBrand, const QString& beanType, const QString& profileName)
{
    QString normalized = beanBrand.toLower().trimmed() + "|" +
                         beanType.toLower().trimmed() + "|" +
                         profileName.toLower().trimmed();
    QByteArray hash = QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha1);
    return hash.toHex().left(16);
}

void AIManager::loadConversationIndex()
{
    QSettings settings;
    QByteArray indexJson = settings.value("ai/conversations/index").toByteArray();
    m_conversationIndex.clear();

    if (!indexJson.isEmpty()) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(indexJson, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            qWarning() << "AIManager::loadConversationIndex: JSON parse error:" << parseError.errorString();
        } else if (doc.isArray()) {
            QJsonArray arr = doc.array();
            for (const QJsonValue& val : arr) {
                ConversationEntry entry = ConversationEntry::fromJson(val.toObject());
                if (entry.key.isEmpty()) {
                    qWarning() << "AIManager::loadConversationIndex: Skipping entry with empty key";
                    continue;
                }
                m_conversationIndex.append(entry);
            }
        }
    }
    qDebug() << "AIManager: Loaded conversation index with" << m_conversationIndex.size() << "entries";
}

void AIManager::saveConversationIndex()
{
    QJsonArray arr;
    for (const auto& entry : m_conversationIndex) {
        arr.append(entry.toJson());
    }
    QSettings settings;
    settings.setValue("ai/conversations/index", QJsonDocument(arr).toJson(QJsonDocument::Compact));
    emit conversationIndexChanged();
}

void AIManager::touchConversationEntry(const QString& key)
{
    qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < m_conversationIndex.size(); i++) {
        if (m_conversationIndex[i].key == key) {
            m_conversationIndex[i].timestamp = now;
            // Move to front (most recent)
            if (i > 0) {
                auto entry = m_conversationIndex.takeAt(i);
                m_conversationIndex.prepend(entry);
            }
            saveConversationIndex();
            return;
        }
    }
}

void AIManager::evictOldestConversation()
{
    if (m_conversationIndex.size() < MAX_CONVERSATIONS) return;

    // Remove the last (oldest) entry
    ConversationEntry oldest = m_conversationIndex.takeLast();

    // Remove its QSettings data
    QSettings settings;
    QString prefix = "ai/conversations/" + oldest.key + "/";
    settings.remove(prefix + "systemPrompt");
    settings.remove(prefix + "messages");
    settings.remove(prefix + "timestamp");

    qDebug() << "AIManager: Evicted oldest conversation:" << oldest.beanBrand << oldest.beanType << oldest.profileName;
    saveConversationIndex();
}

void AIManager::clearAllConversationsOnce(const QString& migrationId)
{
    QSettings settings;
    const QString markerKey = QStringLiteral("ai/migrations/") + migrationId;
    if (settings.value(markerKey).toBool())
        return;

    settings.beginGroup(QStringLiteral("ai/conversations"));
    settings.remove(QString());  // removes all keys in this group
    settings.endGroup();

    settings.setValue(markerKey, true);
    qDebug() << "AIManager: cleared all conversations for migration" << migrationId;
}

void AIManager::migrateFromLegacyConversation()
{
    QSettings settings;

    // Check if legacy data exists and new index doesn't
    QByteArray legacyMessages = settings.value("ai/conversation/messages").toByteArray();
    QByteArray existingIndex = settings.value("ai/conversations/index").toByteArray();

    if (legacyMessages.isEmpty() || !existingIndex.isEmpty()) return;

    QJsonDocument doc = QJsonDocument::fromJson(legacyMessages);
    if (!doc.isArray() || doc.array().isEmpty()) return;

    qDebug() << "AIManager: Migrating legacy conversation to keyed storage";

    // Use a fixed key for the legacy conversation
    QString legacyKey = "_legacy";

    // Copy data to new keyed location
    QString prefix = "ai/conversations/" + legacyKey + "/";
    settings.setValue(prefix + "systemPrompt", settings.value("ai/conversation/systemPrompt"));
    settings.setValue(prefix + "messages", legacyMessages);
    settings.setValue(prefix + "timestamp", settings.value("ai/conversation/timestamp"));

    // Create index entry
    ConversationEntry entry;
    entry.key = legacyKey;
    entry.beanBrand = "";
    entry.beanType = "";
    entry.profileName = "(Previous conversation)";
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    QJsonArray indexArr;
    indexArr.append(entry.toJson());
    settings.setValue("ai/conversations/index", QJsonDocument(indexArr).toJson(QJsonDocument::Compact));

    // Keep legacy keys as recovery fallback — they'll be harmless if left in place
    // settings.remove("ai/conversation/systemPrompt");
    // settings.remove("ai/conversation/messages");
    // settings.remove("ai/conversation/timestamp");

    qDebug() << "AIManager: Legacy conversation migrated to key:" << legacyKey;
}

QString AIManager::switchConversation(const QString& beanBrand, const QString& beanType, const QString& profileName)
{
    QString key = conversationKey(beanBrand, beanType, profileName);

    // Already on this key — just touch LRU
    if (m_conversation->storageKey() == key) {
        touchConversationEntry(key);
        return key;
    }

    // Refuse if busy
    if (m_conversation->isBusy()) {
        qWarning() << "AIManager: Cannot switch conversation while busy";
        return m_conversation->storageKey();
    }

    // Save current conversation if it has history
    if (m_conversation->hasHistory()) {
        m_conversation->saveToStorage();
    }

    // Clear in-memory state without touching QSettings (clearHistory() would delete stored data)
    m_conversation->resetInMemory();

    // Check if key exists in index
    bool exists = false;
    for (const auto& entry : m_conversationIndex) {
        if (entry.key == key) {
            exists = true;
            break;
        }
    }

    // Set new storage key and load whatever is actually on disk for it —
    // regardless of `exists` (m_conversationIndex only tracks conversations
    // the IN-APP flow has touched before; it's never updated by the MCP
    // ai_advisor_invoke path's AIConversation::appendAssistantTurnForKey,
    // which writes turns directly to QSettings). Without this, a key with
    // real MCP-written turns but no in-app history looks empty here,
    // hasHistory() reads false, QML calls ask() instead of followUp(), and
    // the next saveToStorage() silently destroys those turns — the root
    // cause of the persistence gap found in manual verification of
    // fix-multishot-advice-tracking (loadFromStorage is a safe no-op when
    // the key genuinely has nothing on disk).
    m_conversation->setStorageKey(key);
    m_conversation->setContextLabel(beanBrand, beanType, profileName);
    m_conversation->loadFromStorage();

    if (exists) {
        touchConversationEntry(key);
    } else {
        // Evict oldest if at capacity
        evictOldestConversation();

        // Add new entry to front of index
        ConversationEntry newEntry;
        newEntry.key = key;
        newEntry.beanBrand = beanBrand;
        newEntry.beanType = beanType;
        newEntry.profileName = profileName;
        newEntry.timestamp = QDateTime::currentSecsSinceEpoch();
        m_conversationIndex.prepend(newEntry);
        saveConversationIndex();
    }

    emit m_conversation->savedConversationChanged();
    qDebug() << "AIManager: Switched to conversation key:" << key
             << "(" << beanBrand << beanType << "/" << profileName << ")";
    return key;
}

void AIManager::loadMostRecentConversation()
{
    if (m_conversationIndex.isEmpty()) {
        m_conversation->setStorageKey(QString());
        m_conversation->setContextLabel(QString(), QString(), QString());
        return;
    }

    const auto& entry = m_conversationIndex.first();
    m_conversation->setStorageKey(entry.key);
    m_conversation->setContextLabel(entry.beanBrand, entry.beanType, entry.profileName);
    m_conversation->loadFromStorage();
    qDebug() << "AIManager: Loaded most recent conversation:" << entry.key
             << "(" << entry.beanBrand << entry.beanType << "/" << entry.profileName << ")";
}

void AIManager::clearCurrentConversation()
{
    QString key = m_conversation->storageKey();
    m_conversation->clearHistory();

    // Remove the entry from the conversation index
    if (!key.isEmpty()) {
        for (int i = 0; i < m_conversationIndex.size(); i++) {
            if (m_conversationIndex[i].key == key) {
                m_conversationIndex.removeAt(i);
                saveConversationIndex();
                break;
            }
        }
    }
}

bool AIManager::isSupportedBeverageType(const QString& beverageType) const
{
    QString bev = beverageType.toLower().trimmed();
    return bev.isEmpty() || bev == "espresso" || bev == "filter" || bev == "pourover";
}

bool AIManager::isMistakeShot(const QVariant& shotVariant) const
{
    const ShotProjection shotData = coerceShot(shotVariant);
    if (shotData.durationSec < 10.0) return true;
    if (shotData.finalWeightG < 5.0) return true;
    if (shotData.targetWeightG > 0.0 && shotData.finalWeightG < shotData.targetWeightG / 3.0) return true;
    return false;
}

// ============================================================================
// Logging
// ============================================================================

QString AIManager::logPath() const
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (basePath.isEmpty()) {
        basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    QString aiLogPath = basePath + "/ai_logs";
    QDir().mkpath(aiLogPath);
    return aiLogPath;
}

void AIManager::logPrompt(const QString& provider, const QString& systemPrompt, const QString& userPrompt)
{
    // Store for pairing with response
    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = userPrompt;

    QString path = logPath();
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");

    // Write individual prompt file
    QString promptFile = path + "/prompt_" + timestamp + ".txt";
    QFile file(promptFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << "=== AI PROMPT LOG ===\n";
        out << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        out << "Provider: " << provider << "\n";
        out << "\n=== SYSTEM PROMPT ===\n\n";
        out << systemPrompt << "\n";
        out << "\n=== USER PROMPT ===\n\n";
        out << userPrompt << "\n";
        file.close();
        qDebug() << "AI: Logged prompt to" << promptFile;
    } else {
        qWarning() << "AI: Failed to write prompt log:" << file.errorString();
    }

    // Also append to conversation history
    QString historyFile = path + "/conversation_history.txt";
    QFile history(historyFile);
    if (history.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&history);
        out << "\n" << QString("=").repeated(80) << "\n";
        out << "PROMPT - " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        out << "Provider: " << provider << "\n";
        out << QString("-").repeated(40) << "\n";
        out << userPrompt << "\n";
        history.close();
    } else {
        qWarning() << "AI: Failed to append to conversation history:" << history.errorString();
    }
}

void AIManager::logResponse(const QString& provider, const QString& response, bool success)
{
    QString path = logPath();
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");

    // Write individual response file
    QString responseFile = path + "/response_" + timestamp + ".txt";
    QFile file(responseFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << "=== AI RESPONSE LOG ===\n";
        out << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        out << "Provider: " << provider << "\n";
        out << "Success: " << (success ? "Yes" : "No") << "\n";
        out << "\n=== RESPONSE ===\n\n";
        out << response << "\n";
        file.close();
        qDebug() << "AI: Logged response to" << responseFile;
    } else {
        qWarning() << "AI: Failed to write response log:" << file.errorString();
    }

    // Write complete Q&A file (prompt + response together)
    QString qaFile = path + "/qa_" + timestamp + ".txt";
    QFile qa(qaFile);
    if (qa.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&qa);
        out << "=== AI Q&A LOG ===\n";
        out << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        out << "Provider: " << provider << "\n";
        out << "Success: " << (success ? "Yes" : "No") << "\n";
        out << "\n" << QString("=").repeated(60) << "\n";
        out << "SYSTEM PROMPT\n";
        out << QString("=").repeated(60) << "\n\n";
        out << m_lastSystemPrompt << "\n";
        out << "\n" << QString("=").repeated(60) << "\n";
        out << "USER PROMPT\n";
        out << QString("=").repeated(60) << "\n\n";
        out << m_lastUserPrompt << "\n";
        out << "\n" << QString("=").repeated(60) << "\n";
        out << "AI RESPONSE\n";
        out << QString("=").repeated(60) << "\n\n";
        out << response << "\n";
        qa.close();
        qDebug() << "AI: Logged Q&A to" << qaFile;
    } else {
        qWarning() << "AI: Failed to write Q&A log:" << qa.errorString();
    }

    // Also append to conversation history
    QString historyFile = path + "/conversation_history.txt";
    QFile history(historyFile);
    if (history.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&history);
        out << QString("-").repeated(40) << "\n";
        out << "RESPONSE - " << (success ? "SUCCESS" : "FAILED") << "\n";
        out << QString("-").repeated(40) << "\n";
        out << response << "\n";
        history.close();
    } else {
        qWarning() << "AI: Failed to append to conversation history:" << history.errorString();
    }
}
