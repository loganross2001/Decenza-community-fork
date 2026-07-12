#include "baristaactions.h"

#include "../core/settings.h"
#include "../core/settings_dye.h"
#include "../core/settings_brew.h"
#include "../machine/machinestate.h"
#include "baristadiagnostics.h"   // [barista-fork] audit apply_from_next

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
    // [barista-fork] Each change is VERIFIED by reading it back after the write; only a confirmed change
    // lands in `applied`/`queued`. `failed` = the app tried but the read-back didn't confirm — the barista
    // must report from THIS result, never from intent, so it can't claim a grind/ratio it didn't actually set.
    QStringList applied, queued, failed;

    QStringList rejected;
    // Never mutate the dial while a shot is physically flowing.
    if (m_machine && m_machine->isFlowing()) {
        result["blocked"] = true;
        result["blockedReason"] = QStringLiteral("a shot is in progress");
        result["applied"] = applied; result["queued"] = queued; result["failed"] = failed; result["rejected"] = rejected;
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
            if (qAbs(dye->dyeBeanWeight() - dose) < 0.05)          // verify the write actually landed
                applied << QStringLiteral("dose %1 g").arg(dose, 0, 'f', 1);
            else failed << QStringLiteral("dose");
        } else { rejected << QStringLiteral("dose"); }
    }
    const double lastRatioBefore = brew ? brew->lastUsedRatio() : 0.0;   // [barista-fork] for the diag + undo
    const double yield = next.value("targetWeightG").toDouble();
    if (brew && yield > 0) {
        if (yield >= 10.0 && yield <= 120.0) {
            m_undo["targetWeightG_had"] = brew->hasBrewYieldOverride();
            m_undo["targetWeightG"] = brew->brewYieldOverride();
            brew->setBrewYieldOverride(yield);
            // [barista-fork] Also sync lastUsedRatio (mirrors BrewDialog): the idle bean auto-capture recomputes
            // the yield from lastUsedRatio on the next weigh-in, so leaving it stale would silently revert this.
            const double curDose = dye ? dye->dyeBeanWeight() : 0.0;
            if (curDose > 0) {
                m_undo["lastUsedRatio_had"] = true;
                m_undo["lastUsedRatio"] = lastRatioBefore;
                brew->setLastUsedRatio(yield / curDose);
            }
            if (qAbs(brew->brewYieldOverride() - yield) < 0.05)
                applied << QStringLiteral("yield %1 g").arg(yield, 0, 'f', 1);
            else failed << QStringLiteral("yield");
        } else { rejected << QStringLiteral("yield"); }
    }
    // Ratio (e.g. 2.0 for 1:2.0): apply it the SAME way the ratio quick-select does — set lastUsedRatio AND the
    // yield override (= ratio × dose, with an 18 g dose fallback like the pill/dialogs), so the ratio pill and
    // Brew Settings reflect it AND the next weigh-in doesn't revert it. An explicit targetWeightG wins.
    const double ratio = next.value("ratio").toDouble();
    if (brew && dye && ratio >= 1.0 && ratio <= 5.0 && yield <= 0) {
        const double baseDose = dye->dyeBeanWeight() > 0 ? dye->dyeBeanWeight() : 18.0;   // pill/dialog fallback
        const double computedYield = baseDose * ratio;
        if (computedYield >= 10.0 && computedYield <= 120.0) {
            m_undo["targetWeightG_had"] = brew->hasBrewYieldOverride();
            m_undo["targetWeightG"] = brew->brewYieldOverride();
            m_undo["lastUsedRatio_had"] = true;
            m_undo["lastUsedRatio"] = lastRatioBefore;
            brew->setLastUsedRatio(ratio);
            brew->setBrewYieldOverride(computedYield);
            if (qAbs(brew->lastUsedRatio() - ratio) < 0.01 && qAbs(brew->brewYieldOverride() - computedYield) < 0.05)
                applied << QStringLiteral("ratio 1:%1").arg(ratio, 0, 'f', 2);
            else failed << QStringLiteral("ratio");
        } else { rejected << QStringLiteral("ratio"); }
    }
    const double temp = next.value("temperatureC").toDouble();
    if (brew && temp > 0) {
        if (temp >= 80.0 && temp <= 100.0) {
            m_undo["temperatureC_had"] = brew->hasTemperatureOverride();
            m_undo["temperatureC"] = brew->temperatureOverride();
            brew->setTemperatureOverride(temp);
            if (qAbs(brew->temperatureOverride() - temp) < 0.05)
                applied << QStringLiteral("%1 °C").arg(temp, 0, 'f', 1);
            else failed << QStringLiteral("temperature");
        } else { rejected << QStringLiteral("temperature"); }
    }
    // Grinder is off-machine — queue it, don't write the dial (the shot must not claim a grind the
    // user never physically set). Resolved to Settings.dye.dyeGrinderSetting on confirmation.
    // Undo snapshots the WHOLE pending queue BEFORE enqueuing (grind lives in the queue, not the dial —
    // undoLast() restores the pre-enqueue list, cleanly reversing the supersede/append enqueueGrind does).
    const QString grind = next.value("grinderSetting").toString().trimmed();
    if (dye && !grind.isEmpty()) {
        m_undo["pendingBefore_had"] = true;
        m_undo["pendingBefore"] = loadPending();
        enqueueGrind(grind, anchorShotId);
        // Verify the enqueue actually persisted a pending setGrinder with this value before claiming it —
        // otherwise the barista would say "grind queued" when nothing was written (the reported bug).
        bool queuedOk = false;
        for (const QVariant& item : loadPending()) {
            const QVariantMap m = item.toMap();
            if (m.value("type").toString() == QLatin1String("setGrinder")
                    && m.value("status").toString() == QLatin1String("pending")
                    && m.value("value").toString() == grind) { queuedOk = true; break; }
        }
        if (queuedOk) queued << QStringLiteral("grinder %1").arg(grind);
        else failed << QStringLiteral("grinder");
    }

    result["applied"] = applied;
    result["queued"] = queued;
    result["failed"] = failed;   // the barista MUST NOT claim anything in here (see the report-from-machine persona rule)
    result["rejected"] = rejected;
    result["blocked"] = false;
    // [barista-fork] Make the apply auditable (was invisible): dose used, ratio, and the lastUsedRatio move so a
    // "pill didn't update" report can be told apart from a rejection or a stale-ratio revert.
    BaristaDiagnostics::record(QStringLiteral("actions"), QStringLiteral("apply_from_next"),
        {{QStringLiteral("ratio"), ratio},
         {QStringLiteral("yield"), yield},
         {QStringLiteral("dose"), dye ? dye->dyeBeanWeight() : 0.0},
         {QStringLiteral("lastRatioBefore"), lastRatioBefore},
         {QStringLiteral("lastRatioAfter"), brew ? brew->lastUsedRatio() : 0.0},
         {QStringLiteral("applied"), applied.join(QLatin1Char(','))},
         {QStringLiteral("failed"), failed.join(QLatin1Char(','))},
         {QStringLiteral("rejected"), rejected.join(QLatin1Char(','))}});
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
    if (brew && m_undo.value("lastUsedRatio_had").toBool())   // [barista-fork] restore the pre-apply ratio
        brew->setLastUsedRatio(m_undo.value("lastUsedRatio").toDouble());
    // Restore the off-machine grind queue to its pre-enqueue state (reverses the supersede+append).
    if (m_undo.value("pendingBefore_had").toBool())
        savePending(m_undo.value("pendingBefore").toList());
    m_undo.clear();
}

bool BaristaActions::undoLastAutoApply() {
    if (m_undo.isEmpty()) return false;
    undoLast();   // one-level snapshot restore (dose/yield/temp override + the queued grind)
    return true;
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
