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
#include <QRegularExpression>

BaristaActions::BaristaActions(Settings* settings, MachineState* machineState, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_machine(machineState) {}

// ── apply ──────────────────────────────────────────────────────────────────────
QVariantMap BaristaActions::applyFromNext(const QVariantMap& next, qint64 anchorShotId) {
    QVariantMap result;
    QStringList applied, queued;

    QStringList rejected;
    // Never mutate the dial while a shot is physically flowing.
    if (m_machine && m_machine->isFlowing()) {
        result["blocked"] = true;
        result["blockedReason"] = QStringLiteral("a shot is in progress");
        result["applied"] = applied; result["queued"] = queued; result["rejected"] = rejected;
        return result;
    }

    m_undo.clear();
    SettingsDye* dye = m_settings ? m_settings->dye() : nullptr;
    SettingsBrew* brew = m_settings ? m_settings->brew() : nullptr;

    // Reject values outside sane espresso ranges (S2) — a hallucinated 200 °C or 360 g must never reach
    // the machine sight-unseen on a voice "OK".
    const double dose = next.value("doseG").toDouble();
    if (dye && dose > 0) {
        if (dose >= 5.0 && dose <= 30.0) {
            m_undo["doseG"] = dye->dyeBeanWeight();
            dye->setDyeBeanWeight(dose);
            applied << QStringLiteral("dose %1 g").arg(dose, 0, 'f', 1);
        } else { rejected << QStringLiteral("dose"); }
    }
    const double yield = next.value("targetWeightG").toDouble();
    if (brew && yield > 0) {
        if (yield >= 10.0 && yield <= 120.0) {
            m_undo["targetWeightG_had"] = brew->hasBrewYieldOverride();
            m_undo["targetWeightG"] = brew->brewYieldOverride();
            brew->setBrewYieldOverride(yield);
            applied << QStringLiteral("yield %1 g").arg(yield, 0, 'f', 1);
        } else { rejected << QStringLiteral("yield"); }
    }
    // Ratio (e.g. 2.0 for 1:2.0): compute the yield from ratio × the (current or just-set) dose, so a
    // "make it 1:2.5" request applies without the model doing the arithmetic. An explicit targetWeightG
    // wins; ratio only fills in when no yield was given.
    const double ratio = next.value("ratio").toDouble();
    if (brew && dye && ratio >= 1.0 && ratio <= 5.0 && yield <= 0) {
        const double baseDose = dye->dyeBeanWeight();   // reflects the dose just applied above, if any
        const double computedYield = baseDose * ratio;
        if (baseDose > 0 && computedYield >= 10.0 && computedYield <= 120.0) {
            m_undo["targetWeightG_had"] = brew->hasBrewYieldOverride();
            m_undo["targetWeightG"] = brew->brewYieldOverride();
            brew->setBrewYieldOverride(computedYield);
            applied << QStringLiteral("ratio 1:%1 → yield %2 g").arg(ratio, 0, 'f', 2).arg(computedYield, 0, 'f', 1);
        } else { rejected << QStringLiteral("ratio"); }
    }
    const double temp = next.value("temperatureC").toDouble();
    if (brew && temp > 0) {
        if (temp >= 80.0 && temp <= 100.0) {
            m_undo["temperatureC_had"] = brew->hasTemperatureOverride();
            m_undo["temperatureC"] = brew->temperatureOverride();
            brew->setTemperatureOverride(temp);
            applied << QStringLiteral("%1 °C").arg(temp, 0, 'f', 1);
        } else { rejected << QStringLiteral("temperature"); }
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
    result["rejected"] = rejected;
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
    if (brew && m_undo.contains("targetWeightG")) {
        if (m_undo.value("targetWeightG_had").toBool())
            brew->setBrewYieldOverride(m_undo.value("targetWeightG").toDouble());
        else
            brew->setBrewYieldOverride(0);   // S3: clear the override we created (0 = no override)
    }
    m_undo.clear();
}

// ── confirmation parsing ─────────────────────────────────────────────────────────
std::optional<bool> BaristaActions::parseConfirmationReply(const QString& reply) {
    QString r = reply.trimmed().toLower();
    if (r.isEmpty()) return std::nullopt;
    // Only SHORT replies are confirmations. "it was okay, a bit sour" is taste feedback, NOT "OK" —
    // matching a substring there would silently apply a dial change (B4). Normalise + word-split.
    r.replace(QRegularExpression(QStringLiteral("[^a-z0-9'\\s]")), QStringLiteral(" "));
    const QStringList words = r.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.isEmpty() || words.size() > 4)
        return std::nullopt;   // a sentence → treat as normal conversation, not a yes/no
    const QString padded = QLatin1Char(' ') + words.join(QLatin1Char(' ')) + QLatin1Char(' ');
    // Whole-word / whole-phrase matching only (each entry padded with spaces on both sides).
    static const QStringList no = {
        QStringLiteral("no"), QStringLiteral("nope"), QStringLiteral("nah"), QStringLiteral("skip"),
        QStringLiteral("leave it"), QStringLiteral("dont"), QStringLiteral("do not"),
        QStringLiteral("cancel"), QStringLiteral("not now"), QStringLiteral("not yet"),
        QStringLiteral("never mind"), QStringLiteral("nevermind")
    };
    static const QStringList yes = {
        QStringLiteral("ok"), QStringLiteral("okay"), QStringLiteral("yes"), QStringLiteral("yeah"),
        QStringLiteral("yep"), QStringLiteral("yup"), QStringLiteral("sure"), QStringLiteral("do it"),
        QStringLiteral("go ahead"), QStringLiteral("go for it"), QStringLiteral("apply"),
        QStringLiteral("set it"), QStringLiteral("please do"), QStringLiteral("sounds good"),
        QStringLiteral("confirmed"), QStringLiteral("done"), QStringLiteral("lets do it")
    };
    auto phraseHit = [&padded](const QString& t) {
        return padded.contains(QLatin1Char(' ') + t + QLatin1Char(' '));
    };
    for (const QString& n : no)  if (phraseHit(n)) return false;   // negatives win ("not now", "leave it")
    for (const QString& y : yes) if (phraseHit(y)) return true;
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
    // S8: drop long-resolved records (done/declined/superseded/expired > 30 days) so the queue, which is
    // parsed on every greeting, can't grow without bound.
    {
        const QDateTime cutoffNow = QDateTime::currentDateTime();
        for (int i = list.size() - 1; i >= 0; --i) {
            const QVariantMap m = list.at(i).toMap();
            if (m.value("status").toString() == QStringLiteral("pending")) continue;
            const QString stamp = !m.value("resolvedAt").toString().isEmpty()
                ? m.value("resolvedAt").toString() : m.value("createdAt").toString();
            const QDateTime when = QDateTime::fromString(stamp, Qt::ISODate);
            if (when.isValid() && when.daysTo(cutoffNow) > 30)
                list.removeAt(i);
        }
    }
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
