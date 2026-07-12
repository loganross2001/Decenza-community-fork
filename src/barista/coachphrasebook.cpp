#include "coachphrasebook.h"

#include "../ai/aimanager.h"
#include "../core/dbutils.h"
#include "baristadiagnostics.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDatabase>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

namespace {
constexpr qint64 kFreshSecs = 7LL * 24 * 3600;   // regenerate weekly (or on bean change)

// Reject a model line that leaked a placeholder / is empty / too long for a spoken cue.
bool badLine(const QString& s) {
    return s.trimmed().isEmpty() || s.size() > 120
        || s.contains('%') || s.contains('{') || s.contains('}');
}
}  // namespace

CoachPhrasebook::CoachPhrasebook(AIManager* ai, QObject* parent)
    : QObject(parent), m_ai(ai) {
    if (m_ai) {
        connect(m_ai, &AIManager::phrasebookReady, this, &CoachPhrasebook::onReady);
        connect(m_ai, &AIManager::phrasebookFailed, this, &CoachPhrasebook::onFailed);
    }
}

void CoachPhrasebook::initialize(const QString& assistantDbPath) {
    m_dbPath = assistantDbPath;
    withTempDb(m_dbPath, "coach_pb_init", [](QSqlDatabase& db) {
        QSqlQuery q(db);
        q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS coach_phrasebook ("
            "id INTEGER PRIMARY KEY CHECK(id=1), json TEXT, generated_at INTEGER, bean TEXT)"));
    });
    load();
}

QString CoachPhrasebook::lineFor(const QString& cueId, const QString& fallback) {
    auto it = m_pools.find(cueId);
    if (it == m_pools.end() || it->isEmpty()) {
        BaristaDiagnostics::record(QStringLiteral("coach"), QStringLiteral("pool_pick"),
            {{QStringLiteral("cueId"), cueId}, {QStringLiteral("source"), QStringLiteral("fallback")}});
        return fallback;
    }
    const int idx = (m_lastIdx.value(cueId, -1) + 1) % it->size();
    m_lastIdx[cueId] = idx;
    BaristaDiagnostics::record(QStringLiteral("coach"), QStringLiteral("pool_pick"),
        {{QStringLiteral("cueId"), cueId}, {QStringLiteral("source"), QStringLiteral("model")}});
    return it->at(idx);
}

bool CoachPhrasebook::isFresh(const QString& beanId) const {
    if (m_pools.isEmpty() || m_generatedAt <= 0 || m_bean != beanId)
        return false;
    return (QDateTime::currentSecsSinceEpoch() - m_generatedAt) < kFreshSecs;
}

void CoachPhrasebook::refresh(const QString& contextBlock, const QString& beanId, bool force) {
    if (!m_ai)
        return;
    if (!force && isFresh(beanId))
        return;
    if (!m_token.isEmpty())
        return;   // a request is already in flight
    m_token = QStringLiteral("pb-%1-%2").arg(beanId, QString::number(QDateTime::currentMSecsSinceEpoch()));
    m_bean = beanId;   // remember what this refill is for (freshness is bean-scoped)
    m_ai->requestCoachPhrasebook(m_token, contextBlock);
}

void CoachPhrasebook::onReady(const QString& token, const QString& json) {
    if (token != m_token)
        return;   // not ours (or superseded)
    m_token.clear();

    // Strip any ```json fences / prose the model added around the object.
    QString body = json.trimmed();
    const int b = body.indexOf('{');
    const int e = body.lastIndexOf('}');
    if (b >= 0 && e > b)
        body = body.mid(b, e - b + 1);

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        BaristaDiagnostics::record(QStringLiteral("coach"), QStringLiteral("pool_refill"),
            {{QStringLiteral("ok"), false}, {QStringLiteral("reason"), QStringLiteral("parse")}});
        return;
    }

    const QJsonObject obj = doc.object();
    QHash<QString, QStringList> pools;
    int lineCount = 0;
    const QJsonObject cues = obj.value(QStringLiteral("cues")).toObject();
    for (auto it = cues.begin(); it != cues.end(); ++it) {
        QStringList lines;
        for (const QJsonValue& v : it.value().toArray()) {
            const QString s = v.toString().trimmed();
            if (!badLine(s)) { lines << s; ++lineCount; }
        }
        if (!lines.isEmpty())
            pools.insert(it.key(), lines);
    }
    if (pools.isEmpty()) {
        BaristaDiagnostics::record(QStringLiteral("coach"), QStringLiteral("pool_refill"),
            {{QStringLiteral("ok"), false}, {QStringLiteral("reason"), QStringLiteral("empty")}});
        return;
    }

    const QString plan = obj.value(QStringLiteral("gameplan")).toString().trimmed();
    m_pools = pools;
    m_lastIdx.clear();
    m_gameplan = (plan.isEmpty() || plan.contains('%') || plan.contains('{')) ? QString() : plan;
    m_generatedAt = QDateTime::currentSecsSinceEpoch();
    save();
    BaristaDiagnostics::record(QStringLiteral("coach"), QStringLiteral("pool_refill"),
        {{QStringLiteral("ok"), true}, {QStringLiteral("lineCount"), lineCount}});
}

void CoachPhrasebook::onFailed(const QString& token, const QString& error) {
    if (token != m_token)
        return;
    m_token.clear();
    BaristaDiagnostics::record(QStringLiteral("coach"), QStringLiteral("pool_refill"),
        {{QStringLiteral("ok"), false}, {QStringLiteral("reason"), error}});
}

void CoachPhrasebook::load() {
    if (m_dbPath.isEmpty())
        return;
    QString json;
    qint64 gen = 0;
    QString bean;
    withTempDb(m_dbPath, "coach_pb_load", [&](QSqlDatabase& db) {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("SELECT json, generated_at, bean FROM coach_phrasebook WHERE id=1")) && q.next()) {
            json = q.value(0).toString();
            gen = q.value(1).toLongLong();
            bean = q.value(2).toString();
        }
    });
    if (json.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return;
    const QJsonObject obj = doc.object();
    m_pools.clear();
    const QJsonObject cues = obj.value(QStringLiteral("cues")).toObject();
    for (auto it = cues.begin(); it != cues.end(); ++it) {
        QStringList lines;
        for (const QJsonValue& v : it.value().toArray())
            lines << v.toString();
        if (!lines.isEmpty())
            m_pools.insert(it.key(), lines);
    }
    m_gameplan = obj.value(QStringLiteral("gameplan")).toString();
    m_generatedAt = gen;
    m_bean = bean;
}

void CoachPhrasebook::save() {
    if (m_dbPath.isEmpty())
        return;
    QJsonObject cues;
    for (auto it = m_pools.begin(); it != m_pools.end(); ++it) {
        QJsonArray arr;
        for (const QString& s : it.value())
            arr.append(s);
        cues.insert(it.key(), arr);
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cues"), cues);
    obj.insert(QStringLiteral("gameplan"), m_gameplan);
    const QString json = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    const qint64 gen = m_generatedAt;
    const QString bean = m_bean;
    withTempDb(m_dbPath, "coach_pb_save", [&](QSqlDatabase& db) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO coach_phrasebook (id, json, generated_at, bean) VALUES (1, :j, :g, :b) "
            "ON CONFLICT(id) DO UPDATE SET json=:j2, generated_at=:g2, bean=:b2"));
        q.bindValue(QStringLiteral(":j"), json);
        q.bindValue(QStringLiteral(":g"), gen);
        q.bindValue(QStringLiteral(":b"), bean);
        q.bindValue(QStringLiteral(":j2"), json);
        q.bindValue(QStringLiteral(":g2"), gen);
        q.bindValue(QStringLiteral(":b2"), bean);
        if (!q.exec())
            qWarning() << "CoachPhrasebook::save failed:" << q.lastError().text();
    });
}
