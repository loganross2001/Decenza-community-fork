#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class QSqlDatabase;
class SerialDbWorker;

// A barista: one person in the roster the idle-screen chip row switches between
// (pr/barista-identity). The roster upgrades the legacy free-text `barista`
// string (still stamped on each shot, sourced from Settings.dye.dyeBarista)
// into a small set of named people with a color + avatar. There are no logins
// or passwords — switching is a one-tap MRU concept. prefs_json is reserved for
// a later phase (per-barista preferences) and is unused for now.
struct Barista {
    qint64 id = 0;

    QString name;       // UNIQUE, the value stamped onto shots
    QString color;      // chip fill (empty = fall back to a Theme token in QML)
    QString avatar;     // emoji or initial (empty = first letter of name in QML)
    QString prefsJson;  // reserved for a later phase; unused today

    qint64 createdEpoch = 0;
    qint64 lastUsedEpoch = 0;  // bumped on selection (MRU ordering)
    qint64 sortOrder = 0;      // manual ordering; primary sort key

    bool isValid() const { return id > 0; }
    QVariantMap toVariantMap() const;
    static Barista fromVariantMap(const QVariantMap& map);
};

// SQLite-backed barista roster in the shot history database (baristas table,
// created by ShotHistoryStorage migration 24). All public request* methods are
// async: DB work runs on a serial background worker and results are delivered
// back via signals, mirroring CoffeeBagStorage. The *Static helpers are
// synchronous, take a caller-provided connection, and are shared with the
// migration and tests.
class BaristaStorage : public QObject {
    Q_OBJECT

public:
    explicit BaristaStorage(QObject* parent = nullptr);
    ~BaristaStorage();

    // dbPath must be the shot history database (table lives there).
    void initialize(const QString& dbPath);
    QString databasePath() const { return m_dbPath; }

    // Async query — result via rosterReady (QVariantList of toVariantMap()).
    Q_INVOKABLE void requestRoster();

    // Async writes — all emit baristasChanged() on success.
    Q_INVOKABLE void requestCreateBarista(const QVariantMap& fields);      // baristaCreated()
    Q_INVOKABLE void requestUpdateBarista(qint64 id, const QVariantMap& fields); // baristaUpdated()
    Q_INVOKABLE void requestDeleteBarista(qint64 id);                       // baristaDeleted()
    Q_INVOKABLE void requestTouchLastUsed(qint64 id);                       // bump MRU (no signal)

    // --- Synchronous static helpers (caller provides the connection) ---

    // Create the baristas table if missing. Used by migration 24 and tests.
    static bool ensureTableStatic(QSqlDatabase& db);

    static qint64 insertStatic(QSqlDatabase& db, const Barista& barista);
    // Sort by sort_order asc, then last_used_epoch desc (MRU within equal order).
    static QVector<Barista> loadRosterStatic(QSqlDatabase& db);
    // Update only the columns named in `fields` (camelCase Barista keys).
    static bool updateFieldsStatic(QSqlDatabase& db, qint64 id, const QVariantMap& fields);
    static bool deleteStatic(QSqlDatabase& db, qint64 id);

    // Copy baristas rows from srcDb into destDb (device transfer / backup
    // restore), mirroring CoffeeBagStorage::importBagsStatic. Row ids change on
    // insert — outIdMap records old->new for symmetry with the other importers,
    // but shots reference the barista by NAME (not id), so NO shot row needs a
    // downstream remap. Merge mode keeps existing dest rows and inserts only
    // source rows whose `name` (UNIQUE, case-insensitive) is not already
    // present; replace mode clears dest baristas first. Source DBs from before
    // migration 24 have no baristas table — returns true with an empty map.
    // Runs inside the caller's destDb transaction.
    static bool importBaristasStatic(QSqlDatabase& srcDb, QSqlDatabase& destDb, bool merge,
                                     QHash<qint64, qint64>& outIdMap);

signals:
    void rosterReady(const QVariantList& baristas);
    void baristaCreated(qint64 id, const QVariantMap& barista); // id -1 on failure
    void baristaUpdated(qint64 id, bool success);
    void baristaDeleted(qint64 id, bool success);
    // Coarse "something changed" signal so views can re-request the roster.
    void baristasChanged();

private:
    // Run `work(db)` on the background worker, then `done(dbOpened)` on the main
    // thread. Read callers skip their "Ready" emission when dbOpened is false.
    void runAsync(const QString& connPrefix,
                  std::function<void(QSqlDatabase&)> work,
                  std::function<void(bool dbOpened)> done);

    static Barista fromQueryRow(const class QSqlQuery& query);

    QString m_dbPath;
    std::shared_ptr<std::atomic<bool>> m_destroyed = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<SerialDbWorker> m_dbWorker;
};
