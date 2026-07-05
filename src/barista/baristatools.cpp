#include "baristatools.h"

#include "../history/shothistorystorage.h"
#include "../history/shotprojection.h"
#include "../ai/shotsummarizer.h"
#include "../core/dbutils.h"

#include <QJsonDocument>
#include <QThread>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QDate>
#include <QTime>
#include <QCoreApplication>
#include <algorithm>

// [barista-fork] The 5 client-tool JSON definitions, moved verbatim from AnthropicProvider::analyzeConversation.
QJsonArray BaristaTools::toolDefinitions()
{
    QJsonArray tools;

    const auto strProp = [](const QString& d){ QJsonObject o; o["type"] = QString("string"); o["description"] = d; return o; };
    const auto intProp = [](const QString& d){ QJsonObject o; o["type"] = QString("integer"); o["description"] = d; return o; };

    // query_shots — on-demand lookup across the user's FULL local shot history.
    QJsonObject qs;
    qs["name"] = QString("query_shots");
    qs["description"] = QString(
        "Look up the user's espresso shots from their FULL local shot history on demand — beyond the "
        "summary already in the data block. Use it for specific shots, counts, or date/bean ranges "
        "(e.g. 'my best shot on this bean', 'shots pulled in June', 'how many shots total', 'first shot "
        "ever'). Returns a compact list of shot summaries, each with a shotId you can pass to "
        "get_shot_detail to dig into one shot.");
    QJsonObject schema;
    schema["type"] = QString("object");
    QJsonObject props;
    props["beanBrand"]    = strProp("Filter by roaster/brand (case-insensitive substring).");
    props["beanType"]     = strProp("Filter by coffee/bean name (case-insensitive substring).");
    props["sinceDate"]    = strProp("Only shots on/after this date, YYYY-MM-DD.");
    props["untilDate"]    = strProp("Only shots on/before this date, YYYY-MM-DD.");
    props["sinceDaysAgo"] = intProp("Alternative to sinceDate: only shots within the last N days.");
    props["sortBy"]       = strProp("'recent' (default, newest first) or 'bestEnjoyment' (highest rated first).");
    props["limit"]        = intProp("Max shots to return (default 15, capped at 50).");
    schema["properties"] = props;
    qs["input_schema"] = schema;
    tools.append(qs);

    // get_shot_detail — the follow-up to query_shots: pull ONE shot's full dial-in + quality analysis so
    // the barista can coach on what actually happened (channeling, truncated pour, grind/temp issues, notes)
    // instead of just the summary row. Same client-side tool-loop as query_shots.
    QJsonObject sd;
    sd["name"] = QString("get_shot_detail");
    sd["description"] = QString(
        "Get the full detail for ONE espresso shot by its shotId (from a query_shots result): exact "
        "dial-in (dose, yield, ratio, grind, temperature, duration, profile), the shot's quality "
        "analysis (channeling, truncated/short pour, grind-too-coarse/fine, temperature stability), TDS/EY "
        "if measured, and the user's notes. Use it after query_shots to actually diagnose or coach on a "
        "specific shot, not just list it.");
    QJsonObject sdSchema;
    sdSchema["type"] = QString("object");
    QJsonObject sdProps;
    sdProps["shotId"] = intProp("The shotId of the shot to inspect (from a query_shots result).");
    sdSchema["properties"] = sdProps;
    sdSchema["required"] = QJsonArray{ QString("shotId") };
    sd["input_schema"] = sdSchema;
    tools.append(sd);

    // compare_shots — diff 2-5 shots side by side (signed deltas + which quality verdicts flipped).
    QJsonObject cs;
    cs["name"] = QString("compare_shots");
    cs["description"] = QString(
        "Compare 2 to 5 shots by their shotIds (from query_shots results): per-shot dial-in scalars plus a "
        "consecutive-changes diff showing what moved (ratio, dose, grind, duration, enjoyment) AND which "
        "quality verdicts flipped (e.g. \"channeling: yes -> no\"). Use it to answer \"why is today worse "
        "than last week\" or \"did the grind change fix the channeling\".");
    QJsonObject csSchema;
    csSchema["type"] = QString("object");
    QJsonObject csProps;
    QJsonObject shotIds;
    shotIds["type"] = QString("array");
    shotIds["description"] = QString("2 to 5 shotIds to compare, in the order you want them diffed (from query_shots results).");
    QJsonObject shotIdsItems;
    shotIdsItems["type"] = QString("integer");
    shotIds["items"] = shotIdsItems;
    csProps["shotIds"] = shotIds;
    csSchema["properties"] = csProps;
    csSchema["required"] = QJsonArray{ QString("shotIds") };
    cs["input_schema"] = csSchema;
    tools.append(cs);

    // get_bean_profile — any bean's freshness + history, or any profile's design intent, on demand.
    QJsonObject bp;
    bp["name"] = QString("get_bean_profile");
    bp["description"] = QString(
        "Look up ANY bean's freshness and history, or ANY profile's design intent, beyond the current one "
        "already in your context. Give a bean (roaster and/or bean name) to get its days-off-roast (or "
        "days-since-thaw if it was frozen), roast level, shot count, best/median enjoyment, and best-rated "
        "recipe; give a profileName to get that profile's curated design intent. Provide at least one.");
    QJsonObject bpSchema;
    bpSchema["type"] = QString("object");
    QJsonObject bpProps;
    bpProps["beanBrand"]   = strProp("Roaster / bean brand to look up (optional; matched loosely).");
    bpProps["beanType"]    = strProp("Bean name / type to look up (optional; matched loosely).");
    bpProps["profileName"] = strProp("Profile name whose design intent to look up (optional; matched loosely).");
    bpSchema["properties"] = bpProps;
    bp["input_schema"] = bpSchema;
    tools.append(bp);

    // detect_grind_drift — has a fixed grind setting drifted faster/slower over time (grinder wear / aging beans)?
    QJsonObject gd;
    gd["name"] = QString("detect_grind_drift");
    gd["description"] = QString(
        "Check whether shots at a FIXED grind setting have drifted faster or slower over time (grinder burr "
        "wear/seasoning, or the beans aging) — a simple recent-vs-older mean-duration comparison, not rigorous "
        "statistics. Call when the user asks why the same setting isn't pulling like it used to. Optionally scope "
        "to a bean and/or a specific setting; otherwise it uses their most-used setting.");
    QJsonObject gdSchema;
    gdSchema["type"] = QString("object");
    QJsonObject gdProps;
    gdProps["beanBrand"]      = strProp("Roaster / bean brand to scope to (optional; matched loosely).");
    gdProps["beanType"]       = strProp("Bean name / type to scope to (optional; matched loosely).");
    gdProps["grinderSetting"] = strProp("Specific grind setting to check (optional; exact match). Omit to use the most-used setting.");
    gdSchema["properties"] = gdProps;
    gd["input_schema"] = gdSchema;
    tools.append(gd);

    return tools;
}

// Freeze-aware freshness for a shot: days since THAW when the bean was frozen+defrosted, else days off
// roast — mirrors the advisor's "age from defrostDate, not roastDate" rule. Sets freshnessKnown either way.
static void baristaFreshness(const ShotProjection& s, QJsonObject& out)
{
    const QDate today   = QDate::currentDate();
    const QDate defrost = QDate::fromString(s.defrostDate.trimmed().left(10), Qt::ISODate);
    const QDate roast   = QDate::fromString(s.roastDate.trimmed().left(10), Qt::ISODate);
    if (defrost.isValid()) {
        out[QStringLiteral("daysSinceThaw")] = static_cast<int>(defrost.daysTo(today));
        if (roast.isValid())
            out[QStringLiteral("daysOffRoast")] = static_cast<int>(roast.daysTo(today));
        out[QStringLiteral("freshnessKnown")] = true;
    } else if (roast.isValid()) {
        out[QStringLiteral("daysOffRoast")] = static_cast<int>(roast.daysTo(today));
        out[QStringLiteral("freshnessKnown")] = true;
    } else {
        out[QStringLiteral("freshnessKnown")] = false;
    }
}

// [barista-fork] Run a barista client-tool query against the local shot DB on a background thread and deliver
// the JSON result to `done` on the main thread. query_shots is a READ-ONLY lookup across the user's FULL shot
// history (any roaster/bean, any date range) — the "access to everything" the barista promised. Kept off the
// main thread because withTempDb opens a fresh connection each call and the target is a slow tablet.
void BaristaTools::executeTool(ShotHistoryStorage* shotHistory, const QString& name, const QJsonObject& input,
                               std::function<void(QJsonValue)> done)
{
    if (!shotHistory) {
        done(QJsonObject{{QStringLiteral("error"), QStringLiteral("shot history unavailable")}});
        return;
    }

    // get_shot_detail — deep-dive ONE shot the barista already learned of via query_shots. Returns the full
    // per-shot projection (dial-in scalars + the five quality detectors + notes) minus the heavy time-series
    // curves, so the barista can actually coach ("that shot channeled", "grind was too coarse") rather than
    // just list. Reuses the exact load path MCP shots_detail uses; runs off the main thread like query_shots.
    if (name == QLatin1String("get_shot_detail")) {
        const qint64 shotId = input.value(QStringLiteral("shotId")).toVariant().toLongLong();
        if (shotId <= 0) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("get_shot_detail needs a positive shotId (from a query_shots result)")}});
            return;
        }
        const QString dbPath = shotHistory->databasePath();
        QThread* thread = QThread::create([=]() {
            QJsonObject result;
            const bool dbOk = withTempDb(dbPath, "barista_shot_detail", [&](QSqlDatabase& db) {
                ShotRecord record = ShotHistoryStorage::loadShotRecordStatic(db, shotId);
                ShotProjection shot = ShotHistoryStorage::convertShotRecord(record);
                if (!shot.isValid()) {
                    result[QStringLiteral("error")] = QStringLiteral("shot not found: ") + QString::number(shotId);
                    return;
                }
                QJsonObject o = shot.toJsonObject();
                // Drop the heavy per-sample curves + debug/profile blobs — the barista coaches on scalars and
                // detector verdicts, not raw traces (mirrors MCP shots_detail's "summary" strip).
                static const char* heavy[] = {
                    "pressure", "flow", "temperature", "temperatureMix", "resistance", "conductance",
                    "darcyResistance", "conductanceDerivative", "waterDispensed", "pressureGoal", "flowGoal",
                    "temperatureGoal", "weight", "weightFlowRate", "debugLog", "profileJson", "beanBaseJson"
                };
                for (const char* k : heavy)
                    o.remove(QLatin1String(k));
                // Drop each detector's scratch `gates` — expose only the user-facing verdict scalars, never the
                // internal thresholds (same reason MCP strips them: they read like dialing knobs but aren't).
                if (o.contains(QStringLiteral("detectorResults"))) {
                    QJsonObject dr = o.value(QStringLiteral("detectorResults")).toObject();
                    for (const QString& dk : {QStringLiteral("grind"), QStringLiteral("channeling"),
                                              QStringLiteral("flowTrend"), QStringLiteral("preinfusion")}) {
                        if (!dr.contains(dk))
                            continue;
                        QJsonObject d = dr.value(dk).toObject();
                        d.remove(QStringLiteral("gates"));
                        dr[dk] = d;
                    }
                    o[QStringLiteral("detectorResults")] = dr;
                }
                result = o;
            });
            // DB-open failure must surface as an error, not an empty object — same rule as query_shots.
            if (!dbOk && !result.contains(QStringLiteral("error")))
                result[QStringLiteral("error")] = QStringLiteral("shot database unavailable");
            QMetaObject::invokeMethod(qApp, [done, result]() { done(result); }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }

    // compare_shots — diff 2-5 shots the barista learned of via query_shots: per-shot lean scalars plus a
    // consecutive-changes diff (signed deltas + which quality VERDICTS flipped), so it can answer "why is
    // today worse than last week". Lean by design (no phase summaries / curves) — respects the tool budget.
    if (name == QLatin1String("compare_shots")) {
        QVector<qint64> ids;
        for (const QJsonValue& v : input.value(QStringLiteral("shotIds")).toArray()) {
            const qint64 id = v.toVariant().toLongLong();
            if (id > 0 && !ids.contains(id)) ids.append(id);
        }
        if (ids.size() < 2) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("compare_shots needs at least 2 valid shotIds (from query_shots results)")}});
            return;
        }
        if (ids.size() > 5) ids.resize(5);
        const QString dbPath = shotHistory->databasePath();
        QThread* thread = QThread::create([=]() {
            QJsonArray perShot;
            const bool dbOk = withTempDb(dbPath, "barista_compare_shots", [&](QSqlDatabase& db) {
                for (const qint64 id : ids) {
                    ShotProjection s = ShotHistoryStorage::convertShotRecord(
                        ShotHistoryStorage::loadShotRecordStatic(db, id));
                    QJsonObject o;
                    o[QStringLiteral("shotId")] = id;
                    if (!s.isValid()) { o[QStringLiteral("error")] = QStringLiteral("not found"); perShot.append(o); continue; }
                    o[QStringLiteral("date")] = QDateTime::fromSecsSinceEpoch(s.timestamp)
                                                    .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                    if (s.doseWeightG > 0)  o[QStringLiteral("doseG")]  = QString::number(s.doseWeightG, 'f', 1).toDouble();
                    if (s.finalWeightG > 0) o[QStringLiteral("yieldG")] = QString::number(s.finalWeightG, 'f', 1).toDouble();
                    if (s.doseWeightG > 0 && s.finalWeightG > 0)
                        o[QStringLiteral("ratio")] = QString::number(s.finalWeightG / s.doseWeightG, 'f', 2).toDouble();
                    if (s.durationSec > 0) o[QStringLiteral("durationSec")] = qRound(s.durationSec);
                    if (const QString g = s.grinderSetting.trimmed(); !g.isEmpty()) o[QStringLiteral("grind")] = g;
                    if (s.enjoyment0to100 > 0) o[QStringLiteral("enjoyment0to100")] = s.enjoyment0to100;
                    if (const QString sb = s.stoppedBy.trimmed(); !sb.isEmpty()) o[QStringLiteral("stoppedBy")] = sb;
                    if (const QString pp = s.puckPrep.trimmed(); !pp.isEmpty()) o[QStringLiteral("puckPrep")] = pp;
                    // Refractometer values are the ONLY real EY — never fabricate one; pass through only when measured.
                    if (s.drinkTdsPct > 0) o[QStringLiteral("tdsPct")] = QString::number(s.drinkTdsPct, 'f', 2).toDouble();
                    if (s.drinkEyPct  > 0) o[QStringLiteral("eyPct")]  = QString::number(s.drinkEyPct,  'f', 1).toDouble();
                    baristaFreshness(s, o);
                    QJsonObject q;   // quality verdicts (always present so the consecutive diff can detect flips)
                    q[QStringLiteral("channeling")]    = s.channelingDetected;
                    q[QStringLiteral("grindIssue")]    = s.grindIssueDetected;
                    q[QStringLiteral("pourTruncated")] = s.pourTruncatedDetected;
                    o[QStringLiteral("quality")] = q;
                    perShot.append(o);
                }
            });
            // Consecutive-changes diff: signed deltas + verdict flips between shot[i-1] and shot[i].
            QJsonArray changes;
            for (int i = 1; i < perShot.size(); ++i) {
                const QJsonObject a = perShot.at(i - 1).toObject(), b = perShot.at(i).toObject();
                if (a.contains(QStringLiteral("error")) || b.contains(QStringLiteral("error"))) continue;
                QJsonObject ch;
                ch[QStringLiteral("fromShotId")] = a.value(QStringLiteral("shotId"));
                ch[QStringLiteral("toShotId")]   = b.value(QStringLiteral("shotId"));
                const auto delta = [&](const char* key, const char* outKey, int prec) {
                    if (!a.contains(QLatin1String(key)) || !b.contains(QLatin1String(key))) return;
                    const double d = b.value(QLatin1String(key)).toDouble() - a.value(QLatin1String(key)).toDouble();
                    const double thr = (prec == 0) ? 0.5 : (prec == 1 ? 0.05 : 0.005);
                    if (qAbs(d) >= thr) ch[QLatin1String(outKey)] = QString::number(d, 'f', prec).toDouble();
                };
                delta("ratio", "ratioDelta", 2);
                delta("doseG", "doseDeltaG", 1);
                delta("durationSec", "durationDeltaSec", 0);
                delta("enjoyment0to100", "enjoymentDelta", 0);
                if (a.contains(QStringLiteral("grind")) && b.contains(QStringLiteral("grind"))
                        && a.value(QStringLiteral("grind")).toString() != b.value(QStringLiteral("grind")).toString())
                    ch[QStringLiteral("grind")] = a.value(QStringLiteral("grind")).toString()
                        + QStringLiteral(" -> ") + b.value(QStringLiteral("grind")).toString();
                const QJsonObject qa = a.value(QStringLiteral("quality")).toObject();
                const QJsonObject qb = b.value(QStringLiteral("quality")).toObject();
                QJsonArray flips;
                for (const QString& k : {QStringLiteral("channeling"), QStringLiteral("grindIssue"),
                                         QStringLiteral("pourTruncated")}) {
                    if (qa.value(k).toBool() != qb.value(k).toBool())
                        flips.append(k + (qb.value(k).toBool() ? QStringLiteral(": no -> yes")
                                                               : QStringLiteral(": yes -> no")));
                }
                if (!flips.isEmpty()) ch[QStringLiteral("verdictFlips")] = flips;
                changes.append(ch);
            }
            QJsonObject result;
            if (!dbOk) result[QStringLiteral("error")] = QStringLiteral("shot database unavailable");
            else {
                result[QStringLiteral("shots")] = perShot;
                if (!changes.isEmpty()) result[QStringLiteral("changes")] = changes;
            }
            QMetaObject::invokeMethod(qApp, [done, result]() { done(result); }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }

    // get_bean_profile — look up ANY bean's freshness + history or ANY profile's design intent on demand
    // (the session context only carries the CURRENT bean/profile). Bean side: freeze-aware freshness +
    // per-bean aggregates + best-shot recipe. Profile side: the curated KB design-intent (main-thread lookup).
    if (name == QLatin1String("get_bean_profile")) {
        const QString beanBrand   = input.value(QStringLiteral("beanBrand")).toString().trimmed();
        const QString beanType    = input.value(QStringLiteral("beanType")).toString().trimmed();
        const QString profileName = input.value(QStringLiteral("profileName")).toString().trimmed();
        if (beanBrand.isEmpty() && beanType.isEmpty() && profileName.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("get_bean_profile needs a beanBrand/beanType or a profileName")}});
            return;
        }
        const QString dbPath = shotHistory->databasePath();
        QThread* thread = QThread::create([=]() {
            QJsonObject result;
            QString profileKbId;
            const bool dbOk = withTempDb(dbPath, "barista_bean_profile", [&](QSqlDatabase& db) {
                if (!beanBrand.isEmpty() || !beanType.isEmpty()) {
                    QString where = QStringLiteral(" WHERE 1=1");
                    if (!beanBrand.isEmpty()) where += QStringLiteral(" AND bean_brand LIKE :brand");
                    if (!beanType.isEmpty())  where += QStringLiteral(" AND bean_type LIKE :type");
                    const auto bind = [&](QSqlQuery& q) {
                        if (!beanBrand.isEmpty()) q.bindValue(QStringLiteral(":brand"), QStringLiteral("%") + beanBrand + QStringLiteral("%"));
                        if (!beanType.isEmpty())  q.bindValue(QStringLiteral(":type"),  QStringLiteral("%") + beanType  + QStringLiteral("%"));
                    };
                    QJsonObject bean;
                    // Most-recent shot -> roast level, freeze-aware freshness, and the bean's usual profile.
                    QSqlQuery rq(db);
                    rq.prepare(QStringLiteral("SELECT id FROM shots") + where + QStringLiteral(" ORDER BY timestamp DESC LIMIT 1"));
                    bind(rq);
                    if (rq.exec() && rq.next()) {
                        ShotProjection r = ShotHistoryStorage::convertShotRecord(
                            ShotHistoryStorage::loadShotRecordStatic(db, rq.value(0).toLongLong()));
                        if (r.isValid()) {
                            if (const QString b = r.beanBrand.trimmed(); !b.isEmpty()) bean[QStringLiteral("roaster")] = b;
                            if (const QString t = r.beanType.trimmed();  !t.isEmpty()) bean[QStringLiteral("bean")] = t;
                            if (const QString rl = r.roastLevel.trimmed(); !rl.isEmpty()) bean[QStringLiteral("roastLevel")] = rl;
                            baristaFreshness(r, bean);
                            profileKbId = r.profileKbId.trimmed();
                        }
                    }
                    // Count + enjoyment distribution over ALL this bean's shots.
                    QSqlQuery eq(db);
                    eq.prepare(QStringLiteral("SELECT enjoyment FROM shots") + where);
                    bind(eq);
                    int total = 0; QVector<int> enj;
                    if (eq.exec()) while (eq.next()) { ++total; if (const int e = eq.value(0).toInt(); e > 0) enj.append(e); }
                    bean[QStringLiteral("shotCount")] = total;
                    if (!enj.isEmpty()) {
                        std::sort(enj.begin(), enj.end());
                        bean[QStringLiteral("enjoymentBest")]   = enj.last();
                        bean[QStringLiteral("enjoymentMedian")] = enj.at(enj.size() / 2);
                    }
                    // Best-rated shot's recipe — the "known-good" recommendation anchor.
                    QSqlQuery bq(db);
                    bq.prepare(QStringLiteral("SELECT id FROM shots") + where
                               + QStringLiteral(" AND enjoyment > 0 ORDER BY enjoyment DESC, timestamp DESC LIMIT 1"));
                    bind(bq);
                    if (bq.exec() && bq.next()) {
                        ShotProjection b = ShotHistoryStorage::convertShotRecord(
                            ShotHistoryStorage::loadShotRecordStatic(db, bq.value(0).toLongLong()));
                        if (b.isValid()) {
                            QJsonObject best;
                            if (b.doseWeightG > 0)  best[QStringLiteral("doseG")]  = QString::number(b.doseWeightG, 'f', 1).toDouble();
                            if (b.finalWeightG > 0) best[QStringLiteral("yieldG")] = QString::number(b.finalWeightG, 'f', 1).toDouble();
                            if (b.doseWeightG > 0 && b.finalWeightG > 0)
                                best[QStringLiteral("ratio")] = QString::number(b.finalWeightG / b.doseWeightG, 'f', 2).toDouble();
                            if (const QString g = b.grinderSetting.trimmed(); !g.isEmpty()) best[QStringLiteral("grind")] = g;
                            if (b.enjoyment0to100 > 0) best[QStringLiteral("enjoyment0to100")] = b.enjoyment0to100;
                            if (const QString p = b.profileName.trimmed(); !p.isEmpty()) best[QStringLiteral("profile")] = p;
                            bean[QStringLiteral("bestShot")] = best;
                        }
                    }
                    if (!bean.isEmpty()) result[QStringLiteral("bean")] = bean;
                }
                // Explicit profile name overrides the bean's usual profile for the KB lookup.
                if (!profileName.isEmpty()) {
                    QSqlQuery pq(db);
                    pq.prepare(QStringLiteral("SELECT profile_kb_id FROM shots WHERE profile_name LIKE :pn "
                                              "AND profile_kb_id IS NOT NULL AND profile_kb_id != '' "
                                              "ORDER BY timestamp DESC LIMIT 1"));
                    pq.bindValue(QStringLiteral(":pn"), QStringLiteral("%") + profileName + QStringLiteral("%"));
                    if (pq.exec() && pq.next())
                        if (const QString kb = pq.value(0).toString().trimmed(); !kb.isEmpty()) profileKbId = kb;
                }
            });
            // Profile design-intent from the curated KB — a static lookup, resolved on the main thread with done().
            QMetaObject::invokeMethod(qApp, [done, result, profileKbId, dbOk]() mutable {
                if (!dbOk && !result.contains(QStringLiteral("error")))
                    result[QStringLiteral("error")] = QStringLiteral("shot database unavailable");
                if (!profileKbId.isEmpty()) {
                    const QString kb = ShotSummarizer::profileKnowledgeForKbId(profileKbId).trimmed();
                    if (!kb.isEmpty()) {
                        QJsonObject prof;
                        prof[QStringLiteral("profileKbId")] = profileKbId;
                        prof[QStringLiteral("designIntent")] = kb.left(1400);
                        result[QStringLiteral("profile")] = prof;
                    }
                }
                if (result.isEmpty())
                    result[QStringLiteral("error")] = QStringLiteral("no shots found for that bean, and no known profile");
                done(result);
            }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }

    // detect_grind_drift — a LEAN, HONEST heuristic: at a FIXED grind setting, have shot times drifted over
    // the run of shots (grinder burrs opening up / seasoning, or the bag aging)? Compares the mean duration of
    // the OLDER half vs the RECENT half of shots at that setting. NOT statistical changepoint detection — a
    // simple recent-vs-older comparison the barista can act on ("your 3.2 shots run faster now — try a hair finer").
    if (name == QLatin1String("detect_grind_drift")) {
        const QString beanBrand = input.value(QStringLiteral("beanBrand")).toString().trimmed();
        const QString beanType  = input.value(QStringLiteral("beanType")).toString().trimmed();
        const QString settingIn = input.value(QStringLiteral("grinderSetting")).toString().trimmed();
        const QString dbPath = shotHistory->databasePath();
        QThread* thread = QThread::create([=]() {
            QJsonObject result;
            const bool dbOk = withTempDb(dbPath, "barista_grind_drift", [&](QSqlDatabase& db) {
                QString beanWhere;
                if (!beanBrand.isEmpty()) beanWhere += QStringLiteral(" AND bean_brand LIKE :brand");
                if (!beanType.isEmpty())  beanWhere += QStringLiteral(" AND bean_type LIKE :type");
                const auto bindBean = [&](QSqlQuery& q) {
                    if (!beanBrand.isEmpty()) q.bindValue(QStringLiteral(":brand"), QStringLiteral("%") + beanBrand + QStringLiteral("%"));
                    if (!beanType.isEmpty())  q.bindValue(QStringLiteral(":type"),  QStringLiteral("%") + beanType  + QStringLiteral("%"));
                };
                // Resolve the setting: explicit, else the most-frequent non-empty setting for this bean (or overall).
                QString setting = settingIn;
                if (setting.isEmpty()) {
                    QSqlQuery sq(db);
                    sq.prepare(QStringLiteral("SELECT grinder_setting, COUNT(*) c FROM shots WHERE grinder_setting IS NOT NULL "
                                              "AND grinder_setting != ''") + beanWhere
                               + QStringLiteral(" GROUP BY grinder_setting ORDER BY c DESC, MAX(timestamp) DESC LIMIT 1"));
                    bindBean(sq);
                    if (sq.exec() && sq.next()) setting = sq.value(0).toString().trimmed();
                }
                if (setting.isEmpty()) {
                    result[QStringLiteral("error")] = QStringLiteral("no grind setting found to analyze");
                    return;
                }
                // Durations at that setting, oldest first.
                QSqlQuery q(db);
                q.prepare(QStringLiteral("SELECT duration_seconds, timestamp FROM shots WHERE grinder_setting = :setting "
                                         "AND duration_seconds > 0") + beanWhere + QStringLiteral(" ORDER BY timestamp ASC"));
                q.bindValue(QStringLiteral(":setting"), setting);
                bindBean(q);
                QVector<double> durs; QVector<qint64> ts;
                if (q.exec()) while (q.next()) { durs.append(q.value(0).toDouble()); ts.append(q.value(1).toLongLong()); }
                result[QStringLiteral("grinderSetting")] = setting;
                result[QStringLiteral("shotCount")] = durs.size();
                if (durs.size() < 6) {
                    result[QStringLiteral("driftDetected")] = false;
                    result[QStringLiteral("note")] = QStringLiteral("not enough shots at this setting to judge drift (need ~6+)");
                    return;
                }
                const int half = durs.size() / 2;
                const auto mean = [](const QVector<double>& v, int lo, int hi) {
                    double s = 0; for (int i = lo; i < hi; ++i) s += v[i]; return (hi > lo) ? s / (hi - lo) : 0.0;
                };
                const double olderMean  = mean(durs, 0, half);
                const double recentMean = mean(durs, durs.size() - half, durs.size());
                const double shift = recentMean - olderMean;
                const double spanDays = (ts.last() - ts.first()) / 86400.0;
                result[QStringLiteral("spanDays")]              = QString::number(spanDays, 'f', 1).toDouble();
                result[QStringLiteral("olderMeanDurationSec")]  = QString::number(olderMean, 'f', 1).toDouble();
                result[QStringLiteral("recentMeanDurationSec")] = QString::number(recentMean, 'f', 1).toDouble();
                result[QStringLiteral("shiftSec")]              = QString::number(shift, 'f', 1).toDouble();
                result[QStringLiteral("driftDetected")]         = (qAbs(shift) >= 3.0);
                result[QStringLiteral("direction")] = (shift < 0) ? QStringLiteral("faster/shorter") : QStringLiteral("slower/longer");
                result[QStringLiteral("note")] = QStringLiteral(
                    "Simple recent-vs-older mean-duration comparison at a fixed setting, not a statistical changepoint. "
                    "A faster drift can mean the grind opened up (burr wear/seasoning) OR the beans aged; a slower drift "
                    "the opposite. If the drift is real, a small grind nudge (finer if faster, coarser if slower) re-centers it.");
            });
            if (!dbOk && !result.contains(QStringLiteral("error")))
                result[QStringLiteral("error")] = QStringLiteral("shot database unavailable");
            QMetaObject::invokeMethod(qApp, [done, result]() { done(result); }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }

    if (name != QLatin1String("query_shots")) {
        done(QJsonObject{{QStringLiteral("error"), QStringLiteral("unknown tool: ") + name}});
        return;
    }

    // Parse + clamp inputs on the main thread (cheap), then hand pure values to the worker.
    const QString beanBrand = input.value(QStringLiteral("beanBrand")).toString().trimmed();
    const QString beanType  = input.value(QStringLiteral("beanType")).toString().trimmed();
    const QString sortBy    = input.value(QStringLiteral("sortBy")).toString().trimmed();
    int limit = input.value(QStringLiteral("limit")).toInt(15);
    if (limit <= 0) limit = 15;
    if (limit > 50) limit = 50;

    // Resolve date filters to epoch bounds here so the worker stays pure. sinceDate wins over sinceDaysAgo.
    qint64 sinceEpoch = 0, untilEpoch = 0;
    if (const int days = input.value(QStringLiteral("sinceDaysAgo")).toInt(0); days > 0)
        sinceEpoch = QDateTime::currentDateTime().addDays(-days).toSecsSinceEpoch();
    if (const QDate d = QDate::fromString(input.value(QStringLiteral("sinceDate")).toString().trimmed(),
                                          QStringLiteral("yyyy-MM-dd")); d.isValid())
        sinceEpoch = QDateTime(d, QTime(0, 0)).toSecsSinceEpoch();
    if (const QDate d = QDate::fromString(input.value(QStringLiteral("untilDate")).toString().trimmed(),
                                          QStringLiteral("yyyy-MM-dd")); d.isValid())
        untilEpoch = QDateTime(d, QTime(23, 59, 59)).toSecsSinceEpoch();

    const bool bestFirst = (sortBy.compare(QLatin1String("bestEnjoyment"), Qt::CaseInsensitive) == 0);
    const QString dbPath = shotHistory->databasePath();

    QThread* thread = QThread::create([=]() {
        QJsonArray shots;
        qint64 totalMatched = 0;
        QString errMsg;
        const bool dbOk = withTempDb(dbPath, "barista_query_shots", [&](QSqlDatabase& db) {
            QString where = QStringLiteral(" WHERE 1=1");
            if (!beanBrand.isEmpty()) where += QStringLiteral(" AND bean_brand LIKE :brand");
            if (!beanType.isEmpty())  where += QStringLiteral(" AND bean_type LIKE :type");
            if (sinceEpoch > 0)       where += QStringLiteral(" AND timestamp >= :since");
            if (untilEpoch > 0)       where += QStringLiteral(" AND timestamp <= :until");
            const auto bind = [&](QSqlQuery& q) {
                if (!beanBrand.isEmpty()) q.bindValue(QStringLiteral(":brand"), QStringLiteral("%") + beanBrand + QStringLiteral("%"));
                if (!beanType.isEmpty())  q.bindValue(QStringLiteral(":type"),  QStringLiteral("%") + beanType  + QStringLiteral("%"));
                if (sinceEpoch > 0)       q.bindValue(QStringLiteral(":since"), sinceEpoch);
                if (untilEpoch > 0)       q.bindValue(QStringLiteral(":until"), untilEpoch);
            };

            // Total matched — lets the barista answer "how many" even when the returned list is capped.
            QSqlQuery cq(db);
            cq.prepare(QStringLiteral("SELECT COUNT(*) FROM shots") + where);
            bind(cq);
            if (cq.exec() && cq.next())
                totalMatched = cq.value(0).toLongLong();
            else
                errMsg = QStringLiteral("count: ") + cq.lastError().text();

            const QString order = bestFirst
                ? QStringLiteral(" ORDER BY enjoyment DESC, timestamp DESC")
                : QStringLiteral(" ORDER BY timestamp DESC");
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "SELECT id, timestamp, profile_name, dose_weight, final_weight, duration_seconds, "
                "enjoyment, grinder_setting, bean_brand, bean_type, espresso_notes FROM shots")
                + where + order + QStringLiteral(" LIMIT ") + QString::number(limit));
            bind(q);
            if (q.exec()) {
                while (q.next()) {
                    QJsonObject s;
                    // shotId lets the barista follow up with get_shot_detail on any listed shot.
                    s[QStringLiteral("shotId")] = q.value(0).toLongLong();
                    s[QStringLiteral("date")] = QDateTime::fromSecsSinceEpoch(q.value(1).toLongLong())
                                                    .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                    const QString profile = q.value(2).toString().trimmed();
                    if (!profile.isEmpty()) s[QStringLiteral("profile")] = profile;
                    const double dose  = q.value(3).toDouble();
                    const double yield = q.value(4).toDouble();
                    if (dose  > 0) s[QStringLiteral("doseG")]  = QString::number(dose,  'f', 1).toDouble();
                    if (yield > 0) s[QStringLiteral("yieldG")] = QString::number(yield, 'f', 1).toDouble();
                    if (dose > 0 && yield > 0) s[QStringLiteral("ratio")] = QString::number(yield / dose, 'f', 2).toDouble();
                    if (const double dur = q.value(5).toDouble(); dur > 0)
                        s[QStringLiteral("durationSec")] = QString::number(dur, 'f', 0).toInt();
                    if (const int enjoy = q.value(6).toInt(); enjoy > 0)
                        s[QStringLiteral("enjoyment0to100")] = enjoy;
                    if (const QString grind = q.value(7).toString().trimmed(); !grind.isEmpty())
                        s[QStringLiteral("grind")] = grind;
                    if (const QString brand = q.value(8).toString().trimmed(); !brand.isEmpty())
                        s[QStringLiteral("roaster")] = brand;
                    if (const QString type = q.value(9).toString().trimmed(); !type.isEmpty())
                        s[QStringLiteral("bean")] = type;
                    if (const QString notes = q.value(10).toString().trimmed(); !notes.isEmpty())
                        s[QStringLiteral("notes")] = notes.left(240);
                    shots.append(s);
                }
            } else {
                errMsg = QStringLiteral("query: ") + q.lastError().text();
            }
        });

        // A DB-open or query failure must surface as an error — NOT as an empty result, or the barista
        // would falsely tell the user they have no shot history (the exact bug this tool exists to prevent).
        QJsonObject result;
        if (!dbOk)
            result[QStringLiteral("error")] = QStringLiteral("shot database unavailable");
        else if (!errMsg.isEmpty())
            result[QStringLiteral("error")] = QStringLiteral("shot query failed: ") + errMsg;
        else {
            result[QStringLiteral("matchedCount")]  = totalMatched;
            result[QStringLiteral("returnedCount")] = static_cast<int>(shots.size());
            result[QStringLiteral("shots")] = shots;
        }
        QMetaObject::invokeMethod(qApp, [done, result]() { done(result); }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
