// tst_maintenancedocsync — [barista-fork] unit coverage for the "periodic Decent
// maintenance-docs check" DB layer (TasksStorage static helpers).
//
// Runtime for this feature is UNVERIFIED by the build (raw-resourced QML + a live network
// fetch can't be exercised in CI), so these static-helper tests are the ONLY pre-ship guard
// on the two correctness properties that matter:
//   1. update_maintenance_default touches ONLY still-default rows (is_default=1) and NEVER an
//      owner-overridden row — and it KEEPS is_default=1 (a Decent default value is still a default,
//      unlike updateMaintenanceTaskStatic which flips is_default→0 to record an owner override).
//   2. the doc-state machine: silent first-fetch baseline → a later changed hash flips reviewed=0
//      → mark-reviewed (accept/dismiss) advances the baseline so the same change never re-offers.
//
// All against a real SQLite file via a scoped raw connection (same pattern as tst_dialing_blocks).

#include <QtTest>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariantMap>

#include "barista/tasksstorage.h"

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

// Read one maintenance_tasks row's (interval_days, is_default).
QPair<int,int> readTask(QSqlDatabase& db, const QString& key)
{
    QSqlQuery q(db);
    q.prepare("SELECT interval_days, is_default FROM maintenance_tasks WHERE task_key = :k");
    q.bindValue(":k", key);
    if (q.exec() && q.next())
        return { q.value(0).toInt(), q.value(1).toInt() };
    return { -1, -1 };
}

} // namespace

class TstMaintenanceDocSync : public QObject
{
    Q_OBJECT

private slots:
    void init() { QTest::failOnWarning(); }
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // --- update_maintenance_default: is_default guard ------------------------------------------

    void updatesStillDefaultRowAndKeepsItDefault()
    {
        const QString path = m_dir.path() + "/apply_default.db";
        withRawDb(path, "apply_default", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            // backflush seeds at 7 days, is_default=1.
            QCOMPARE(readTask(db, "backflush"), qMakePair(7, 1));

            const bool applied = TasksStorage::updateMaintenanceDefaultStatic(db, "backflush", 5, QString());
            QVERIFY(applied);
            // interval updated, and CRUCIALLY still is_default=1 (a Decent default value is still a default).
            QCOMPARE(readTask(db, "backflush"), qMakePair(5, 1));
        });
    }

    void neverTouchesOwnerOverriddenRow()
    {
        const QString path = m_dir.path() + "/guard_override.db";
        withRawDb(path, "guard_override", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            // Owner edits backflush → is_default flips to 0 (their authoritative override).
            QVariantMap edit;
            edit.insert("taskKey", QStringLiteral("backflush"));
            edit.insert("intervalDays", 3);
            QVERIFY(TasksStorage::updateMaintenanceTaskStatic(db, edit));
            QCOMPARE(readTask(db, "backflush"), qMakePair(3, 0));

            // A Decent-doc update must NOT clobber it: returns false, row unchanged.
            const bool applied = TasksStorage::updateMaintenanceDefaultStatic(db, "backflush", 9, QString());
            QVERIFY(!applied);
            QCOMPARE(readTask(db, "backflush"), qMakePair(3, 0));   // still the owner's 3, still override
        });
    }

    void skipsUnknownTaskKey()
    {
        const QString path = m_dir.path() + "/guard_unknown.db";
        withRawDb(path, "guard_unknown", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(!TasksStorage::updateMaintenanceDefaultStatic(db, "no_such_task", 5, QString()));
        });
    }

    void appliesLabelOnlyWhenProvided()
    {
        const QString path = m_dir.path() + "/apply_label.db";
        withRawDb(path, "apply_label", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            const auto label = [&](const QString& key) {
                QSqlQuery q(db);
                q.prepare("SELECT label FROM maintenance_tasks WHERE task_key = :k");
                q.bindValue(":k", key);
                return (q.exec() && q.next()) ? q.value(0).toString() : QString();
            };
            const QString before = label("backflush");
            // Empty label leaves it unchanged.
            QVERIFY(TasksStorage::updateMaintenanceDefaultStatic(db, "backflush", 5, QString()));
            QCOMPARE(label("backflush"), before);
            // Non-empty label is applied.
            QVERIFY(TasksStorage::updateMaintenanceDefaultStatic(db, "backflush", 5, QStringLiteral("Backflush weekly")));
            QCOMPARE(label("backflush"), QStringLiteral("Backflush weekly"));
        });
    }

    // --- maintenance_doc_state machine ---------------------------------------------------------

    void defaultsAreEnabledAndReviewed()
    {
        const QString path = m_dir.path() + "/docstate_seed.db";
        withRawDb(path, "docstate_seed", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            const QVariantMap s = TasksStorage::fetchDocStateStatic(db);
            QCOMPARE(s.value("enabled").toBool(), true);      // default ON
            QCOMPARE(s.value("reviewed").toBool(), true);     // nothing pending
            QCOMPARE(s.value("lastCheckedAt").toLongLong(), qint64(0));
            QVERIFY(s.value("baselineHash").toString().isEmpty());
        });
    }

    void firstFetchIsSilentBaseline()
    {
        const QString path = m_dir.path() + "/docstate_first.db";
        withRawDb(path, "docstate_first", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "clean daily", "hashA", 1000));
            const QVariantMap s = TasksStorage::fetchDocStateStatic(db);
            // First fetch seeds baseline=current → reviewed stays true (no offer on the very first read).
            QCOMPARE(s.value("reviewed").toBool(), true);
            QCOMPARE(s.value("baselineHash").toString(), QStringLiteral("hashA"));
            QCOMPARE(s.value("currentHash").toString(), QStringLiteral("hashA"));
            QCOMPARE(s.value("lastCheckedAt").toLongLong(), qint64(1000));
            QCOMPARE(s.value("docText").toString(), QStringLiteral("clean daily"));
        });
    }

    void changedHashFlipsReviewedFalse()
    {
        const QString path = m_dir.path() + "/docstate_change.db";
        withRawDb(path, "docstate_change", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "clean daily", "hashA", 1000));   // baseline
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "clean twice daily", "hashB", 2000)); // change
            const QVariantMap s = TasksStorage::fetchDocStateStatic(db);
            QCOMPARE(s.value("reviewed").toBool(), false);    // a NEW change is pending the owner's review
            QCOMPARE(s.value("currentHash").toString(), QStringLiteral("hashB"));
            QCOMPARE(s.value("baselineHash").toString(), QStringLiteral("hashA")); // baseline not advanced yet
        });
    }

    void unchangedRefetchStaysReviewed()
    {
        const QString path = m_dir.path() + "/docstate_same.db";
        withRawDb(path, "docstate_same", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "clean daily", "hashA", 1000));
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "clean daily", "hashA", 3000)); // same hash
            const QVariantMap s = TasksStorage::fetchDocStateStatic(db);
            QCOMPARE(s.value("reviewed").toBool(), true);
            QCOMPARE(s.value("lastCheckedAt").toLongLong(), qint64(3000)); // still records the check time
        });
    }

    void markReviewedAdvancesBaselineSoChangeNotReoffered()
    {
        const QString path = m_dir.path() + "/docstate_review.db";
        withRawDb(path, "docstate_review", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "v1", "hashA", 1000));
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "v2", "hashB", 2000));
            QCOMPARE(TasksStorage::fetchDocStateStatic(db).value("reviewed").toBool(), false);

            // Accept/dismiss → baseline advances to current, reviewed=1.
            QVERIFY(TasksStorage::markDocReviewedStatic(db));
            const QVariantMap s = TasksStorage::fetchDocStateStatic(db);
            QCOMPARE(s.value("reviewed").toBool(), true);
            QCOMPARE(s.value("baselineHash").toString(), QStringLiteral("hashB"));

            // Re-fetching the SAME (now-baselined) hash does not re-offer.
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "v2", "hashB", 4000));
            QCOMPARE(TasksStorage::fetchDocStateStatic(db).value("reviewed").toBool(), true);

            // But a FURTHER change re-triggers.
            QVERIFY(TasksStorage::recordFetchedDocStatic(db, "v3", "hashC", 5000));
            QCOMPARE(TasksStorage::fetchDocStateStatic(db).value("reviewed").toBool(), false);
        });
    }

    void toggleEnabledPersists()
    {
        const QString path = m_dir.path() + "/docstate_toggle.db";
        withRawDb(path, "docstate_toggle", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(TasksStorage::setDocSyncEnabledStatic(db, false));
            QCOMPARE(TasksStorage::fetchDocStateStatic(db).value("enabled").toBool(), false);
            QVERIFY(TasksStorage::setDocSyncEnabledStatic(db, true));
            QCOMPARE(TasksStorage::fetchDocStateStatic(db).value("enabled").toBool(), true);
        });
    }

    // --- user_facts: the barista's durable basic-fact memory --------------------------------------

    static QVariantMap fact(const QString& user, const QString& f, const QString& cat = QString())
    {
        QVariantMap m;
        m.insert(QStringLiteral("user"), user);
        m.insert(QStringLiteral("fact"), f);
        if (!cat.isEmpty()) m.insert(QStringLiteral("category"), cat);
        return m;
    }

    void addAndFetchUserFact()
    {
        const QString path = m_dir.path() + "/facts_add.db";
        withRawDb(path, "facts_add", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            const qint64 id = TasksStorage::insertUserFactStatic(db, fact("Chris", "Oldest daughter Audrey attends UW", "family"));
            QVERIFY(id > 0);
            const QVariantList rows = TasksStorage::fetchUserFactsStatic(db, "Chris", 30);
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows.first().toMap().value("fact").toString(), QStringLiteral("Oldest daughter Audrey attends UW"));
            QCOMPARE(rows.first().toMap().value("category").toString(), QStringLiteral("family"));
        });
    }

    void dedupeCaseInsensitiveKeepsOneRow()
    {
        const QString path = m_dir.path() + "/facts_dedupe.db";
        withRawDb(path, "facts_dedupe", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            const qint64 a = TasksStorage::insertUserFactStatic(db, fact("Chris", "Has a dog named Max"));
            // Same fact, different case → dedupes to the existing id, no second row.
            const qint64 b = TasksStorage::insertUserFactStatic(db, fact("Chris", "has a DOG named max"));
            QVERIFY(a > 0);
            QCOMPARE(a, b);
            QCOMPARE(TasksStorage::fetchUserFactsStatic(db, "Chris", 30).size(), 1);
        });
    }

    void perUserScopingIsolatesFacts()
    {
        const QString path = m_dir.path() + "/facts_scope.db";
        withRawDb(path, "facts_scope", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(TasksStorage::insertUserFactStatic(db, fact("Chris", "Prefers lighter roasts")) > 0);
            QVERIFY(TasksStorage::insertUserFactStatic(db, fact("Dana", "Drinks decaf only")) > 0);
            QVERIFY(TasksStorage::insertUserFactStatic(db, fact("", "House has hard water")) > 0);   // unattributed

            // Chris sees his own + the unattributed row, NOT Dana's.
            const QVariantList chris = TasksStorage::fetchUserFactsStatic(db, "Chris", 30);
            QCOMPARE(chris.size(), 2);
            QStringList cf; for (const QVariant& r : chris) cf << r.toMap().value("fact").toString();
            QVERIFY(cf.contains("Prefers lighter roasts"));
            QVERIFY(cf.contains("House has hard water"));
            QVERIFY(!cf.contains("Drinks decaf only"));

            // Dana likewise sees her own + unattributed, not Chris's.
            const QVariantList dana = TasksStorage::fetchUserFactsStatic(db, "Dana", 30);
            QCOMPARE(dana.size(), 2);
            QStringList dfacts; for (const QVariant& r : dana) dfacts << r.toMap().value("fact").toString();
            QVERIFY(dfacts.contains("Drinks decaf only"));
            QVERIFY(!dfacts.contains("Prefers lighter roasts"));
        });
    }

    void forgetRemovesMatchingFacts()
    {
        const QString path = m_dir.path() + "/facts_forget.db";
        withRawDb(path, "facts_forget", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            QVERIFY(TasksStorage::insertUserFactStatic(db, fact("Chris", "Has a dog named Max")) > 0);
            QVERIFY(TasksStorage::insertUserFactStatic(db, fact("Chris", "Works at Boeing")) > 0);
            // Substring, case-insensitive; scoped to Chris.
            const int removed = TasksStorage::deleteUserFactsStatic(db, "Chris", "dog named max");
            QCOMPARE(removed, 1);
            const QVariantList rows = TasksStorage::fetchUserFactsStatic(db, "Chris", 30);
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows.first().toMap().value("fact").toString(), QStringLiteral("Works at Boeing"));
        });
    }

    void fetchHonorsCap()
    {
        const QString path = m_dir.path() + "/facts_cap.db";
        withRawDb(path, "facts_cap", [](QSqlDatabase& db) {
            QVERIFY(TasksStorage::ensureSchemaStatic(db));
            for (int i = 0; i < 10; ++i)
                QVERIFY(TasksStorage::insertUserFactStatic(db, fact("Chris", QStringLiteral("Fact number %1").arg(i))) > 0);
            QCOMPARE(TasksStorage::fetchUserFactsStatic(db, "Chris", 3).size(), 3);
            QCOMPARE(TasksStorage::fetchUserFactsStatic(db, "Chris", 30).size(), 10);
        });
    }

private:
    QTemporaryDir m_dir;
};

QTEST_MAIN(TstMaintenanceDocSync)
#include "tst_maintenancedocsync.moc"
