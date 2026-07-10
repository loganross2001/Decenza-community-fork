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

    query.exec("CREATE INDEX IF NOT EXISTS idx_reminders_open ON reminders(completed_at, due_at)");

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
