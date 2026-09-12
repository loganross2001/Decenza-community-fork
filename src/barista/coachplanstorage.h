#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <memory>

class QSqlDatabase;
class QJsonObject;
class ShotProjection;
class SerialDbWorker;

// [barista-fork] Async storage for the barista's plan-outcome ledger (DoR §1).
//
// [fork-index] seam=CoachPlanStorage | domain=barista | change=-
//
// A THIRD schema co-resident in the SAME `assistant.db` as FeedbackStorage and
// TasksStorage (beside shots.db). Like both of those, this is NOT a versioned
// migration: `ensureSchemaStatic` is idempotent (CREATE TABLE IF NOT EXISTS) and
// NEVER touches FeedbackStorage's shared `schema_version` marker (W2 — assistant.db
// has no migration runner; each class owns its own idempotent create).
//
// Mirrors FeedbackStorage/TasksStorage exactly: async request* methods run DB work
// on a serial background worker (SerialDbWorker) and return via signals; synchronous
// *Static helpers take a caller-provided OPEN connection and are shared by the write
// path, the lazy judge pass, the context-block builder and the recall tool — ONE
// query path (the centralization rule).
//
// This layer is a PERSISTENCE+AGGREGATION store over the existing #1053 grader
// (DialingBlocks::computeAdherence / computeOutcomeInPredictedRange), NOT a new
// matcher (W1). It records each recommendation as an event and, once the shot that
// directly followed it exists, freezes that grader's verdict against it.
//
// Schema (coach_plans; the outcome half is filled once by the judge pass and then
// immutable — see DoR §1.2/§1.4):
//   coach_plans(
//     id INTEGER PRIMARY KEY,
//     created_at INTEGER,           -- epoch secs, stamped app-side
//     anchor_shot_id INTEGER,       -- the shot the advice was about
//     bean_brand TEXT, bean_type TEXT,   -- from the app-side anchor snapshot, NEVER the model
//     profile_kb_id TEXT, equipment_id INTEGER,
//     source TEXT,                  -- 'fenced' | 'tool_applied'
//     structured_next TEXT,         -- the model's prediction object, verbatim JSON
//     kb_rec_id TEXT, kb_confidence TEXT, kb_prep_gate INTEGER,  -- recommend_next_shot lineage
//     lever TEXT, direction TEXT,   -- derived at write time (see deriveLeverDirection)
//     -- outcome half, filled once and frozen:
//     follow_up_shot_id INTEGER, adherence TEXT, in_predicted_range TEXT,
//     status TEXT,                  -- 'open' | 'judged' | 'superseded'
//     judged_at INTEGER)
//
// Deliberately NOT stored: the follow-up's rating or taste words. Those arrive later
// than the shot (taste picker / log_tasting_feedback) and live authoritatively in
// shots.db; the aggregation path joins them LIVE by follow_up_shot_id so no stale
// copy is ever frozen (DoR §1.2).
class CoachPlanStorage : public QObject {
    Q_OBJECT

public:
    explicit CoachPlanStorage(QObject* parent = nullptr);
    ~CoachPlanStorage();

    // dbPath must be the assistant.db path (derived beside shots.db by the caller).
    void initialize(const QString& dbPath);
    QString databasePath() const { return m_dbPath; }

    // Async write of a plan row (the plan half only; outcome half stays default until
    // the judge pass). `fields` is a whitelisted QVariantMap keyed by the camelCase
    // names in kWritableCols (coachplanstorage.cpp). created_at is stamped app-side if
    // absent. Emits planLogged(id) (id -1 on failure). The caller derives lever/direction
    // via deriveLeverDirection and includes them in `fields`.
    Q_INVOKABLE void requestLogPlan(const QVariantMap& fields);   // planLogged(qint64 id)

    // --- Derivation (pure; testable; no DB) -----------------------------------------

    struct LeverDirection {
        QString lever;      // 'grind'|'dose'|'profile'|'repeat'|'multi' (see below)
        QString direction;  // 'finer'|'coarser'|'up'|'down'|'switch'|''|'unclear'
    };

    // Derive (lever, direction) deterministically from the model's `structuredNext`
    // prediction vs the anchor shot's dial. Uses GrinderAliases for the numeric grinder
    // comparison, exactly as computeAdherence does. Rules (DoR §1.2):
    //   - prose grinderSetting (not a recordable setting) -> lever='grind', direction='unclear'
    //   - more than one distinct lever actually moved      -> lever='multi',  direction=''
    //   - no scoreable field moved (ranges-only / repeat)  -> lever='repeat', direction=''
    //   - single grind move: finer/coarser by dial sign (lower dial number = finer, the
    //     near-universal espresso-grinder convention); compound/rpm-only move -> 'unclear'
    //   - single dose move: 'up'/'down' by grams sign
    //   - single profile move: 'switch'
    // Note: the structuredNext contract carries no temp/yield field (see
    // summarizeStructuredNext), so those levers never arise from real predictions.
    static LeverDirection deriveLeverDirection(const QJsonObject& structuredNext,
                                               const ShotProjection& anchor);

    // Confidence tier for a JUDGED plan, applied at read/aggregation time over the judged row + the LIVE anchor
    // and follow-up shots (ratings/channeling/bean/timestamp join live — never frozen; DoR §1.5). Pure.
    //   Tier A — validated, may feed an asserted pattern: adherence='followed' AND a single directional lever
    //            (grind/dose/profile with a concrete direction) AND same bean AND ≤14 days AND follow-up not
    //            channeling-flagged AND BOTH shots rated. deltaSign is then the rating-delta fact.
    //   Tier B — grounded observation, hedged: 'followed' + within the gate above but multi-lever or a rating missing.
    //   Tier C — recorded, never evidence: not-followed (ignored/unclear/partial), channeled follow-up, bean drift,
    //            or >14 days.
    struct TierResult {
        QString tier;       // "A" | "B" | "C"
        QString deltaSign;  // "improved" | "worse" | "flat" | ""  (meaningful only for Tier A)
    };
    static TierResult classifyTier(const QVariantMap& judgedPlan,
                                   const ShotProjection& anchor,
                                   const ShotProjection& followUp);

    // --- Synchronous static helpers (caller provides an OPEN assistant.db connection) ---

    // Create coach_plans + indexes if missing. Idempotent. Does NOT create or touch
    // schema_version (FeedbackStorage owns that shared marker).
    static bool ensureSchemaStatic(QSqlDatabase& db);

    // Insert one plan row from a whitelisted field map. Returns new id, or -1 on failure.
    static qint64 insertPlanStatic(QSqlDatabase& db, const QVariantMap& fields);

    // On writing a new plan for a (bean, profile, equipment) scope, flip every OTHER
    // still-'open' plan in that exact scope to 'superseded' so a plan is only ever judged
    // against the shot that DIRECTLY followed it (DoR §1.4). `exceptId` is the just-written
    // row (pass 0 to supersede all open rows in scope). Returns the number flipped.
    static int supersedeOpenPlansStatic(QSqlDatabase& db,
                                        const QString& beanBrand, const QString& beanType,
                                        const QString& profileKbId, qint64 equipmentId,
                                        qint64 exceptId);

    // All status='open' plans, oldest first (so the judge pass evaluates them in the order
    // they were made). Row maps carry every column (camelCase keys + id).
    static QVariantList fetchOpenPlansStatic(QSqlDatabase& db, int limit);

    // Freeze the outcome half of one plan and mark it judged. Immutable thereafter — the
    // caller only invokes this for a row it read as 'open'. Returns false on failure or if
    // the row was not open (guards against a double-judge race). `inPredictedRangeJson` is
    // computeOutcomeInPredictedRange's object serialized to compact JSON.
    static bool writeJudgmentStatic(QSqlDatabase& db, qint64 id,
                                    const QString& adherence,
                                    const QString& inPredictedRangeJson,
                                    qint64 followUpShotId, qint64 judgedAt);

    // Fetch plan rows for aggregation/recall. Filters are all optional: empty
    // beanBrand/beanType = all beans; empty lever = all levers; empty statusFilter =
    // any status (pass 'judged' for the confirmed-pattern path). Newest first.
    static QVariantList fetchPlansStatic(QSqlDatabase& db,
                                         const QString& beanBrand, const QString& beanType,
                                         const QString& lever, const QString& statusFilter,
                                         int limit);

signals:
    void planLogged(qint64 id);   // id -1 on failure

private:
    void runAsync(const QString& connPrefix,
                  std::function<void(QSqlDatabase&)> work,
                  std::function<void(bool dbOpened)> done);

    QString m_dbPath;
    std::shared_ptr<std::atomic<bool>> m_destroyed = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<SerialDbWorker> m_dbWorker;
};
