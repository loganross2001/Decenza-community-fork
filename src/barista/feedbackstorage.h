#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <memory>

class QSqlDatabase;
class SerialDbWorker;

// [barista-fork] Async storage for the barista's verbal tasting-feedback knowledge base.
//
// Owns a SEPARATE `assistant.db` (NOT shots.db) placed in the SAME app-data directory as
// shots.db. Keeping the schema out of shots.db avoids the upstream-migration collision the
// fork already hit once (barista roster vs visualizer both landing on migration 24): every
// new shots.db migration re-runs that risk, whereas assistant.db isolates all fork schema
// from upstream's chain entirely and is independently relocatable.
//
// Mirrors BaristaStorage: all public request* methods are async — DB work runs on a serial
// background worker (SerialDbWorker) and results come back via signals. Synchronous *Static
// helpers take a caller-provided connection and are shared with the proactive-context read
// path in AIManager (which opens its own withTempDb on assistant.db) and the search tool.
//
// Schema (schema_version starts at 1; no migration runner yet):
//   shot_feedback(id INTEGER PRIMARY KEY, shot_id INTEGER, bean_brand TEXT, bean_type TEXT,
//                 profile TEXT, dose_g REAL, yield_g REAL, grind TEXT, temp_c REAL,
//                 raw_text TEXT, descriptors TEXT, structured_json TEXT, rating_0to100 INTEGER,
//                 source TEXT, created_at INTEGER)
//   feedback_fts USING fts5(raw_text, descriptors, bean_brand, bean_type, profile,
//                           content='shot_feedback')  -- kept in sync via triggers
//
// SPEC RECONCILIATION: the design doc lists a `descriptors` column in feedback_fts but not in
// shot_feedback. An external-content FTS5 table's triggers reference `new.<col>` on the CONTENT
// table by name, so a `descriptors` FTS column with no backing column cannot sync. We therefore
// add a real `descriptors TEXT` column to shot_feedback (populated app-side by joining the
// model's descriptors[] array), matching the shots_fts external-content trigger idiom exactly.
class FeedbackStorage : public QObject {
    Q_OBJECT

public:
    explicit FeedbackStorage(QObject* parent = nullptr);
    ~FeedbackStorage();

    // dbPath must be the assistant.db path (derived beside shots.db by the caller).
    void initialize(const QString& dbPath);
    QString databasePath() const { return m_dbPath; }

    // Async write — emits feedbackLogged(id) (id -1 on failure). `fields` is a whitelisted
    // QVariantMap with column keys: shotId, beanBrand, beanType, profile, doseG, yieldG,
    // grind, tempC, rawText, descriptors, structuredJson, rating0to100, source. Missing keys
    // fall back to column defaults. created_at is stamped app-side if absent.
    Q_INVOKABLE void requestLogFeedback(const QVariantMap& fields);   // feedbackLogged(qint64 id)

    // Async read — top-N past feedback for a bean, optionally FTS-filtered by descriptor text.
    // Result via feedbackForBeanReady (QVariantList of row maps). descriptorFilter is a plain
    // FTS5 term string; empty means "all feedback for this bean, newest first".
    Q_INVOKABLE void requestFeedbackForBean(const QString& beanBrand, const QString& beanType,
                                            const QString& descriptorFilter = QString());

    // --- Synchronous static helpers (caller provides an OPEN assistant.db connection) ---

    // Create shot_feedback + feedback_fts + triggers + schema_version if missing. Idempotent.
    static bool ensureSchemaStatic(QSqlDatabase& db);

    // Insert one feedback row from a whitelisted field map. Returns new id, or -1 on failure.
    static qint64 insertFeedbackStatic(QSqlDatabase& db, const QVariantMap& fields);

    // Load up to `limit` feedback rows for the given bean (case-insensitive exact brand+type),
    // newest first. If descriptorFilter is non-empty, restrict to rows matching it via FTS.
    // Returns a QVariantList of row maps (same keys as the field map plus id + createdAt).
    static QVariantList fetchFeedbackForBeanStatic(QSqlDatabase& db,
                                                   const QString& beanBrand, const QString& beanType,
                                                   const QString& descriptorFilter, int limit);

signals:
    void feedbackLogged(qint64 id);                       // id -1 on failure
    void feedbackForBeanReady(const QVariantList& rows);

private:
    void runAsync(const QString& connPrefix,
                  std::function<void(QSqlDatabase&)> work,
                  std::function<void(bool dbOpened)> done);

    QString m_dbPath;
    std::shared_ptr<std::atomic<bool>> m_destroyed = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<SerialDbWorker> m_dbWorker;
};
