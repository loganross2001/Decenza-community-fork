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
//   maintenance_tasks(task_key TEXT PRIMARY KEY, label TEXT, interval_days INTEGER,
//                     enabled INTEGER DEFAULT 1, last_done_at INTEGER DEFAULT 0,
//                     is_default INTEGER DEFAULT 1, note TEXT DEFAULT '')
//     Seeded from a CLEARLY-LABELED editable default schedule of common Decent DE1 tasks (see
//     kDefaultMaintenance in the .cpp). interval_days / enabled are the OWNER'S OVERRIDE — once the
//     owner edits a row it is authoritative over the seeded default (is_default flips to 0). Due =
//     last_done_at + interval_days*86400 (or never-done → due now). The seeded intervals are
//     CONSERVATIVE PLACEHOLDERS to be confirmed against Decent's published maintenance schedule —
//     surfaced to the user + the model as such (no-fabrication guard); never presented as authoritative.
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

    // --- Maintenance (async, for the settings dialog) ---

    // All maintenance tasks (enabled + disabled), for the settings dialog. Emits maintenanceTasksReady.
    Q_INVOKABLE void requestMaintenanceTasks();                          // maintenanceTasksReady(rows)
    // Edit one task's interval / enabled / note (owner override — flips is_default off). `fields`
    // keys: taskKey (required), intervalDays (int), enabled (bool), note (QString). Missing keys
    // are left unchanged. Emits maintenanceTaskUpdated(taskKey).
    Q_INVOKABLE void requestUpdateMaintenanceTask(const QVariantMap& fields);
    // Mark a maintenance task done now (last_done_at = now). Emits maintenanceLogged(taskKey).
    Q_INVOKABLE void requestLogMaintenance(const QString& taskKey, qint64 whenEpoch = 0);

    // --- Synchronous static helpers (caller provides an OPEN assistant.db connection) ---

    // Create reminders + maintenance tables and seed the default maintenance schedule if the table
    // is empty. Idempotent; never touches FeedbackStorage's shared schema_version marker.
    static bool ensureSchemaStatic(QSqlDatabase& db);

    static qint64 insertReminderStatic(QSqlDatabase& db, const QVariantMap& fields);
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

signals:
    void reminderCreated(qint64 id);                 // id -1 on failure
    void dueRemindersReady(const QVariantList& rows);
    void reminderCompleted(qint64 id);               // id -1 on failure / not found
    void maintenanceTasksReady(const QVariantList& rows);
    void maintenanceTaskUpdated(const QString& taskKey);
    void maintenanceLogged(const QString& taskKey);  // empty on failure

private:
    void runAsync(const QString& connPrefix,
                  std::function<void(QSqlDatabase&)> work,
                  std::function<void(bool dbOpened)> done);

    QString m_dbPath;
    std::shared_ptr<std::atomic<bool>> m_destroyed = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<SerialDbWorker> m_dbWorker;
};
