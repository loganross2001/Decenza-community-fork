#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

// [barista-fork] Voice-ID Increment 1 — on-device store of enrolled voiceprints.
//
// PRIVACY INVARIANT (load-bearing): voiceprints are BIOMETRIC and must NEVER leave the device. They live in a
// DEDICATED `voiceprints.db` file (its own SQLite file in AppDataLocation, sibling of assistant.db) that is
// deliberately excluded from EVERY export/sync path:
//   * the KB backup (baristabackup.cpp) VACUUMs ONLY assistant.db — it never names voiceprints.db;
//   * the device-to-device transfer (shotserver_backup.cpp) transfers shots.db + *.json + media ONLY;
// so a separate file is off-device-safe by construction. Do NOT add voiceprints.db to any backup/export list,
// and the deliberate consequence — voiceprints are LOST on uninstall — is the correct trade for biometrics
// (re-enroll beats syncing biometric data off-device). Opt-in per person; per-person deletable.
class VoiceprintStore : public QObject {
    Q_OBJECT
public:
    struct Voiceprint {
        QString name;
        QVector<float> vector;
    };

    explicit VoiceprintStore(QObject* parent = nullptr);

    // Point the store at its own DB file and create the table if needed (off-main).
    void initialize(const QString& dbPath);
    QString databasePath() const { return m_dbPath; }

    // Insert-or-replace this person's voiceprint (off-main; `done(ok)` on the main thread).
    void upsert(const QString& name, const QVector<float>& vector, const QString& embedder,
                std::function<void(bool)> done);
    // Load every enrolled voiceprint (off-main; `done(list)` on the main thread).
    void loadAll(std::function<void(QVector<Voiceprint>)> done);
    // Delete one person's voiceprint (off-main; `done(ok)` on the main thread).
    void remove(const QString& name, std::function<void(bool)> done);

private:
    void ensureSchema();
    QString m_dbPath;
};
