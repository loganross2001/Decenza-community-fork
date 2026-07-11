#include "baristatools.h"

#include "../history/shothistorystorage.h"
#include "../history/shotprojection.h"
#include "../ai/shotsummarizer.h"
#include "../core/dbutils.h"
#include "feedbackstorage.h"
#include "tasksstorage.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QStringList>
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

    // [barista-fork] log_tasting_feedback (WRITE) — the barista's verbal-feedback KB. Claude fills the LIGHT
    // structured schema INLINE as tool input during the tool-use loop it's already in (no second extraction
    // call). CRITICAL: shot_id is NOT a field here — the executor stamps it app-side from the current anchor,
    // so a model-supplied id can't attach feedback to the wrong shot. Everything but raw_text is optional.
    QJsonObject lf;
    lf["name"] = QString("log_tasting_feedback");
    lf["description"] = QString(
        "Record the user's tasting/texture feedback about a shot to their private feedback knowledge base, so "
        "you can reason over it across sessions (\"3rd time this bean's been sour\"). Call this WHENEVER the user "
        "describes how a shot TASTED or FELT — at close-out, mid-conversation, or an unprompted \"that last one "
        "was sour\". Fill only the fields you can infer from what they actually said; leave the rest out. Do NOT "
        "invent numbers or descriptors. The shot it attaches to and the dial (dose/yield/grind/temp/bean/profile) "
        "are recorded automatically — you do NOT supply a shot id.");
    QJsonObject lfSchema;
    lfSchema["type"] = QString("object");
    QJsonObject lfProps;
    lfProps["raw_text"]              = strProp("What the user actually said about the taste/texture, in their own words (required).");
    lfProps["overall_rating_0to100"]= intProp("How much they liked it, 0-100, ONLY if they gave a clear sense of it (optional).");
    lfProps["acidity"]              = strProp("One of: sour, bright, balanced, flat (optional).");
    lfProps["bitterness"]           = strProp("One of: none, mild, harsh (optional).");
    lfProps["body"]                 = strProp("One of: thin, medium, syrupy (optional).");
    lfProps["sweetness"]            = strProp("One of: low, balanced, high (optional).");
    lfProps["balance"]              = strProp("One of: under, balanced, over (optional).");
    lfProps["milk"]                 = strProp("For a milk drink, one of: thin, silky, stiff (optional).");
    QJsonObject lfDesc;
    lfDesc["type"] = QString("array");
    lfDesc["description"] = QString("Short freeform descriptor words the user used or implied, e.g. [\"sour\",\"thin milk\"] (optional).");
    QJsonObject lfDescItems; lfDescItems["type"] = QString("string");
    lfDesc["items"] = lfDescItems;
    lfProps["descriptors"] = lfDesc;
    lfProps["suggested_adjustment"] = strProp("A next-shot change the user or you inferred, e.g. \"grind finer\" (optional).");
    lfSchema["properties"] = lfProps;
    lfSchema["required"] = QJsonArray{ QString("raw_text") };
    lf["input_schema"] = lfSchema;
    tools.append(lf);

    // [barista-fork] search_tasting_feedback (READ) — ad-hoc/filtered lookup of past feedback for a bean.
    // Proactive current-bean feedback is ALREADY folded into the context block every turn; this tool is the
    // secondary path for filtered queries (e.g. "when did I last call this sour") or a DIFFERENT bean.
    QJsonObject sf;
    sf["name"] = QString("search_tasting_feedback");
    sf["description"] = QString(
        "Look up the user's PAST tasting feedback for a bean from their feedback knowledge base — what they said, "
        "the dial they used, and any adjustment tried. Use it to build advice on what worked before (\"you called "
        "this sour twice; last time a half-step finer helped\") or to answer a specific recall question. The "
        "current bean's recent feedback is already in your context; reach for this for a filtered search or a "
        "different bean.");
    QJsonObject sfSchema;
    sfSchema["type"] = QString("object");
    QJsonObject sfProps;
    sfProps["bean_brand"]        = strProp("Roaster / bean brand to look up (matched exactly, case-insensitive).");
    sfProps["bean_type"]         = strProp("Bean name / type to look up (matched exactly, case-insensitive).");
    sfProps["descriptor_filter"] = strProp("Optional word to filter feedback by, e.g. \"sour\" or \"thin\" (full-text match).");
    sfSchema["properties"] = sfProps;
    sfSchema["required"] = QJsonArray{ QString("bean_brand"), QString("bean_type") };
    sf["input_schema"] = sfSchema;
    tools.append(sf);

    // [barista-fork] apply_dial_change (WRITE) — apply an agreed next-shot dial change. This is the
    // approve-then-apply seam: the barista PROPOSES a change and asks; only once the user clearly approves
    // ("yes" / "do it" / "go ahead") does it call this. dose/yield/temperature are written to the next-shot
    // dial immediately; grinderSetting (off-machine) is queued and confirmed at the next shot. The executor
    // range-checks every value (a hallucinated 200 °C never reaches the machine) and reports back what was
    // applied/queued/rejected so the barista can confirm accurately.
    QJsonObject ad;
    ad["name"] = QString("apply_dial_change");
    ad["description"] = QString(
        "Apply an agreed change to the NEXT shot's dial-in. Call this ONLY after you have proposed the change "
        "and the user has clearly approved it (\"yes\", \"do it\", \"go ahead\") — NEVER unprompted, and never "
        "just to acknowledge or to restate unchanged settings. Include ONLY the field(s) that actually change. "
        "dose, yield, and temperature are set on the machine's next-shot dial right away; the grinder setting is "
        "off-machine, so it is queued and you'll remind the user to set it at the next shot. The result tells you "
        "exactly what was applied, queued, or rejected (out of range) — confirm to the user from that, naturally "
        "(\"Done — grind's at 4.4 for the next one\").");
    QJsonObject adSchema;
    adSchema["type"] = QString("object");
    QJsonObject adProps;
    adProps["grinderSetting"] = strProp("New grinder dial setting (off-machine; queued for the next shot). Optional.");
    QJsonObject doseP;  doseP["type"] = QString("number"); doseP["description"] = QString("New dose IN, grams (5-30). Optional.");
    QJsonObject yieldP; yieldP["type"] = QString("number"); yieldP["description"] = QString("New yield OUT, grams (10-120). Optional.");
    QJsonObject ratioP; ratioP["type"] = QString("number"); ratioP["description"] = QString("Brew ratio, e.g. 2.0 for 1:2.0 (yield is computed from dose x ratio when no explicit yield is given). Optional.");
    QJsonObject tempP;  tempP["type"] = QString("number"); tempP["description"] = QString("Brew temperature, degrees C (80-100). Optional.");
    adProps["doseG"]         = doseP;
    adProps["targetWeightG"] = yieldP;
    adProps["ratio"]         = ratioP;
    adProps["temperatureC"]  = tempP;
    adSchema["properties"] = adProps;
    ad["input_schema"] = adSchema;
    tools.append(ad);

    // [barista-fork] end_conversation — the barista ends its OWN session when the user signals they're done
    // ("that'll be all", "thanks, I'm good", "bye", "see you later"). It speaks a short warm sign-off FIRST
    // (in the same reply) and calls this; the app collapses the dock only AFTER the sign-off finishes speaking,
    // so the user never has to hit stop and the sign-off is never cut off. No required args (an optional reason
    // is fine). The executor merely fires the endConversation seam (a main-thread signal emit) and returns.
    QJsonObject ec;
    ec["name"] = QString("end_conversation");
    ec["description"] = QString(
        "End this conversation and let the assistant panel collapse, when the user has clearly signalled they're "
        "DONE — a genuine wrap-up or farewell (\"that'll be all\", \"thanks, I'm good\", \"I'm done\", \"bye\", "
        "\"see you later\", \"ok got it thanks\"). Give a SHORT warm sign-off in your reply FIRST (\"Anytime — "
        "enjoy!\"), then call this in the same turn; the app speaks your sign-off and only then closes the panel. "
        "Judge intent: do NOT call it on a mid-conversation \"thanks\" that's clearly followed by more, only on a "
        "real close-out. NEVER call it unprompted.");
    QJsonObject ecSchema;
    ecSchema["type"] = QString("object");
    QJsonObject ecProps;
    ecProps["reason"] = strProp("Optional short note on why the conversation is ending (e.g. \"user said goodbye\").");
    ecSchema["properties"] = ecProps;
    ec["input_schema"] = ecSchema;
    tools.append(ec);

    // [barista-fork] create_reminder (WRITE) — the user asks to be reminded of something ("remind me to
    // flush the group head on Saturday"). The MODEL resolves the natural-language due to an ISO datetime
    // (it can see today's date in sessionContext) and passes both the ISO due AND the user's own phrasing
    // through; the executor stamps epoch app-side. Optional recurrence for repeating chores.
    QJsonObject cr;
    cr["name"] = QString("create_reminder");
    cr["description"] = QString(
        "Create a reminder for the user when they ask to be reminded of something (\"remind me to flush the "
        "group head on Saturday\", \"remind me to descale next month\"). Resolve their timing to an ISO 8601 "
        "datetime using today's date from sessionContext (e.g. the next Saturday at a sensible morning hour if "
        "they don't give a time), and pass their ORIGINAL phrasing through in userPhrasing. Set recurrence only "
        "if they clearly want it to repeat. Confirm naturally once it's saved (\"Got it — I'll remind you "
        "Saturday\"). Do NOT invent reminders they didn't ask for.");
    QJsonObject crSchema;
    crSchema["type"] = QString("object");
    QJsonObject crProps;
    crProps["text"]         = strProp("What to remind the user to do, in a few words (required), e.g. \"flush the group head\".");
    crProps["due"]          = strProp("When it's due, as an ISO 8601 datetime you compute from today's date (required), e.g. \"2026-07-11T08:00:00\".");
    crProps["userPhrasing"] = strProp("The user's own words for the timing, passed through verbatim, e.g. \"Saturday\" or \"tomorrow morning\" (optional).");
    crProps["recurrence"]   = strProp("One of: daily, weekly, monthly — ONLY if the user wants it to repeat (optional).");
    crSchema["properties"] = crProps;
    crSchema["required"] = QJsonArray{ QString("text"), QString("due") };
    cr["input_schema"] = crSchema;
    tools.append(cr);

    // [barista-fork] add_personal_date (WRITE) — the owner tells the barista an important day to remember
    // ("remember my anniversary is June 3", "my daughter's birthday is on the 12th of March"). The MODEL
    // resolves the words to a month + day (and an optional year); the executor validates and stores it. It
    // combines with the built-in US holidays to drive the barista's greeting/goodbye "todaysOccasion".
    QJsonObject pd;
    pd["name"] = QString("add_personal_date");
    pd["description"] = QString(
        "Remember a personal important date the user asks you to keep (\"remember my anniversary is June 3\", "
        "\"my birthday is October 12\", \"our wedding was 2019-06-03\"). Resolve their words to a month (1-12) "
        "and day (1-31); include year ONLY if they gave a specific one-time year, otherwise leave it out so it "
        "recurs every year (the usual case). Give a short natural label for the occasion (e.g. \"anniversary\", "
        "\"Mom's birthday\"). Confirm briefly once it's saved (\"Got it — I'll remember your anniversary on "
        "June 3\"). Do NOT invent dates the user didn't give you.");
    QJsonObject pdSchema;
    pdSchema["type"] = QString("object");
    QJsonObject pdProps;
    pdProps["label"] = strProp("A short name for the occasion, e.g. \"anniversary\" or \"Mom's birthday\" (required).");
    pdProps["month"] = intProp("Month of the date, 1-12 (required).");
    pdProps["day"]   = intProp("Day of the month, 1-31 (required).");
    pdProps["year"]  = intProp("A specific year, ONLY if the user pinned it to one (optional; omit for a yearly recurring date).");
    pdSchema["properties"] = pdProps;
    pdSchema["required"] = QJsonArray{ QString("label"), QString("month"), QString("day") };
    pd["input_schema"] = pdSchema;
    tools.append(pd);

    // [barista-fork] list_due_reminders (READ) — surface reminders that are now due. Due reminders/maintenance
    // are ALSO folded into the context block (dueItems) each turn, so reach for this for an explicit recall
    // ("what am I supposed to do today?") or after completing one, to see what's left.
    QJsonObject lr;
    lr["name"] = QString("list_due_reminders");
    lr["description"] = QString(
        "List the user's reminders that are now due (things they earlier asked to be reminded of). The most "
        "pressing due item is already summarised in your context block (dueItems); use this tool for an explicit "
        "\"what do I need to do\" question or to see everything outstanding. Each result has a reminderId you can "
        "pass to complete_reminder when they say it's done.");
    QJsonObject lrSchema;
    lrSchema["type"] = QString("object");
    lrSchema["properties"] = QJsonObject{};
    lr["input_schema"] = lrSchema;
    tools.append(lr);

    // [barista-fork] complete_reminder (WRITE) — clear a reminder the user says they've done. A recurring
    // reminder rolls forward to its next occurrence instead of closing.
    QJsonObject cp;
    cp["name"] = QString("complete_reminder");
    cp["description"] = QString(
        "Mark a reminder done when the user says they've handled it (\"done\", \"flushed it\", \"already did "
        "that\"). Pass the reminderId from a list_due_reminders result or the dueItems block. A recurring "
        "reminder automatically rolls forward to its next occurrence. Acknowledge briefly (\"Nice — cleared\").");
    QJsonObject cpSchema;
    cpSchema["type"] = QString("object");
    QJsonObject cpProps;
    cpProps["reminderId"] = intProp("The id of the reminder to complete (from list_due_reminders or the dueItems block).");
    cpSchema["properties"] = cpProps;
    cpSchema["required"] = QJsonArray{ QString("reminderId") };
    cp["input_schema"] = cpSchema;
    tools.append(cp);

    // [barista-fork] log_maintenance (WRITE) — record that a recurring maintenance task was done, so its
    // next-due date resets. taskKey comes from the dueItems maintenance block (each due task carries its key).
    QJsonObject lm;
    lm["name"] = QString("log_maintenance");
    lm["description"] = QString(
        "Record that the user just completed a maintenance task (backflush, descale, cleaned the shower screen, "
        "etc.) so its next-due date resets. Use the taskKey from the dueItems maintenance block. Only call this "
        "for a task that actually exists in that block — never invent a task or its schedule; the intervals are "
        "editable defaults the user confirms in settings, not authoritative facts. Acknowledge briefly.");
    QJsonObject lmSchema;
    lmSchema["type"] = QString("object");
    QJsonObject lmProps;
    lmProps["taskKey"] = strProp("The maintenance task's key, from the dueItems maintenance block (e.g. \"backflush\").");
    lmSchema["properties"] = lmProps;
    lmSchema["required"] = QJsonArray{ QString("taskKey") };
    lm["input_schema"] = lmSchema;
    tools.append(lm);

    // [barista-fork] update_maintenance_default (WRITE) — the approve-then-apply seam for the periodic
    // "Decent updated their cleaning guide" flow. When the context block's maintenanceDocChanged is present,
    // the barista OFFERS specific default-interval updates; ONLY after the owner clearly approves a given one
    // does it call this. It updates ONLY a task that is STILL on its seeded default (is_default=1) — an
    // owner-overridden interval is never touched (the executor reports it skipped). Marks the doc change
    // reviewed so it isn't re-offered.
    QJsonObject um;
    um["name"] = QString("update_maintenance_default");
    um["description"] = QString(
        "Apply an agreed update to a maintenance task's DEFAULT interval, sourced from Decent's updated "
        "cleaning guide (the context block's maintenanceDocChanged). Call this ONLY after you have OFFERED the "
        "specific change and the owner has clearly approved it (\"yes\", \"go ahead\") — one call per accepted "
        "task. It updates the interval only if the task is still on its default; if the owner had already "
        "customised that task, the change is skipped and you should say so and leave their setting alone. "
        "Confirm from the result naturally (\"Done — backflush is now every 5 days\"). This is the ONLY way "
        "Decent-doc changes get applied; nothing changes without the owner's yes.");
    QJsonObject umSchema;
    umSchema["type"] = QString("object");
    QJsonObject umProps;
    umProps["task_key"]      = strProp("The maintenance task's key from the maintenanceDocChanged block (e.g. \"backflush\").");
    umProps["interval_days"] = intProp("The new default interval in days from Decent's guide (0-3650).");
    umProps["label"]         = strProp("An updated task label, ONLY if Decent's guide clearly renamed it (optional).");
    umSchema["properties"] = umProps;
    umSchema["required"] = QJsonArray{ QString("task_key"), QString("interval_days") };
    um["input_schema"] = umSchema;
    tools.append(um);

    // [barista-fork] dismiss_maintenance_doc_change (WRITE) — the "no thanks" path. When the owner declines
    // the Decent-doc offer (or after all accepted changes are applied), call this ONCE to mark the change
    // reviewed so it is never re-offered until Decent changes the guide AGAIN. Takes no fields.
    QJsonObject dm;
    dm["name"] = QString("dismiss_maintenance_doc_change");
    dm["description"] = QString(
        "Dismiss the current Decent cleaning-guide change (the maintenanceDocChanged block) so it is not "
        "offered again. Call this when the owner declines the update (\"no thanks\", \"leave it\", \"not now\") "
        "OR once you have applied every change they accepted — it simply marks the change reviewed. It changes "
        "no maintenance intervals. No arguments.");
    QJsonObject dmSchema;
    dmSchema["type"] = QString("object");
    dmSchema["properties"] = QJsonObject{};
    dm["input_schema"] = dmSchema;
    tools.append(dm);

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

// [barista-fork] Map the model's tasting axes to the canonical one-tap taste choice ("sour" | "balanced" |
// "bitter") the post-shot review persists — so a SPOKEN rating lands the SAME direction token the tap buttons
// do. The advisor reads "Tasted sour" as an UNDER-extraction direction signal (enjoyment alone doesn't encode
// sour-vs-bitter), so the mapping mirrors the two-axis dialing model: acidity=="sour" → "sour"; else
// bitterness=="harsh" OR balance=="over" → "bitter"; else an explicit balanced read → "balanced". NOTE:
// acidity=="bright" is deliberately NOT mapped to "sour" — in coffee "bright" is a POSITIVE acidity descriptor,
// not the under-extraction fault "sour" denotes, so stamping "Tasted sour" on a shot the user liked would send
// a wrong "grind finer" signal to the advisor. Body/strength-only feedback ("thin") also yields NO marker (the
// tap buttons don't cover it either) — that nuance stays in the feedback KB. Empty = no clear taste axis.
static QString baristaTasteChoice(const QJsonObject& input)
{
    const QString acidity    = input.value(QStringLiteral("acidity")).toString().trimmed().toLower();
    const QString bitterness = input.value(QStringLiteral("bitterness")).toString().trimmed().toLower();
    const QString balance    = input.value(QStringLiteral("balance")).toString().trimmed().toLower();
    if (acidity == QLatin1String("sour"))
        return QStringLiteral("sour");
    if (bitterness == QLatin1String("harsh") || balance == QLatin1String("over"))
        return QStringLiteral("bitter");
    if (acidity == QLatin1String("balanced") || acidity == QLatin1String("bright")
        || balance == QLatin1String("balanced"))
        return QStringLiteral("balanced");
    return QString();
}

// [barista-fork] Enjoyment fallback for a taste choice when the user gave a direction but no explicit number —
// the SAME mapping the post-shot tap buttons use (PostShotReviewPage enjoymentForTaste). An explicit
// overall_rating_0to100 always wins over this.
static int baristaEnjoymentForTaste(const QString& choice)
{
    if (choice == QLatin1String("sour"))     return 45;
    if (choice == QLatin1String("balanced")) return 82;
    if (choice == QLatin1String("bitter"))   return 55;
    return 0;
}

// [barista-fork] The "Tasted <choice>" note-merge now lives in ShotHistoryStorage::requestApplyTasteToShot
// (notesWithTasteMarkerStatic), which merges against a LIVE read of the shot's notes on the DB thread — so a
// spoken rating can't clobber notes typed after the barista opened. (Was a stale-snapshot merge here.)

// [barista-fork] Run a barista client-tool query against the local shot DB on a background thread and deliver
// the JSON result to `done` on the main thread. query_shots is a READ-ONLY lookup across the user's FULL shot
// history (any roaster/bean, any date range) — the "access to everything" the barista promised. Kept off the
// main thread because withTempDb opens a fresh connection each call and the target is a slow tablet.
void BaristaTools::executeTool(ShotHistoryStorage* shotHistory, FeedbackStorage* feedback,
                               TasksStorage* tasks,
                               const std::function<QVariantMap(const QVariantMap&, qint64)>& applyDial,
                               const std::function<void()>& endConversation,
                               const QVariantMap& anchorSnapshot,
                               const QString& name, const QJsonObject& input,
                               std::function<void(QJsonValue)> done)
{
    // [barista-fork] end_conversation — the barista dismisses itself on a spoken goodbye. The seam merely
    // emits a main-thread signal (AssistantOrchestrator::requestDismiss → dismissRequested); the overlay
    // collapses the dock AFTER the sign-off finishes speaking, so the sign-off is never cut off. We return
    // immediately so the model's turn (which already carries the sign-off text) completes normally.
    if (name == QLatin1String("end_conversation")) {
        if (!endConversation) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("end-conversation unavailable")}});
            return;
        }
        endConversation();
        done(QJsonObject{{QStringLiteral("ended"), true}});
        return;
    }

    // [barista-fork] create_reminder (WRITE) — the model resolved the user's timing to an ISO due; the
    // executor parses it to epoch app-side (rejecting anything unparseable so a bad due never persists as
    // "due now"), validates recurrence, and passes the user's phrasing through for provenance.
    if (name == QLatin1String("create_reminder")) {
        if (!tasks) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("reminder storage unavailable")}});
            return;
        }
        const QString text = input.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("create_reminder needs text (what to remind the user to do)")}});
            return;
        }
        const QString dueStr = input.value(QStringLiteral("due")).toString().trimmed();
        QDateTime due = QDateTime::fromString(dueStr, Qt::ISODate);
        if (!due.isValid())   // tolerate a date-only ISO (default to 8am local)
            if (const QDate d = QDate::fromString(dueStr.left(10), Qt::ISODate); d.isValid())
                due = QDateTime(d, QTime(8, 0));
        if (!due.isValid()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("create_reminder needs a valid ISO 8601 due datetime (e.g. 2026-07-11T08:00:00)")}});
            return;
        }
        QString recurrence = input.value(QStringLiteral("recurrence")).toString().trimmed().toLower();
        if (recurrence != QLatin1String("daily") && recurrence != QLatin1String("weekly")
            && recurrence != QLatin1String("monthly"))
            recurrence.clear();

        QVariantMap fields;
        fields.insert(QStringLiteral("text"), text);
        fields.insert(QStringLiteral("dueAt"), due.toSecsSinceEpoch());
        fields.insert(QStringLiteral("recurrence"), recurrence);
        fields.insert(QStringLiteral("userPhrasing"), input.value(QStringLiteral("userPhrasing")).toString().trimmed());

        const QString dueEcho = due.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(tasks, &TasksStorage::reminderCreated, tasks,
            [done, conn, dueEcho, recurrence](qint64 id) {
                QObject::disconnect(*conn);
                if (id > 0) {
                    QJsonObject o{{QStringLiteral("ok"), true}, {QStringLiteral("reminderId"), id},
                                  {QStringLiteral("dueAt"), dueEcho}};
                    if (!recurrence.isEmpty()) o[QStringLiteral("recurrence")] = recurrence;
                    done(o);
                } else {
                    done(QJsonObject{{QStringLiteral("error"), QStringLiteral("failed to save reminder")}});
                }
            });
        tasks->requestCreateReminder(fields);
        return;
    }

    // [barista-fork] add_personal_date (WRITE) — store the owner's own important day. The model resolves the
    // words to month/day (+ optional year); the executor validates ranges app-side (a bad value never persists)
    // and stores via TasksStorage (its own async worker). Combines with built-in US holidays for todaysOccasion.
    if (name == QLatin1String("add_personal_date")) {
        if (!tasks) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("personal-date storage unavailable")}});
            return;
        }
        const QString label = input.value(QStringLiteral("label")).toString().trimmed();
        const int month = input.value(QStringLiteral("month")).toVariant().toInt();
        const int day   = input.value(QStringLiteral("day")).toVariant().toInt();
        if (label.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("add_personal_date needs a label (e.g. \"anniversary\")")}});
            return;
        }
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("add_personal_date needs a valid month (1-12) and day (1-31)")}});
            return;
        }
        QVariantMap fields;
        fields.insert(QStringLiteral("label"), label);
        fields.insert(QStringLiteral("month"), month);
        fields.insert(QStringLiteral("day"), day);
        if (input.contains(QStringLiteral("year")))
            fields.insert(QStringLiteral("year"), input.value(QStringLiteral("year")).toVariant().toInt());

        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(tasks, &TasksStorage::personalDateAdded, tasks,
            [done, conn, label, month, day](qint64 id) {
                QObject::disconnect(*conn);
                if (id > 0)
                    done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("personalDateId"), id},
                                     {QStringLiteral("label"), label},
                                     {QStringLiteral("month"), month}, {QStringLiteral("day"), day}});
                else
                    done(QJsonObject{{QStringLiteral("error"), QStringLiteral("failed to save personal date")}});
            });
        tasks->requestAddPersonalDate(fields);
        return;
    }

    // [barista-fork] list_due_reminders (READ) — open reminders due now, newest-due first.
    if (name == QLatin1String("list_due_reminders")) {
        if (!tasks) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("reminder storage unavailable")}});
            return;
        }
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(tasks, &TasksStorage::dueRemindersReady, tasks,
            [done, conn](const QVariantList& rows) {
                QObject::disconnect(*conn);
                QJsonArray arr;
                for (const QVariant& r : rows) {
                    const QVariantMap m = r.toMap();
                    QJsonObject o;
                    o[QStringLiteral("reminderId")] = m.value(QStringLiteral("id")).toLongLong();
                    o[QStringLiteral("text")]       = m.value(QStringLiteral("text")).toString();
                    o[QStringLiteral("due")]        = QDateTime::fromSecsSinceEpoch(
                        m.value(QStringLiteral("dueAt")).toLongLong()).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                    if (const QString rec = m.value(QStringLiteral("recurrence")).toString(); !rec.isEmpty())
                        o[QStringLiteral("recurrence")] = rec;
                    if (const QString phr = m.value(QStringLiteral("userPhrasing")).toString(); !phr.isEmpty())
                        o[QStringLiteral("userPhrasing")] = phr;
                    arr.append(o);
                }
                done(QJsonObject{{QStringLiteral("dueCount"), arr.size()}, {QStringLiteral("reminders"), arr}});
            });
        tasks->requestDueReminders(0);
        return;
    }

    // [barista-fork] complete_reminder (WRITE) — clear (or roll forward, if recurring) a reminder.
    if (name == QLatin1String("complete_reminder")) {
        if (!tasks) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("reminder storage unavailable")}});
            return;
        }
        const qint64 reminderId = input.value(QStringLiteral("reminderId")).toVariant().toLongLong();
        if (reminderId <= 0) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("complete_reminder needs a positive reminderId (from list_due_reminders)")}});
            return;
        }
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(tasks, &TasksStorage::reminderCompleted, tasks,
            [done, conn](qint64 id) {
                QObject::disconnect(*conn);
                if (id > 0)
                    done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("reminderId"), id}});
                else
                    done(QJsonObject{{QStringLiteral("error"),
                        QStringLiteral("reminder not found or already completed")}});
            });
        tasks->requestCompleteReminder(reminderId);
        return;
    }

    // [barista-fork] log_maintenance (WRITE) — record a completed maintenance task; resets its next-due.
    if (name == QLatin1String("log_maintenance")) {
        if (!tasks) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("maintenance storage unavailable")}});
            return;
        }
        const QString taskKey = input.value(QStringLiteral("taskKey")).toString().trimmed();
        if (taskKey.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("log_maintenance needs a taskKey (from the dueItems maintenance block)")}});
            return;
        }
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(tasks, &TasksStorage::maintenanceLogged, tasks,
            [done, conn, taskKey](const QString& logged) {
                QObject::disconnect(*conn);
                if (!logged.isEmpty())
                    done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("taskKey"), logged}});
                else
                    done(QJsonObject{{QStringLiteral("error"),
                        QStringLiteral("no maintenance task with key: ") + taskKey}});
            });
        tasks->requestLogMaintenance(taskKey, 0);
        return;
    }

    // [barista-fork] update_maintenance_default (WRITE) — approve-then-apply for a Decent-doc-sourced default
    // interval. Guarded to is_default=1 rows (updateMaintenanceDefaultStatic's `AND is_default = 1`); an
    // owner-overridden task is skipped (reported), never clobbered. On a successful apply we ALSO advance the
    // doc-change baseline (markDocReviewedStatic) so the offer isn't re-raised. Runs off the main thread via
    // withTempDb on the assistant.db path (same threading discipline as the shot read tools below).
    if (name == QLatin1String("update_maintenance_default")) {
        if (!tasks || tasks->databasePath().isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("maintenance storage unavailable")}});
            return;
        }
        const QString taskKey = input.value(QStringLiteral("task_key")).toString().trimmed();
        if (taskKey.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("update_maintenance_default needs a task_key (from the maintenanceDocChanged block)")}});
            return;
        }
        if (!input.contains(QStringLiteral("interval_days"))) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("update_maintenance_default needs interval_days")}});
            return;
        }
        const int intervalDays = input.value(QStringLiteral("interval_days")).toVariant().toInt();
        if (intervalDays < 0 || intervalDays > 3650) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("interval_days out of range (0-3650)")}});
            return;
        }
        const QString label = input.value(QStringLiteral("label")).toString().trimmed();
        const QString dbPath = tasks->databasePath();
        QThread* thread = QThread::create([=]() {
            QJsonObject result;
            const bool dbOk = withTempDb(dbPath, "barista_maint_default", [&](QSqlDatabase& db) {
                TasksStorage::ensureSchemaStatic(db);
                const bool applied = TasksStorage::updateMaintenanceDefaultStatic(db, taskKey, intervalDays, label);
                if (applied) {
                    TasksStorage::markDocReviewedStatic(db);   // this change acknowledged → don't re-offer
                    result[QStringLiteral("ok")] = true;
                    result[QStringLiteral("applied")] = true;
                    result[QStringLiteral("taskKey")] = taskKey;
                    result[QStringLiteral("intervalDays")] = intervalDays;
                } else {
                    // is_default=0 (owner override) or unknown key — leave the owner's setting alone.
                    result[QStringLiteral("ok")] = true;
                    result[QStringLiteral("applied")] = false;
                    result[QStringLiteral("skipped")] = QStringLiteral(
                        "task is owner-customised or unknown — left unchanged");
                    result[QStringLiteral("taskKey")] = taskKey;
                }
            });
            if (!dbOk && !result.contains(QStringLiteral("error")))
                result[QStringLiteral("error")] = QStringLiteral("maintenance database unavailable");
            QMetaObject::invokeMethod(qApp, [done, result]() { done(result); }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }

    // [barista-fork] dismiss_maintenance_doc_change (WRITE) — the "no thanks" path. Advances the doc baseline
    // so the same Decent-guide change is never re-offered. Changes no intervals. Off the main thread.
    if (name == QLatin1String("dismiss_maintenance_doc_change")) {
        if (!tasks || tasks->databasePath().isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("maintenance storage unavailable")}});
            return;
        }
        const QString dbPath = tasks->databasePath();
        QThread* thread = QThread::create([=]() {
            QJsonObject result;
            const bool dbOk = withTempDb(dbPath, "barista_maint_dismiss", [&](QSqlDatabase& db) {
                TasksStorage::ensureSchemaStatic(db);
                TasksStorage::markDocReviewedStatic(db);
                result[QStringLiteral("ok")] = true;
                result[QStringLiteral("dismissed")] = true;
            });
            if (!dbOk && !result.contains(QStringLiteral("error")))
                result[QStringLiteral("error")] = QStringLiteral("maintenance database unavailable");
            QMetaObject::invokeMethod(qApp, [done, result]() { done(result); }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }

    // [barista-fork] apply_dial_change (WRITE) — the approve-then-apply seam. Runs on the main thread (this
    // executor is invoked there), so the applyDial handler (BaristaActions::applyFromNext) mutates Settings
    // synchronously and safely. The model has already gotten the user's go-ahead; we range-check and report
    // what actually landed.
    if (name == QLatin1String("apply_dial_change")) {
        if (!applyDial) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("dial actions unavailable")}});
            return;
        }
        // Map the tool input straight onto the applyFromNext contract (same field names/ranges as the fenced
        // structuredNext block). anchorShotId comes from the APP-SIDE snapshot (never the model), so the change
        // enters the same closed loop as a fenced-block apply.
        QVariantMap next;
        for (const char* k : {"grinderSetting", "doseG", "targetWeightG", "ratio", "temperatureC"}) {
            if (input.contains(QLatin1String(k)))
                next.insert(QLatin1String(k), input.value(QLatin1String(k)).toVariant());
        }
        if (next.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("apply_dial_change needs at least one field to change "
                               "(grinderSetting/doseG/targetWeightG/ratio/temperatureC)")}});
            return;
        }
        const qint64 anchorShotId = anchorSnapshot.value(QStringLiteral("shotId")).toLongLong();
        const QVariantMap res = applyDial(next, anchorShotId);
        QJsonObject out;
        if (res.value(QStringLiteral("blocked")).toBool()) {
            out[QStringLiteral("blocked")] = true;
            out[QStringLiteral("reason")] = res.value(QStringLiteral("blockedReason")).toString();
        } else {
            out[QStringLiteral("applied")] = QJsonArray::fromStringList(res.value(QStringLiteral("applied")).toStringList());
            out[QStringLiteral("queued")]  = QJsonArray::fromStringList(res.value(QStringLiteral("queued")).toStringList());
            out[QStringLiteral("rejected")]= QJsonArray::fromStringList(res.value(QStringLiteral("rejected")).toStringList());
            out[QStringLiteral("ok")] = true;
        }
        done(out);
        return;
    }

    // [barista-fork] log_tasting_feedback (WRITE) — Claude fills the light structured schema inline; the
    // executor VALIDATES/WHITELISTS those fields and stamps the shot anchor + dial snapshot APP-SIDE (never
    // from the model), then persists via FeedbackStorage. shot_id 0 = a bean-general note (allowed).
    if (name == QLatin1String("log_tasting_feedback")) {
        if (!feedback) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("feedback storage unavailable")}});
            return;
        }
        const QString rawText = input.value(QStringLiteral("raw_text")).toString().trimmed();
        if (rawText.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("log_tasting_feedback needs raw_text (what the user said about the taste)")}});
            return;
        }

        // Build the structured_json from ONLY the whitelisted axes the model may fill. Unknown keys are dropped.
        QJsonObject structured;
        structured[QStringLiteral("raw_text")] = rawText;
        for (const char* axis : {"acidity", "bitterness", "body", "sweetness", "balance", "milk",
                                 "suggested_adjustment"}) {
            const QString v = input.value(QLatin1String(axis)).toString().trimmed();
            if (!v.isEmpty())
                structured[QLatin1String(axis)] = v;
        }
        // descriptors[]: keep as an array in structured_json AND flatten to a space-joined string for FTS.
        QStringList descriptorList;
        for (const QJsonValue& d : input.value(QStringLiteral("descriptors")).toArray()) {
            const QString s = d.toString().trimmed();
            if (!s.isEmpty())
                descriptorList << s;
        }
        if (!descriptorList.isEmpty())
            structured[QStringLiteral("descriptors")] = QJsonArray::fromStringList(descriptorList);
        bool hasRating = false;
        int rating = 0;
        if (input.contains(QStringLiteral("overall_rating_0to100"))) {
            rating = input.value(QStringLiteral("overall_rating_0to100")).toInt();
            if (rating > 0 && rating <= 100) {
                hasRating = true;
                structured[QStringLiteral("overall_rating_0to100")] = rating;
            }
        }

        // Field map for FeedbackStorage: provenance (bean/profile/dial/shotId) from the APP-SIDE snapshot,
        // taste content from the (validated) model input. anchorSnapshot keys mirror the column keys.
        QVariantMap fields;
        for (const char* k : {"shotId", "beanBrand", "beanType", "profile", "doseG", "yieldG",
                              "grind", "tempC", "source"}) {
            if (anchorSnapshot.contains(QLatin1String(k)))
                fields.insert(QLatin1String(k), anchorSnapshot.value(QLatin1String(k)));
        }
        fields.insert(QStringLiteral("rawText"), rawText);
        fields.insert(QStringLiteral("descriptors"), descriptorList.join(QStringLiteral(" ")));
        fields.insert(QStringLiteral("structuredJson"),
                      QString::fromUtf8(QJsonDocument(structured).toJson(QJsonDocument::Compact)));
        if (hasRating)
            fields.insert(QStringLiteral("rating0to100"), rating);
        if (!fields.contains(QStringLiteral("source")))
            fields.insert(QStringLiteral("source"), QStringLiteral("volunteered"));

        // [barista-fork] LAND IT ON THE SHOT RECORD TOO. A spoken rating/taste must fully replace the post-shot
        // tap buttons, which write enjoyment0to100 + a "Tasted <choice>" marker onto the SHOT (shots.db) — not
        // just the feedback KB. When we have a REAL anchored shot (shotId > 0, stamped app-side; 0 = a
        // bean-general note with no shot to attach to) AND the user gave a rating and/or a clear taste axis,
        // mirror that write via requestUpdateShotMetadata. enjoyment: an explicit overall_rating_0to100 wins;
        // else the tap-button fallback for the derived taste choice. notes: strip any prior "Tasted " marker and
        // append the new one, preserving the user's own typed notes (never a full-column clobber). This keeps
        // the barista's shot write byte-identical to the tap path so PostShotReviewPage round-trips the selection.
        const qint64 anchorShotId = anchorSnapshot.value(QStringLiteral("shotId")).toLongLong();
        const QString tasteChoice = baristaTasteChoice(input);
        if (shotHistory && anchorShotId > 0 && (hasRating || !tasteChoice.isEmpty())) {
            const int shotEnjoyment = hasRating ? rating : baristaEnjoymentForTaste(tasteChoice);
            // Route through requestApplyTasteToShot: it does a LIVE read-modify-write of espresso_notes on the
            // serialized DB thread, so a spoken rating can NEVER clobber notes the user typed after the barista
            // opened (a stale session-snapshot merge could). enjoyment applied only when > 0.
            shotHistory->requestApplyTasteToShot(anchorShotId, shotEnjoyment, shotEnjoyment > 0, tasteChoice);
        }

        // FeedbackStorage::requestLogFeedback is async (its own worker); fire it, and confirm to the model
        // once the row lands. The connection uses `feedback` as its context object, so it self-disconnects if
        // storage is destroyed; single-shot so the lambda disconnects itself on the first emission.
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(feedback, &FeedbackStorage::feedbackLogged, feedback,
            [done, conn](qint64 id) {
                QObject::disconnect(*conn);
                if (id > 0)
                    done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("feedbackId"), id}});
                else
                    done(QJsonObject{{QStringLiteral("error"), QStringLiteral("failed to save feedback")}});
            });
        feedback->requestLogFeedback(fields);
        return;
    }

    // [barista-fork] search_tasting_feedback (READ) — filtered/ad-hoc lookup of past feedback for a bean.
    if (name == QLatin1String("search_tasting_feedback")) {
        if (!feedback) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("feedback storage unavailable")}});
            return;
        }
        const QString beanBrand = input.value(QStringLiteral("bean_brand")).toString().trimmed();
        const QString beanType  = input.value(QStringLiteral("bean_type")).toString().trimmed();
        if (beanBrand.isEmpty() && beanType.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("search_tasting_feedback needs a bean_brand and/or bean_type")}});
            return;
        }
        const QString descriptorFilter = input.value(QStringLiteral("descriptor_filter")).toString().trimmed();

        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(feedback, &FeedbackStorage::feedbackForBeanReady, feedback,
            [done, conn](const QVariantList& rows) {
                QObject::disconnect(*conn);
                QJsonArray arr;
                for (const QVariant& r : rows) {
                    const QVariantMap m = r.toMap();
                    QJsonObject o;
                    o[QStringLiteral("shotId")]  = m.value(QStringLiteral("shotId")).toLongLong();
                    o[QStringLiteral("date")]    = QDateTime::fromSecsSinceEpoch(
                        m.value(QStringLiteral("createdAt")).toLongLong())
                        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                    const QString profile = m.value(QStringLiteral("profile")).toString();
                    if (!profile.isEmpty()) o[QStringLiteral("profile")] = profile;
                    if (const double dose  = m.value(QStringLiteral("doseG")).toDouble();  dose  > 0)
                        o[QStringLiteral("doseG")]  = QString::number(dose,  'f', 1).toDouble();
                    if (const double yield = m.value(QStringLiteral("yieldG")).toDouble(); yield > 0)
                        o[QStringLiteral("yieldG")] = QString::number(yield, 'f', 1).toDouble();
                    if (const QString grind = m.value(QStringLiteral("grind")).toString(); !grind.isEmpty())
                        o[QStringLiteral("grind")] = grind;
                    if (const double temp = m.value(QStringLiteral("tempC")).toDouble(); temp > 0)
                        o[QStringLiteral("temperatureC")] = QString::number(temp, 'f', 1).toDouble();
                    if (const int rating = m.value(QStringLiteral("rating0to100")).toInt(); rating > 0)
                        o[QStringLiteral("rating0to100")] = rating;
                    const QString rawText = m.value(QStringLiteral("rawText")).toString();
                    if (!rawText.isEmpty()) o[QStringLiteral("saidAboutTaste")] = rawText;
                    const QString descriptors = m.value(QStringLiteral("descriptors")).toString();
                    if (!descriptors.isEmpty()) o[QStringLiteral("descriptors")] = descriptors;
                    // structured_json holds the parsed axes + suggested_adjustment.
                    const QJsonObject sj = QJsonDocument::fromJson(
                        m.value(QStringLiteral("structuredJson")).toString().toUtf8()).object();
                    if (sj.contains(QStringLiteral("suggested_adjustment")))
                        o[QStringLiteral("suggestedAdjustment")] = sj.value(QStringLiteral("suggested_adjustment"));
                    arr.append(o);
                }
                done(QJsonObject{{QStringLiteral("returnedCount"), arr.size()},
                                 {QStringLiteral("feedback"), arr}});
            });
        feedback->requestFeedbackForBean(beanBrand, beanType, descriptorFilter);
        return;
    }

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
