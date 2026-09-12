// tst_coachplanstorage — [barista-fork] unit coverage for the plan-outcome ledger
// (CoachPlanStorage static helpers) and the pure lever/direction derivation. DoR §1 / P1.
//
// This layer ships SILENTLY in P1 — nothing reads or writes it until the P2 capture seam —
// so these static-helper tests are the only pre-ship guard on two correctness properties:
//   1. deriveLeverDirection maps a structuredNext prediction vs the anchor dial to the exact
//      (lever, direction) the confidence gate later keys on — incl. prose→unclear, multi, repeat.
//   2. the ledger's write/supersede/judge state machine: a new plan supersedes older open plans
//      in its scope; a judged row is immutable (a second judge no-ops); ensureSchemaStatic never
//      creates FeedbackStorage's shared schema_version marker (W2).
//
// All against a real SQLite file via a scoped raw connection (same pattern as tst_maintenancedocsync).

#include <QtTest>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QJsonObject>
#include <QJsonArray>

#include "barista/coachplanstorage.h"
#include "history/shotprojection.h"

namespace {

template<typename Work>
void withRawDb(const QString& path, const QString& connName, Work&& work)
{
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(path);
        QVERIFY2(db.open(), qPrintable(db.lastError().text()));
        work(db);
    }
    QSqlDatabase::removeDatabase(connName);
}

// An anchor shot with a given dial. Only the fields deriveLeverDirection reads are set.
ShotProjection anchorShot(const QString& grind, double dose, const QString& profile, qint64 rpm = 0)
{
    ShotProjection s;
    s.grinderSetting = grind;
    s.doseWeightG = dose;
    s.profileName = profile;
    s.rpm = rpm;
    return s;
}

QJsonObject snGrind(const QString& grinderSetting)
{
    QJsonObject o;
    o.insert(QStringLiteral("grinderSetting"), grinderSetting);
    return o;
}

// A plan field-map for insertPlanStatic in the given scope.
QVariantMap planFields(const QString& brand, const QString& type, const QString& profile,
                       qint64 equipment, const QString& lever, const QString& direction,
                       qint64 anchorShotId, qint64 createdAt)
{
    QVariantMap m;
    m.insert(QStringLiteral("beanBrand"), brand);
    m.insert(QStringLiteral("beanType"), type);
    m.insert(QStringLiteral("profileKbId"), profile);
    m.insert(QStringLiteral("equipmentId"), equipment);
    m.insert(QStringLiteral("lever"), lever);
    m.insert(QStringLiteral("direction"), direction);
    m.insert(QStringLiteral("anchorShotId"), anchorShotId);
    m.insert(QStringLiteral("source"), QStringLiteral("fenced"));
    m.insert(QStringLiteral("structuredNext"), QStringLiteral("{\"grinderSetting\":\"4.75\"}"));
    m.insert(QStringLiteral("createdAt"), createdAt);
    return m;
}

// A follow-up/anchor shot carrying the fields classifyTier reads.
ShotProjection tierShot(const QString& brand, const QString& type, qint64 timestamp,
                        int enjoyment, bool channeling = false)
{
    ShotProjection s;
    s.beanBrand = brand;
    s.beanType = type;
    s.timestamp = timestamp;
    s.enjoyment0to100 = enjoyment;
    s.channelingDetected = channeling;
    return s;
}

// A judged plan row as classifyTier reads it.
QVariantMap judgedPlanMap(const QString& adherence, const QString& lever, const QString& direction,
                          const QString& brand, const QString& type)
{
    QVariantMap m;
    m.insert(QStringLiteral("adherence"), adherence);
    m.insert(QStringLiteral("lever"), lever);
    m.insert(QStringLiteral("direction"), direction);
    m.insert(QStringLiteral("beanBrand"), brand);
    m.insert(QStringLiteral("beanType"), type);
    return m;
}

constexpr qint64 kDay = 24 * 60 * 60;

} // namespace

class TstCoachPlanStorage : public QObject
{
    Q_OBJECT

private slots:
    void init() { QTest::failOnWarning(); }
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // --- deriveLeverDirection ------------------------------------------------------------------

    void grindFinerWhenDialDrops()
    {
        const auto ld = CoachPlanStorage::deriveLeverDirection(snGrind("4.75"), anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("grind"));
        QCOMPARE(ld.direction, QStringLiteral("finer"));
    }

    void grindCoarserWhenDialRises()
    {
        const auto ld = CoachPlanStorage::deriveLeverDirection(snGrind("5.25"), anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("grind"));
        QCOMPARE(ld.direction, QStringLiteral("coarser"));
    }

    void grindWithRpmSuffixStillScores()
    {
        // Variable-RPM cohort annotates the setting; leadingDialNumber strips it.
        const auto ld = CoachPlanStorage::deriveLeverDirection(snGrind("4.5 1400rpm"),
                                                               anchorShot("5.0 1400rpm", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("grind"));
        QCOMPARE(ld.direction, QStringLiteral("finer"));
    }

    void proseGrinderIsGrindUnclear()
    {
        const auto ld = CoachPlanStorage::deriveLeverDirection(snGrind("a touch coarser than 9"),
                                                               anchorShot("9", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("grind"));
        QCOMPARE(ld.direction, QStringLiteral("unclear"));
    }

    void compoundGrindMoveIsUnclear()
    {
        const auto ld = CoachPlanStorage::deriveLeverDirection(snGrind("1+6"), anchorShot("1+4", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("grind"));
        QCOMPARE(ld.direction, QStringLiteral("unclear"));
    }

    void doseUpAndDown()
    {
        QJsonObject up;   up.insert(QStringLiteral("doseG"), 18.5);
        QJsonObject down; down.insert(QStringLiteral("doseG"), 17.0);
        const auto u = CoachPlanStorage::deriveLeverDirection(up,   anchorShot("5.0", 18.0, "Prof"));
        const auto d = CoachPlanStorage::deriveLeverDirection(down, anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(u.lever, QStringLiteral("dose"));      QCOMPARE(u.direction, QStringLiteral("up"));
        QCOMPARE(d.lever, QStringLiteral("dose"));      QCOMPARE(d.direction, QStringLiteral("down"));
    }

    void doseWithinToleranceIsNotAMove()
    {
        QJsonObject o; o.insert(QStringLiteral("doseG"), 18.1);   // < 0.3g tolerance
        const auto ld = CoachPlanStorage::deriveLeverDirection(o, anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("repeat"));
    }

    void profileSwitch()
    {
        QJsonObject o; o.insert(QStringLiteral("profileTitle"), QStringLiteral("Blooming Espresso"));
        const auto ld = CoachPlanStorage::deriveLeverDirection(o, anchorShot("5.0", 18.0, "Extractamundo"));
        QCOMPARE(ld.lever, QStringLiteral("profile"));
        QCOMPARE(ld.direction, QStringLiteral("switch"));
    }

    void rpmOnlyMoveIsGrindUnclear()
    {
        QJsonObject o; o.insert(QStringLiteral("rpm"), 1200);
        const auto ld = CoachPlanStorage::deriveLeverDirection(o, anchorShot("5.0", 18.0, "Prof", 1400));
        QCOMPARE(ld.lever, QStringLiteral("grind"));
        QCOMPARE(ld.direction, QStringLiteral("unclear"));
    }

    void twoLeversIsMulti()
    {
        QJsonObject o;
        o.insert(QStringLiteral("grinderSetting"), QStringLiteral("4.75"));
        o.insert(QStringLiteral("doseG"), 18.5);
        const auto ld = CoachPlanStorage::deriveLeverDirection(o, anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("multi"));
        QVERIFY(ld.direction.isEmpty());
    }

    void proseGrinderPlusDoseIsMulti()
    {
        // Prose grind counts as the grind lever; a real dose move alongside it -> multi.
        QJsonObject o;
        o.insert(QStringLiteral("grinderSetting"), QStringLiteral("a bit finer"));
        o.insert(QStringLiteral("doseG"), 18.7);
        const auto ld = CoachPlanStorage::deriveLeverDirection(o, anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("multi"));
    }

    void rangesOnlyIsRepeat()
    {
        QJsonObject o;
        QJsonArray dur; dur.append(32); dur.append(38);
        o.insert(QStringLiteral("expectedDurationSec"), dur);
        const auto ld = CoachPlanStorage::deriveLeverDirection(o, anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("repeat"));
        QVERIFY(ld.direction.isEmpty());
    }

    void restatedSameGrindIsRepeat()
    {
        const auto ld = CoachPlanStorage::deriveLeverDirection(snGrind("5.0"), anchorShot("5.0", 18.0, "Prof"));
        QCOMPARE(ld.lever, QStringLiteral("repeat"));
    }

    // --- classifyTier (pure) -------------------------------------------------------------------

    void tierA_improvedWorseFlat()
    {
        const auto anchor = tierShot("Onyx", "Geo", 1000, 60);
        // improved: 60 -> 78
        auto r = CoachPlanStorage::classifyTier(judgedPlanMap("followed", "grind", "finer", "Onyx", "Geo"),
                                                anchor, tierShot("Onyx", "Geo", 1000 + kDay, 78));
        QCOMPARE(r.tier, QStringLiteral("A"));   QCOMPARE(r.deltaSign, QStringLiteral("improved"));
        // worse: 60 -> 50
        r = CoachPlanStorage::classifyTier(judgedPlanMap("followed", "dose", "up", "Onyx", "Geo"),
                                           anchor, tierShot("Onyx", "Geo", 1000 + kDay, 50));
        QCOMPARE(r.tier, QStringLiteral("A"));   QCOMPARE(r.deltaSign, QStringLiteral("worse"));
        // flat: 60 -> 63 (|Δ|=3 ≤ 5)
        r = CoachPlanStorage::classifyTier(judgedPlanMap("followed", "profile", "switch", "Onyx", "Geo"),
                                           anchor, tierShot("Onyx", "Geo", 1000 + kDay, 63));
        QCOMPARE(r.tier, QStringLiteral("A"));   QCOMPARE(r.deltaSign, QStringLiteral("flat"));
    }

    void tierB_multiLeverOrMissingRating()
    {
        const auto anchor = tierShot("Onyx", "Geo", 1000, 60);
        const auto follow = tierShot("Onyx", "Geo", 1000 + kDay, 80);
        // multi-lever → B even when both rated
        QCOMPARE(CoachPlanStorage::classifyTier(judgedPlanMap("followed", "multi", "", "Onyx", "Geo"),
                                                anchor, follow).tier, QStringLiteral("B"));
        // single lever but anchor unrated → B
        QCOMPARE(CoachPlanStorage::classifyTier(judgedPlanMap("followed", "grind", "finer", "Onyx", "Geo"),
                                                tierShot("Onyx", "Geo", 1000, 0), follow).tier, QStringLiteral("B"));
        // 'repeat' is not a directional lever → B
        QCOMPARE(CoachPlanStorage::classifyTier(judgedPlanMap("followed", "repeat", "", "Onyx", "Geo"),
                                                anchor, follow).tier, QStringLiteral("B"));
    }

    void tierC_notFollowed_channeled_beanDrift_longGap()
    {
        const auto anchor = tierShot("Onyx", "Geo", 1000, 60);
        // not followed
        QCOMPARE(CoachPlanStorage::classifyTier(judgedPlanMap("ignored", "grind", "finer", "Onyx", "Geo"),
                                                anchor, tierShot("Onyx", "Geo", 1000 + kDay, 80)).tier, QStringLiteral("C"));
        // channeled follow-up
        QCOMPARE(CoachPlanStorage::classifyTier(judgedPlanMap("followed", "grind", "finer", "Onyx", "Geo"),
                                                anchor, tierShot("Onyx", "Geo", 1000 + kDay, 80, /*channeling=*/true)).tier,
                 QStringLiteral("C"));
        // bean drift (follow-up on a different bean)
        QCOMPARE(CoachPlanStorage::classifyTier(judgedPlanMap("followed", "grind", "finer", "Onyx", "Geo"),
                                                anchor, tierShot("Other", "Bean", 1000 + kDay, 80)).tier, QStringLiteral("C"));
        // long gap (> 14 days)
        QCOMPARE(CoachPlanStorage::classifyTier(judgedPlanMap("followed", "grind", "finer", "Onyx", "Geo"),
                                                anchor, tierShot("Onyx", "Geo", 1000 + 15 * kDay, 80)).tier, QStringLiteral("C"));
    }

    // --- storage state machine -----------------------------------------------------------------

    void schemaIsIdempotentAndOmitsSchemaVersion()
    {
        const QString path = m_dir.path() + "/cp_schema.db";
        withRawDb(path, "cp_schema", [](QSqlDatabase& db) {
            QVERIFY(CoachPlanStorage::ensureSchemaStatic(db));
            QVERIFY(CoachPlanStorage::ensureSchemaStatic(db));   // idempotent, no error
            // coach_plans exists; schema_version does NOT (FeedbackStorage owns that shared marker).
            const QStringList tables = db.tables();
            QVERIFY(tables.contains(QStringLiteral("coach_plans")));
            QVERIFY(!tables.contains(QStringLiteral("schema_version")));
        });
    }

    void insertDefaultsToOpenAndRoundTrips()
    {
        const QString path = m_dir.path() + "/cp_insert.db";
        withRawDb(path, "cp_insert", [](QSqlDatabase& db) {
            QVERIFY(CoachPlanStorage::ensureSchemaStatic(db));
            const qint64 id = CoachPlanStorage::insertPlanStatic(
                db, planFields("Onyx", "Geo", "prof-1", 7, "grind", "finer", 100, 1000));
            QVERIFY(id > 0);
            const QVariantList open = CoachPlanStorage::fetchOpenPlansStatic(db, 100);
            QCOMPARE(open.size(), 1);
            const QVariantMap r = open.first().toMap();
            QCOMPARE(r.value("status").toString(), QStringLiteral("open"));
            QCOMPARE(r.value("lever").toString(), QStringLiteral("grind"));
            QCOMPARE(r.value("direction").toString(), QStringLiteral("finer"));
            QCOMPARE(r.value("beanBrand").toString(), QStringLiteral("Onyx"));
            QCOMPARE(r.value("anchorShotId").toLongLong(), qint64(100));
            QCOMPARE(r.value("followUpShotId").toLongLong(), qint64(0));   // outcome half still default
            QVERIFY(r.value("adherence").toString().isEmpty());
        });
    }

    void newPlanSupersedesOlderOpenInSameScopeOnly()
    {
        const QString path = m_dir.path() + "/cp_supersede.db";
        withRawDb(path, "cp_supersede", [](QSqlDatabase& db) {
            QVERIFY(CoachPlanStorage::ensureSchemaStatic(db));
            const qint64 a = CoachPlanStorage::insertPlanStatic(
                db, planFields("Onyx", "Geo", "prof-1", 7, "grind", "finer", 10, 1000));
            // A different scope (different profile) must be untouched.
            const qint64 other = CoachPlanStorage::insertPlanStatic(
                db, planFields("Onyx", "Geo", "prof-2", 7, "dose", "up", 11, 1100));
            const qint64 b = CoachPlanStorage::insertPlanStatic(
                db, planFields("Onyx", "Geo", "prof-1", 7, "grind", "coarser", 12, 1200));
            QVERIFY(a > 0 && other > 0 && b > 0);

            // Writing b supersedes a (same scope) but not b itself, not `other`.
            const int flipped = CoachPlanStorage::supersedeOpenPlansStatic(db, "Onyx", "Geo", "prof-1", 7, b);
            QCOMPARE(flipped, 1);

            const QVariantList open = CoachPlanStorage::fetchOpenPlansStatic(db, 100);
            QStringList openAnchors;
            for (const QVariant& v : open) openAnchors << QString::number(v.toMap().value("anchorShotId").toLongLong());
            QCOMPARE(open.size(), 2);
            QVERIFY(openAnchors.contains("12"));   // b, the new plan
            QVERIFY(openAnchors.contains("11"));   // other scope, untouched
            QVERIFY(!openAnchors.contains("10"));  // a, superseded
        });
    }

    void judgmentFreezesAndIsImmutable()
    {
        const QString path = m_dir.path() + "/cp_judge.db";
        withRawDb(path, "cp_judge", [](QSqlDatabase& db) {
            QVERIFY(CoachPlanStorage::ensureSchemaStatic(db));
            const qint64 id = CoachPlanStorage::insertPlanStatic(
                db, planFields("Onyx", "Geo", "prof-1", 7, "grind", "finer", 10, 1000));
            QVERIFY(id > 0);

            QVERIFY(CoachPlanStorage::writeJudgmentStatic(
                db, id, "followed", "{\"duration\":true,\"flow\":true}", 55, 2000));

            // No longer open; a second judge no-ops (returns false) and cannot overwrite it.
            QVERIFY(CoachPlanStorage::fetchOpenPlansStatic(db, 100).isEmpty());
            QVERIFY(!CoachPlanStorage::writeJudgmentStatic(db, id, "ignored", "{}", 999, 3000));

            const QVariantList judged =
                CoachPlanStorage::fetchPlansStatic(db, QString(), QString(), QString(), "judged", 50);
            QCOMPARE(judged.size(), 1);
            const QVariantMap r = judged.first().toMap();
            QCOMPARE(r.value("adherence").toString(), QStringLiteral("followed"));   // NOT overwritten to "ignored"
            QCOMPARE(r.value("followUpShotId").toLongLong(), qint64(55));            // NOT 999
            QCOMPARE(r.value("judgedAt").toLongLong(), qint64(2000));
        });
    }

    void fetchPlansFiltersByBeanLeverStatus()
    {
        const QString path = m_dir.path() + "/cp_fetch.db";
        withRawDb(path, "cp_fetch", [](QSqlDatabase& db) {
            QVERIFY(CoachPlanStorage::ensureSchemaStatic(db));
            CoachPlanStorage::insertPlanStatic(db, planFields("Onyx", "Geo", "p", 7, "grind", "finer", 1, 1000));
            CoachPlanStorage::insertPlanStatic(db, planFields("Onyx", "Geo", "p", 7, "dose", "up", 2, 1100));
            CoachPlanStorage::insertPlanStatic(db, planFields("Other", "Bean", "p", 7, "grind", "finer", 3, 1200));

            // bean filter (case-insensitive) — only Onyx/Geo rows.
            QCOMPARE(CoachPlanStorage::fetchPlansStatic(db, "onyx", "geo", QString(), QString(), 50).size(), 2);
            // lever filter within that bean.
            const QVariantList grindOnly =
                CoachPlanStorage::fetchPlansStatic(db, "Onyx", "Geo", "grind", QString(), 50);
            QCOMPARE(grindOnly.size(), 1);
            QCOMPARE(grindOnly.first().toMap().value("anchorShotId").toLongLong(), qint64(1));
            // status filter — nothing judged yet.
            QCOMPARE(CoachPlanStorage::fetchPlansStatic(db, QString(), QString(), QString(), "judged", 50).size(), 0);
            // all-beans, newest first.
            const QVariantList all = CoachPlanStorage::fetchPlansStatic(db, QString(), QString(), QString(), QString(), 50);
            QCOMPARE(all.size(), 3);
            QCOMPARE(all.first().toMap().value("anchorShotId").toLongLong(), qint64(3));   // newest created_at
        });
    }

private:
    QTemporaryDir m_dir;
};

QTEST_MAIN(TstCoachPlanStorage)
#include "tst_coachplanstorage.moc"
