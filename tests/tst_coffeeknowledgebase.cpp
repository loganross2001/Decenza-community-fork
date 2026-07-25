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

#include "barista/coffeeknowledgebase.h"

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
};

QTEST_MAIN(TstCoffeeKnowledgeBase)
#include "tst_coffeeknowledgebase.moc"
