#include "coachplanstorage.h"
#include "../core/dbutils.h"
#include "../core/grinderaliases.h"
#include "../history/shotprojection.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDatabase>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QDebug>
#include <cmath>
#include <optional>

namespace {

// Single source of truth for the writable coach_plans columns (the plan half).
// `key` is the camelCase QVariantMap key the caller supplies; `sql` is the column.
// `id` is autoincrement; `created_at` is stamped app-side; the outcome-half columns
// (follow_up_shot_id, adherence, in_predicted_range, status, judged_at) are written
// only by the judge pass via writeJudgmentStatic, never here.
struct Col { const char* sql; const char* key; };

const Col kWritableCols[] = {
    { "anchor_shot_id",  "anchorShotId" },
    { "bean_brand",      "beanBrand" },
    { "bean_type",       "beanType" },
    { "profile_kb_id",   "profileKbId" },
    { "equipment_id",    "equipmentId" },
    { "source",          "source" },
    { "structured_next", "structuredNext" },
    { "kb_rec_id",       "kbRecId" },
    { "kb_confidence",   "kbConfidence" },
    { "kb_prep_gate",    "kbPrepGate" },
    { "lever",           "lever" },
    { "direction",       "direction" },
};

// Dose tolerance matches computeAdherence's kDoseToleranceG so the ledger's
// "did the dose move" agrees with the grader's "was the dose followed".
constexpr double kDoseToleranceG = 0.3;

// Tier gate constants (DoR §1.4/§1.5). A follow-up must directly follow the anchor within this window to count
// as evidence; a rating delta smaller than the band is "flat" not a real improvement/regression.
constexpr qint64 kMaxOutcomeGapSecs = qint64(14) * 24 * 60 * 60;   // 14 days
constexpr int kFlatRatingBand = 5;                                 // |Δ| ≤ 5 on the 0–100 scale = flat

// SELECT list covering every column, in a fixed order shared by all readers.
const char* kSelectAllCols =
    "SELECT id, created_at, anchor_shot_id, bean_brand, bean_type, profile_kb_id, "
    "equipment_id, source, structured_next, kb_rec_id, kb_confidence, kb_prep_gate, "
    "lever, direction, follow_up_shot_id, adherence, in_predicted_range, status, judged_at "
    "FROM coach_plans";

QVariantMap rowToMap(const QSqlQuery& q)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"),               q.value(0).toLongLong());
    m.insert(QStringLiteral("createdAt"),        q.value(1).toLongLong());
    m.insert(QStringLiteral("anchorShotId"),     q.value(2).toLongLong());
    m.insert(QStringLiteral("beanBrand"),        q.value(3).toString());
    m.insert(QStringLiteral("beanType"),         q.value(4).toString());
    m.insert(QStringLiteral("profileKbId"),      q.value(5).toString());
    m.insert(QStringLiteral("equipmentId"),      q.value(6).toLongLong());
    m.insert(QStringLiteral("source"),           q.value(7).toString());
    m.insert(QStringLiteral("structuredNext"),   q.value(8).toString());
    m.insert(QStringLiteral("kbRecId"),          q.value(9).toString());
    m.insert(QStringLiteral("kbConfidence"),     q.value(10).toString());
    m.insert(QStringLiteral("kbPrepGate"),       q.value(11).toInt());
    m.insert(QStringLiteral("lever"),            q.value(12).toString());
    m.insert(QStringLiteral("direction"),        q.value(13).toString());
    m.insert(QStringLiteral("followUpShotId"),   q.value(14).toLongLong());
    m.insert(QStringLiteral("adherence"),        q.value(15).toString());
    m.insert(QStringLiteral("inPredictedRange"), q.value(16).toString());
    m.insert(QStringLiteral("status"),           q.value(17).toString());
    m.insert(QStringLiteral("judgedAt"),         q.value(18).toLongLong());
    return m;
}

} // namespace

CoachPlanStorage::CoachPlanStorage(QObject* parent)
    : QObject(parent)
{
}

CoachPlanStorage::~CoachPlanStorage()
{
    // Suppress in-flight result callbacks, then stop the worker before members vanish
    // (same rationale as FeedbackStorage/TasksStorage).
    *m_destroyed = true;
    m_dbWorker.reset();
}

void CoachPlanStorage::initialize(const QString& dbPath)
{
    m_dbPath = dbPath;
}

void CoachPlanStorage::runAsync(const QString& connPrefix,
                                std::function<void(QSqlDatabase&)> work,
                                std::function<void(bool dbOpened)> done)
{
    if (m_dbPath.isEmpty()) {
        qWarning() << "CoachPlanStorage: not initialized, dropping" << connPrefix;
        return;
    }
    if (!m_dbWorker)
        m_dbWorker = std::make_unique<SerialDbWorker>(QStringLiteral("CoachPlanStorageWorker"));
    m_dbWorker->run(m_dbPath, connPrefix, std::move(work), std::move(done), this, m_destroyed);
}

void CoachPlanStorage::requestLogPlan(const QVariantMap& fields)
{
    // Guarantee a terminal planLogged even when uninitialized so a caller arming a
    // one-shot response doesn't hang.
    if (m_dbPath.isEmpty()) {
        qWarning() << "CoachPlanStorage: requestLogPlan on uninitialized storage";
        emit planLogged(-1);
        return;
    }
    auto newId = std::make_shared<qint64>(-1);
    runAsync("coach_plan_log",
        [fields, newId](QSqlDatabase& db) {
            // The write path shares ensureSchemaStatic so a first-ever write self-creates the
            // schema (coach_plans may not exist yet on a fresh install).
            if (!CoachPlanStorage::ensureSchemaStatic(db))
                return;
            const qint64 id = CoachPlanStorage::insertPlanStatic(db, fields);
            *newId = id;
            if (id < 0)
                return;
            // A new plan for this scope supersedes any older still-open plan there, so a
            // plan is only ever judged against the shot that directly followed it.
            CoachPlanStorage::supersedeOpenPlansStatic(
                db,
                fields.value(QStringLiteral("beanBrand")).toString(),
                fields.value(QStringLiteral("beanType")).toString(),
                fields.value(QStringLiteral("profileKbId")).toString(),
                fields.value(QStringLiteral("equipmentId")).toLongLong(),
                id);
        },
        // Write: emit regardless — *newId is -1 on failure, a terminal status.
        [this, newId](bool) { emit planLogged(*newId); });
}

CoachPlanStorage::LeverDirection
CoachPlanStorage::deriveLeverDirection(const QJsonObject& sn, const ShotProjection& anchor)
{
    // --- grind lever (grinderSetting and/or rpm) ---
    bool grindMoved = false;
    QString grindDir;   // set only when grinderSetting gives a clear numeric direction
    if (sn.contains(QStringLiteral("grinderSetting"))) {
        const QString rec = sn.value(QStringLiteral("grinderSetting")).toString().trimmed();
        if (!rec.isEmpty()) {
            if (!GrinderAliases::looksLikeSetting(rec)) {
                // Prose ("a touch coarser than 9") — a grind change was intended but its
                // direction can't be scored. Counts as the grind lever, direction unclear.
                grindMoved = true;
                grindDir = QStringLiteral("unclear");
            } else {
                const std::optional<double> recNum = GrinderAliases::leadingDialNumber(rec);
                const std::optional<double> anchorNum =
                    GrinderAliases::leadingDialNumber(anchor.grinderSetting.trimmed());
                if (recNum && anchorNum) {
                    const double diff = *recNum - *anchorNum;
                    if (std::abs(diff) > 1e-9) {
                        grindMoved = true;
                        // Lower dial number = finer (the near-universal espresso-grinder
                        // convention; grinders that invert it are rare and only mis-sign a
                        // piece of advisory metadata, never the shot record).
                        grindDir = diff < 0.0 ? QStringLiteral("finer") : QStringLiteral("coarser");
                    }
                } else {
                    // Compound ("1+4") or lettered ("8C") — compare normalized strings; the
                    // sign can't be derived cheaply without the grinder's own notation model.
                    const QString recKey = GrinderAliases::compoundKey(rec);
                    const QString anchorKey = GrinderAliases::compoundKey(anchor.grinderSetting.trimmed());
                    const QString a = recKey.isEmpty() ? rec : recKey;
                    const QString b = anchorKey.isEmpty() ? anchor.grinderSetting.trimmed() : anchorKey;
                    if (a.compare(b, Qt::CaseInsensitive) != 0) {
                        grindMoved = true;
                        grindDir = QStringLiteral("unclear");
                    }
                }
            }
        }
    }
    // rpm folds into the grind lever (it is a grinder parameter). An rpm-only move gives
    // no finer/coarser reading.
    if (sn.contains(QStringLiteral("rpm"))) {
        const int recRpm = sn.value(QStringLiteral("rpm")).toInt();
        if (recRpm > 0 && recRpm != static_cast<int>(anchor.rpm)) {
            if (!grindMoved) {
                grindMoved = true;
                grindDir = QStringLiteral("unclear");
            }
            // else grinderSetting already set the direction; rpm doesn't override it.
        }
    }

    // --- dose lever ---
    bool doseMoved = false;
    QString doseDir;
    if (sn.contains(QStringLiteral("doseG"))) {
        const double recDose = sn.value(QStringLiteral("doseG")).toDouble();
        if (recDose > 0.0 && std::abs(recDose - anchor.doseWeightG) > kDoseToleranceG + 1e-9) {
            doseMoved = true;
            doseDir = recDose > anchor.doseWeightG ? QStringLiteral("up") : QStringLiteral("down");
        }
    }

    // --- profile lever ---
    bool profileMoved = false;
    if (sn.contains(QStringLiteral("profileTitle"))) {
        const QString rec = sn.value(QStringLiteral("profileTitle")).toString();
        if (!rec.isEmpty() && rec != anchor.profileName)
            profileMoved = true;
    }

    const int movedLevers = (grindMoved ? 1 : 0) + (doseMoved ? 1 : 0) + (profileMoved ? 1 : 0);
    if (movedLevers >= 2)
        return { QStringLiteral("multi"), QString() };
    if (movedLevers == 0)
        return { QStringLiteral("repeat"), QString() };   // ranges-only, or restated same setup
    if (grindMoved)
        return { QStringLiteral("grind"), grindDir };
    if (doseMoved)
        return { QStringLiteral("dose"), doseDir };
    return { QStringLiteral("profile"), QStringLiteral("switch") };
}

CoachPlanStorage::TierResult
CoachPlanStorage::classifyTier(const QVariantMap& plan, const ShotProjection& anchor, const ShotProjection& followUp)
{
    // --- Tier C: never evidence. Ordered so the disqualifiers are checked before the A/B split. ---
    if (plan.value(QStringLiteral("adherence")).toString() != QLatin1String("followed"))
        return { QStringLiteral("C"), QString() };       // ignored / partial / unclear
    if (followUp.channelingDetected)
        return { QStringLiteral("C"), QString() };       // a channeled follow-up can't confirm/refute the lever
    const bool sameBean =
        plan.value(QStringLiteral("beanBrand")).toString().compare(followUp.beanBrand, Qt::CaseInsensitive) == 0 &&
        plan.value(QStringLiteral("beanType")).toString().compare(followUp.beanType, Qt::CaseInsensitive) == 0;
    if (!sameBean)
        return { QStringLiteral("C"), QString() };       // bean drift
    const qint64 gap = followUp.timestamp - anchor.timestamp;
    if (gap <= 0 || gap > kMaxOutcomeGapSecs)
        return { QStringLiteral("C"), QString() };       // long gap (or follow-up not actually after the anchor)

    // --- followed + same bean + ≤14 days + not channeled: A if a scoreable directional lever AND both rated. ---
    const QString lever = plan.value(QStringLiteral("lever")).toString();
    const QString direction = plan.value(QStringLiteral("direction")).toString();
    const bool singleDirectionalLever =
        (lever == QLatin1String("grind") || lever == QLatin1String("dose") || lever == QLatin1String("profile"))
        && !direction.isEmpty() && direction != QLatin1String("unclear");
    const bool bothRated = anchor.enjoyment0to100 > 0 && followUp.enjoyment0to100 > 0;
    if (singleDirectionalLever && bothRated) {
        const int d = followUp.enjoyment0to100 - anchor.enjoyment0to100;
        const QString sign = (d > kFlatRatingBand)  ? QStringLiteral("improved")
                           : (d < -kFlatRatingBand) ? QStringLiteral("worse")
                                                    : QStringLiteral("flat");
        return { QStringLiteral("A"), sign };
    }
    return { QStringLiteral("B"), QString() };            // grounded observation: followed + measured, taste/lever short of A
}

bool CoachPlanStorage::ensureSchemaStatic(QSqlDatabase& db)
{
    QSqlQuery query(db);

    if (!query.exec(R"(
        CREATE TABLE IF NOT EXISTS coach_plans (
            id INTEGER PRIMARY KEY,
            created_at INTEGER DEFAULT 0,
            anchor_shot_id INTEGER DEFAULT 0,
            bean_brand TEXT DEFAULT '',
            bean_type TEXT DEFAULT '',
            profile_kb_id TEXT DEFAULT '',
            equipment_id INTEGER DEFAULT 0,
            source TEXT DEFAULT '',
            structured_next TEXT DEFAULT '',
            kb_rec_id TEXT DEFAULT '',
            kb_confidence TEXT DEFAULT '',
            kb_prep_gate INTEGER DEFAULT 0,
            lever TEXT DEFAULT '',
            direction TEXT DEFAULT '',
            follow_up_shot_id INTEGER DEFAULT 0,
            adherence TEXT DEFAULT '',
            in_predicted_range TEXT DEFAULT '',
            status TEXT DEFAULT 'open',
            judged_at INTEGER DEFAULT 0
        )
    )")) {
        qWarning() << "CoachPlanStorage: failed to create coach_plans:" << query.lastError().text();
        return false;
    }

    // NOTE: schema_version is FeedbackStorage's shared marker — never created or touched here
    // (assistant.db has no migration runner; each class self-creates idempotently). W2.

    // Scope lookup for supersede + the judge pass matches on (bean, profile, equipment, status).
    query.exec("CREATE INDEX IF NOT EXISTS idx_coach_plans_scope "
               "ON coach_plans(bean_brand, bean_type, profile_kb_id, equipment_id, status)");
    // The judge pass scans open plans; aggregation/recall reads newest-first by bean.
    query.exec("CREATE INDEX IF NOT EXISTS idx_coach_plans_status ON coach_plans(status)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_coach_plans_created ON coach_plans(created_at DESC)");

    return true;
}

qint64 CoachPlanStorage::insertPlanStatic(QSqlDatabase& db, const QVariantMap& fields)
{
    QStringList columns, placeholders;
    QVariantList binds;
    for (const Col& c : kWritableCols) {
        const QString key = QString::fromLatin1(c.key);
        columns << QString::fromLatin1(c.sql);
        placeholders << QStringLiteral("?");
        // Missing keys -> NULL, which falls back to the column DEFAULT.
        binds << (fields.contains(key) ? fields.value(key) : QVariant());
    }
    // created_at: use the supplied value or stamp now.
    columns << QStringLiteral("created_at");
    placeholders << QStringLiteral("?");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    binds << (fields.contains(QStringLiteral("createdAt"))
                  ? fields.value(QStringLiteral("createdAt")).toLongLong()
                  : now);

    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT INTO coach_plans (%1) VALUES (%2)")
                      .arg(columns.join(QStringLiteral(", ")), placeholders.join(QStringLiteral(", "))));
    for (qsizetype i = 0; i < binds.size(); ++i)
        query.bindValue(static_cast<int>(i), binds.at(i));

    if (!query.exec()) {
        qWarning() << "CoachPlanStorage: insert failed:" << query.lastError().text();
        return -1;
    }
    return query.lastInsertId().toLongLong();
}

int CoachPlanStorage::supersedeOpenPlansStatic(QSqlDatabase& db,
                                               const QString& beanBrand, const QString& beanType,
                                               const QString& profileKbId, qint64 equipmentId,
                                               qint64 exceptId)
{
    QSqlQuery query(db);
    query.prepare(
        "UPDATE coach_plans SET status = 'superseded' "
        "WHERE status = 'open' AND id != :exceptId "
        "AND LOWER(COALESCE(bean_brand,'')) = LOWER(:brand) "
        "AND LOWER(COALESCE(bean_type,'')) = LOWER(:type) "
        "AND COALESCE(profile_kb_id,'') = :profile "
        "AND COALESCE(equipment_id,0) = :equipment");
    query.bindValue(":exceptId", exceptId);
    query.bindValue(":brand", beanBrand);
    query.bindValue(":type", beanType);
    query.bindValue(":profile", profileKbId);
    query.bindValue(":equipment", equipmentId);
    if (!query.exec()) {
        qWarning() << "CoachPlanStorage: supersede failed:" << query.lastError().text();
        return 0;
    }
    return query.numRowsAffected();
}

QVariantList CoachPlanStorage::fetchOpenPlansStatic(QSqlDatabase& db, int limit)
{
    QVariantList rows;
    const int cappedLimit = (limit > 0 && limit <= 500) ? limit : 100;
    QSqlQuery query(db);
    query.prepare(QString::fromLatin1(kSelectAllCols) +
                  " WHERE status = 'open' ORDER BY created_at ASC, id ASC LIMIT :limit");
    query.bindValue(":limit", cappedLimit);
    if (!query.exec()) {
        qWarning() << "CoachPlanStorage: fetchOpenPlans failed:" << query.lastError().text();
        return rows;
    }
    while (query.next())
        rows.append(rowToMap(query));
    return rows;
}

bool CoachPlanStorage::writeJudgmentStatic(QSqlDatabase& db, qint64 id,
                                           const QString& adherence,
                                           const QString& inPredictedRangeJson,
                                           qint64 followUpShotId, qint64 judgedAt)
{
    QSqlQuery query(db);
    // Guard on status='open' so a judged row can never be overwritten (immutability) and a
    // concurrent second judge pass no-ops rather than re-freezing.
    query.prepare(
        "UPDATE coach_plans SET status = 'judged', adherence = :adherence, "
        "in_predicted_range = :range, follow_up_shot_id = :followUp, judged_at = :judgedAt "
        "WHERE id = :id AND status = 'open'");
    query.bindValue(":adherence", adherence);
    query.bindValue(":range", inPredictedRangeJson);
    query.bindValue(":followUp", followUpShotId);
    query.bindValue(":judgedAt", judgedAt);
    query.bindValue(":id", id);
    if (!query.exec()) {
        qWarning() << "CoachPlanStorage: writeJudgment failed:" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() == 1;
}

QVariantList CoachPlanStorage::fetchPlansStatic(QSqlDatabase& db,
                                               const QString& beanBrand, const QString& beanType,
                                               const QString& lever, const QString& statusFilter,
                                               int limit)
{
    QVariantList rows;
    const int cappedLimit = (limit > 0 && limit <= 200) ? limit : 50;

    QStringList where;
    if (!beanBrand.isEmpty() || !beanType.isEmpty()) {
        where << "LOWER(COALESCE(bean_brand,'')) = LOWER(:brand)";
        where << "LOWER(COALESCE(bean_type,'')) = LOWER(:type)";
    }
    if (!lever.isEmpty())
        where << "lever = :lever";
    if (!statusFilter.isEmpty())
        where << "status = :status";

    QString sql = QString::fromLatin1(kSelectAllCols);
    if (!where.isEmpty())
        sql += " WHERE " + where.join(QStringLiteral(" AND "));
    sql += " ORDER BY created_at DESC, id DESC LIMIT :limit";

    QSqlQuery query(db);
    query.prepare(sql);
    if (!beanBrand.isEmpty() || !beanType.isEmpty()) {
        query.bindValue(":brand", beanBrand);
        query.bindValue(":type", beanType);
    }
    if (!lever.isEmpty())
        query.bindValue(":lever", lever);
    if (!statusFilter.isEmpty())
        query.bindValue(":status", statusFilter);
    query.bindValue(":limit", cappedLimit);

    if (!query.exec()) {
        qWarning() << "CoachPlanStorage: fetchPlans failed:" << query.lastError().text();
        return rows;
    }
    while (query.next())
        rows.append(rowToMap(query));
    return rows;
}
