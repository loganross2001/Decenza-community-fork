#include "baristaknowledge.h"

#include "assistantsettings.h"
#include "../ai/aimanager.h"

#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

BaristaKnowledge::BaristaKnowledge(AssistantSettings* settings, AIManager* aiManager, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_aiManager(aiManager) {}

QString BaristaKnowledge::defaultLocation() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + QStringLiteral("/Decenza Barista");
}

QString BaristaKnowledge::location() const {
    const QString custom = QSettings().value(QStringLiteral("barista/knowledgePath")).toString();
    return custom.isEmpty() ? defaultLocation() : custom;
}

void BaristaKnowledge::setLocation(const QString& dir) {
    if (location() == dir)
        return;
    QSettings().setValue(QStringLiteral("barista/knowledgePath"), dir.trimmed());
    emit locationChanged();
    emit backupsChanged();
    emit lastBackupChanged();
}

QString BaristaKnowledge::lastBackup() const {
    return QSettings().value(QStringLiteral("barista/lastBackup")).toString();
}

QStringList BaristaKnowledge::backups() const {
    QDir dir(location());
    if (!dir.exists())
        return {};
    QStringList files = dir.entryList({QStringLiteral("decenza-barista-*.conf")}, QDir::Files, QDir::Name);
    std::reverse(files.begin(), files.end());   // newest first (names are timestamp-sorted)
    return files;
}

void BaristaKnowledge::copyGroups(QSettings& from, QSettings& to) const {
    // The barista's durable knowledge: its per-bean conversations (+ index) and its own settings.
    const QStringList groups = {QStringLiteral("ai/conversations"), QStringLiteral("barista")};
    for (const QString& g : groups) {
        from.beginGroup(g);
        const QStringList keys = from.allKeys();   // relative to the group
        for (const QString& k : keys) {
            // Don't put secrets or device-local bookkeeping in the portable file: API keys are
            // re-entered per device (redact-by-default, like SettingsSerializer), and the store
            // path / last-backup stamp are local state, not portable knowledge.
            if (k.endsWith(QStringLiteral("ApiKey")) || k == QStringLiteral("knowledgePath")
                    || k == QStringLiteral("lastBackup")
                    || k == QStringLiteral("pendingActions"))   // S6: don't resurrect stale grind reminders
                continue;
            to.setValue(g + QLatin1Char('/') + k, from.value(k));
        }
        from.endGroup();
    }
    to.sync();
}

void BaristaKnowledge::mergeIndex(const QByteArray& localIndexJson, QSettings& dest) const {
    // `dest` currently holds the backup's index (copyGroups just wrote it). Union it with the device's
    // prior index by conversation key, keeping the newer timestamp, so neither side is orphaned (S6).
    const QByteArray backupIndexJson = dest.value(QStringLiteral("ai/conversations/index")).toByteArray();
    QMap<QString, QJsonObject> byKey;
    const auto ingest = [&byKey](const QByteArray& json) {
        const QJsonArray arr = QJsonDocument::fromJson(json).array();
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            const QString key = o.value(QStringLiteral("key")).toString();
            if (key.isEmpty())
                continue;
            const auto it = byKey.constFind(key);
            if (it == byKey.constEnd()
                    || o.value(QStringLiteral("timestamp")).toDouble()
                       >= it.value().value(QStringLiteral("timestamp")).toDouble())
                byKey.insert(key, o);
        }
    };
    ingest(localIndexJson);    // device
    ingest(backupIndexJson);   // backup (>= so it wins exact-timestamp ties)
    QJsonArray merged;
    for (const QJsonObject& o : byKey)
        merged.append(o);
    dest.setValue(QStringLiteral("ai/conversations/index"),
                  QJsonDocument(merged).toJson(QJsonDocument::Compact));
}

bool BaristaKnowledge::backupNow() {
    const QString dir = location();
    if (!QDir().mkpath(dir)) {
        setStatus(QStringLiteral("Couldn't create %1").arg(dir));
        return false;
    }
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString path = dir + QStringLiteral("/decenza-barista-") + ts + QStringLiteral(".conf");

    QSettings src;   // app default (DecentEspresso/DE1Qt)
    QSettings dest(path, QSettings::IniFormat);
    copyGroups(src, dest);
    if (dest.status() != QSettings::NoError || !QFileInfo::exists(path)) {
        setStatus(QStringLiteral("Backup failed"));
        return false;
    }
    QSettings().setValue(QStringLiteral("barista/lastBackup"),
                         QDateTime::currentDateTime().toString(Qt::ISODate));
    emit lastBackupChanged();
    emit backupsChanged();
    setStatus(QStringLiteral("Backed up to ") + QFileInfo(path).fileName());
    return true;
}

bool BaristaKnowledge::restore(const QString& fileName) {
    const QString path = location() + QLatin1Char('/') + fileName;
    if (!QFileInfo::exists(path)) {
        setStatus(QStringLiteral("Backup not found"));
        return false;
    }
    QSettings dest;   // app default
    // S6: capture the device's conversation index BEFORE the copy so restore MERGES rather than replaces
    // it — otherwise local conversations absent from the backup get orphaned (and later overwritten).
    const QByteArray localIndex = dest.value(QStringLiteral("ai/conversations/index")).toByteArray();
    QSettings src(path, QSettings::IniFormat);
    copyGroups(src, dest);
    mergeIndex(localIndex, dest);
    dest.sync();
    if (m_aiManager)
        m_aiManager->reloadConversations();   // refresh the in-memory conversation index
    emit lastBackupChanged();
    setStatus(QStringLiteral("Restored from ") + fileName);
    return true;
}

void BaristaKnowledge::refresh() {
    emit backupsChanged();
    emit lastBackupChanged();
    emit locationChanged();
}

void BaristaKnowledge::setStatus(const QString& s) {
    if (m_status == s)
        return;
    m_status = s;
    emit statusChanged();
}
