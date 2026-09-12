// [barista-fork] Tests for CoffeeKnowledgeBase — the grounded coffee-science brain.
//
// Loads the SHIPPED resources/barista/coffee_knowledge.json (via DECENZA_SOURCE_DIR, not a
// duplicated copy) through CoffeeKnowledgeBase::fromJson, then exercises the three query paths
// the barista tools call: translate_taste, recommend_next_shot, plan_for_goal. The reasoning
// rails from the research brief are asserted here — prep-before-parameters when the trace
// channels, roast-conditioned goal paths, and a citation attached to every recommendation.

#include <QtTest/QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>

#include "barista/coffeeknowledgebase.h"
#include "barista/baristatrace.h"        // [barista-fork] detector verdicts -> KB trace_signatures
#include "history/shotprojection.h"

class TstCoffeeKnowledgeBase : public QObject {
    Q_OBJECT

    static QByteArray shippedKb() {
        QFile f(QStringLiteral(DECENZA_SOURCE_DIR "/resources/barista/coffee_knowledge.json"));
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return f.readAll();
    }

private slots:
    void init() { QTest::failOnWarning(); }

    void loadsShippedKb() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        QVERIFY2(kb.isLoaded(), "shipped coffee_knowledge.json must load with descriptors + diagnostics");
    }

    void translateResolvesSynonymAndCites() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        // "sharp" is a synonym of the "sour" descriptor.
        const QJsonObject r = kb.translateTaste(QStringLiteral("sharp"));
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        QCOMPARE(r.value(QStringLiteral("modality")).toString(), QStringLiteral("taste"));
        QCOMPARE(r.value(QStringLiteral("extraction_direction")).toString(), QStringLiteral("under"));
        const QJsonArray cites = r.value(QStringLiteral("citations")).toArray();
        QVERIFY2(!cites.isEmpty(), "a translated descriptor must carry at least one citation");
        // The contested alternative (channeling, not global under-extraction) is preserved, not collapsed.
        bool anyContested = false;
        for (const QJsonValue& c : cites)
            anyContested = anyContested || c.toObject().value(QStringLiteral("contested")).toBool();
        QVERIFY2(anyContested, "sour carries a contested alternative-cause citation");
    }

    void translateUnknownWordIsHonest() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.translateTaste(QStringLiteral("zorptastic"));
        QVERIFY(!r.value(QStringLiteral("found")).toBool());
    }

    // Fast, clean, medium roast + sour → global under-extraction → grind finer. High confidence
    // (two conditions confirmed), no prep gate.
    void recommendSourFastGrindsFiner() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.recommendNextShot(QJsonObject{
            {QStringLiteral("taste"), QStringLiteral("sour")},
            {QStringLiteral("roast"), QStringLiteral("medium")},
            {QStringLiteral("trace"), QStringLiteral("fast")},
        });
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        const QJsonObject rec = r.value(QStringLiteral("recommendation")).toObject();
        QVERIFY2(rec.value(QStringLiteral("change")).toString().contains(QStringLiteral("finer")),
                 "a fast, clean, sour medium roast should be told to grind finer");
        QCOMPARE(r.value(QStringLiteral("confidence")).toString(), QStringLiteral("high"));
        QVERIFY(!r.value(QStringLiteral("prep_gate")).toBool());
        QVERIFY2(!r.value(QStringLiteral("citations")).toArray().isEmpty(),
                 "every recommendation must be citable");
    }

    // The prep-before-parameters rail: a channeling trace wins over any grind advice, priority 1.
    void recommendChannelingHitsPrepGate() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.recommendNextShot(QJsonObject{
            {QStringLiteral("taste"), QStringLiteral("sour")},
            {QStringLiteral("trace"), QStringLiteral("channeling")},
        });
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        QVERIFY2(r.value(QStringLiteral("prep_gate")).toBool(),
                 "a channeling trace must trip the prep gate");
        const QJsonObject rec = r.value(QStringLiteral("recommendation")).toObject();
        const QString change = rec.value(QStringLiteral("change")).toString().toLower();
        QVERIFY2(change.contains(QStringLiteral("prep")) || change.contains(QStringLiteral("wdt")),
                 "channeling should be fixed with puck prep, not a grind number");
        QCOMPARE(rec.value(QStringLiteral("priority")).toInt(), 1);
    }

    // A conflicting condition drops a record: a fast trace must NOT return the "runs slow → coarser"
    // bitter rule, and a light roast must not get the medium-roast grind rule.
    void conflictingConditionsAreExcluded() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        // Light-roast sour, smooth: the inherent-brightness rule (asks intent) beats the medium rule.
        const QJsonObject r = kb.recommendNextShot(QJsonObject{
            {QStringLiteral("taste"), QStringLiteral("sour")},
            {QStringLiteral("roast"), QStringLiteral("light")},
            {QStringLiteral("trace"), QStringLiteral("smooth")},
        });
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        QCOMPARE(r.value(QStringLiteral("recommendation")).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("sour_light_inherent"));
    }

    void recommendBitterSlowGrindsCoarser() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.recommendNextShot(QJsonObject{
            {QStringLiteral("taste"), QStringLiteral("bitter")},
            {QStringLiteral("roast"), QStringLiteral("medium")},
            {QStringLiteral("trace"), QStringLiteral("slow")},
        });
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        QVERIFY(r.value(QStringLiteral("recommendation")).toObject()
                    .value(QStringLiteral("change")).toString().contains(QStringLiteral("coarser")));
    }

    // Bare symptom with no context → still answers, but low confidence and asks for what would sharpen it.
    void recommendWithNoContextAsksForMore() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.recommendNextShot(QJsonObject{{QStringLiteral("taste"), QStringLiteral("sour")}});
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        QCOMPARE(r.value(QStringLiteral("confidence")).toString(), QStringLiteral("low"));
        QVERIFY2(!r.value(QStringLiteral("need_more_context")).toArray().isEmpty(),
                 "with no trace/roast the KB should ask for what would disambiguate");
    }

    // Roast conditions the goal path: "sweeter" on a light roast means MORE extraction, not less.
    void planForGoalIsRoastAware() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.planForGoal(QStringLiteral("sweeter"), QStringLiteral("light"));
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        QVERIFY(!r.value(QStringLiteral("target_region")).toString().isEmpty());
        QVERIFY2(r.value(QStringLiteral("roast_specific_path")).toString().contains(QStringLiteral("MORE")),
                 "sweeter on a light roast should push toward MORE extraction");
    }

    void planForUnknownGoalIsHonest() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.planForGoal(QStringLiteral("make it purple"), QString());
        QVERIFY(!r.value(QStringLiteral("found")).toBool());
    }

    // ===== [barista-fork] Trace->KB signature bridge (T1) =====================================
    // BaristaTrace::objectiveTraceSignatures maps detector verdicts onto the KB's named
    // trace_signatures, and CoffeeKnowledgeBase::traceSignature(id) resolves each to its cited
    // meaning. The load-bearing contract: every id the bridge can emit MUST resolve in the shipped
    // KB (a typo is a build-time failure here, never a runtime-invented signature).

    static ShotProjection shotWith(const QVariantMap &detectorResults) {
        ShotProjection s;
        s.detectorResults = detectorResults;
        return s;
    }
    static QVariantMap grindMap(const QString &direction, const QVariantMap &extra = {}) {
        QVariantMap g{{QStringLiteral("checked"), true}, {QStringLiteral("hasData"), true},
                      {QStringLiteral("direction"), direction}};
        for (auto it = extra.cbegin(); it != extra.cend(); ++it) g.insert(it.key(), it.value());
        return g;
    }
    static QVariantMap channelingMap(const QString &severity, double spikeSec = 0.0) {
        return {{QStringLiteral("checked"), true}, {QStringLiteral("severity"), severity},
                {QStringLiteral("spikeTimeSec"), spikeSec}};
    }
    static QVariantMap flowTrendMap(const QString &direction, double deltaMlPerSec = 0.0) {
        return {{QStringLiteral("checked"), true}, {QStringLiteral("direction"), direction},
                {QStringLiteral("deltaMlPerSec"), deltaMlPerSec}};
    }

    void traceSignatureResolvesAndCites() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = kb.traceSignature(QStringLiteral("choke"));
        QVERIFY(r.value(QStringLiteral("found")).toBool());
        QCOMPARE(r.value(QStringLiteral("class")).toString(), QStringLiteral("grind"));
        QVERIFY2(!r.value(QStringLiteral("meaning")).toString().isEmpty(), "signature carries a meaning");
        QVERIFY2(!r.value(QStringLiteral("next_change")).toString().isEmpty(), "signature carries a next_change");
        QVERIFY2(!r.value(QStringLiteral("citations")).toArray().isEmpty(), "signature carries provenance");
    }

    void traceSignatureUnknownIdIsHonest() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        QVERIFY(!kb.traceSignature(QStringLiteral("not_a_real_signature")).value(QStringLiteral("found")).toBool());
    }

    // The full set of ids the bridge is capable of emitting must each resolve in the shipped KB.
    void everyEmittableTraceIdResolvesInKb() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QStringList ids{
            QStringLiteral("flow_exceeds_pressure"), QStringLiteral("pressure_notch_heal"),
            QStringLiteral("choke"), QStringLiteral("gusher"),
            QStringLiteral("flow_stall_after_pi"), QStringLiteral("trace_flow_never_caps")};
        for (const QString &id : ids)
            QVERIFY2(kb.traceSignature(id).value(QStringLiteral("found")).toBool(), qPrintable(id));
    }

    void mapsMeasuredGrindAndChannelingFaults() {
        using BaristaTrace::objectiveTraceSignatures;

        const auto choke = objectiveTraceSignatures(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("chokedPuck"), {
                {QStringLiteral("chokedPuck"), true},
                {QStringLiteral("gates"), QVariantMap{{QStringLiteral("meanPressurizedFlowMlPerSec"), 0.31},
                                                      {QStringLiteral("pressurizedDurationSec"), 18.0}}}})}}));
        QCOMPARE(choke.size(), 1);
        QCOMPARE(choke.first().signatureId, QStringLiteral("choke"));
        QCOMPARE(choke.first().grounding, QStringLiteral("measured"));
        QVERIFY2(choke.first().evidence.contains(QStringLiteral("0.31")), "choke evidence carries the measured flow");

        const auto gusher = objectiveTraceSignatures(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("yieldOvershoot"),
                {{QStringLiteral("yieldOvershoot"), true}, {QStringLiteral("yieldRatio"), 1.35}})}}));
        QCOMPARE(gusher.size(), 1);
        QCOMPARE(gusher.first().signatureId, QStringLiteral("gusher"));
        QCOMPARE(gusher.first().grounding, QStringLiteral("measured"));

        const auto sustained = objectiveTraceSignatures(shotWith({
            {QStringLiteral("channeling"), channelingMap(QStringLiteral("sustained"), 9.5)}}));
        QCOMPARE(sustained.size(), 1);
        QCOMPARE(sustained.first().signatureId, QStringLiteral("flow_exceeds_pressure"));
        QCOMPARE(sustained.first().grounding, QStringLiteral("measured"));

        const auto transient = objectiveTraceSignatures(shotWith({
            {QStringLiteral("channeling"), channelingMap(QStringLiteral("transient"), 6.0)}}));
        QCOMPARE(transient.size(), 1);
        QCOMPARE(transient.first().signatureId, QStringLiteral("pressure_notch_heal"));
    }

    // tooFine splits: with flow trending DOWN it is a measured post-PI stall; alone it is only
    // an inferred "never caps" (average shortfall is not a per-sample claim).
    void tooFineSplitsByFlowTrend() {
        using BaristaTrace::objectiveTraceSignatures;

        const auto stall = objectiveTraceSignatures(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("tooFine"), {{QStringLiteral("deltaMlPerSec"), -0.5}})},
            {QStringLiteral("flowTrend"), flowTrendMap(QStringLiteral("falling"), -0.3)}}));
        QCOMPARE(stall.size(), 1);
        QCOMPARE(stall.first().signatureId, QStringLiteral("flow_stall_after_pi"));
        QCOMPARE(stall.first().grounding, QStringLiteral("measured"));

        const auto neverCaps = objectiveTraceSignatures(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("tooFine"), {{QStringLiteral("deltaMlPerSec"), -0.6}})},
            {QStringLiteral("flowTrend"), flowTrendMap(QStringLiteral("stable"), 0.0)}}));
        QCOMPARE(neverCaps.size(), 1);
        QCOMPARE(neverCaps.first().signatureId, QStringLiteral("trace_flow_never_caps"));
        QVERIFY2(neverCaps.first().grounding == QStringLiteral("inferred"),
                 "trace_flow_never_caps is consistent-with only, never asserted");
    }

    void cleanShotEmitsNoTraceSignature() {
        using BaristaTrace::objectiveTraceSignatures;
        QVERIFY(objectiveTraceSignatures(shotWith({})).isEmpty());
        const auto onTarget = objectiveTraceSignatures(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("onTarget"))},
            {QStringLiteral("channeling"), channelingMap(QStringLiteral("none"))},
            {QStringLiteral("flowTrend"), flowTrendMap(QStringLiteral("stable"))}}));
        QVERIFY2(onTarget.isEmpty(), "an on-target, unchanneled shot asserts no curve fault");
    }

    // Contract, exercised end-to-end: whatever the bridge emits across a battery of shots, every
    // hit's id resolves in the KB and its grounding is one of the two sanctioned labels.
    void everyEmittedHitResolvesAndIsLabelled() {
        using BaristaTrace::objectiveTraceSignatures;
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QList<ShotProjection> battery{
            shotWith({{QStringLiteral("grind"), grindMap(QStringLiteral("chokedPuck"), {{QStringLiteral("chokedPuck"), true}})}}),
            shotWith({{QStringLiteral("grind"), grindMap(QStringLiteral("yieldOvershoot"), {{QStringLiteral("yieldOvershoot"), true}})}}),
            shotWith({{QStringLiteral("grind"), grindMap(QStringLiteral("tooFine"), {{QStringLiteral("deltaMlPerSec"), -0.5}})},
                      {QStringLiteral("flowTrend"), flowTrendMap(QStringLiteral("falling"), -0.3)}}),
            shotWith({{QStringLiteral("grind"), grindMap(QStringLiteral("tooFine"), {{QStringLiteral("deltaMlPerSec"), -0.6}})}}),
            shotWith({{QStringLiteral("channeling"), channelingMap(QStringLiteral("sustained"), 9.0)}}),
            shotWith({{QStringLiteral("channeling"), channelingMap(QStringLiteral("transient"), 5.0)}})};
        int totalHits = 0;
        for (const ShotProjection &s : battery) {
            for (const BaristaTrace::SignatureHit &h : objectiveTraceSignatures(s)) {
                ++totalHits;
                QVERIFY2(kb.traceSignature(h.signatureId).value(QStringLiteral("found")).toBool(),
                         qPrintable(QStringLiteral("emitted unknown signature id: ") + h.signatureId));
                QVERIFY2(h.grounding == QStringLiteral("measured") || h.grounding == QStringLiteral("inferred"),
                         qPrintable(QStringLiteral("bad grounding label: ") + h.grounding));
                QVERIFY2(!h.evidence.isEmpty(), "every hit carries its measured evidence");
            }
        }
        QCOMPARE(totalHits, battery.size());  // each fixture emits exactly one signature
    }

    // buildLastShotTraceRead joins the bridge hits to their KB meaning/next_change/citation and
    // splits measured (assert) vs inferred (a maybe) for the opening read.
    void lastShotTraceReadLeadsWithMeasuredFault() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = BaristaTrace::buildLastShotTraceRead(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("chokedPuck"), {
                {QStringLiteral("chokedPuck"), true},
                {QStringLiteral("gates"), QVariantMap{{QStringLiteral("meanPressurizedFlowMlPerSec"), 0.31},
                                                      {QStringLiteral("pressurizedDurationSec"), 18.0}}}})}}), kb);
        const QJsonArray measured = r.value(QStringLiteral("measured")).toArray();
        QCOMPARE(measured.size(), 1);
        const QJsonObject e = measured.first().toObject();
        QCOMPARE(e.value(QStringLiteral("signatureId")).toString(), QStringLiteral("choke"));
        QCOMPARE(e.value(QStringLiteral("class")).toString(), QStringLiteral("grind"));
        QVERIFY2(!e.value(QStringLiteral("evidence")).toString().isEmpty(), "measured entry carries its evidence");
        QVERIFY2(!e.value(QStringLiteral("meaning")).toString().isEmpty(), "measured entry carries the KB meaning");
        QVERIFY2(!e.value(QStringLiteral("nextChange")).toString().isEmpty(), "measured entry carries the cited next move");
        QVERIFY2(!r.contains(QStringLiteral("inferred")), "a purely-measured shot has no inferred array");
        QVERIFY2(!r.value(QStringLiteral("note")).toString().isEmpty(), "the read carries the grounding note");
        // Citation cap: choke has two provenance entries; the read keeps at most one.
        QVERIFY2(e.value(QStringLiteral("citations")).toArray().size() <= 1, "citations capped at 1");
    }

    void lastShotTraceReadHedgesInferredSignal() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        const QJsonObject r = BaristaTrace::buildLastShotTraceRead(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("tooFine"), {{QStringLiteral("deltaMlPerSec"), -0.6}})}}), kb);
        const QJsonArray inferred = r.value(QStringLiteral("inferred")).toArray();
        QCOMPARE(inferred.size(), 1);
        const QJsonObject e = inferred.first().toObject();
        QCOMPARE(e.value(QStringLiteral("signatureId")).toString(), QStringLiteral("trace_flow_never_caps"));
        QVERIFY2(!e.value(QStringLiteral("basis")).toString().isEmpty(), "inferred entry carries its basis");
        QVERIFY2(!e.contains(QStringLiteral("meaning")), "inferred entry is NOT presented as a measured meaning");
        QVERIFY2(!r.contains(QStringLiteral("measured")), "a purely-inferred shot has no measured array");
    }

    void lastShotTraceReadEmptyOnCleanShot() {
        const CoffeeKnowledgeBase kb = CoffeeKnowledgeBase::fromJson(shippedKb());
        QVERIFY(BaristaTrace::buildLastShotTraceRead(shotWith({}), kb).isEmpty());
        QVERIFY(BaristaTrace::buildLastShotTraceRead(shotWith({
            {QStringLiteral("grind"), grindMap(QStringLiteral("onTarget"))}}), kb).isEmpty());
    }
};

QTEST_MAIN(TstCoffeeKnowledgeBase)
#include "tst_coffeeknowledgebase.moc"
