#include "baristatrace.h"

#include "../history/shotprojection.h"
#include "coffeeknowledgebase.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <cmath>

namespace BaristaTrace {

namespace {
// One decimal, units-free — the caller's evidence strings supply the units.
QString num(double v, int prec = 2) { return QString::number(v, 'f', prec); }
}  // namespace

// The mapping is deliberately a cascade of clean boolean/enum reads off the detector verdicts —
// no thresholds are re-derived here. grindDirection is exactly one label when the grind detector
// has data (yieldOvershoot / chokedPuck / tooFine / tooCoarse / onTarget), so the grind arms are
// mutually exclusive by construction. The two threshold-dependent inferred signatures the KB also
// carries (early_first_drops, trace_fast_pressure_bleed) are NOT emitted: they would fire off a
// near-universal observation (preinfusionObserved) and assert a fault on normal shots — deferred
// until a rate threshold is validated against the shot_eval corpus. See DESIGN §2.2(b).
QVector<SignatureHit> objectiveTraceSignatures(const ShotProjection &s)
{
    QVector<SignatureHit> hits;

    const QVariantMap channeling = s.detectorResults.value(QStringLiteral("channeling")).toMap();
    const QVariantMap grind      = s.detectorResults.value(QStringLiteral("grind")).toMap();
    const QVariantMap flowTrend  = s.detectorResults.value(QStringLiteral("flowTrend")).toMap();

    // --- Channeling (prep class), MEASURED. sustained = water escaping via a channel;
    // transient = a channel opened and healed. Mirrors objectiveTraceShape's severity read. ---
    const QString severity  = channeling.value(QStringLiteral("severity")).toString();
    const double  spikeTime = channeling.value(QStringLiteral("spikeTimeSec")).toDouble();
    if (severity == QLatin1String("sustained")) {
        QString ev = QStringLiteral("sustained channeling detected");
        if (spikeTime > 0.0) ev += QStringLiteral(", from %1 s into the pour").arg(num(spikeTime, 1));
        hits.push_back({QStringLiteral("flow_exceeds_pressure"), QStringLiteral("measured"), ev});
    } else if (severity == QLatin1String("transient")) {
        QString ev = QStringLiteral("a channel opened and healed mid-pour");
        if (spikeTime > 0.0) ev += QStringLiteral(", around %1 s").arg(num(spikeTime, 1));
        hits.push_back({QStringLiteral("pressure_notch_heal"), QStringLiteral("measured"), ev});
    }

    // --- Grind-class verdicts, mutually exclusive. choke / gusher are MEASURED. A too-fine puck
    // splits: with flow trending DOWN it is a measured post-PI stall; alone it is only consistent
    // with a flow-capped frame never reaching its cap (average shortfall != per-sample "never"). ---
    const QString dir = grind.value(QStringLiteral("direction")).toString();
    if (grind.value(QStringLiteral("chokedPuck")).toBool() || dir == QLatin1String("chokedPuck")) {
        QString ev = QStringLiteral("flow stayed pinned low under full pressure");
        const QVariantMap gates = grind.value(QStringLiteral("gates")).toMap();
        if (gates.contains(QStringLiteral("meanPressurizedFlowMlPerSec")))
            ev = QStringLiteral("mean flow %1 ml/s across %2 s of pressure")
                     .arg(num(gates.value(QStringLiteral("meanPressurizedFlowMlPerSec")).toDouble()))
                     .arg(num(gates.value(QStringLiteral("pressurizedDurationSec")).toDouble(), 0));
        hits.push_back({QStringLiteral("choke"), QStringLiteral("measured"), ev});
    } else if (grind.value(QStringLiteral("yieldOvershoot")).toBool() || dir == QLatin1String("yieldOvershoot")) {
        QString ev = QStringLiteral("yield overshot the target");
        if (grind.contains(QStringLiteral("yieldRatio")))
            ev = QStringLiteral("yield ran %1x the target").arg(num(grind.value(QStringLiteral("yieldRatio")).toDouble()));
        hits.push_back({QStringLiteral("gusher"), QStringLiteral("measured"), ev});
    } else if (dir == QLatin1String("tooFine")) {
        const double flowShortfall = std::abs(grind.value(QStringLiteral("deltaMlPerSec")).toDouble());
        if (flowTrend.value(QStringLiteral("direction")).toString() == QLatin1String("falling")) {
            const double drop = std::abs(flowTrend.value(QStringLiteral("deltaMlPerSec")).toDouble());
            hits.push_back({QStringLiteral("flow_stall_after_pi"), QStringLiteral("measured"),
                            QStringLiteral("flow ran %1 ml/s under the profile's target and fell a further %2 ml/s across the pour")
                                .arg(num(flowShortfall)).arg(num(drop))});
        } else {
            hits.push_back({QStringLiteral("trace_flow_never_caps"), QStringLiteral("inferred"),
                            QStringLiteral("flow averaged %1 ml/s under the profile's target — consistent with the puck being too resistive for this profile, not confirmed")
                                .arg(num(flowShortfall))});
        }
    }

    return hits;
}

QJsonObject buildLastShotTraceRead(const ShotProjection &s, const CoffeeKnowledgeBase &kb)
{
    const QVector<SignatureHit> hits = objectiveTraceSignatures(s);
    if (hits.isEmpty())
        return {};

    QJsonArray measured, inferred;
    int emitted = 0;
    for (const SignatureHit &h : hits) {
        if (emitted >= 2)  // opening-read discipline: at most 2 entries (bridge emits ≤2 anyway)
            break;
        const QJsonObject sig = kb.traceSignature(h.signatureId);
        if (!sig.value(QStringLiteral("found")).toBool())
            continue;  // never emit an id the KB can't cite — the no-invented-signature contract

        QJsonObject entry{
            {QStringLiteral("signatureId"), h.signatureId},
            {QStringLiteral("class"), sig.value(QStringLiteral("class"))},
            {QStringLiteral("nextChange"), sig.value(QStringLiteral("next_change"))},
        };
        if (const QJsonArray cites = sig.value(QStringLiteral("citations")).toArray(); !cites.isEmpty())
            entry.insert(QStringLiteral("citations"), QJsonArray{cites.first()});  // one citation is enough to ground the claim

        if (h.grounding == QLatin1String("measured")) {
            entry.insert(QStringLiteral("evidence"), h.evidence);
            entry.insert(QStringLiteral("meaning"), sig.value(QStringLiteral("meaning")));
            measured.append(entry);
        } else {
            entry.insert(QStringLiteral("basis"), h.evidence);  // the "consistent-with, not confirmed" wording
            inferred.append(entry);
        }
        ++emitted;
    }

    if (measured.isEmpty() && inferred.isEmpty())
        return {};

    QJsonObject out;
    if (!measured.isEmpty()) out.insert(QStringLiteral("measured"), measured);
    if (!inferred.isEmpty()) out.insert(QStringLiteral("inferred"), inferred);
    out.insert(QStringLiteral("note"), QStringLiteral(
        "measured = the machine detected it on the LAST shot; state it plainly with its evidence. "
        "inferred = consistent-with only; raise it as a maybe or not at all. "
        "Absent entirely = do not claim any curve fault."));
    return out;
}

}  // namespace BaristaTrace
