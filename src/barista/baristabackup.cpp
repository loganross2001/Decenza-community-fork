#include "baristabackup.h"
#include "baristadiagnostics.h"
#include "../core/dbutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QStandardPaths>
#include <QSettings>
#include <QDateTime>
#include <QTimer>
#include <QThread>
#include <QCoreApplication>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPointer>
#include <QSqlDatabase>

namespace {
constexpr const char* kEnabledKey = "barista/kbBackupEnabled";

QString todayStamp() { return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")); }

// Never write credentials into a backup file (owner rule: keys never leave their intended place).
bool isSecretKey(const QString& key) {
    const QString k = key.toLower();
    return k.contains(QLatin1String("key")) || k.contains(QLatin1String("password"))
        || k.contains(QLatin1String("token")) || k.contains(QLatin1String("secret"));
}
}  // namespace

BaristaBackup::BaristaBackup(QObject* parent) : QObject(parent) {
    m_enabled = QSettings().value(QLatin1String(kEnabledKey), true).toBool();
    // Live beside the app's debug.log (AppDataLocation) — the one location retrievable off the tablet.
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    m_dir = base + QStringLiteral("/BaristaKnowledgeBackups");
    QDir().mkpath(m_dir);
}

BaristaBackup::~BaristaBackup() = default;

void BaristaBackup::initialize(const QString& assistantDbPath) {
    m_dbPath = assistantDbPath;
    refreshStatus();
    // Startup backup if today's set is missing (covers the common restart-driven cadence).
    if (m_enabled)
        runBackup(/*force=*/false);
    // Re-check every 6h so a device left running for days still gets its daily set.
    if (!m_daily) {
        m_daily = new QTimer(this);
        m_daily->setInterval(6 * 60 * 60 * 1000);
        connect(m_daily, &QTimer::timeout, this, [this]() { if (m_enabled) runBackup(false); });
        m_daily->start();
    }
}

bool BaristaBackup::enabled() const { return m_enabled; }

void BaristaBackup::setEnabled(bool on) {
    if (m_enabled == on) return;
    m_enabled = on;
    QSettings().setValue(QLatin1String(kEnabledKey), on);
    emit enabledChanged();
    if (on) runBackup(false);   // catch up immediately when turned on
}

qint64 BaristaBackup::lastBackupAt() const { return m_lastBackupAt; }
int BaristaBackup::backupCount() const { return m_backupCount; }
QString BaristaBackup::lastError() const { return m_lastError; }

void BaristaBackup::backupNow() { runBackup(/*force=*/true); }

QString BaristaBackup::newestBackupPath() const {
    QDir dir(m_dir);
    const QFileInfoList files = dir.entryInfoList(QStringList{ QStringLiteral("assistant-*.db") },
                                                  QDir::Files, QDir::Time);
    return files.isEmpty() ? QString() : files.first().absoluteFilePath();
}

void BaristaBackup::refreshStatus() {
    QDir dir(m_dir);
    const QFileInfoList files = dir.entryInfoList(QStringList{ QStringLiteral("assistant-*.db") },
                                                  QDir::Files, QDir::Time);
    m_backupCount = static_cast<int>(files.size());
    m_lastBackupAt = files.isEmpty() ? 0 : files.first().lastModified().toSecsSinceEpoch();
    emit statusChanged();
}

void BaristaBackup::runBackup(bool force) {
    if (m_dbPath.isEmpty() || !QFile::exists(m_dbPath))
        return;   // nothing to back up yet (fresh install before the KB exists)
    const QString stamp = todayStamp();
    const QString destDb = m_dir + QStringLiteral("/assistant-") + stamp + QStringLiteral(".db");
    if (!force && QFileInfo::exists(destDb) && QFileInfo(destDb).size() > 0)
        return;   // today's set already exists

    // Snapshot the (non-secret) settings on the main thread (QSettings), pass the JSON to the worker to write.
    QByteArray settingsJson;
    {
        QSettings s;
        QJsonObject obj;
        const QStringList keys = s.allKeys();
        for (const QString& key : keys) {
            if (isSecretKey(key)) continue;
            obj.insert(key, QJsonValue::fromVariant(s.value(key)));
        }
        settingsJson = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    }

    const QString dbPath = m_dbPath;
    const QString dir = m_dir;
    const QString destSettings = m_dir + QStringLiteral("/barista-settings-") + stamp + QStringLiteral(".json");
    QPointer<BaristaBackup> self(this);

    QThread* thread = QThread::create([self, dbPath, destDb, destSettings, settingsJson, dir]() {
        QString err;
        bool ok = false;
        // Consistent, WAL-safe snapshot via VACUUM INTO (writes a fresh defragmented copy).
        const QString tmp = destDb + QStringLiteral(".tmp");
        QFile::remove(tmp);
        withTempDb(dbPath, "barista_kb_backup", [&](QSqlDatabase& db) {
            QSqlQuery q(db);
            QString escaped = tmp;
            escaped.replace('\'', QStringLiteral("''"));
            if (q.exec(QStringLiteral("VACUUM INTO '%1'").arg(escaped))) {
                ok = true;
            } else {
                err = q.lastError().text();
            }
        });
        if (ok) {
            QFile::remove(destDb);
            if (!QFile::rename(tmp, destDb)) { ok = false; err = QStringLiteral("could not finalize backup file"); }
        }
        QFile::remove(tmp);

        // Settings snapshot (best-effort; the DB is the critical artifact).
        if (!settingsJson.isEmpty()) {
            QFile sf(destSettings);
            if (sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { sf.write(settingsJson); sf.close(); }
        }

        // Prune anything older than kKeepDays for BOTH file kinds.
        const QDateTime cutoff = QDateTime::currentDateTime().addDays(-kKeepDays);
        QDir d(dir);
        const QStringList pats{ QStringLiteral("assistant-*.db"), QStringLiteral("barista-settings-*.json") };
        for (const QFileInfo& fi : d.entryInfoList(pats, QDir::Files)) {
            if (fi.lastModified() < cutoff)
                QFile::remove(fi.absoluteFilePath());
        }

        const bool okFinal = ok;
        const QString errFinal = err;
        QMetaObject::invokeMethod(qApp, [self, okFinal, errFinal]() {
            if (!self) return;
            self->m_lastError = okFinal ? QString() : errFinal;
            if (okFinal)
                BaristaDiagnostics::record(QStringLiteral("backup"), QStringLiteral("kb_backup_ok"));
            else
                BaristaDiagnostics::record(QStringLiteral("backup"), QStringLiteral("kb_backup_FAILED"),
                                           {{QStringLiteral("err"), errFinal}});
            self->refreshStatus();
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
