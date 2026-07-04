#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class AssistantSettings;
class AIManager;

// [barista-fork] The barista's portable, relocatable, backed-up knowledge store.
//
// The durable knowledge is the assistant's per-bean CONVERSATIONS (its longitudinal memory + its own
// past advice) and its SETTINGS — both live in QSettings under "ai/conversations/" and "barista/".
// This exports those groups to a single portable file (INI) at a user-chosen folder, and restores from
// one — so the knowledge is never trapped: you own it, can relocate the folder (e.g. a synced/cloud
// folder), back it up, and carry it to another device. Shots stay in the app DB (covered by the app's
// own backup); this store is the AI's accumulated understanding on top of them.
class BaristaKnowledge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString location READ location WRITE setLocation NOTIFY locationChanged)
    Q_PROPERTY(QString lastBackup READ lastBackup NOTIFY lastBackupChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QStringList backups READ backups NOTIFY backupsChanged)

public:
    BaristaKnowledge(AssistantSettings* settings, AIManager* aiManager, QObject* parent = nullptr);

    QString location() const;             // folder where backups live (relocatable); a sensible default
    void setLocation(const QString& dir);
    QString lastBackup() const;
    QString status() const { return m_status; }
    QStringList backups() const;          // backup file names in `location`, newest first

    Q_INVOKABLE bool backupNow();                       // export → location/decenza-barista-<timestamp>.conf
    Q_INVOKABLE bool restore(const QString& fileName);  // import location/<fileName>, then reload conversations
    Q_INVOKABLE void refresh();                         // re-scan the folder + emit backups/lastBackup

signals:
    void locationChanged();
    void lastBackupChanged();
    void statusChanged();
    void backupsChanged();

private:
    QString defaultLocation() const;
    void copyGroups(class QSettings& from, class QSettings& to) const;   // ai/conversations + barista
    void mergeIndex(const QByteArray& localIndexJson, class QSettings& dest) const;   // union, keep newer
    void setStatus(const QString& s);

    AssistantSettings* m_settings = nullptr;
    AIManager* m_aiManager = nullptr;
    QString m_status;
};
