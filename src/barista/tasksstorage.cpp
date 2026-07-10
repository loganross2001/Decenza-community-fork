#include "tasksstorage.h"
#include "../core/dbutils.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDatabase>
#include <QDateTime>
#include <QDebug>

namespace {

// [barista-fork] Default maintenance schedule for the Decent DE1, sourced from Decent's official
// DE1 Quickstart Guide cleaning section (decentespresso.com/doc/quickstart/).
//
// These reflect Decent's documented daily/weekly/periodic cleaning tasks. Descaling is deliberately
// water-dependent: Decent says water under ~30ppm TDS (RO/distilled/deionized) needs NO descaling,
// 30-120ppm needs roughly ANNUAL descaling with 5% citric acid, and hard water >120ppm needs it every
// 4-8 weeks — so the owner MUST set the descale interval for their own water. Every seeded row is
// is_default=1; the moment the owner edits an interval it becomes their override (is_default=0) and is
// authoritative over this seed. Intervals are Decent's general guidance — adjust for your usage/water.
struct DefaultTask { const char* key; const char* label; int intervalDays; const char* note; };

const DefaultTask kDefaultMaintenance[] = {
    { "flush_group",      "Flush the group head after use",                 1,
      "Decent: quick hot-water flush after each session to clear the group (Flush / Forward Flush)." },
    { "clean_steam_wand", "Clean the steam wand after steaming",            1,
      "Decent daily: purge and wipe the steam wand after each use so milk doesn't dry inside it." },
    { "clean_water_tank", "Clean & refill the water tank",                  1,
      "Decent daily: keep the water tank clean and topped up with fresh low-mineral water." },
    { "clean_drip_tray",  "Empty & rinse the drip tray",                    1,
      "Decent daily: empty and rinse the drip tray to prevent buildup and odor." },
    { "steam_wand_weekly","Deep-clean the steam wand",                      7,
      "Decent weekly: a more thorough steam-wand clean/soak beyond the daily purge." },
    { "backflush",        "Flush the group head with detergent (backflush)",7,
      "Decent weekly: backflush the group with espresso-machine detergent to clear oils." },
    { "soak_group_parts", "Remove & soak the group head parts in detergent",30,
      "Decent periodic: remove the group head parts (incl. shower screen) and soak them in detergent." },
    { "descale",          "Descale the machine",                            365,
      "Decent, WATER-DEPENDENT: none if <30ppm TDS (RO/distilled); ~annual at 30-120ppm (5% citric acid); every 4-8 weeks if >120ppm. Set this for YOUR water." },
};

// Roll an epoch second forward by one recurrence step from `from`. Unknown/empty → 0 (one-off).
qint64 nextOccurrence(const QString& recurrence, qint64 from)
{
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(from);
    const QString r = recurrence.trimmed().toLower();
    if (r == QLatin1String("daily"))   return dt.addDays(1).toSecsSinceEpoch();
    if (r == QLatin1String("weekly"))  return dt.addDays(7).toSecsSinceEpoch();
    if (r == QLatin1String("monthly")) return dt.addMonths(1).toSecsSinceEpoch();
    return 0;
}

} // namespace

TasksStorage::TasksStorage(QObject* parent)
    : QObject(parent)
{
}

TasksStorage::~TasksStorage()
{
    *m_destroyed = true;
    m_dbWorker.reset();
}

void TasksStorage::initialize(const QString& dbPath)
{
    m_dbPath = dbPath;
}

void TasksStorage::runAsync(const QString& connPrefix,
                            std::function<void(QSqlDatabase&)> work,
                            std::function<void(bool dbOpened)> done)
{
    if (m_dbPath.isEmpty()) {
        qWarning() << "TasksStorage: not initialized, dropping" << connPrefix;
        return;
    }
    if (!m_dbWorker)
        m_dbWorker = std::make_unique<SerialDbWorker>(QStringLiteral("TasksStorageWorker"));
    m_dbWorker->run(m_dbPath, connPrefix, std::move(work), std::move(done), this, m_destroyed);
}

// ---------------------------------------------------------------------------- Reminders (async)

void TasksStorage::requestCreateReminder(const QVariantMap& fields)
{
    if (m_dbPath.isEmpty()) {
        qWarning() << "TasksStorage: requestCreateReminder on uninitialized storage";
        emit reminderCreated(-1);
        return;
    }
    auto newId = std::make_shared<qint64>(-1);
    runAsync("reminder_create",
        [fields, newId](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            *newId = TasksStorage::insertReminderStatic(db, fields);
        },
        [this, newId](bool) { emit reminderCreated(*newId); });
}

void TasksStorage::requestDueReminders(qint64 nowEpoch)
{
    const qint64 now = nowEpoch > 0 ? nowEpoch : QDateTime::currentSecsSinceEpoch();
    auto rows = std::make_shared<QVariantList>();
    runAsync("reminder_due",
        [rows, now](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            *rows = TasksStorage::fetchDueRemindersStatic(db, now, 20);
        },
        [this, rows](bool dbOpened) { if (dbOpened) emit dueRemindersReady(*rows); });
}

void TasksStorage::requestCompleteReminder(qint64 reminderId)
{
    if (m_dbPath.isEmpty()) {
        emit reminderCompleted(-1);
        return;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    auto outId = std::make_shared<qint64>(-1);
    runAsync("reminder_complete",
        [reminderId, now, outId](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            *outId = TasksStorage::completeReminderStatic(db, reminderId, now);
        },
        [this, outId](bool) { emit reminderCompleted(*outId); });
}

void TasksStorage::requestAddPersonalDate(const QVariantMap& fields)
{
    if (m_dbPath.isEmpty()) {
        qWarning() << "TasksStorage: requestAddPersonalDate on uninitialized storage";
        emit personalDateAdded(-1);
        return;
    }
    auto newId = std::make_shared<qint64>(-1);
    runAsync("personal_date_add",
        [fields, newId](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            *newId = TasksStorage::insertPersonalDateStatic(db, fields);
        },
        [this, newId](bool) { emit personalDateAdded(*newId); });
}

// -------------------------------------------------------------------------- Maintenance (async)

void TasksStorage::requestMaintenanceTasks()
{
    auto rows = std::make_shared<QVariantList>();
    runAsync("maint_list",
        [rows](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            *rows = TasksStorage::fetchMaintenanceTasksStatic(db);
        },
        [this, rows](bool dbOpened) { if (dbOpened) emit maintenanceTasksReady(*rows); });
}

void TasksStorage::requestUpdateMaintenanceTask(const QVariantMap& fields)
{
    const QString taskKey = fields.value(QStringLiteral("taskKey")).toString();
    if (taskKey.isEmpty() || m_dbPath.isEmpty()) {
        emit maintenanceTaskUpdated(taskKey);
        return;
    }
    runAsync("maint_update",
        [fields](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            TasksStorage::updateMaintenanceTaskStatic(db, fields);
        },
        [this, taskKey](bool) { emit maintenanceTaskUpdated(taskKey); });
}

void TasksStorage::requestLogMaintenance(const QString& taskKey, qint64 whenEpoch)
{
    const qint64 when = whenEpoch > 0 ? whenEpoch : QDateTime::currentSecsSinceEpoch();
    if (taskKey.isEmpty() || m_dbPath.isEmpty()) {
        emit maintenanceLogged(QString());
        return;
    }
    auto ok = std::make_shared<bool>(false);
    runAsync("maint_log",
        [taskKey, when, ok](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            *ok = TasksStorage::logMaintenanceStatic(db, taskKey, when);
        },
        [this, taskKey, ok](bool) { emit maintenanceLogged(*ok ? taskKey : QString()); });
}

// ----------------------------------------------------------------- Maintenance-doc sync (async)

void TasksStorage::requestMaintenanceDocState()
{
    auto state = std::make_shared<QVariantMap>();
    runAsync("docstate_read",
        [state](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            *state = TasksStorage::fetchDocStateStatic(db);
        },
        [this, state](bool dbOpened) { if (dbOpened) emit maintenanceDocStateReady(*state); });
}

void TasksStorage::setMaintenanceDocSyncEnabled(bool enabled)
{
    auto state = std::make_shared<QVariantMap>();
    runAsync("docstate_toggle",
        [enabled, state](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            TasksStorage::setDocSyncEnabledStatic(db, enabled);
            *state = TasksStorage::fetchDocStateStatic(db);
        },
        [this, state](bool dbOpened) { if (dbOpened) emit maintenanceDocStateReady(*state); });
}

void TasksStorage::recordFetchedMaintenanceDoc(const QString& normalizedText, const QString& hash)
{
    if (hash.isEmpty())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    auto state = std::make_shared<QVariantMap>();
    runAsync("docstate_fetch",
        [normalizedText, hash, now, state](QSqlDatabase& db) {
            if (!TasksStorage::ensureSchemaStatic(db))
                return;
            TasksStorage::recordFetchedDocStatic(db, normalizedText, hash, now);
            *state = TasksStorage::fetchDocStateStatic(db);
        },
        [this, state](bool dbOpened) { if (dbOpened) emit maintenanceDocStateReady(*state); });
}

// ---------------------------------------------------------------------------- Static helpers

bool TasksStorage::ensureSchemaStatic(QSqlDatabase& db)
{
    QSqlQuery query(db);

    if (!query.exec(R"(
        CREATE TABLE IF NOT EXISTS reminders (
            id INTEGER PRIMARY KEY,
            text TEXT DEFAULT '',
            due_at INTEGER DEFAULT 0,
            recurrence TEXT DEFAULT '',
            user_phrasing TEXT DEFAULT '',
            created_at INTEGER DEFAULT 0,
            completed_at INTEGER DEFAULT 0
        )
    )")) {
        qWarning() << "TasksStorage: failed to create reminders:" << query.lastError().text();
        return false;
    }

    if (!query.exec(R"(
        CREATE TABLE IF NOT EXISTS maintenance_tasks (
            task_key TEXT PRIMARY KEY,
            label TEXT DEFAULT '',
            interval_days INTEGER DEFAULT 0,
            enabled INTEGER DEFAULT 1,
            last_done_at INTEGER DEFAULT 0,
            is_default INTEGER DEFAULT 1,
            note TEXT DEFAULT ''
        )
    )")) {
        qWarning() << "TasksStorage: failed to create maintenance_tasks:" << query.lastError().text();
        return false;
    }

    // [barista-fork] Personal dates — the owner's own important days (added by voice). Rides this idempotent
    // schema pass; the shared assistant.db is backed up wholesale, so no per-table backup registration needed.
    if (!query.exec(R"(
        CREATE TABLE IF NOT EXISTS personal_dates (
            id INTEGER PRIMARY KEY,
            label TEXT DEFAULT '',
            month INTEGER DEFAULT 0,
            day INTEGER DEFAULT 0,
            year INTEGER DEFAULT 0,
            created_at INTEGER DEFAULT 0
        )
    )")) {
        qWarning() << "TasksStorage: failed to create personal_dates:" << query.lastError().text();
        return false;
    }

    query.exec("CREATE INDEX IF NOT EXISTS idx_reminders_open ON reminders(completed_at, due_at)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_personal_dates_md ON personal_dates(month, day)");

    // Seed the editable-default maintenance schedule only when the table is empty (a first run). We
    // INSERT OR IGNORE by task_key so re-seeding never clobbers an owner's edited row. Seeding only
    // on an empty table means removing all rows and adding new defaults is possible later, and a
    // future added default won't silently reappear after the owner deleted it.
    {
        QSqlQuery cq(db);
        int count = -1;
        if (cq.exec("SELECT COUNT(*) FROM maintenance_tasks") && cq.next())
            count = cq.value(0).toInt();
        if (count == 0) {
            for (const DefaultTask& t : kDefaultMaintenance) {
                QSqlQuery iq(db);
                iq.prepare("INSERT OR IGNORE INTO maintenance_tasks "
                           "(task_key, label, interval_days, enabled, last_done_at, is_default, note) "
                           "VALUES (:key, :label, :interval, 1, 0, 1, :note)");
                iq.bindValue(":key", QString::fromLatin1(t.key));
                iq.bindValue(":label", QString::fromLatin1(t.label));
                iq.bindValue(":interval", t.intervalDays);
                iq.bindValue(":note", QString::fromLatin1(t.note));
                if (!iq.exec())
                    qWarning() << "TasksStorage: seed failed for" << t.key << ":" << iq.lastError().text();
            }
        }
    }

    // [barista-fork] Doc-sync state table (single row id=1) rides the same idempotent schema pass.
    if (!ensureDocStateSchemaStatic(db))
        return false;
    return true;
}

bool TasksStorage::ensureDocStateSchemaStatic(QSqlDatabase& db)
{
    QSqlQuery query(db);
    if (!query.exec(R"(
        CREATE TABLE IF NOT EXISTS maintenance_doc_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            enabled INTEGER DEFAULT 1,
            last_checked_at INTEGER DEFAULT 0,
            baseline_hash TEXT DEFAULT '',
            current_hash TEXT DEFAULT '',
            doc_text TEXT DEFAULT '',
            reviewed INTEGER DEFAULT 1
        )
    )")) {
        qWarning() << "TasksStorage: failed to create maintenance_doc_state:" << query.lastError().text();
        return false;
    }
    // Seed the single row (default ON, nothing pending) on first run. INSERT OR IGNORE keeps an
    // existing owner-toggled row untouched.
    QSqlQuery iq(db);
    iq.prepare("INSERT OR IGNORE INTO maintenance_doc_state "
               "(id, enabled, last_checked_at, baseline_hash, current_hash, doc_text, reviewed) "
               "VALUES (1, 1, 0, '', '', '', 1)");
    if (!iq.exec()) {
        qWarning() << "TasksStorage: seed maintenance_doc_state failed:" << iq.lastError().text();
        return false;
    }
    return true;
}

qint64 TasksStorage::insertReminderStatic(QSqlDatabase& db, const QVariantMap& fields)
{
    const QString text = fields.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty())
        return -1;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare("INSERT INTO reminders (text, due_at, recurrence, user_phrasing, created_at, completed_at) "
              "VALUES (:text, :due, :rec, :phr, :created, 0)");
    q.bindValue(":text", text);
    q.bindValue(":due", fields.value(QStringLiteral("dueAt")).toLongLong());
    q.bindValue(":rec", fields.value(QStringLiteral("recurrence")).toString().trimmed().toLower());
    q.bindValue(":phr", fields.value(QStringLiteral("userPhrasing")).toString().trimmed());
    q.bindValue(":created", now);
    if (!q.exec()) {
        qWarning() << "TasksStorage: reminder insert failed:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

qint64 TasksStorage::insertPersonalDateStatic(QSqlDatabase& db, const QVariantMap& fields)
{
    const QString label = fields.value(QStringLiteral("label")).toString().trimmed();
    const int month = fields.value(QStringLiteral("month")).toInt();
    const int day   = fields.value(QStringLiteral("day")).toInt();
    // Validate app-side so a bad value never persists (the barista resolves the words → numbers).
    if (label.isEmpty() || month < 1 || month > 12 || day < 1 || day > 31)
        return -1;
    const int year = qMax(0, fields.value(QStringLiteral("year")).toInt());   // 0 = recurs yearly
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare("INSERT INTO personal_dates (label, month, day, year, created_at) "
              "VALUES (:label, :month, :day, :year, :created)");
    q.bindValue(":label", label);
    q.bindValue(":month", month);
    q.bindValue(":day", day);
    q.bindValue(":year", year);
    q.bindValue(":created", now);
    if (!q.exec()) {
        qWarning() << "TasksStorage: personal_date insert failed:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

QVariantList TasksStorage::fetchPersonalDatesForTodayStatic(QSqlDatabase& db, int month, int day, int year)
{
    QVariantList rows;
    if (month < 1 || month > 12 || day < 1 || day > 31)
        return rows;
    QSqlQuery q(db);
    // Match today's month/day; include recurring rows (year 0) AND a row pinned to THIS year.
    q.prepare("SELECT id, label, month, day, year FROM personal_dates "
              "WHERE month = :month AND day = :day AND (year = 0 OR year = :year) "
              "ORDER BY created_at ASC");
    q.bindValue(":month", month);
    q.bindValue(":day", day);
    q.bindValue(":year", year);
    if (!q.exec()) {
        qWarning() << "TasksStorage: fetchPersonalDatesForToday failed:" << q.lastError().text();
        return rows;
    }
    while (q.next()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"),    q.value(0).toLongLong());
        m.insert(QStringLiteral("label"), q.value(1).toString());
        m.insert(QStringLiteral("month"), q.value(2).toInt());
        m.insert(QStringLiteral("day"),   q.value(3).toInt());
        m.insert(QStringLiteral("year"),  q.value(4).toInt());
        rows.append(m);
    }
    return rows;
}

QVariantList TasksStorage::fetchDueRemindersStatic(QSqlDatabase& db, qint64 nowEpoch, int limit)
{
    QVariantList rows;
    const int cappedLimit = (limit > 0 && limit <= 50) ? limit : 20;
    QSqlQuery q(db);
    q.prepare("SELECT id, text, due_at, recurrence, user_phrasing, created_at FROM reminders "
              "WHERE completed_at = 0 AND due_at > 0 AND due_at <= :now "
              "ORDER BY due_at DESC LIMIT :limit");
    q.bindValue(":now", nowEpoch);
    q.bindValue(":limit", cappedLimit);
    if (!q.exec()) {
        qWarning() << "TasksStorage: fetchDueReminders failed:" << q.lastError().text();
        return rows;
    }
    while (q.next()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"),           q.value(0).toLongLong());
        m.insert(QStringLiteral("text"),         q.value(1).toString());
        m.insert(QStringLiteral("dueAt"),        q.value(2).toLongLong());
        m.insert(QStringLiteral("recurrence"),   q.value(3).toString());
        m.insert(QStringLiteral("userPhrasing"), q.value(4).toString());
        m.insert(QStringLiteral("createdAt"),    q.value(5).toLongLong());
        rows.append(m);
    }
    return rows;
}

qint64 TasksStorage::completeReminderStatic(QSqlDatabase& db, qint64 reminderId, qint64 nowEpoch)
{
    QSqlQuery sel(db);
    sel.prepare("SELECT due_at, recurrence FROM reminders WHERE id = :id AND completed_at = 0");
    sel.bindValue(":id", reminderId);
    if (!sel.exec() || !sel.next())
        return -1;   // not found or already completed
    const qint64 dueAt = sel.value(0).toLongLong();
    const QString recurrence = sel.value(1).toString();
    const qint64 rolled = nextOccurrence(recurrence, dueAt > 0 ? dueAt : nowEpoch);

    QSqlQuery upd(db);
    if (rolled > 0) {
        // Recurring: keep it OPEN and roll the due date forward (skip any missed occurrences so a
        // long-idle reminder doesn't fire N times in a row).
        qint64 next = rolled;
        while (next <= nowEpoch) {
            const qint64 step = nextOccurrence(recurrence, next);
            if (step <= next) break;   // guard against a non-advancing recurrence
            next = step;
        }
        upd.prepare("UPDATE reminders SET due_at = :due WHERE id = :id");
        upd.bindValue(":due", next);
    } else {
        upd.prepare("UPDATE reminders SET completed_at = :now WHERE id = :id");
        upd.bindValue(":now", nowEpoch);
    }
    upd.bindValue(":id", reminderId);
    if (!upd.exec()) {
        qWarning() << "TasksStorage: completeReminder failed:" << upd.lastError().text();
        return -1;
    }
    return reminderId;
}

QVariantList TasksStorage::fetchMaintenanceTasksStatic(QSqlDatabase& db)
{
    QVariantList rows;
    QSqlQuery q(db);
    if (!q.exec("SELECT task_key, label, interval_days, enabled, last_done_at, is_default, note "
                "FROM maintenance_tasks ORDER BY enabled DESC, label ASC")) {
        qWarning() << "TasksStorage: fetchMaintenanceTasks failed:" << q.lastError().text();
        return rows;
    }
    while (q.next()) {
        QVariantMap m;
        m.insert(QStringLiteral("taskKey"),      q.value(0).toString());
        m.insert(QStringLiteral("label"),        q.value(1).toString());
        m.insert(QStringLiteral("intervalDays"), q.value(2).toInt());
        m.insert(QStringLiteral("enabled"),      q.value(3).toInt() != 0);
        m.insert(QStringLiteral("lastDoneAt"),   q.value(4).toLongLong());
        m.insert(QStringLiteral("isDefault"),    q.value(5).toInt() != 0);
        m.insert(QStringLiteral("note"),         q.value(6).toString());
        rows.append(m);
    }
    return rows;
}

QVariantList TasksStorage::fetchDueMaintenanceStatic(QSqlDatabase& db, qint64 nowEpoch, int limit)
{
    QVariantList rows;
    const int cappedLimit = (limit > 0 && limit <= 50) ? limit : 20;
    QSqlQuery q(db);
    // Enabled AND (never done OR last_done + interval*86400 <= now), longest-overdue first.
    q.prepare("SELECT task_key, label, interval_days, last_done_at, is_default, note "
              "FROM maintenance_tasks "
              "WHERE enabled = 1 AND interval_days > 0 "
              "AND (last_done_at = 0 OR (last_done_at + interval_days * 86400) <= :now) "
              "ORDER BY last_done_at ASC LIMIT :limit");
    q.bindValue(":now", nowEpoch);
    q.bindValue(":limit", cappedLimit);
    if (!q.exec()) {
        qWarning() << "TasksStorage: fetchDueMaintenance failed:" << q.lastError().text();
        return rows;
    }
    while (q.next()) {
        const int intervalDays = q.value(2).toInt();
        const qint64 lastDone = q.value(3).toLongLong();
        QVariantMap m;
        m.insert(QStringLiteral("taskKey"),      q.value(0).toString());
        m.insert(QStringLiteral("label"),        q.value(1).toString());
        m.insert(QStringLiteral("intervalDays"), intervalDays);
        m.insert(QStringLiteral("lastDoneAt"),   lastDone);
        m.insert(QStringLiteral("isDefault"),    q.value(4).toInt() != 0);
        m.insert(QStringLiteral("note"),         q.value(5).toString());
        // How many days overdue (0 = never done). Lets the model say "a few days overdue" honestly.
        if (lastDone > 0) {
            const qint64 dueAt = lastDone + static_cast<qint64>(intervalDays) * 86400;
            m.insert(QStringLiteral("overdueDays"),
                     static_cast<int>((nowEpoch - dueAt) / 86400));
        }
        rows.append(m);
    }
    return rows;
}

bool TasksStorage::updateMaintenanceTaskStatic(QSqlDatabase& db, const QVariantMap& fields)
{
    const QString taskKey = fields.value(QStringLiteral("taskKey")).toString();
    if (taskKey.isEmpty())
        return false;

    QStringList sets;
    QVariantMap binds;
    if (fields.contains(QStringLiteral("intervalDays"))) {
        sets << QStringLiteral("interval_days = :interval");
        binds.insert(QStringLiteral(":interval"), qMax(0, fields.value(QStringLiteral("intervalDays")).toInt()));
    }
    if (fields.contains(QStringLiteral("enabled"))) {
        sets << QStringLiteral("enabled = :enabled");
        binds.insert(QStringLiteral(":enabled"), fields.value(QStringLiteral("enabled")).toBool() ? 1 : 0);
    }
    if (fields.contains(QStringLiteral("note"))) {
        sets << QStringLiteral("note = :note");
        binds.insert(QStringLiteral(":note"), fields.value(QStringLiteral("note")).toString());
    }
    if (sets.isEmpty())
        return false;
    // Any owner edit flips is_default off — the row is now the owner's authoritative override.
    sets << QStringLiteral("is_default = 0");

    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE maintenance_tasks SET ") + sets.join(QStringLiteral(", "))
              + QStringLiteral(" WHERE task_key = :key"));
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
        q.bindValue(it.key(), it.value());
    q.bindValue(QStringLiteral(":key"), taskKey);
    if (!q.exec()) {
        qWarning() << "TasksStorage: updateMaintenanceTask failed:" << q.lastError().text();
        return false;
    }
    return q.numRowsAffected() > 0;
}

bool TasksStorage::logMaintenanceStatic(QSqlDatabase& db, const QString& taskKey, qint64 whenEpoch)
{
    if (taskKey.trimmed().isEmpty())
        return false;
    QSqlQuery q(db);
    q.prepare("UPDATE maintenance_tasks SET last_done_at = :when WHERE task_key = :key");
    q.bindValue(":when", whenEpoch);
    q.bindValue(":key", taskKey.trimmed());
    if (!q.exec()) {
        qWarning() << "TasksStorage: logMaintenance failed:" << q.lastError().text();
        return false;
    }
    return q.numRowsAffected() > 0;   // false = no such task_key (never fabricates a task)
}

bool TasksStorage::updateMaintenanceDefaultStatic(QSqlDatabase& db, const QString& taskKey,
                                                  int intervalDays, const QString& label)
{
    const QString key = taskKey.trimmed();
    if (key.isEmpty() || intervalDays < 0)
        return false;

    QStringList sets;
    sets << QStringLiteral("interval_days = :interval");
    // Keep is_default=1 explicitly (a Decent-sourced default value is STILL a default, not an owner
    // override) — harmless on a row that's already 1, and it re-asserts the class if ever needed.
    sets << QStringLiteral("is_default = 1");
    const QString trimmedLabel = label.trimmed();
    if (!trimmedLabel.isEmpty())
        sets << QStringLiteral("label = :label");

    QSqlQuery q(db);
    // The `AND is_default = 1` clause is the GUARD: an owner-overridden row (is_default=0) matches zero
    // rows and is left completely untouched.
    q.prepare(QStringLiteral("UPDATE maintenance_tasks SET ") + sets.join(QStringLiteral(", "))
              + QStringLiteral(" WHERE task_key = :key AND is_default = 1"));
    q.bindValue(QStringLiteral(":interval"), qMax(0, intervalDays));
    if (!trimmedLabel.isEmpty())
        q.bindValue(QStringLiteral(":label"), trimmedLabel);
    q.bindValue(QStringLiteral(":key"), key);
    if (!q.exec()) {
        qWarning() << "TasksStorage: updateMaintenanceDefault failed:" << q.lastError().text();
        return false;
    }
    // 0 rows → the task was owner-overridden (is_default=0) or does not exist; caller reports "skipped".
    return q.numRowsAffected() > 0;
}

QVariantMap TasksStorage::fetchDocStateStatic(QSqlDatabase& db)
{
    ensureDocStateSchemaStatic(db);
    QVariantMap out;
    QSqlQuery q(db);
    if (!q.exec("SELECT enabled, last_checked_at, baseline_hash, current_hash, doc_text, reviewed "
                "FROM maintenance_doc_state WHERE id = 1") || !q.next()) {
        // Never-seeded/failed read → return the safe defaults (enabled ON, nothing pending).
        out.insert(QStringLiteral("enabled"), true);
        out.insert(QStringLiteral("lastCheckedAt"), qint64(0));
        out.insert(QStringLiteral("baselineHash"), QString());
        out.insert(QStringLiteral("currentHash"), QString());
        out.insert(QStringLiteral("docText"), QString());
        out.insert(QStringLiteral("reviewed"), true);
        return out;
    }
    out.insert(QStringLiteral("enabled"),       q.value(0).toInt() != 0);
    out.insert(QStringLiteral("lastCheckedAt"), q.value(1).toLongLong());
    out.insert(QStringLiteral("baselineHash"),  q.value(2).toString());
    out.insert(QStringLiteral("currentHash"),   q.value(3).toString());
    out.insert(QStringLiteral("docText"),       q.value(4).toString());
    out.insert(QStringLiteral("reviewed"),      q.value(5).toInt() != 0);
    return out;
}

bool TasksStorage::setDocSyncEnabledStatic(QSqlDatabase& db, bool enabled)
{
    ensureDocStateSchemaStatic(db);
    QSqlQuery q(db);
    q.prepare("UPDATE maintenance_doc_state SET enabled = :en WHERE id = 1");
    q.bindValue(":en", enabled ? 1 : 0);
    if (!q.exec()) {
        qWarning() << "TasksStorage: setDocSyncEnabled failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool TasksStorage::recordFetchedDocStatic(QSqlDatabase& db, const QString& normalizedText,
                                          const QString& hash, qint64 whenEpoch)
{
    if (hash.trimmed().isEmpty())
        return false;
    ensureDocStateSchemaStatic(db);

    // Read the current baseline to decide whether this is the silent first-fetch or a real change.
    QString baseline;
    {
        QSqlQuery sel(db);
        if (sel.exec("SELECT baseline_hash FROM maintenance_doc_state WHERE id = 1") && sel.next())
            baseline = sel.value(0).toString();
    }

    QSqlQuery q(db);
    if (baseline.isEmpty()) {
        // FIRST fetch ever: seed baseline = current = hash and reviewed=1 — a silent baseline, so the
        // barista never offers a "change" the owner never had a chance to see the original of.
        q.prepare("UPDATE maintenance_doc_state SET last_checked_at = :when, doc_text = :txt, "
                  "current_hash = :hash, baseline_hash = :hash, reviewed = 1 WHERE id = 1");
    } else {
        // Later fetch: record it and flag reviewed = (unchanged). A NEW hash → reviewed=0 (pending offer).
        q.prepare("UPDATE maintenance_doc_state SET last_checked_at = :when, doc_text = :txt, "
                  "current_hash = :hash, reviewed = CASE WHEN :hash = baseline_hash THEN 1 ELSE 0 END "
                  "WHERE id = 1");
    }
    q.bindValue(":when", whenEpoch);
    q.bindValue(":txt",  normalizedText);
    q.bindValue(":hash", hash);
    if (!q.exec()) {
        qWarning() << "TasksStorage: recordFetchedDoc failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool TasksStorage::markDocReviewedStatic(QSqlDatabase& db)
{
    ensureDocStateSchemaStatic(db);
    QSqlQuery q(db);
    // Advance baseline to whatever's current so THIS change is never re-offered (accept OR dismiss).
    if (!q.exec("UPDATE maintenance_doc_state SET baseline_hash = current_hash, reviewed = 1 WHERE id = 1")) {
        qWarning() << "TasksStorage: markDocReviewed failed:" << q.lastError().text();
        return false;
    }
    return true;
}
