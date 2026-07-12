#include <QtTest/QtTest>

#include "core/drinktypes.h"

// Tea profile type-matching + temp-proximity + default-temp helpers for the
// recipe wizard's ranked profile step (add-recipe-wizard-tea task 2.4).
class tst_DrinkTypes : public QObject {
    Q_OBJECT

private slots:

    // Grind-bearing drink types (fix-recipe-grind-integrity): the tea family
    // stores no grind — a dial edit while a tea recipe is active must not
    // stamp one onto it. Empty (legacy pre-migration-28 rows) reads as coffee.
    void hasGrindByDrinkType() {
        QVERIFY(DrinkTypes::hasGrind("espresso"));
        QVERIFY(DrinkTypes::hasGrind("filter"));
        QVERIFY(DrinkTypes::hasGrind("americano"));
        QVERIFY(DrinkTypes::hasGrind("long_black"));
        QVERIFY(DrinkTypes::hasGrind("latte"));
        QVERIFY(DrinkTypes::hasGrind(""));  // legacy row: coffee assumption
        QVERIFY(!DrinkTypes::hasGrind("tea"));
        QVERIFY(!DrinkTypes::hasGrind("tea_hotwater"));
    }

    // Every stock tea_portafilter title must be reachable from its tea type —
    // this pins the keyword table to the shipped profile set.
    void stockTitlesMatchTheirType() {
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/black tea", "black"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/Japanese green", "green"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/Chinese green", "green"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/Yunnan green", "green"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/Sencha", "green"));
        QVERIFY(DrinkTypes::teaProfileMatchesType(
            "Tea portafilter/Blue Willow Tsuyuhikari Sensha", "green"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/Oolong 1st extraction", "oolong"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/Oolong 2nd extraction", "oolong"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/Bug Bite Oolong", "oolong"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/oolong dark", "oolong"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/white tea", "white"));
        QVERIFY(DrinkTypes::teaProfileMatchesType("Tea portafilter/tisane", "herbal"));
    }

    void mismatchesAndEdgeCases() {
        // Wrong type: no match.
        QVERIFY(!DrinkTypes::teaProfileMatchesType("Tea portafilter/black tea", "green"));
        QVERIFY(!DrinkTypes::teaProfileMatchesType("Tea portafilter/Sencha", "black"));
        // Generic profiles match nothing (they rank by temp proximity instead).
        QVERIFY(!DrinkTypes::teaProfileMatchesType("Tea portafilter/no pressure", "black"));
        QVERIFY(!DrinkTypes::teaProfileMatchesType("Tea portafilter/pressurized tea", "oolong"));
        // Empty inputs never match.
        QVERIFY(!DrinkTypes::teaProfileMatchesType("", "black"));
        QVERIFY(!DrinkTypes::teaProfileMatchesType("Tea portafilter/black tea", ""));
        // Case/whitespace tolerant on the type (extraction normalizes, but be safe).
        QVERIFY(DrinkTypes::teaProfileMatchesType("TEA PORTAFILTER/BLACK TEA", " Black "));
    }

    void tempProximityOrdering() {
        // Sencha bag at 70°: the 74° profile beats the 94° one.
        QVERIFY(DrinkTypes::teaTempProximity(74, 70) < DrinkTypes::teaTempProximity(94, 70));
        QCOMPARE(DrinkTypes::teaTempProximity(74, 70), 4.0);
        // No stated temp: every profile keys 0 (caller's alphabetical order holds).
        QCOMPARE(DrinkTypes::teaTempProximity(74, 0), 0.0);
        QCOMPARE(DrinkTypes::teaTempProximity(0, 70), 0.0);
    }

    void defaultTempsByType() {
        QCOMPARE(DrinkTypes::defaultTeaTempC("green"), 80.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC("white"), 80.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC("oolong"), 90.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC("pu-erh"), 95.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC("black"), 98.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC("herbal"), 100.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC(""), 90.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC("unknown"), 90.0);
        QCOMPARE(DrinkTypes::defaultTeaTempC(" Black "), 98.0);
    }

    // Espresso type from ratio — the buckets the barista speaks in, with
    // boundaries in the gaps between the ranges.
    void espressoTypeFromRatio() {
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(1.2),  QString("ristretto"));
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(1.5),  QString("ristretto"));
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(2.0),  QString("normale"));
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(2.4),  QString("normale"));
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(3.0),  QString("lungo"));
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(2.75), QString("lungo"));   // boundary → lungo
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(2.74), QString("normale")); // just under
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(0.0),  QString());          // unknown
        QCOMPARE(DrinkTypes::espressoTypeFromRatio(-1.0), QString());
    }

    // Speakable descriptor: "<type> espresso on the <bean> beans", degrading
    // gracefully as bean info is missing.
    void espressoShotDescriptor() {
        QCOMPARE(DrinkTypes::espressoShotDescriptor(2.2, "Kenya AA", "Peaberry"),
                 QString("normale espresso on the Kenya AA beans"));
        QCOMPARE(DrinkTypes::espressoShotDescriptor(3.1, "Ethiopia", "Guji"),
                 QString("lungo espresso on the Ethiopia beans"));
        // No roaster → fall back to the varietal.
        QCOMPARE(DrinkTypes::espressoShotDescriptor(2.2, "", "Peaberry"),
                 QString("normale espresso on the Peaberry beans"));
        // No bean at all → just the type.
        QCOMPARE(DrinkTypes::espressoShotDescriptor(1.3, "", ""),
                 QString("ristretto espresso"));
        // Unknown ratio, no bean → bare "espresso".
        QCOMPARE(DrinkTypes::espressoShotDescriptor(0.0, "", ""), QString("espresso"));
        // Unknown ratio but a bean is known.
        QCOMPARE(DrinkTypes::espressoShotDescriptor(0.0, "Onyx", ""),
                 QString("espresso on the Onyx beans"));
    }
};

QTEST_GUILESS_MAIN(tst_DrinkTypes)
#include "tst_drinktypes.moc"
