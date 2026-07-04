#include "baristaknowledge.h"

#include "assistantsettings.h"
#include "../ai/aimanager.h"

#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QStandardPaths>

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
                    || k == QStringLiteral("lastBackup"))
                continue;
            to.setValue(g + QLatin1Char('/') + k, from.value(k));
        }
        from.endGroup();
    }
    to.sync();
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
    QSettings src(path, QSettings::IniFormat);
    QSettings dest;   // app default
    copyGroups(src, dest);
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
