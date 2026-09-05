#include "aimanager.h"
#include "core/appsettings.h"
#include "core/beanbaselogging.h"
#include "aiprovider.h"
#include "aiconversation.h"
#include "conversationkey.h"
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
    // Pre-change threads are keyed without the equipment package and their turns
    // carry cross-equipment context verbatim, so changing the key already orphans
    // them; and their user messages are the old prose-wrapped shape, which nothing
    // reads any more — one JSON object per turn is the only shape now.
    //
    // v2, not v1: the marker is per id and v1 has already fired on any device that
    // ran an interim build of this change. Those devices would keep prose turns
    // that no reader can render.
    clearAllConversationsOnce(QStringLiteral("equipment_scoped_conversations_v2"));

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
    openai->setBaseUrl(m_settings->ai()->openaiEndpoint());
    connect(openai, &AIProvider::analysisComplete, this, &AIManager::onAnalysisComplete);
    connect(openai, &AIProvider::analysisFailed, this, &AIManager::onAnalysisFailed);
    connect(openai, &AIProvider::testResult, this, &AIManager::onTestResult);
    m_openaiProvider.reset(openai);

    // Create Anthropic provider
    QString anthropicKey = m_settings->ai()->anthropicApiKey();
    auto* anthropic = new AnthropicProvider(m_networkManager, anthropicKey, this);
    anthropic->setModel(m_settings->ai()->providerModel("anthropic"));  // empty → keeps default
    anthropic->setBaseUrl(m_settings->ai()->anthropicEndpoint());
    connect(anthropic, &AIProvider::analysisComplete, this, &AIManager::onAnalysisComplete);
    connect(anthropic, &AIProvider::analysisFailed, this, &AIManager::onAnalysisFailed);
    connect(anthropic, &AIProvider::testResult, this, &AIManager::onTestResult);
    // [barista-fork] Interim pre-tool lead-in (only Anthropic emits it — the tool_use/pause_turn paths).
    connect(anthropic, &AIProvider::interimText, this, &AIManager::onInterimText);
    // [barista-fork] Register the barista's private client-side tools (definitions + executor) behind the
    // provider's generic seam. The executor reads m_shotHistory lazily (it's wired after construction via
    // setShotHistoryStorage), so capturing `this` and forwarding at call time preserves the original behavior.
    // [barista-fork] Shared client-tool executor. Registered on EVERY provider that runs the barista's
    // function-calling loop (Anthropic + Gemini) so switching the provider keeps the full tool surface — the
    // barista drives shots/recipes/taste/memory identically on either. Captures `this` and forwards at call time
    // (m_shotHistory etc. are wired after construction via setShotHistoryStorage), so a copy per provider is safe.
    auto baristaToolExecutor =
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
                                      m_endConversationHandler, m_openBagCameraHandler, m_webToolsHandler,
                                      m_getActiveRecipeHandler, m_activateRecipeHandler, m_deactivateRecipeHandler,
                                      m_updateRecipeHandler,
                                      m_recipeOpHandler,
                                      m_bagOpHandler,
                                      m_listProfilesHandler,
                                      m_setActiveUserHandler,
                                      anchor,
                                      name, input, std::move(done));
        };
    anthropic->setClientTools(BaristaTools::toolDefinitions(), baristaToolExecutor);
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
    // [barista-fork] Give Gemini the SAME barista tool surface as Anthropic — the client tools (shots/recipes/
    // taste/memory) via the shared executor, and the keyless fast-path web tools. Gemini's own function-calling
    // loop (GeminiProvider::analyzeConversation/onAnalysisReply) drives them. So selecting Gemini in Settings ▸ AI
    // now runs the full interactive barista, not just plain chat. (Gemini has no server-side general web search
    // here, so a broad "search the web" degrades to whatever the keyless weather/stock/news tools cover.)
    gemini->setClientTools(BaristaTools::toolDefinitions(), baristaToolExecutor);
    gemini->setWebTools(BaristaTools::webToolDefinitions());
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

QString AIManager::tr_(const char* key, const char* fallback) const {
    return translateOrFallback(m_translationManager, key, fallback);
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

QString AIManager::costHint(const QString& providerId, const QString& modelId) const
{
    AIProvider* provider = providerById(providerId);
    if (!provider)
        return {};
    return modelId.isEmpty() ? provider->costHint() : provider->costHintFor(modelId);
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
    // No silent substitution. An unrecognised id (a value written by a newer
    // build, a provider since removed, a corrupted setting) used to resolve to
    // OpenAI, which meant a user could be billed on a provider they had not
    // selected — and `logPrompt(selectedProvider(), …)` would record the name
    // they DID select, so the substitution was invisible in the one place you
    // would look for it. Every caller already handles null by reporting
    // "no provider configured", which names the real problem.
    AIProvider* provider = providerById(selectedProvider());
    if (!provider && !selectedProvider().isEmpty())
        qWarning() << "AIManager: no provider for selected id" << selectedProvider();
    return provider;
}

bool AIManager::currentProviderSupportsTools() const
{
    AIProvider* p = currentProvider();
    return p && p->supportsClientTools();
}

bool AIManager::currentProviderSupportsWebSearch() const
{
    AIProvider* p = currentProvider();
    return p && p->supportsWebSearch();
}

bool AIManager::currentProviderSupportsVision() const
{
    AIProvider* p = currentProvider();
    return p && p->supportsVision();
}

void AIManager::stagePendingImage(const QByteArray& data, const QString& mediaType)
{
    m_pendingImageData = data;
    m_pendingImageMediaType = mediaType;
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
    // Same reason as the bean-metadata capture below: this is the user's own
    // rating, so a write that lands nowhere must not pass unnoticed.
    m_pendingMetadataWrites[shotId]++;
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
    // Tracked so the outcome is read. This write carries something the USER
    // just said; losing it silently is the defect this replaces.
    m_pendingMetadataWrites[shotId]++;
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
                                       const DialingBlocks::AdvisorContextBlocks& blocks) const
{
    if (!blocks.dialInSessions.isEmpty())
        payload["dialInSessions"] = blocks.dialInSessions;
    // Mutually exclusive with dialInSessions by construction — the builder only
    // fills this one when the history query ran and matched nothing.
    if (!blocks.noDialInHistory.isEmpty())
        payload["noDialInHistory"] = blocks.noDialInHistory;
    if (!blocks.bestRecentShot.isEmpty())
        payload["bestRecentShot"] = blocks.bestRecentShot;
    // Bean memory [barista-fork]: the user's best rated shot for the CURRENT
    // bean on this profile. Anchors advice to "your best on THIS bean", not just
    // the profile. Empty → key omitted (no placeholder).
    if (!blocks.beanBestShot.isEmpty())
        payload["beanBestShot"] = blocks.beanBestShot;
    if (!blocks.grinderContext.isEmpty())
        payload["grinderContext"] = blocks.grinderContext;
    if (!blocks.grinderCalibration.isEmpty())
        payload["grinderCalibration"] = blocks.grinderCalibration;
    // Closed-loop coaching: prior advisor turns paired with the user's
    // actual next shots (issue #1053). Empty array (no qualifying turns
    // yet) → key omitted; never `recentAdvice: []` placeholder.
    if (!blocks.recentAdvice.isEmpty())
        payload["recentAdvice"] = blocks.recentAdvice;
    if (shotData.isValid()) {
        const QJsonObject sawPrediction = DialingBlocks::buildSawPredictionBlock(
            m_settings, m_profileManager, shotData);
        if (!sawPrediction.isEmpty())
            payload["sawPrediction"] = sawPrediction;
    }
}

void AIManager::setShotHistoryStorage(ShotHistoryStorage* storage)
{
    if (m_shotHistory == storage) return;
    if (m_shotHistory)
        disconnect(m_shotHistory, &ShotHistoryStorage::shotMetadataUpdated, this, nullptr);

    m_shotHistory = storage;

    // The manager loads the most recent conversation in its own constructor,
    // which runs BEFORE MainController wires the storage — so that conversation
    // reached repairStaleTurnShotIds with nothing to check against and was left
    // holding whatever stale ids it had. It is the conversation the user is most
    // likely to continue, so the repair has to catch up as soon as we can check.
    if (m_shotHistory && m_conversation)
        m_conversation->repairStaleTurnShotIds();

    // Watch the outcome of OUR OWN metadata writes. The storage layer already
    // reported the outcome through this signal — ShotHistoryExporter has been
    // connected to it for a long time — but nothing acted on the FAILURE case
    // (the exporter returns early on it), so a write to a shot that does not
    // exist logged one warning and the user's answer was gone.
    //
    // m_pendingMetadataWrites keeps this to writes this class initiated: every
    // other subsystem's writes arrive on the same signal. It is a REFCOUNT, not
    // a set. One reply can drive two writes for the same shot id (a rating and
    // bean metadata, see AIConversation), and with a set the first outcome
    // consumed the only entry, so the second — possibly the failing one — was
    // discarded as "not ours" by the very filter added to catch it.
    if (m_shotHistory) {
        connect(m_shotHistory, &ShotHistoryStorage::shotMetadataUpdated, this,
                [this](qint64 shotId, bool success) {
            const auto it = m_pendingMetadataWrites.find(shotId);
            if (it == m_pendingMetadataWrites.end()) return;  // not ours
            if (--it.value() <= 0)
                m_pendingMetadataWrites.erase(it);
            if (success) return;
            // Do NOT assert a cause here. `success == false` also arrives from a
            // not-ready database, from no valid fields to update, and from a
            // prepare or exec failure — only one of those is "no such shot".
            // Field logs are read and acted on by users' own AI assistants, so a
            // confident wrong diagnosis costs more than a neutral one.
            qWarning() << "AIManager: metadata write to shot" << shotId
                       << "did not land, so what the user told the advisor was not saved."
                       << "Leading candidate is a conversation turn still naming a shot id"
                       << "from a database this device no longer has; a database that was"
                       << "not ready, or an SQL failure, produce the same result.";
            emit shotMetadataCaptureFailed(shotId);
        });
    }
}


// File-scope helper: render one `recentAdvice` entry (see
// DialingBlocks::buildRecentAdviceBlock) as a markdown block. Every other
// section of the in-app historicalContext is hand-rendered prose, not a
// JSON blob, so this keeps the format consistent — the underlying data is
// identical to what the MCP `recentAdvice` JSON array carries for the same
// inputs (turnsAgo/recommendation/structuredNext/userResponse).

void AIManager::requestRecentShotContext(const QVariant& shotData, qint64 contextShotId)
{
    const ShotProjection shot = coerceShot(shotData);
    if (!m_shotHistory || (shot.beanBrand.isEmpty() && shot.profileName.isEmpty())) {
        emit recentShotContextReady();
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
    // `shot` is deliberately NOT captured: it is only the validity gate above.
    // The worker re-reads the shot whole from the database so both surfaces feed
    // the assembler identical input.
    QThread* thread = QThread::create([self, dbPath, contextShotId, serial]() {
        DialingBlocks::AdvisorContextBlocks blocks;
        withTempDb(dbPath, "ai_advisor_ctx", [&](QSqlDatabase& db) {
            // The shot the advice is about, read whole — the same record
            // ai_advisor_invoke resolves, so both surfaces feed the one
            // assembler identical input.
            const ShotRecord record =
                ShotHistoryStorage::loadShotRecordStatic(db, contextShotId);
            const ShotProjection ctxShot = ShotHistoryStorage::convertShotRecord(record);
            // The database is the only source of this shot now, and
            // loadShotRecordStatic returns a default record for BOTH a failed
            // query and a genuine miss. Without this the blocks come back empty,
            // get cached, and QML clears its spinner — byte-identical to a
            // successful empty result, so the advisor answers with no history and
            // nobody is told why. Bail with a log instead; the caller's bare
            // "ready" still fires below.
            if (!ctxShot.isValid()) {
                qWarning() << "AIManager::requestRecentShotContext: shot" << contextShotId
                           << "did not resolve — advisor context will be empty";
                return;
            }

            // Loaded here rather than inside the assembler: see
            // buildAdvisorContextBlocks on why it is a parameter.
            QList<AIConversation::HistoricalAssistantTurn> turns;
            if (!ctxShot.profileKbId.isEmpty()) {
                turns = AIConversation::loadRecentAssistantTurnsForKey(
                    AIManager::conversationKey(ctxShot), DialingBlocks::kRecentAdviceTurns);
            }
            blocks = DialingBlocks::buildAdvisorContextBlocks(
                db, ctxShot, contextShotId, turns);
        });

        QMetaObject::invokeMethod(qApp, [self, serial, contextShotId, blocks]() {
            if (!self) return;
            self->emitRecentShotContext(blocks, contextShotId, serial);
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void AIManager::emitRecentShotContext(const DialingBlocks::AdvisorContextBlocks& blocks,
                                      qint64 contextShotId,
                                      int serial)
{
    if (serial != m_contextSerial) {
        // Stale request superseded by a newer one — emit empty so QML clears
        // contextLoading, and leave the cache alone: the newer request owns it.
        emit recentShotContextReady();
        return;
    }

    m_contextBlocks = blocks;
    m_contextBlocksShotId = contextShotId;

    // A bare "ready", with no payload. It used to carry a JSON rendering of the
    // blocks, "so the web surface and the tests can see what resolved" — no such
    // reader existed: the one consumer stored the string in a QML property
    // nothing read. That left a SECOND assembler over AdvisorContextBlocks, and
    // it had already fallen behind, omitting noDialInHistory while
    // enrichUserPromptObject included it. The payload QML sends is assembled
    // once, by buildConversationUserPrompt, from the cache above.
    emit recentShotContextReady();
}

QString AIManager::buildConversationUserPrompt(const QVariant& shotData,
                                               const QString& question,
                                               const QString& shotLabel)
{
    const ShotProjection shot = coerceShot(shotData);
    if (!shot.isValid()) return question;

    // The shot's own payload, from the same builder ai_advisor_invoke uses.
    ShotSummary summary = m_summarizer->summarizeFromHistory(shot);
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(m_summarizer->buildUserPrompt(summary).toUtf8(), &err);
    QJsonObject payload = (err.error == QJsonParseError::NoError && doc.isObject())
                              ? doc.object()
                              : QJsonObject();

    // Context blocks, only when they were resolved for THIS shot. A mismatch means
    // the user opened another shot while the request was in flight; sending the
    // other shot's history would be worse than sending none.
    if (m_contextBlocksShotId == shot.id) {
        enrichUserPromptObject(payload, shot, m_contextBlocks);
    } else {
        // The function returned above on an invalid shot, so no second validity
        // test is needed here.
        // sawPrediction does not come from the DB pass, so it is available either way.
        const QJsonObject sawPrediction =
            DialingBlocks::buildSawPredictionBlock(m_settings, m_profileManager, shot);
        if (!sawPrediction.isEmpty()) payload["sawPrediction"] = sawPrediction;
    }

    if (!shotLabel.isEmpty()) payload["shotLabel"] = shotLabel;

    // What moved since the previous shot in this conversation. A field, not a
    // banner prepended to the text — the banner is what made the payload a
    // prose/JSON sandwich that then had to be taken apart again to be read.
    if (m_conversation) {
        const QString soFar =
            QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        const QJsonObject changes = m_conversation->changesFromPreviousShot(shotLabel, soFar);
        if (!changes.isEmpty()) payload["changesFromPreviousShotInConversation"] = changes;
    }

    if (!question.isEmpty()) payload["question"] = question;

    return QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
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
        // [barista-fork] All dialing-context blocks travel as one struct, from
        // the same assembler the in-app and MCP advisors use, so a block added
        // upstream (or newly scoped, like beanBestShot) reaches the barista by
        // construction rather than a remembered edit here.
        DialingBlocks::AdvisorContextBlocks blocks;
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

            // Same shared assembler the in-app and MCP advisors use — one scope
            // (the shot's equipment package) threaded into every block. beanBestShot
            // and grinderCalibration ride in the struct; noDialInHistory is filled
            // when the scoped history query ran and matched nothing.
            // [barista-fork] turn-cost: 3 recent sessions (5→3) and 2 prior advice
            // turns (3→2). The last three shots carry the dialing trend the barista
            // reasons on; the 4th/5th were pure token weight, and query_shots reaches
            // deeper history on demand. Older advice is stale and re-derivable.
            QList<AIConversation::HistoricalAssistantTurn> turns;
            if (!shot.profileKbId.isEmpty()) {
                const QString convKey = AIManager::conversationKey(shot);
                turns = AIConversation::loadRecentAssistantTurnsForKey(convKey, 2);
            }
            blocks = DialingBlocks::buildAdvisorContextBlocks(
                db, shot, anchorId, turns, /*historyLimit=*/3,
                DialingBlocks::GrinderCalibration::Include);

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
                // [barista-fork] turn-cost: 12→6 bean aggregates. This is "you have a whole history" awareness
                // padding, not dialing data; the top few beans convey it. query_shots covers the rest on demand.
                if (bq.exec("SELECT bean_brand, bean_type, COUNT(*) c, MIN(timestamp) mn, MAX(timestamp) mx "
                            "FROM shots GROUP BY bean_brand, bean_type ORDER BY c DESC LIMIT 6")) {
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
                // and capped at 15 so the injected prompt can't grow without bound. Injected as background
                // continuity the barista already knows — it should weave them in, not recite or re-ask them.
                // [barista-fork] turn-cost: 30→15 saved facts. Background continuity padding, not dialing data;
                // 15 most-recent facts keep the personal touch without the injected prompt growing unbounded.
                for (const QVariant& r : TasksStorage::fetchUserFactsStatic(db, activeUser, 15)) {
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
                                         occasion, knownFacts, blocks, fullHistory, recipesBlock, whoBlock]() {
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
            self->enrichUserPromptObject(obj, shot, blocks);
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
    m_isProductPageSearch = false;
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

// [barista-fork] Step 6: cross-session rolling summary. A separate one-shot call at session close; best-effort,
// silently skipped if a turn is in flight (never queued behind, never blocks re-engagement more than momentarily).
void AIManager::requestSessionSummary(const QString& userToken, const QString& conversationText)
{
    if (m_analyzing || conversationText.trimmed().isEmpty())
        return;   // best-effort: don't fight a live turn, don't summarize nothing
    AIProvider* provider = currentProvider();
    if (!provider || !isConfigured())
        return;

    const QString systemPrompt = QStringLiteral(
        "You summarize a conversation between a user and their friendly espresso barista, to remember useful "
        "context for NEXT time. In ONE or TWO short sentences, capture only DURABLE, non-obvious things worth "
        "remembering: the user's stated preferences, tastes, plans, or personal context (e.g. 'prefers less "
        "fruity coffees', 'hosting a dinner Saturday', 'still learning the LRv3 profile'). Do NOT include shot "
        "numbers, grind settings, ratios, or dial-in data — those are stored separately. Write it as a plain note "
        "to yourself, no preamble. If there is nothing durable worth remembering, reply with exactly: NONE");

    m_analyzing = true;
    m_isConversationRequest = false;
    m_isBagExtractionRequest = false;
    m_isCoachPhrasebookRequest = false;
    m_isSessionSummaryRequest = true;
    m_sessionSummaryUser = userToken;
    emit analyzingChanged();
    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = QStringLiteral("[session summary]");
    logPrompt(selectedProvider(), systemPrompt, m_lastUserPrompt);
    provider->analyze(systemPrompt, conversationText);
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
    m_isProductPageSearch = false;
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
    m_isProductPageSearch = false;
    m_bagExtractionToken = requestToken;
    emit analyzingChanged();

    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = userPrompt;
    logPrompt(selectedProvider(), systemPrompt, userPrompt);
    provider->analyzeUrl(systemPrompt, userPrompt);
}

bool AIManager::supportsProductPageSearch() const
{
    AIProvider* provider = const_cast<AIManager*>(this)->currentProvider();
    return provider && provider->supportsWebSearch();
}

void AIManager::logProductPageSearchDeclined(const QString& reason) const
{
    BEANBASE_INFO_STDERR("FindPage", QStringLiteral("Automatic search declined - %1").arg(reason));
}

void AIManager::findProductPage(const QString& requestToken, const QString& roaster,
                                const QString& coffee, const QString& kind)
{
    // The last rung of the photo/details ladder: the bag has no usable URL by
    // any deterministic route, so the provider is asked to find one with its
    // own web tool. The result is a SUGGESTION — the caller confirms it before
    // it is stored, because a model's guess written into `link` would be read
    // by every downstream consumer as fact.
    if (m_analyzing) {
        emit productPageSearchFailed(requestToken, QStringLiteral("busy"));
        return;
    }
    AIProvider* provider = currentProvider();
    if (!provider || !isConfigured()) {
        // No substitution, ever: a user with one provider selected must not be
        // billed on another because this one is unconfigured.
        emit productPageSearchFailed(requestToken, QStringLiteral("notConfigured"));
        return;
    }
    // SEARCH, not fetch. Only OpenAI's tool does both; Anthropic's web_fetch
    // and Gemini's url_context can only open a URL the prompt already names,
    // so routing this through analyzeUrl left two of the three providers
    // answering from memory.
    if (!provider->supportsWebSearch()) {
        emit productPageSearchFailed(requestToken, QStringLiteral("webSearchUnsupported"));
        return;
    }

    const QString what = (kind == QLatin1String("tea"))
        ? QStringLiteral("loose-leaf tea") : QStringLiteral("coffee");
    const QString systemPrompt = QStringLiteral(
        "You find the vendor's own product page for a named %1 using web search. "
        "Reply with ONLY a JSON object: {\"url\": \"<absolute https URL>\"} for the page that sells "
        "or describes exactly that product, or {} when you cannot find one you are confident in. "
        "The URL MUST be the vendor's own product page - never a marketplace listing, a review, a "
        "blog post, a category or search page, or a page for a different lot or roast of the same "
        "name. Never guess a URL by pattern; only report a page you actually found. No markdown, "
        "no commentary.").arg(what);
    const QString userPrompt = QStringLiteral("Vendor: %1\nProduct: %2").arg(roaster, coffee);

    m_analyzing = true;
    m_isConversationRequest = false;
    m_isBagExtractionRequest = false;
    m_isProductPageSearch = true;
    m_productPageToken = requestToken;
    emit analyzingChanged();

    m_lastSystemPrompt = systemPrompt;
    m_lastUserPrompt = userPrompt;
    logPrompt(selectedProvider(), systemPrompt, userPrompt);
    provider->searchWeb(systemPrompt, userPrompt);
}

// static
QString AIManager::parseProductPageUrl(const QString& response)
{
    const qsizetype start = response.indexOf(QLatin1Char('{'));
    const qsizetype end = response.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end <= start)
        return {};
    const QJsonDocument doc =
        QJsonDocument::fromJson(response.mid(start, end - start + 1).toUtf8());
    if (!doc.isObject())
        return {};
    const QString url = doc.object().value(QStringLiteral("url")).toString().trimmed();
    // https only: the URL is about to be fetched and handed to a provider, and
    // an http (or file, or data) URL must never reach either.
    if (!url.startsWith(QLatin1String("https://")))
        return {};
    const QUrl parsed(url);
    return (parsed.isValid() && !parsed.host().isEmpty()) ? url : QString();
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
    m_isProductPageSearch = false;
    m_isBagExtractionRequest = false;
    m_isCoachPhrasebookRequest = false;   // [barista-fork]
    m_isSessionSummaryRequest = false;    // [barista-fork] Step 6
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
    // [barista-fork] forceRespond = clientTools: the barista conversation (the only caller that enables client
    // tools) forces tool_choice:"any" + a `respond` answer tool on Anthropic, so a turn can never end with a bare
    // "let me check…" promise and no tool call. Scoped here — analyzeUrl/advisor never set clientTools, so they're
    // untouched; non-Anthropic providers ignore the flag.
    // [barista-fork] Consume any image staged for THIS turn (add-a-bean-from-a-photo). Cleared unconditionally so
    // it can ride only one turn — never persisted, never re-billed on a follow-up. Only attached when the selected
    // provider can read images; otherwise dropped (the UI already gates the affordance on currentProviderSupportsVision).
    QByteArray turnImage = m_pendingImageData;
    QString turnImageType = m_pendingImageMediaType;
    m_pendingImageData.clear();
    m_pendingImageMediaType.clear();
    if (!turnImage.isEmpty() && !(provider->supportsVision())) {
        turnImage.clear();
        turnImageType.clear();
    }
    // [barista-fork] Consume the stable-core prefix length staged for this turn (Anthropic cache breakpoint).
    const int turnCachePrefixLen = m_pendingCachePrefixLen;
    m_pendingCachePrefixLen = -1;
    provider->analyzeConversation(systemPrompt, apiMessages,
                                  // [barista-fork] {webSearch, clientTools, timeoutMs, forceRespond, imageData,
                                  // imageMediaType, cachePrefixLen} — imageData set only on a photo turn; cachePrefixLen
                                  // ≥0 only on a tailored barista turn (else -1 = cache the whole prompt).
                                  AIProvider::RequestOptions{webSearch, clientTools, 30000, clientTools,
                                                             turnImage, turnImageType, turnCachePrefixLen});
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
    if (m_isProductPageSearch) {
        m_isProductPageSearch = false;
        const QString token = m_productPageToken;
        m_productPageToken.clear();
        const QString url = parseProductPageUrl(response);
        if (url.isEmpty())
            emit productPageSearchFailed(token, QStringLiteral("notFound"));
        else
            emit productPageFound(token, url);
    } else if (m_isSessionSummaryRequest) {   // [barista-fork] Step 6
        m_isSessionSummaryRequest = false;
        const QString user = m_sessionSummaryUser;
        m_sessionSummaryUser.clear();
        emit sessionSummaryReady(user, response);
    } else if (m_isCoachPhrasebookRequest) {   // [barista-fork]
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
    if (m_isProductPageSearch) {
        m_isProductPageSearch = false;
        const QString token = m_productPageToken;
        m_productPageToken.clear();
        emit productPageSearchFailed(token, error);
    } else if (m_isSessionSummaryRequest) {   // [barista-fork] Step 6 — a failed summary is best-effort; drop silently.
        m_isSessionSummaryRequest = false;
        m_sessionSummaryUser.clear();
    } else if (m_isCoachPhrasebookRequest) {   // [barista-fork]
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
        openai->setBaseUrl(m_settings->ai()->openaiEndpoint());
    }

    auto* anthropic = dynamic_cast<AnthropicProvider*>(m_anthropicProvider.get());
    if (anthropic) {
        anthropic->setApiKey(m_settings->ai()->anthropicApiKey());
        anthropic->setModel(m_settings->ai()->providerModel("anthropic"));  // empty → keeps default
        anthropic->setBaseUrl(m_settings->ai()->anthropicEndpoint());
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

QString AIManager::ConversationEntry::label() const
{
    QStringList bean;
    if (!beanBrand.isEmpty()) bean << beanBrand;
    if (!beanType.isEmpty()) bean << beanType;

    QStringList parts;
    if (!bean.isEmpty()) parts << bean.join(QStringLiteral(" "));
    if (!profileName.isEmpty()) parts << profileName;
    if (!equipmentLabel.isEmpty()) parts << equipmentLabel;
    return parts.join(QStringLiteral(" / "));
}

QJsonObject AIManager::ConversationEntry::toJson() const
{
    QJsonObject obj;
    obj["key"] = key;
    obj["beanBrand"] = beanBrand;
    obj["beanType"] = beanType;
    obj["profileName"] = profileName;
    obj["equipmentLabel"] = equipmentLabel;
    obj["equipmentId"] = equipmentId;
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
    entry.equipmentLabel = obj["equipmentLabel"].toString();
    entry.equipmentId = obj["equipmentId"].toVariant().toLongLong();
    entry.timestamp = obj["timestamp"].toVariant().toLongLong();
    return entry;
}

AIManager::ConversationEntry AIManager::conversationEntry(const QString& key) const
{
    for (const auto& entry : m_conversationIndex) {
        if (entry.key == key)
            return entry;
    }
    return {};
}

QString AIManager::conversationKey(const ShotProjection& shot)
{
    // Derivation lives in ConversationKey::derive — the import path re-derives
    // it after renumbering the equipment package, and a second copy of the hash
    // would orphan every restored thread.
    return ConversationKey::derive(shot.beanBrand, shot.beanType,
                                   shot.profileName, shot.equipmentId);
}

void AIManager::loadConversationIndex()
{
    AppSettings settings;
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
    AppSettings settings;
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
    AppSettings settings;
    QString prefix = "ai/conversations/" + oldest.key + "/";
    settings.remove(prefix + "systemPrompt");
    settings.remove(prefix + "messages");
    settings.remove(prefix + "timestamp");

    qDebug() << "AIManager: Evicted oldest conversation:" << oldest.beanBrand << oldest.beanType << oldest.profileName;
    saveConversationIndex();
}

void AIManager::clearAllConversationsOnce(const QString& migrationId)
{
    AppSettings settings;
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
    AppSettings settings;

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

QString AIManager::switchConversation(const QVariant& shotData)
{
    const ShotProjection shot = coerceShot(shotData);
    const QString key = conversationKey(shot);

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
    ConversationEntry entry;
    entry.key = key;
    entry.beanBrand = shot.beanBrand;
    entry.beanType = shot.beanType;
    entry.profileName = shot.profileName;
    entry.equipmentLabel = shot.equipmentLabel();
    entry.equipmentId = shot.equipmentId;
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    m_conversation->setStorageKey(key);
    m_conversation->setContextLabel(entry.label());
    m_conversation->loadFromStorage();

    if (exists) {
        touchConversationEntry(key);
    } else {
        evictOldestConversation();
        m_conversationIndex.prepend(entry);
        saveConversationIndex();
    }

    emit m_conversation->savedConversationChanged();
    qDebug() << "AIManager: Switched to conversation key:" << key << "(" << entry.label() << ")";
    return key;
}

void AIManager::loadMostRecentConversation()
{
    if (m_conversationIndex.isEmpty()) {
        m_conversation->setStorageKey(QString());
        m_conversation->setContextLabel(QString());
        return;
    }

    const auto& entry = m_conversationIndex.first();
    m_conversation->setStorageKey(entry.key);
    m_conversation->setContextLabel(entry.label());
    m_conversation->loadFromStorage();
    qDebug() << "AIManager: Loaded most recent conversation:" << entry.key
             << "(" << entry.label() << ")";
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
