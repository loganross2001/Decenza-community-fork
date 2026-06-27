#include "baristastorage.h"
#include "core/dbutils.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDatabase>
#include <QDateTime>
#include <QDebug>

namespace {

// Single source of truth for the baristas column set, mirroring kCols in
// coffeebagstorage.cpp but kept deliberately lean (no Visualizer flag, no
// generated SELECT machinery): the column list is small enough to spell out.
// `key` is the camelCase Barista / QVariantMap key; `sql` is the column name.
struct Col { const char* sql; const char* key; };

// id is read positionally but never written (autoincrement); the rest are
// writable (in the INSERT + the update map).
const Col kWritableCols[] = {
    { "name",            "name" },
    { "color",           "color" },
    { "avatar",          "avatar" },
    { "prefs_json",      "prefsJson" },
    { "created_epoch",   "createdEpoch" },
    { "last_used_epoch", "lastUsedEpoch" },
    { "sort_order",      "sortOrder" },
};

QVariant valueForKey(const Barista& b, const QString& key) {
    if (key == QLatin1String("name"))          return b.name;
    if (key == QLatin1String("color"))         return b.color;
    if (key == QLatin1String("avatar"))        return b.avatar;
    if (key == QLatin1String("prefsJson"))     return b.prefsJson;
    if (key == QLatin1String("createdEpoch"))  return b.createdEpoch;
    if (key == QLatin1String("lastUsedEpoch")) return b.lastUsedEpoch;
    if (key == QLatin1String("sortOrder"))     return b.sortOrder;
    return QVariant();
}

} // namespace

QVariantMap Barista::toVariantMap() const
{
    QVariantMap map;
    map.insert(QStringLiteral("id"), id);
    map.insert(QStringLiteral("name"), name);
    map.insert(QStringLiteral("color"), color);
    map.insert(QStringLiteral("avatar"), avatar);
    map.insert(QStringLiteral("prefsJson"), prefsJson);
    map.insert(QStringLiteral("createdEpoch"), createdEpoch);
    map.insert(QStringLiteral("lastUsedEpoch"), lastUsedEpoch);
    map.insert(QStringLiteral("sortOrder"), sortOrder);
    return map;
}

Barista Barista::fromVariantMap(const QVariantMap& map)
{
    // Absent keys keep the struct's member defaults (0 / empty string).
    Barista b;
    if (map.contains(QStringLiteral("id")))            b.id = map.value(QStringLiteral("id")).toLongLong();
    if (map.contains(QStringLiteral("name")))          b.name = map.value(QStringLiteral("name")).toString();
    if (map.contains(QStringLiteral("color")))         b.color = map.value(QStringLiteral("color")).toString();
    if (map.contains(QStringLiteral("avatar")))        b.avatar = map.value(QStringLiteral("avatar")).toString();
    if (map.contains(QStringLiteral("prefsJson")))     b.prefsJson = map.value(QStringLiteral("prefsJson")).toString();
    if (map.contains(QStringLiteral("createdEpoch")))  b.createdEpoch = map.value(QStringLiteral("createdEpoch")).toLongLong();
    if (map.contains(QStringLiteral("lastUsedEpoch"))) b.lastUsedEpoch = map.value(QStringLiteral("lastUsedEpoch")).toLongLong();
    if (map.contains(QStringLiteral("sortOrder")))     b.sortOrder = map.value(QStringLiteral("sortOrder")).toLongLong();
    return b;
}

BaristaStorage::BaristaStorage(QObject* parent)
    : QObject(parent)
{
}

BaristaStorage::~BaristaStorage()
{
    // Suppress in-flight result callbacks, then stop the worker before members
    // vanish (see CoffeeBagStorage::~CoffeeBagStorage for the full rationale).
    *m_destroyed = true;
    m_dbWorker.reset();
}

void BaristaStorage::initialize(const QString& dbPath)
{
    m_dbPath = dbPath;
}

void BaristaStorage::runAsync(const QString& connPrefix,
                              std::function<void(QSqlDatabase&)> work,
                              std::function<void(bool dbOpened)> done)
{
    if (m_dbPath.isEmpty()) {
        qWarning() << "BaristaStorage: not initialized, dropping" << connPrefix;
        return;
    }
    if (!m_dbWorker)
        m_dbWorker = std::make_unique<SerialDbWorker>(QStringLiteral("BaristaStorageWorker"));
    m_dbWorker->run(m_dbPath, connPrefix, std::move(work), std::move(done), this, m_destroyed);
}

void BaristaStorage::requestRoster()
{
    auto baristas = std::make_shared<QVariantList>();
    runAsync("baristas_roster",
        [baristas](QSqlDatabase& db) {
            const QVector<Barista> roster = loadRosterStatic(db);
            for (const Barista& b : roster)
                baristas->append(b.toVariantMap());
        },
        // Read: skip the emit on open failure so the UI keeps its current list.
        [this, baristas](bool dbOpened) { if (dbOpened) emit rosterReady(*baristas); });
}

void BaristaStorage::requestCreateBarista(const QVariantMap& fields)
{
    auto newId = std::make_shared<qint64>(-1);
    auto created = std::make_shared<QVariantMap>();
    runAsync("baristas_create",
        [fields, newId, created](QSqlDatabase& db) {
            Barista b = Barista::fromVariantMap(fields);
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            if (b.createdEpoch <= 0)
                b.createdEpoch = now;
            b.lastUsedEpoch = now;
            *newId = insertStatic(db, b);
            if (*newId > 0) {
                b.id = *newId;
                *created = b.toVariantMap();
            }
        },
        // Write: emit regardless — *newId is -1 on failure, a terminal status.
        [this, newId, created](bool) {
            emit baristaCreated(*newId, *created);
            if (*newId > 0)
                emit baristasChanged();
        });
}

void BaristaStorage::requestUpdateBarista(qint64 id, const QVariantMap& fields)
{
    // Guarantee a terminal baristaUpdated even when uninitialized so callers
    // arming a one-shot response don't hang (CoffeeBagStorage precedent).
    if (m_dbPath.isEmpty()) {
        qWarning() << "BaristaStorage: requestUpdateBarista on uninitialized storage, id" << id;
        emit baristaUpdated(id, false);
        return;
    }
    auto success = std::make_shared<bool>(false);
    runAsync("baristas_update",
        [id, fields, success](QSqlDatabase& db) {
            *success = updateFieldsStatic(db, id, fields);
        },
        [this, id, success](bool) {
            emit baristaUpdated(id, *success);
            if (*success)
                emit baristasChanged();
        });
}

void BaristaStorage::requestDeleteBarista(qint64 id)
{
    auto success = std::make_shared<bool>(false);
    runAsync("baristas_delete",
        [id, success](QSqlDatabase& db) {
            *success = deleteStatic(db, id);
        },
        [this, id, success](bool) {
            emit baristaDeleted(id, *success);
            if (*success)
                emit baristasChanged();
        });
}

void BaristaStorage::requestTouchLastUsed(qint64 id)
{
    runAsync("baristas_touch",
        [id](QSqlDatabase& db) {
            QSqlQuery query(db);
            query.prepare("UPDATE baristas SET last_used_epoch = :now WHERE id = :id");
            query.bindValue(":now", QDateTime::currentSecsSinceEpoch());
            query.bindValue(":id", id);
            if (!query.exec())
                qWarning() << "BaristaStorage: touch last_used failed:" << query.lastError().text();
        },
        [](bool) {});
}

bool BaristaStorage::ensureTableStatic(QSqlDatabase& db)
{
    QSqlQuery query(db);
    const bool ok = query.exec(R"(
        CREATE TABLE IF NOT EXISTS baristas (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            color TEXT DEFAULT '',
            avatar TEXT DEFAULT '',
            prefs_json TEXT DEFAULT '',
            created_epoch INTEGER DEFAULT 0,
            last_used_epoch INTEGER DEFAULT 0,
            sort_order INTEGER DEFAULT 0
        )
    )");
    if (!ok) {
        qWarning() << "BaristaStorage: failed to create baristas table:" << query.lastError().text();
        return false;
    }
    return true;
}

Barista BaristaStorage::fromQueryRow(const QSqlQuery& query)
{
    Barista b;
    b.id            = query.value(0).toLongLong();
    b.name          = query.value(1).toString();
    b.color         = query.value(2).toString();
    b.avatar        = query.value(3).toString();
    b.prefsJson     = query.value(4).toString();
    b.createdEpoch  = query.value(5).toLongLong();
    b.lastUsedEpoch = query.value(6).toLongLong();
    b.sortOrder     = query.value(7).toLongLong();
    return b;
}

qint64 BaristaStorage::insertStatic(QSqlDatabase& db, const Barista& barista)
{
    QStringList columns, placeholders;
    QVariantList binds;
    for (const Col& c : kWritableCols) {
        columns << QString::fromLatin1(c.sql);
        placeholders << QStringLiteral("?");
        binds << valueForKey(barista, QString::fromLatin1(c.key));
    }

    QSqlQuery query(db);
    query.prepare(QString("INSERT INTO baristas (%1) VALUES (%2)")
                      .arg(columns.join(QStringLiteral(", ")), placeholders.join(QStringLiteral(", "))));
    for (qsizetype i = 0; i < binds.size(); ++i)
        query.bindValue(static_cast<int>(i), binds.at(i));

    if (!query.exec()) {
        qWarning() << "BaristaStorage: insert failed:" << query.lastError().text();
        return -1;
    }
    return query.lastInsertId().toLongLong();
}

QVector<Barista> BaristaStorage::loadRosterStatic(QSqlDatabase& db)
{
    QVector<Barista> roster;
    QSqlQuery query(db);
    if (!query.exec("SELECT id, name, color, avatar, prefs_json, "
                    "created_epoch, last_used_epoch, sort_order "
                    "FROM baristas ORDER BY sort_order ASC, last_used_epoch DESC, id ASC")) {
        qWarning() << "BaristaStorage: roster query failed:" << query.lastError().text();
        return roster;
    }
    while (query.next())
        roster.append(fromQueryRow(query));
    return roster;
}

bool BaristaStorage::updateFieldsStatic(QSqlDatabase& db, qint64 id, const QVariantMap& fields)
{
    // camelCase Barista key -> column name (writable columns only). Only listed
    // keys are updatable; id is never writable.
    QStringList assignments;
    QVariantList values;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        const char* sqlName = nullptr;
        for (const Col& c : kWritableCols) {
            if (it.key() == QLatin1String(c.key)) {
                sqlName = c.sql;
                break;
            }
        }
        if (!sqlName) {
            qWarning() << "BaristaStorage: ignoring unknown field" << it.key();
            continue;
        }
        assignments << QString("%1 = ?").arg(QString::fromLatin1(sqlName));
        values << it.value();
    }
    if (assignments.isEmpty())
        return false;

    QSqlQuery query(db);
    query.prepare(QString("UPDATE baristas SET %1 WHERE id = ?").arg(assignments.join(QStringLiteral(", "))));
    int pos = 0;
    for (const QVariant& value : values)
        query.bindValue(pos++, value);
    query.bindValue(pos, id);

    if (!query.exec()) {
        qWarning() << "BaristaStorage: update failed for id" << id << ":" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool BaristaStorage::deleteStatic(QSqlDatabase& db, qint64 id)
{
    QSqlQuery query(db);
    query.prepare("DELETE FROM baristas WHERE id = :id");
    query.bindValue(":id", id);
    if (!query.exec()) {
        qWarning() << "BaristaStorage: delete failed for id" << id << ":" << query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool BaristaStorage::importBaristasStatic(QSqlDatabase& srcDb, QSqlDatabase& destDb, bool merge,
                                          QHash<qint64, qint64>& outIdMap)
{
    // Pre-migration-24 source: no baristas table, nothing to import.
    {
        QSqlQuery srcCheck(srcDb);
        if (!srcCheck.exec("SELECT COUNT(*) FROM baristas"))
            return true;
    }

    if (!merge) {
        QSqlQuery clearQuery(destDb);
        if (!clearQuery.exec("DELETE FROM baristas")) {
            qWarning() << "BaristaStorage: failed to clear baristas for replace import:"
                       << clearQuery.lastError().text();
            return false;
        }
    }

    QSqlQuery srcBaristas(srcDb);
    if (!srcBaristas.exec("SELECT id, name, color, avatar, prefs_json, "
                          "created_epoch, last_used_epoch, sort_order FROM baristas")) {
        qWarning() << "BaristaStorage: failed to query source baristas:" << srcBaristas.lastError().text();
        return false;
    }

    int imported = 0, matched = 0;
    while (srcBaristas.next()) {
        const Barista b = fromQueryRow(srcBaristas);

        // shots reference the barista by NAME, so the id map is for symmetry
        // with the other importers only — no shot row is rewritten.
        qint64 destId = -1;
        if (merge) {
            // name is UNIQUE — match case-insensitively so a re-imported roster
            // doesn't duplicate an existing person under a different casing.
            QSqlQuery dupQuery(destDb);
            dupQuery.prepare("SELECT id FROM baristas WHERE "
                             "LOWER(COALESCE(name,'')) = LOWER(:name) LIMIT 1");
            dupQuery.bindValue(":name", b.name);
            if (dupQuery.exec() && dupQuery.next()) {
                destId = dupQuery.value(0).toLongLong();
                matched++;
            }
        }

        if (destId < 0) {
            destId = insertStatic(destDb, b);
            if (destId < 0)
                return false;
            imported++;
        }
        outIdMap.insert(b.id, destId);
    }

    qDebug() << "BaristaStorage: barista import -" << imported << "imported," << matched << "matched existing";
    return true;
}
