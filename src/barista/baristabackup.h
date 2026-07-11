#pragma once

#include <QObject>
#include <QString>

class QTimer;

// [barista-fork] Independent, automatic backup of the barista's PRIVATE knowledge base — the accumulating
// personal data the owner is building and does not want to lose:
//   * assistant.db  (tasting-feedback KB, reminders, maintenance history + schedule, personal dates)
//   * a curated settings snapshot (all app/barista preferences EXCEPT secrets — API keys/passwords)
//
// Kept as a ROLLING 10-DAY HISTORY (one dated set per day), entirely SEPARATE from Decenza's main shots
// backup: its own directory, its own always-on schedule, and it runs regardless of whether the main daily
// backup is enabled. Local only — nothing leaves the device. Backups live in AppDataLocation (beside the
// app's debug.log), the one spot proven retrievable off the DE1 tablet.
//
// Safety: the DB copy uses SQLite `VACUUM INTO`, which writes a fresh, transactionally-consistent, WAL-safe
// snapshot (never a torn mid-write file). All work runs off the main thread.
class BaristaBackup : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(qint64 lastBackupAt READ lastBackupAt NOTIFY statusChanged)   // epoch secs; 0 = never
    Q_PROPERTY(int backupCount READ backupCount NOTIFY statusChanged)
    Q_PROPERTY(QString backupDir READ backupDir NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)

public:
    explicit BaristaBackup(QObject* parent = nullptr);
    ~BaristaBackup() override;

    // Set the assistant.db path and run a startup backup if today's is missing. Safe to call once the DB path
    // is known (from BaristaModule, right after feedback/tasks storage are initialized).
    void initialize(const QString& assistantDbPath);

    bool enabled() const;
    void setEnabled(bool on);
    qint64 lastBackupAt() const;
    int backupCount() const;
    QString backupDir() const { return m_dir; }
    QString lastError() const;

    // Force a backup now (async, off-main). Overwrites today's set if it already exists.
    Q_INVOKABLE void backupNow();
    // Newest backup .db path (for the settings card / retrieval hint); empty if none.
    Q_INVOKABLE QString newestBackupPath() const;

signals:
    void enabledChanged();
    void statusChanged();

private:
    void runBackup(bool force);
    void refreshStatus();   // recompute count/last from the directory

    static constexpr int kKeepDays = 10;

    QString m_dir;
    QString m_dbPath;
    bool m_enabled = true;
    QTimer* m_daily = nullptr;
    mutable QString m_lastError;
    qint64 m_lastBackupAt = 0;
    int m_backupCount = 0;
};
