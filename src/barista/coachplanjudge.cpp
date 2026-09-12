#include "coachplanjudge.h"
#include "coachplanstorage.h"
#include "../ai/dialing_blocks.h"
#include "../history/shothistorystorage.h"
#include "../history/shotscope.h"
#include "../history/shotprojection.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantList>
#include <QVariantMap>
#include <QDateTime>
#include <QMap>
#include <QPair>
#include <QVector>
#include <QDebug>
#include <algorithm>

namespace CoachPlanJudge {

namespace {

// One judged plan classified against its LIVE anchor/follow-up shots.
struct Classified {
    QVariantMap plan;
    QString tier;        // "A" | "B" | "C"
    QString deltaSign;   // "improved" | "worse" | "flat" | ""
    int anchorRating = 0;
    int followRating = 0;
};

// Load both shots for each judged plan and run the pure tier classifier. Rows whose anchor or follow-up shot was
// deleted after judging are skipped (can't classify live). Preserves input order (newest-first from fetch).
QVector<Classified> classifyRows(QSqlDatabase& shotsDb, const QVariantList& rows)
{
    QVector<Classified> out;
    for (const QVariant& v : rows) {
        const QVariantMap plan = v.toMap();
        const ShotProjection anchor = ShotHistoryStorage::convertShotRecord(
            ShotHistoryStorage::loadShotRecordStatic(shotsDb, plan.value(QStringLiteral("anchorShotId")).toLongLong()));
        const ShotProjection follow = ShotHistoryStorage::convertShotRecord(
            ShotHistoryStorage::loadShotRecordStatic(shotsDb, plan.value(QStringLiteral("followUpShotId")).toLongLong()));
        if (!anchor.isValid() || !follow.isValid())
            continue;
        const CoachPlanStorage::TierResult t = CoachPlanStorage::classifyTier(plan, anchor, follow);
        out.append(Classified{ plan, t.tier, t.deltaSign, anchor.enjoyment0to100, follow.enjoyment0to100 });
    }
    return out;
}

// "grind finer" / "dose up" / "profile switch" / "grind" (empty direction).
QString leverDirLabel(const QVariantMap& plan)
{
    const QString lever = plan.value(QStringLiteral("lever")).toString();
    const QString dir = plan.value(QStringLiteral("direction")).toString();
    return dir.isEmpty() ? lever : (lever + QLatin1Char(' ') + dir);
}

// Human phrasing for a consistent rating-delta sign across a confirmed pattern.
QString outcomeWord(const QString& deltaSign)
{
    if (deltaSign == QLatin1String("improved")) return QStringLiteral("improved");
    if (deltaSign == QLatin1String("worse"))    return QStringLiteral("got worse");
    return QStringLiteral("held about the same");   // flat
}

// yyyy-MM-dd from an epoch-seconds field on the plan row.
QString planDate(const QVariantMap& plan)
{
    return QDateTime::fromSecsSinceEpoch(plan.value(QStringLiteral("createdAt")).toLongLong())
        .toString(QStringLiteral("yyyy-MM-dd"));
}

// { duration, flow } parsed from the frozen in_predicted_range JSON.
QJsonObject ranAsPredicted(const QVariantMap& plan)
{
    return QJsonDocument::fromJson(plan.value(QStringLiteral("inPredictedRange")).toString().toUtf8()).object();
}

// One-line advice digest from the stored structuredNext (the hoisted #1053 summarizer).
QString adviceSummary(const QVariantMap& plan)
{
    const QJsonObject sn =
        QJsonDocument::fromJson(plan.value(QStringLiteral("structuredNext")).toString().toUtf8()).object();
    return DialingBlocks::synthesizeRecommendationSummary(sn);
}

}  // namespace

int runJudgePassStatic(QSqlDatabase& assistantDb, QSqlDatabase& shotsDb, qint64 nowSecs)
{
    if (!CoachPlanStorage::ensureSchemaStatic(assistantDb))
        return 0;

    const QVariantList open = CoachPlanStorage::fetchOpenPlansStatic(assistantDb, 200);
    int judged = 0;
    for (const QVariant& v : open) {
        const QVariantMap plan = v.toMap();
        const qint64 anchorId = plan.value(QStringLiteral("anchorShotId")).toLongLong();
        const QString profileKbId = plan.value(QStringLiteral("profileKbId")).toString();
        if (anchorId <= 0 || profileKbId.isEmpty())
            continue;   // unscopable — P2 already skips these; stay safe against hand-written rows.

        // Load the anchor shot: its timestamp bounds the follow-up search, and it is the `prior` shot
        // computeAdherence needs to catch "the user didn't actually move" cases.
        const ShotProjection anchor = ShotHistoryStorage::convertShotRecord(
            ShotHistoryStorage::loadShotRecordStatic(shotsDb, anchorId));
        if (!anchor.isValid() || anchor.timestamp <= 0)
            continue;   // anchor shot deleted → can't judge; leave open (retried cheaply next pass).

        // First follow-up shot on the SAME profile + equipment scope, postdating the anchor (the
        // buildRecentAdviceBlock matcher shape, tightened by equipment — grind doesn't transfer across baskets).
        const AdviceScope scope(plan.value(QStringLiteral("equipmentId")).toLongLong());
        QSqlQuery nextQ(shotsDb);
        nextQ.prepare(QStringLiteral(
            "SELECT id FROM shots WHERE profile_kb_id = ? AND timestamp > ? AND id != ? AND ")
            + scope.sql() + QStringLiteral(" ORDER BY timestamp ASC LIMIT 1"));
        nextQ.addBindValue(profileKbId);
        nextQ.addBindValue(anchor.timestamp);
        nextQ.addBindValue(anchorId);
        if (!nextQ.exec()) {
            qWarning() << "CoachPlanJudge: follow-up lookup failed:" << nextQ.lastError().text();
            continue;
        }
        if (!nextQ.next())
            continue;   // user hasn't pulled a follow-up on this scope yet → stays open.

        const qint64 followUpId = nextQ.value(0).toLongLong();
        const ShotProjection followUp = ShotHistoryStorage::convertShotRecord(
            ShotHistoryStorage::loadShotRecordStatic(shotsDb, followUpId));
        if (!followUp.isValid())
            continue;

        // Freeze the #1053 graders' verdict (adherence uses actual=followUp, prior=anchor — same arg order
        // as buildRecentAdviceBlock).
        const QJsonObject sn = QJsonDocument::fromJson(
            plan.value(QStringLiteral("structuredNext")).toString().toUtf8()).object();
        const QString adherence = DialingBlocks::computeAdherence(sn, followUp, anchor);
        const QJsonObject inRange = DialingBlocks::computeOutcomeInPredictedRange(sn, followUp);
        const QString inRangeJson = QString::fromUtf8(
            QJsonDocument(inRange).toJson(QJsonDocument::Compact));

        if (CoachPlanStorage::writeJudgmentStatic(assistantDb, plan.value(QStringLiteral("id")).toLongLong(),
                                                  adherence, inRangeJson, followUpId, nowSecs))
            ++judged;
    }
    return judged;
}

QJsonObject buildTrackRecord(QSqlDatabase& assistantDb, QSqlDatabase& shotsDb,
                             const QString& beanBrand, const QString& beanType, qint64 nowSecs)
{
    runJudgePassStatic(assistantDb, shotsDb, nowSecs);   // judge-before-read; freshly-judgeable plans now count.
    const QVariantList judged =
        CoachPlanStorage::fetchPlansStatic(assistantDb, beanBrand, beanType, QString(), QStringLiteral("judged"), 50);
    if (judged.isEmpty())
        return {};
    const QVector<Classified> cls = classifyRows(shotsDb, judged);
    if (cls.isEmpty())
        return {};

    // Group Tier-A outcomes by (lever, direction) — the pattern-assertion unit (§1.5).
    struct Group { QString label; QStringList signs; QStringList ratingPairs; };
    QMap<QString, Group> groups;   // key = lever/direction, insertion via first-seen label
    for (const Classified& c : cls) {
        if (c.tier != QLatin1String("A"))
            continue;
        const QString key = c.plan.value(QStringLiteral("lever")).toString()
                          + QLatin1Char('/') + c.plan.value(QStringLiteral("direction")).toString();
        Group& g = groups[key];
        if (g.label.isEmpty()) g.label = leverDirLabel(c.plan);
        g.signs << c.deltaSign;
        if (c.anchorRating > 0 && c.followRating > 0)
            g.ratingPairs << QStringLiteral("%1→%2").arg(c.anchorRating).arg(c.followRating);
    }

    // Confirmed patterns first (≥2 Tier-A, consistent sign, no counter-example), then single Tier-A events.
    QJsonArray confirmed, singles;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const Group& g = it.value();
        const bool allSame = !g.signs.isEmpty()
            && std::all_of(g.signs.cbegin(), g.signs.cend(), [&](const QString& s) { return s == g.signs.first(); });
        if (g.signs.size() >= 2 && allSame) {
            confirmed.append(QJsonObject{
                { "pattern", g.label },
                // `effect` is the machine-readable direction so a reader never has to infer "good/bad" from
                // `confirmed` — a confirmed pattern can be a reliably BAD move (effect="worse" → avoid it).
                { "effect", g.signs.first() },   // "improved" | "worse" | "flat"
                { "outcomes", QStringLiteral("rating %1 %2 of %2 times (%3)")
                      .arg(outcomeWord(g.signs.first())).arg(g.signs.size()).arg(g.ratingPairs.join(QStringLiteral(", "))) },
                { "confirmed", true } });
        } else if (g.signs.size() == 1) {
            singles.append(QJsonObject{
                { "pattern", g.label },
                { "effect", g.signs.first() },
                { "outcomes", QStringLiteral("tried once, rating %1").arg(outcomeWord(g.signs.first())) },
                { "confirmed", false } });
        }
        // ≥2 with mixed signs → a Tier-A counter-example exists → assert nothing (omit the line).
    }
    QJsonArray onThisBean;
    for (const QJsonValue& v : confirmed) { if (onThisBean.size() >= 3) break; onThisBean.append(v); }
    for (const QJsonValue& v : singles)   { if (onThisBean.size() >= 3) break; onThisBean.append(v); }

    // lastPlan = the single most recent judged plan (cls is newest-first).
    const Classified& last = cls.first();
    QJsonObject lastPlan{
        { "when", planDate(last.plan) },
        { "advice", adviceSummary(last.plan) },
        { "adherence", last.plan.value(QStringLiteral("adherence")).toString() },
        { "ranAsPredicted", ranAsPredicted(last.plan) } };
    if (last.followRating > 0)
        lastPlan.insert(QStringLiteral("outcomeRating0to100"), last.followRating);

    // Proactive-worthy only: a pattern/event OR a FOLLOWED last plan. Tier-C-only history (e.g. "suggested,
    // not tried") belongs to the recall tool, not this always-on block.
    const bool lastFollowed = last.plan.value(QStringLiteral("adherence")).toString() == QLatin1String("followed");
    if (onThisBean.isEmpty() && !lastFollowed)
        return {};

    QJsonObject out;
    if (!onThisBean.isEmpty())
        out.insert(QStringLiteral("onThisBean"), onThisBean);
    out.insert(QStringLiteral("lastPlan"), lastPlan);
    out.insert(QStringLiteral("note"), QStringLiteral(
        "The barista's OWN past advice on THIS bean and what actually followed — measured adherence and the "
        "user's own ratings, never assumptions. 'confirmed: true' means the SAME result REPEATED (2+ clean "
        "times) — read `effect`, NOT the word confirmed, to know if it helped: effect 'improved' = that move "
        "reliably helped (may be stated as experience, 'finer's worked on this bean — twice'); effect 'worse' = "
        "that move reliably HURT, so steer AWAY from it ('going finer has backfired here both times — let's try "
        "the other way'); effect 'flat' = it reliably didn't move the needle. 'confirmed: false' lines and "
        "single events may only be recalled as specific history, never generalized. This GROUNDS your one "
        "suggestion; it is not a report to recite."));
    return out;
}

QJsonArray recallOutcomes(QSqlDatabase& assistantDb, QSqlDatabase& shotsDb,
                          const QString& beanBrand, const QString& beanType,
                          const QString& lever, int limit, qint64 nowSecs)
{
    runJudgePassStatic(assistantDb, shotsDb, nowSecs);
    const int cappedLimit = (limit > 0 && limit <= 50) ? limit : 20;
    const QVariantList rows =
        CoachPlanStorage::fetchPlansStatic(assistantDb, beanBrand, beanType, lever, QStringLiteral("judged"), cappedLimit);
    QJsonArray out;
    for (const Classified& c : classifyRows(shotsDb, rows)) {   // newest first; ALL tiers incl. C
        QJsonObject o{
            { "date", planDate(c.plan) },
            { "advice", adviceSummary(c.plan) },
            { "adherence", c.plan.value(QStringLiteral("adherence")).toString() },
            { "ranAsPredicted", ranAsPredicted(c.plan) },
            { "tier", c.tier } };
        if (c.followRating > 0)
            o.insert(QStringLiteral("outcomeRating0to100"), c.followRating);
        out.append(o);
    }
    return out;
}

}  // namespace CoachPlanJudge
