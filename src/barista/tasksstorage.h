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

// [barista-fork] Async storage for the barista's REMINDERS and MAINTENANCE tracking.
//
// Shares the SAME `assistant.db` as FeedbackStorage (placed beside shots.db). Keeping this schema
// out of shots.db avoids upstream migration collisions, exactly like FeedbackStorage — and the
// two stores co-exist in one file (each ensureSchema is idempotent and leaves the shared
// `schema_version` marker FeedbackStorage owns untouched).
//
// Mirrors FeedbackStorage: async request* methods run DB work on a serial background worker and
// deliver results via signals; synchronous *Static helpers take a caller-provided OPEN connection
// and are shared with the AIManager proactive-context read (the "due items" block) and the barista
// client-tool executor (create_reminder / list_due_reminders / complete_reminder / log_maintenance).
//
// Schema (all in assistant.db):
//   reminders(id INTEGER PRIMARY KEY, text TEXT, due_at INTEGER, recurrence TEXT,
//             user_phrasing TEXT, created_at INTEGER, completed_at INTEGER DEFAULT 0)
//     due_at: epoch seconds the reminder is due. completed_at: 0 = open, else epoch seconds done.
//     recurrence: '' | 'daily' | 'weekly' | 'monthly' (best-effort next-occurrence on complete).
//     user_phrasing: the user's own words for the due ("Saturday"), passed through for provenance.
//
//   personal_dates(id INTEGER PRIMARY KEY, label TEXT, month INTEGER, day INTEGER, year INTEGER DEFAULT 0,
//                  created_at INTEGER)
//     [barista-fork] The owner's own important days ("remember my anniversary is June 3"), added by voice
//     via the add_personal_date client tool. month 1-12, day 1-31. year 0 = recurs every year (the common
//     case); a non-zero year pins it to one occurrence. Combined with the built-in US-holiday lookup, these
//     feed the barista's greeting/goodbye "todaysOccasion" block. Rides the shared assistant.db (backed up
//     wholesale by DatabaseBackupManager — no per-table registration needed).
//
//   maintenance_tasks(task_key TEXT PRIMARY KEY, label TEXT, interval_days INTEGER,
//                     enabled INTEGER DEFAULT 1, last_done_at INTEGER DEFAULT 0,
//                     is_default INTEGER DEFAULT 1, note TEXT DEFAULT '')
//     Seeded from a CLEARLY-LABELED editable default schedule of common Decent DE1 tasks (see
//     kDefaultMaintenance in the .cpp). interval_days / enabled are the OWNER'S OVERRIDE — once the
//     owner edits a row it is authoritative over the seeded default (is_default flips to 0). Due =
//     last_done_at + interval_days*86400 (or never-done → due now). The seeded intervals are
//     CONSERVATIVE PLACEHOLDERS to be confirmed against Decent's published maintenance schedule —
//     surfaced to the user + the model as such (no-fabrication guard); never presented as authoritative.
//
//   maintenance_doc_state(id INTEGER PRIMARY KEY CHECK(id=1), enabled INTEGER DEFAULT 1,
//                         last_checked_at INTEGER DEFAULT 0, baseline_hash TEXT DEFAULT '',
//                         current_hash TEXT DEFAULT '', doc_text TEXT DEFAULT '',
//                         reviewed INTEGER DEFAULT 1)
//     [barista-fork] Single-row (id=1) state for the periodic "Decent maintenance-docs check" feature
//     (MaintenanceDocSync): the owner asked the app to occasionally re-read Decent's DE1 Quickstart
//     cleaning section and PROPOSE default-interval updates (approve-then-apply, never silent).
//       enabled         — the owner's toggle for the periodic check (default ON). Only egress is a GET
//                         to decentespresso.com; nothing is ever sent out.
//       last_checked_at — epoch secs of the last successful fetch (the ~30-day rate-limit gate).
//       baseline_hash   — hash of the last-ACKNOWLEDGED normalized doc text (baseline on first fetch;
//                         advanced to current_hash on accept OR dismiss so a change is offered ONCE).
//       current_hash    — hash of the most recently fetched normalized doc text.
//       doc_text        — the fetched normalized text (bounded), so the barista can reason over it.
//       reviewed        — 1 when current_hash == baseline_hash (nothing to offer); 0 when a NEW change
//                         is pending the owner's review. The barista offers only while reviewed==0.
class TasksStorage : public QObject {
    Q_OBJECT

public:
    explicit TasksStorage(QObject* parent = nullptr);
    ~TasksStorage();

    // dbPath must be the assistant.db path (derived beside shots.db by the caller — same file as
    // FeedbackStorage).
    void initialize(const QString& dbPath);
    QString databasePath() const { return m_dbPath; }

    // --- Reminders (async) ---

    // Create a reminder. `fields` keys: text (QString, required), dueAt (qint64 epoch secs),
    // recurrence (QString), userPhrasing (QString). Emits reminderCreated(id) (id -1 on failure).
    Q_INVOKABLE void requestCreateReminder(const QVariantMap& fields);   // reminderCreated(qint64)
    // Open reminders due at/before `nowEpoch` (0 → now), newest-due first. Emits dueRemindersReady.
    Q_INVOKABLE void requestDueReminders(qint64 nowEpoch = 0);           // dueRemindersReady(rows)
    // Mark a reminder complete (recurring → its due_at rolls forward, staying open). Emits
    // reminderCompleted(id) (id -1 on failure / not found).
    Q_INVOKABLE void requestCompleteReminder(qint64 reminderId);        // reminderCompleted(qint64)

    // --- Personal dates (async) — the owner's own important days ("remember my anniversary is June 3") ---

    // Store one personal date. `fields` keys: label (QString, required), month (int 1-12, required),
    // day (int 1-31, required), year (int, optional 0 = recurs every year). Emits personalDateAdded(id)
    // (id -1 on failure). Wired through the add_personal_date barista client tool.
    Q_INVOKABLE void requestAddPersonalDate(const QVariantMap& fields);   // personalDateAdded(qint64)

    // --- User facts (async) — durable BASIC facts the user tells the barista ("my daughter Audrey is at UW") ---

    // Store one durable fact. `fields` keys: user (QString, active roster user, '' = unattributed),
    // fact (QString, required, short canonical statement), category (QString, optional). Dedupes on
    // (user, fact) case-insensitively — a repeat re-emits the existing id, no duplicate row. Emits
    // userFactAdded(id) (id -1 on failure). Wired through the remember_fact barista client tool.
    Q_INVOKABLE void requestAddUserFact(const QVariantMap& fields);       // userFactAdded(qint64)
    // Forget facts. `fields` keys: user (QString), fact (QString, required — a substring identifying the
    // fact, matched case-insensitively within that user's rows). Emits userFactForgotten(count removed).
    Q_INVOKABLE void requestForgetUserFact(const QVariantMap& fields);    // userFactForgotten(int)

    // --- Maintenance (async, for the settings dialog) ---

    // All maintenance tasks (enabled + disabled), for the settings dialog. Emits maintenanceTasksReady.
    Q_INVOKABLE void requestMaintenanceTasks();                          // maintenanceTasksReady(rows)
    // Edit one task's interval / enabled / note (owner override — flips is_default off). `fields`
    // keys: taskKey (required), intervalDays (int), enabled (bool), note (QString). Missing keys
    // are left unchanged. Emits maintenanceTaskUpdated(taskKey).
    Q_INVOKABLE void requestUpdateMaintenanceTask(const QVariantMap& fields);
    // Mark a maintenance task done now (last_done_at = now). Emits maintenanceLogged(taskKey).
    Q_INVOKABLE void requestLogMaintenance(const QString& taskKey, qint64 whenEpoch = 0);

    // --- Maintenance-doc sync state (async, for the settings dialog + the periodic check service) ---

    // Read the single-row doc-sync state (enabled, lastCheckedAt, baselineHash, currentHash,
    // reviewed, docText). Emits maintenanceDocStateReady with a map of those keys. Seeds the row
    // (enabled=1, reviewed=1) on first read so the dialog always has a value to bind.
    Q_INVOKABLE void requestMaintenanceDocState();       // maintenanceDocStateReady(state)
    // Flip the owner's periodic-check toggle. Emits maintenanceDocStateReady with the fresh state.
    Q_INVOKABLE void setMaintenanceDocSyncEnabled(bool enabled);
    // Store a freshly-fetched doc: records last_checked_at=now + doc_text, updates current_hash, and
    // computes `reviewed` (1 if hash unchanged from baseline; on the FIRST fetch baseline is seeded to
    // the fetched hash so it is silent — reviewed=1). Called by MaintenanceDocSync after a GET. Emits
    // maintenanceDocStateReady with the resulting state (so the dialog's last-checked date refreshes).
    Q_INVOKABLE void recordFetchedMaintenanceDoc(const QString& normalizedText, const QString& hash);

    // --- Synchronous static helpers (caller provides an OPEN assistant.db connection) ---

    // Create reminders + maintenance tables and seed the default maintenance schedule if the table
    // is empty. Idempotent; never touches FeedbackStorage's shared schema_version marker.
    static bool ensureSchemaStatic(QSqlDatabase& db);

    static qint64 insertReminderStatic(QSqlDatabase& db, const QVariantMap& fields);

    // [barista-fork] Personal dates. insert returns the new id (-1 on failure). fetchForToday returns the
    // personal dates whose (month, day) match the given local date — both year-specific rows (year == the
    // given year) and recurring rows (year == 0). Each row carries: id, label, month, day, year.
    static qint64 insertPersonalDateStatic(QSqlDatabase& db, const QVariantMap& fields);
    static QVariantList fetchPersonalDatesForTodayStatic(QSqlDatabase& db, int month, int day, int year);

    // [barista-fork] User facts. insert dedupes on (user, fact) case-insensitively → returns the existing id
    // when the fact is already stored (never a duplicate row), else the new id (-1 on failure). fetch returns
    // that user's facts PLUS unattributed (user='') rows, newest first, capped. delete removes rows for `user`
    // whose fact contains `factSubstr` (case-insensitive) and returns the number removed.
    static qint64 insertUserFactStatic(QSqlDatabase& db, const QVariantMap& fields);
    static QVariantList fetchUserFactsStatic(QSqlDatabase& db, const QString& user, int cap);
    static int deleteUserFactsStatic(QSqlDatabase& db, const QString& user, const QString& factSubstr);
    // Open reminders with due_at <= nowEpoch, newest-due first (limit capped). Rows carry:
    // id, text, dueAt, recurrence, userPhrasing, createdAt.
    static QVariantList fetchDueRemindersStatic(QSqlDatabase& db, qint64 nowEpoch, int limit);
    // Complete a reminder. Recurring → roll due_at forward by the recurrence and keep it open;
    // one-off → set completed_at. Returns the id on success, -1 on not-found/failure.
    static qint64 completeReminderStatic(QSqlDatabase& db, qint64 reminderId, qint64 nowEpoch);

    static QVariantList fetchMaintenanceTasksStatic(QSqlDatabase& db);
    // DUE maintenance tasks (enabled AND (never done OR last_done + interval <= now)), each with
    // taskKey, label, intervalDays, lastDoneAt, dueSinceDays, isDefault, note. limit capped.
    static QVariantList fetchDueMaintenanceStatic(QSqlDatabase& db, qint64 nowEpoch, int limit);
    static bool updateMaintenanceTaskStatic(QSqlDatabase& db, const QVariantMap& fields);
    // Record a completion (last_done_at). Returns true on success. Accepts a task_key that already
    // exists; a non-existent key is a no-op returning false (never fabricates a task).
    static bool logMaintenanceStatic(QSqlDatabase& db, const QString& taskKey, qint64 whenEpoch);

    // [barista-fork] Apply a Decent-doc-sourced DEFAULT update to ONE task. UNLIKE updateMaintenanceTaskStatic
    // (which flips is_default→0 to record an owner override), this KEEPS is_default=1 (still a default, just a
    // new value from Decent's guide) and only ever touches a row that is STILL on its default — the
    // `AND is_default = 1` clause is the guard that makes an owner-overridden row untouchable. Returns true
    // only if a still-default row was updated; false (0 rows) means the task was owner-overridden or absent,
    // which the caller reports back as "skipped (owner override)". label is applied only when non-empty.
    static bool updateMaintenanceDefaultStatic(QSqlDatabase& db, const QString& taskKey,
                                               int intervalDays, const QString& label);

    // --- Maintenance-doc sync state (single row id=1) ---

    // Create the maintenance_doc_state table + seed the single row (enabled=1, reviewed=1) if absent.
    // Idempotent; called by ensureSchemaStatic and safe to call directly.
    static bool ensureDocStateSchemaStatic(QSqlDatabase& db);
    // Read the single-row state as a map: enabled(bool), lastCheckedAt(qint64), baselineHash(QString),
    // currentHash(QString), reviewed(bool), docText(QString). Seeds the row if missing.
    static QVariantMap fetchDocStateStatic(QSqlDatabase& db);
    // Set the owner's periodic-check toggle. Returns true on success.
    static bool setDocSyncEnabledStatic(QSqlDatabase& db, bool enabled);
    // Store a freshly-fetched doc (normalizedText + hash). On the FIRST fetch (empty baseline) this seeds
    // baseline=current=hash and reviewed=1 (silent baseline — no offer). On a later fetch it sets
    // current_hash+doc_text+last_checked_at and reviewed = (hash == baseline). Returns true on success.
    static bool recordFetchedDocStatic(QSqlDatabase& db, const QString& normalizedText,
                                       const QString& hash, qint64 whenEpoch);
    // Advance baseline to current (mark the pending change as reviewed) — used on BOTH accept and dismiss,
    // so the same change is never re-offered. Sets reviewed=1. Returns true on success.
    static bool markDocReviewedStatic(QSqlDatabase& db);

signals:
    void reminderCreated(qint64 id);                 // id -1 on failure
    void personalDateAdded(qint64 id);               // [barista-fork] id -1 on failure
    void userFactAdded(qint64 id);                   // [barista-fork] id -1 on failure (dedupe → existing id)
    void userFactForgotten(int removed);             // [barista-fork] number of rows deleted
    void dueRemindersReady(const QVariantList& rows);
    void reminderCompleted(qint64 id);               // id -1 on failure / not found
    void maintenanceTasksReady(const QVariantList& rows);
    void maintenanceTaskUpdated(const QString& taskKey);
    void maintenanceLogged(const QString& taskKey);  // empty on failure
    // [barista-fork] doc-sync state (enabled, lastCheckedAt, baselineHash, currentHash, reviewed, docText).
    void maintenanceDocStateReady(const QVariantMap& state);

private:
    void runAsync(const QString& connPrefix,
                  std::function<void(QSqlDatabase&)> work,
                  std::function<void(bool dbOpened)> done);

    QString m_dbPath;
    std::shared_ptr<std::atomic<bool>> m_destroyed = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<SerialDbWorker> m_dbWorker;
};
