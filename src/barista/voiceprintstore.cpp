#include "voiceprintstore.h"
#include "../core/dbutils.h"

#include <QThread>
#include <QPointer>
#include <QCoreApplication>
#include <QSqlQuery>
#include <QSqlError>
#include <QByteArray>
#include <QDateTime>
#include <cstring>   // memcpy

namespace {
// A QVector<float> ↔ raw-bytes BLOB (little-endian float32; the app only reads back what it wrote).
QByteArray vecToBlob(const QVector<float>& v) {
    return QByteArray(reinterpret_cast<const char*>(v.constData()), int(v.size() * sizeof(float)));
}
QVector<float> blobToVec(const QByteArray& b) {
    const int n = static_cast<int>(b.size() / int(sizeof(float)));
    QVector<float> v(n);
    if (n > 0)
        memcpy(v.data(), b.constData(), n * sizeof(float));
    return v;
}
}  // namespace

VoiceprintStore::VoiceprintStore(QObject* parent) : QObject(parent) {}

void VoiceprintStore::initialize(const QString& dbPath) {
    m_dbPath = dbPath;
    ensureSchema();
}

void VoiceprintStore::ensureSchema() {
    if (m_dbPath.isEmpty())
        return;
    const QString path = m_dbPath;
    QThread* thread = QThread::create([path]() {
        withTempDb(path, "voiceprints_schema", [&](QSqlDatabase& db) {
            QSqlQuery q(db);
            q.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS voiceprints ("
                " id INTEGER PRIMARY KEY,"
                " name TEXT UNIQUE,"
                " vector BLOB,"
                " dims INTEGER,"
                " embedder TEXT,"
                " sample_count INTEGER,"
                " created_epoch INTEGER,"
                " updated_epoch INTEGER)"));
        });
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void VoiceprintStore::upsert(const QString& name, const QVector<float>& vector, const QString& embedder,
                             std::function<void(bool)> done) {
    if (m_dbPath.isEmpty() || name.trimmed().isEmpty() || vector.isEmpty()) {
        if (done) done(false);
        return;
    }
    const QString path = m_dbPath;
    const QString n = name.trimmed();
    const QByteArray blob = vecToBlob(vector);
    const int dims = static_cast<int>(vector.size());
    QPointer<VoiceprintStore> self(this);
    QThread* thread = QThread::create([self, path, n, blob, dims, embedder, done]() {
        bool ok = false;
        const bool dbOk = withTempDb(path, "voiceprints_upsert", [&](QSqlDatabase& db) {
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            QSqlQuery q(db);
            // Preserve created_epoch on replace via COALESCE against any existing row.
            q.prepare(QStringLiteral(
                "INSERT INTO voiceprints (name, vector, dims, embedder, sample_count, created_epoch, updated_epoch)"
                " VALUES (:name, :vector, :dims, :embedder, :sc,"
                "         COALESCE((SELECT created_epoch FROM voiceprints WHERE name = :name), :now), :now)"
                " ON CONFLICT(name) DO UPDATE SET"
                "   vector = excluded.vector, dims = excluded.dims, embedder = excluded.embedder,"
                "   sample_count = excluded.sample_count, updated_epoch = excluded.updated_epoch"));
            q.bindValue(QStringLiteral(":name"), n);
            q.bindValue(QStringLiteral(":vector"), blob);
            q.bindValue(QStringLiteral(":dims"), dims);
            q.bindValue(QStringLiteral(":embedder"), embedder);
            q.bindValue(QStringLiteral(":sc"), 1);
            q.bindValue(QStringLiteral(":now"), static_cast<qlonglong>(now));
            ok = q.exec();
            if (!ok) qWarning() << "VoiceprintStore::upsert failed:" << q.lastError().text();
        });
        const bool finalOk = dbOk && ok;
        QMetaObject::invokeMethod(qApp, [self, finalOk, done]() {
            if (self && done) done(finalOk);
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void VoiceprintStore::loadAll(std::function<void(QVector<Voiceprint>)> done) {
    if (m_dbPath.isEmpty()) {
        if (done) done({});
        return;
    }
    const QString path = m_dbPath;
    QPointer<VoiceprintStore> self(this);
    QThread* thread = QThread::create([self, path, done]() {
        QVector<Voiceprint> out;
        withTempDb(path, "voiceprints_load", [&](QSqlDatabase& db) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT name, vector FROM voiceprints ORDER BY name"))) {
                while (q.next()) {
                    Voiceprint vp;
                    vp.name = q.value(0).toString();
                    vp.vector = blobToVec(q.value(1).toByteArray());
                    out.append(vp);
                }
            }
        });
        QMetaObject::invokeMethod(qApp, [self, out, done]() {
            if (self && done) done(out);
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void VoiceprintStore::remove(const QString& name, std::function<void(bool)> done) {
    if (m_dbPath.isEmpty() || name.trimmed().isEmpty()) {
        if (done) done(false);
        return;
    }
    const QString path = m_dbPath;
    const QString n = name.trimmed();
    QPointer<VoiceprintStore> self(this);
    QThread* thread = QThread::create([self, path, n, done]() {
        bool ok = false;
        const bool dbOk = withTempDb(path, "voiceprints_remove", [&](QSqlDatabase& db) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("DELETE FROM voiceprints WHERE name = :name"));
            q.bindValue(QStringLiteral(":name"), n);
            ok = q.exec();
        });
        const bool finalOk = dbOk && ok;
        QMetaObject::invokeMethod(qApp, [self, finalOk, done]() {
            if (self && done) done(finalOk);
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
