#include "feedbackstorage.h"
#include "../core/dbutils.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDatabase>
#include <QDateTime>
#include <QDebug>

namespace {

// Single source of truth for the writable shot_feedback columns. `key` is the camelCase
// QVariantMap key the tool executor supplies; `sql` is the column name. `id` and `created_at`
// are handled specially (id is autoincrement; created_at is stamped app-side).
struct Col { const char* sql; const char* key; };

const Col kWritableCols[] = {
    { "shot_id",        "shotId" },
    { "bean_brand",     "beanBrand" },
    { "bean_type",      "beanType" },
    { "profile",        "profile" },
    { "dose_g",         "doseG" },
    { "yield_g",        "yieldG" },
    { "grind",          "grind" },
    { "temp_c",         "tempC" },
    { "raw_text",       "rawText" },
    { "descriptors",    "descriptors" },
    { "structured_json","structuredJson" },
    { "rating_0to100",  "rating0to100" },
    { "source",         "source" },
};

} // namespace

FeedbackStorage::FeedbackStorage(QObject* parent)
    : QObject(parent)
{
}

FeedbackStorage::~FeedbackStorage()
{
    // Suppress in-flight result callbacks, then stop the worker before members vanish
    // (same rationale as BaristaStorage::~BaristaStorage / CoffeeBagStorage).
    *m_destroyed = true;
    m_dbWorker.reset();
}

void FeedbackStorage::initialize(const QString& dbPath)
{
    m_dbPath = dbPath;
}

void FeedbackStorage::runAsync(const QString& connPrefix,
                               std::function<void(QSqlDatabase&)> work,
                               std::function<void(bool dbOpened)> done)
{
    if (m_dbPath.isEmpty()) {
        qWarning() << "FeedbackStorage: not initialized, dropping" << connPrefix;
        return;
    }
    if (!m_dbWorker)
        m_dbWorker = std::make_unique<SerialDbWorker>(QStringLiteral("FeedbackStorageWorker"));
    m_dbWorker->run(m_dbPath, connPrefix, std::move(work), std::move(done), this, m_destroyed);
}

void FeedbackStorage::requestLogFeedback(const QVariantMap& fields)
{
    // Guarantee a terminal feedbackLogged even when uninitialized so a caller arming a
    // one-shot response doesn't hang.
    if (m_dbPath.isEmpty()) {
        qWarning() << "FeedbackStorage: requestLogFeedback on uninitialized storage";
        emit feedbackLogged(-1);
        return;
    }
    auto newId = std::make_shared<qint64>(-1);
    runAsync("feedback_log",
        [fields, newId](QSqlDatabase& db) {
            // The write path shares ensureSchemaStatic so a first-ever write self-creates the
            // schema (assistant.db may not exist yet on a fresh install).
            if (!FeedbackStorage::ensureSchemaStatic(db))
                return;
            *newId = FeedbackStorage::insertFeedbackStatic(db, fields);
        },
        // Write: emit regardless — *newId is -1 on failure, a terminal status.
        [this, newId](bool) { emit feedbackLogged(*newId); });
}

void FeedbackStorage::requestFeedbackForBean(const QString& beanBrand, const QString& beanType,
                                             const QString& descriptorFilter)
{
    auto rows = std::make_shared<QVariantList>();
    runAsync("feedback_for_bean",
        [rows, beanBrand, beanType, descriptorFilter](QSqlDatabase& db) {
            if (!FeedbackStorage::ensureSchemaStatic(db))
                return;
            *rows = FeedbackStorage::fetchFeedbackForBeanStatic(db, beanBrand, beanType, descriptorFilter, 10);
        },
        // Read: skip the emit on open failure so the caller keeps its current state.
        [this, rows](bool dbOpened) { if (dbOpened) emit feedbackForBeanReady(*rows); });
}

bool FeedbackStorage::ensureSchemaStatic(QSqlDatabase& db)
{
    QSqlQuery query(db);

    if (!query.exec(R"(
        CREATE TABLE IF NOT EXISTS shot_feedback (
            id INTEGER PRIMARY KEY,
            shot_id INTEGER DEFAULT 0,
            bean_brand TEXT DEFAULT '',
            bean_type TEXT DEFAULT '',
            profile TEXT DEFAULT '',
            dose_g REAL DEFAULT 0,
            yield_g REAL DEFAULT 0,
            grind TEXT DEFAULT '',
            temp_c REAL DEFAULT 0,
            raw_text TEXT DEFAULT '',
            descriptors TEXT DEFAULT '',
            structured_json TEXT DEFAULT '',
            rating_0to100 INTEGER DEFAULT 0,
            source TEXT DEFAULT '',
            created_at INTEGER DEFAULT 0
        )
    )")) {
        qWarning() << "FeedbackStorage: failed to create shot_feedback:" << query.lastError().text();
        return false;
    }

    // schema_version: a plain marker table (no migration runner for v1).
    if (!query.exec("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER)")) {
        qWarning() << "FeedbackStorage: failed to create schema_version:" << query.lastError().text();
        return false;
    }
    {
        QSqlQuery vq(db);
        if (vq.exec("SELECT COUNT(*) FROM schema_version") && vq.next() && vq.value(0).toInt() == 0)
            query.exec("INSERT INTO schema_version (version) VALUES (1)");
    }

    // External-content FTS5 over shot_feedback. content_rowid defaults to `rowid`, and
    // `id INTEGER PRIMARY KEY` is the rowid alias — so triggers map new.id/old.id to the FTS
    // rowid. `descriptors` is a REAL column on shot_feedback (see header spec reconciliation),
    // so the trigger name-mapping is exact (mirrors shots_fts in shothistorystorage.cpp).
    if (!query.exec(R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS feedback_fts USING fts5(
            raw_text,
            descriptors,
            bean_brand,
            bean_type,
            profile,
            content='shot_feedback',
            content_rowid='id'
        )
    )")) {
        // FTS failure is not fatal — bean lookups fall back to the non-FTS path.
        qWarning() << "FeedbackStorage: failed to create feedback_fts:" << query.lastError().text();
    }

    query.exec(R"(
        CREATE TRIGGER IF NOT EXISTS shot_feedback_ai AFTER INSERT ON shot_feedback BEGIN
            INSERT INTO feedback_fts(rowid, raw_text, descriptors, bean_brand, bean_type, profile)
            VALUES (new.id, new.raw_text, new.descriptors, new.bean_brand, new.bean_type, new.profile);
        END
    )");
    query.exec(R"(
        CREATE TRIGGER IF NOT EXISTS shot_feedback_ad AFTER DELETE ON shot_feedback BEGIN
            INSERT INTO feedback_fts(feedback_fts, rowid, raw_text, descriptors, bean_brand, bean_type, profile)
            VALUES ('delete', old.id, old.raw_text, old.descriptors, old.bean_brand, old.bean_type, old.profile);
        END
    )");
    query.exec(R"(
        CREATE TRIGGER IF NOT EXISTS shot_feedback_au AFTER UPDATE ON shot_feedback BEGIN
            INSERT INTO feedback_fts(feedback_fts, rowid, raw_text, descriptors, bean_brand, bean_type, profile)
            VALUES ('delete', old.id, old.raw_text, old.descriptors, old.bean_brand, old.bean_type, old.profile);
            INSERT INTO feedback_fts(rowid, raw_text, descriptors, bean_brand, bean_type, profile)
            VALUES (new.id, new.raw_text, new.descriptors, new.bean_brand, new.bean_type, new.profile);
        END
    )");

    query.exec("CREATE INDEX IF NOT EXISTS idx_feedback_bean ON shot_feedback(bean_brand, bean_type)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_feedback_created ON shot_feedback(created_at DESC)");

    return true;
}

qint64 FeedbackStorage::insertFeedbackStatic(QSqlDatabase& db, const QVariantMap& fields)
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
    query.prepare(QStringLiteral("INSERT INTO shot_feedback (%1) VALUES (%2)")
                      .arg(columns.join(QStringLiteral(", ")), placeholders.join(QStringLiteral(", "))));
    for (qsizetype i = 0; i < binds.size(); ++i)
        query.bindValue(static_cast<int>(i), binds.at(i));

    if (!query.exec()) {
        qWarning() << "FeedbackStorage: insert failed:" << query.lastError().text();
        return -1;
    }
    return query.lastInsertId().toLongLong();
}

QVariantList FeedbackStorage::fetchFeedbackForBeanStatic(QSqlDatabase& db,
                                                         const QString& beanBrand, const QString& beanType,
                                                         const QString& descriptorFilter, int limit)
{
    QVariantList rows;
    const int cappedLimit = (limit > 0 && limit <= 50) ? limit : 10;

    QSqlQuery query(db);
    const QString selectCols = QStringLiteral(
        "SELECT id, shot_id, bean_brand, bean_type, profile, dose_g, yield_g, grind, temp_c, "
        "raw_text, descriptors, structured_json, rating_0to100, source, created_at FROM shot_feedback");

    if (!descriptorFilter.trimmed().isEmpty()) {
        // FTS-filtered: rows for this bean whose text/descriptors match the filter, newest first.
        // All values bound — no string interpolation into SQL (injection-safe).
        query.prepare(selectCols +
            " WHERE id IN (SELECT rowid FROM feedback_fts WHERE feedback_fts MATCH :match) "
            "AND LOWER(COALESCE(bean_brand,'')) = LOWER(:brand) "
            "AND LOWER(COALESCE(bean_type,'')) = LOWER(:type) "
            "ORDER BY created_at DESC LIMIT :limit");
        query.bindValue(":match", descriptorFilter.trimmed());
        query.bindValue(":brand", beanBrand);
        query.bindValue(":type", beanType);
        query.bindValue(":limit", cappedLimit);
    } else {
        query.prepare(selectCols +
            " WHERE LOWER(COALESCE(bean_brand,'')) = LOWER(:brand) "
            "AND LOWER(COALESCE(bean_type,'')) = LOWER(:type) "
            "ORDER BY created_at DESC LIMIT :limit");
        query.bindValue(":brand", beanBrand);
        query.bindValue(":type", beanType);
        query.bindValue(":limit", cappedLimit);
    }

    if (!query.exec()) {
        qWarning() << "FeedbackStorage: fetchFeedbackForBean failed:" << query.lastError().text();
        return rows;
    }
    while (query.next()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"),             query.value(0).toLongLong());
        m.insert(QStringLiteral("shotId"),         query.value(1).toLongLong());
        m.insert(QStringLiteral("beanBrand"),      query.value(2).toString());
        m.insert(QStringLiteral("beanType"),       query.value(3).toString());
        m.insert(QStringLiteral("profile"),        query.value(4).toString());
        m.insert(QStringLiteral("doseG"),          query.value(5).toDouble());
        m.insert(QStringLiteral("yieldG"),         query.value(6).toDouble());
        m.insert(QStringLiteral("grind"),          query.value(7).toString());
        m.insert(QStringLiteral("tempC"),          query.value(8).toDouble());
        m.insert(QStringLiteral("rawText"),        query.value(9).toString());
        m.insert(QStringLiteral("descriptors"),    query.value(10).toString());
        m.insert(QStringLiteral("structuredJson"), query.value(11).toString());
        m.insert(QStringLiteral("rating0to100"),   query.value(12).toInt());
        m.insert(QStringLiteral("source"),         query.value(13).toString());
        m.insert(QStringLiteral("createdAt"),      query.value(14).toLongLong());
        rows.append(m);
    }
    return rows;
}
