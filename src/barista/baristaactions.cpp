#include "baristaactions.h"

#include "../core/settings.h"
#include "../core/settings_dye.h"
#include "../core/settings_brew.h"
#include "../machine/machinestate.h"

#include <QSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QUuid>
#include <QStringList>

BaristaActions::BaristaActions(Settings* settings, MachineState* machineState, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_machine(machineState) {}

// ── apply ──────────────────────────────────────────────────────────────────────
QVariantMap BaristaActions::applyFromNext(const QVariantMap& next, qint64 anchorShotId) {
    QVariantMap result;
    QStringList applied, queued;

    // Never mutate the dial while a shot is physically flowing.
    if (m_machine && m_machine->isFlowing()) {
        result["blocked"] = true;
        result["blockedReason"] = QStringLiteral("a shot is in progress");
        result["applied"] = applied; result["queued"] = queued;
        return result;
    }

    m_undo.clear();
    SettingsDye* dye = m_settings ? m_settings->dye() : nullptr;
    SettingsBrew* brew = m_settings ? m_settings->brew() : nullptr;

    if (dye && next.value("doseG").toDouble() > 0) {
        m_undo["doseG"] = dye->dyeBeanWeight();
        dye->setDyeBeanWeight(next.value("doseG").toDouble());
        applied << QStringLiteral("dose %1 g").arg(next.value("doseG").toDouble(), 0, 'f', 1);
    }
    if (brew && next.value("targetWeightG").toDouble() > 0) {
        m_undo["targetWeightG_had"] = brew->hasBrewYieldOverride();
        m_undo["targetWeightG"] = brew->brewYieldOverride();
        brew->setBrewYieldOverride(next.value("targetWeightG").toDouble());
        applied << QStringLiteral("yield %1 g").arg(next.value("targetWeightG").toDouble(), 0, 'f', 1);
    }
    if (brew && next.value("temperatureC").toDouble() > 0) {
        m_undo["temperatureC_had"] = brew->hasTemperatureOverride();
        m_undo["temperatureC"] = brew->temperatureOverride();
        brew->setTemperatureOverride(next.value("temperatureC").toDouble());
        applied << QStringLiteral("%1 °C").arg(next.value("temperatureC").toDouble(), 0, 'f', 1);
    }
    // Grinder is off-machine — queue it, don't write the dial (the shot must not claim a grind the
    // user never physically set). Resolved to Settings.dye.dyeGrinderSetting on confirmation.
    const QString grind = next.value("grinderSetting").toString().trimmed();
    if (dye && !grind.isEmpty()) {
        enqueueGrind(grind, anchorShotId);
        queued << QStringLiteral("grinder %1").arg(grind);
    }

    result["applied"] = applied;
    result["queued"] = queued;
    result["blocked"] = false;
    return result;
}

void BaristaActions::undoLast() {
    if (m_undo.isEmpty()) return;
    SettingsDye* dye = m_settings ? m_settings->dye() : nullptr;
    SettingsBrew* brew = m_settings ? m_settings->brew() : nullptr;
    if (dye && m_undo.contains("doseG"))
        dye->setDyeBeanWeight(m_undo.value("doseG").toDouble());
    if (brew && m_undo.contains("temperatureC")) {
        if (m_undo.value("temperatureC_had").toBool())
            brew->setTemperatureOverride(m_undo.value("temperatureC").toDouble());
        else
            brew->clearTemperatureOverride();
    }
    if (brew && m_undo.contains("targetWeightG") && m_undo.value("targetWeightG_had").toBool())
        brew->setBrewYieldOverride(m_undo.value("targetWeightG").toDouble());
    m_undo.clear();
}

// ── confirmation parsing ─────────────────────────────────────────────────────────
std::optional<bool> BaristaActions::parseConfirmationReply(const QString& reply) {
    const QString r = reply.trimmed().toLower();
    if (r.isEmpty()) return std::nullopt;
    static const QStringList no = {
        QStringLiteral("no"), QStringLiteral("nope"), QStringLiteral("nah"), QStringLiteral("skip"),
        QStringLiteral("leave it"), QStringLiteral("don't"), QStringLiteral("do not"),
        QStringLiteral("cancel"), QStringLiteral("not now"), QStringLiteral("never mind"),
        QStringLiteral("nevermind")
    };
    static const QStringList yes = {
        QStringLiteral("ok"), QStringLiteral("okay"), QStringLiteral("yes"), QStringLiteral("yeah"),
        QStringLiteral("yep"), QStringLiteral("yup"), QStringLiteral("sure"), QStringLiteral("do it"),
        QStringLiteral("go ahead"), QStringLiteral("apply"), QStringLiteral("set it"),
        QStringLiteral("please do"), QStringLiteral("sounds good"), QStringLiteral("confirmed"),
        QStringLiteral("done"), QStringLiteral("did it"), QStringLiteral("let's do it")
    };
    auto hit = [&r](const QString& t) {
        return r == t || r.startsWith(t + QLatin1Char(' ')) || r.contains(QLatin1Char(' ') + t);
    };
    for (const QString& n : no)  if (hit(n)) return false;   // negatives win ("not now", "leave it")
    for (const QString& y : yes) if (hit(y)) return true;
    return std::nullopt;
}

int BaristaActions::parseConfirmation(const QString& reply) const {
    const std::optional<bool> r = parseConfirmationReply(reply);
    return r.has_value() ? (r.value() ? 1 : 0) : -1;
}

// ── pending grinder queue ────────────────────────────────────────────────────────
QVariantList BaristaActions::loadPending() const {
    const QByteArray json = QSettings().value(QStringLiteral("barista/pendingActions")).toByteArray();
    if (json.isEmpty()) return {};
    return QJsonDocument::fromJson(json).array().toVariantList();
}

void BaristaActions::savePending(const QVariantList& list) {
    const QJsonArray arr = QJsonArray::fromVariantList(list);
    QSettings().setValue(QStringLiteral("barista/pendingActions"),
                         QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void BaristaActions::expireStale(QVariantList& list) const {
    const QDateTime now = QDateTime::currentDateTime();
    for (QVariant& v : list) {
        QVariantMap m = v.toMap();
        if (m.value("status").toString() != QStringLiteral("pending")) continue;
        const QDateTime created = QDateTime::fromString(m.value("createdAt").toString(), Qt::ISODate);
        if (created.isValid() && created.daysTo(now) > 14) {
            m["status"] = QStringLiteral("expired");
            v = m;
        }
    }
}

void BaristaActions::enqueueGrind(const QString& value, qint64 anchorShotId) {
    const QString v = value.trimmed();
    if (v.isEmpty()) return;
    QVariantList list = loadPending();
    expireStale(list);
    // Only the newest grinder recommendation matters — supersede older still-pending ones.
    for (QVariant& item : list) {
        QVariantMap m = item.toMap();
        if (m.value("type").toString() == QStringLiteral("setGrinder")
                && m.value("status").toString() == QStringLiteral("pending")) {
            m["status"] = QStringLiteral("superseded");
            item = m;
        }
    }
    const QString prev = (m_settings && m_settings->dye()) ? m_settings->dye()->dyeGrinderSetting() : QString();
    QVariantMap rec;
    rec["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    rec["type"] = QStringLiteral("setGrinder");
    rec["value"] = v;
    rec["prevValue"] = prev;
    rec["createdAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    rec["anchorShotId"] = anchorShotId;
    rec["status"] = QStringLiteral("pending");
    list.append(rec);
    savePending(list);
}

QVariantMap BaristaActions::outstandingGrind() {
    QVariantList list = loadPending();
    const QVariantList before = list;
    expireStale(list);
    if (list != before) savePending(list);
    QVariantMap newest;   // append order → last pending is newest
    for (const QVariant& v : list) {
        const QVariantMap m = v.toMap();
        if (m.value("type").toString() == QStringLiteral("setGrinder")
                && m.value("status").toString() == QStringLiteral("pending"))
            newest = m;
    }
    return newest;
}

void BaristaActions::resolveGrind(bool done) {
    QVariantList list = loadPending();
    int idx = -1;
    for (int i = 0; i < list.size(); ++i) {
        const QVariantMap m = list.at(i).toMap();
        if (m.value("type").toString() == QStringLiteral("setGrinder")
                && m.value("status").toString() == QStringLiteral("pending"))
            idx = i;
    }
    if (idx < 0) return;
    QVariantMap m = list.at(idx).toMap();
    if (done && m_settings && m_settings->dye())
        m_settings->dye()->setDyeGrinderSetting(m.value("value").toString());
    m["status"] = done ? QStringLiteral("done") : QStringLiteral("declined");
    m["resolvedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    list[idx] = m;
    savePending(list);
}
