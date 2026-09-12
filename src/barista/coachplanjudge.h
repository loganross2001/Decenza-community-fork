#pragma once

#include <QtGlobal>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

class QSqlDatabase;

// [barista-fork] Plan-outcome ledger — the lazy judge pass (DoR §1.4).
//
// Kept in its OWN translation unit (not coachplanstorage.cpp) on purpose: judging needs the #1053 graders
// (DialingBlocks) and the shot loader (ShotHistoryStorage), and folding those into CoachPlanStorage would drag
// dialing_blocks.cpp + shothistorystorage.cpp into every lean target that links the storage class (e.g.
// tst_coachplanstorage). This TU is linked only by the app and the one DB-integration test — the narrow-library
// discipline in CLAUDE.md.
namespace CoachPlanJudge {

// Judge every status='open' plan in `assistantDb` against the shots in `shotsDb`. For each open plan: load its
// anchor shot (for the anchor timestamp + as the `prior` shot adherence needs), find the FIRST follow-up shot on
// the same profile_kb_id + equipment scope postdating the anchor, then freeze adherence +
// in_predicted_range (via DialingBlocks::computeAdherence / computeOutcomeInPredictedRange) through
// CoachPlanStorage::writeJudgmentStatic. `nowSecs` stamps judged_at (injected so the pass stays deterministic in
// tests). Idempotent: only 'open' rows are read and writeJudgmentStatic guards on status, so re-running judges
// nothing already frozen. A plan whose anchor shot was deleted, or whose follow-up hasn't been pulled yet, is
// left open and retried on the next pass. Returns the number of rows judged this pass.
//
// The two connections are separate because the ledger lives in assistant.db and the shots in shots.db; callers
// open both (the barista context worker already holds an assistant.db withTempDb and knows the shots.db path).
int runJudgePassStatic(QSqlDatabase& assistantDb, QSqlDatabase& shotsDb, qint64 nowSecs);

// The resident `coachTrackRecord` block (DoR §3.2) for the CURRENT bean: the barista's OWN past advice on this
// bean and what actually followed, aggregated. Runs the judge pass FIRST (so freshly-judgeable plans count and
// the read is judge-before-anything), then classifies each judged plan via CoachPlanStorage::classifyTier over
// the LIVE anchor/follow-up shots (ratings/channeling join live — never frozen). Returns:
//   { "onThisBean": [ { pattern, outcomes, confirmed } ...≤3 ], "lastPlan": {...}, "note": "..." }
// A `confirmed:true` pattern requires ≥2 Tier-A outcomes of consistent sign for one (lever,direction), no
// Tier-A counter-example (the §1.5 floor, mirroring palateProfile). Single Tier-A outcomes appear as
// `confirmed:false` events, never generalized. Returns an EMPTY object (caller omits the key) when there is no
// proactive-worthy content — no Tier-A data AND no `followed` last plan (Tier-C-only history is the recall
// tool's job, not the proactive block).
QJsonObject buildTrackRecord(QSqlDatabase& assistantDb, QSqlDatabase& shotsDb,
                             const QString& beanBrand, const QString& beanType, qint64 nowSecs);

// The `recall_coaching_outcomes` tool's rows (DoR §3.2): per-EVENT judged plans, newest first, INCLUDING the
// Tier-C history the block omits ("suggested, not tried"). Shares the judge pass + fetch + classifyTier with
// buildTrackRecord (one path). Filters: empty bean = all beans; empty lever = all levers. Each row:
//   { date, advice, adherence, ranAsPredicted, outcomeRating0to100?, tier }
QJsonArray recallOutcomes(QSqlDatabase& assistantDb, QSqlDatabase& shotsDb,
                          const QString& beanBrand, const QString& beanType,
                          const QString& lever, int limit, qint64 nowSecs);

}  // namespace CoachPlanJudge
