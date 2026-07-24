#include "baristatools.h"

#include "../history/shothistorystorage.h"
#include "../history/shotprojection.h"
#include "../ai/shotsummarizer.h"
#include "../core/dbutils.h"
#include "../core/drinktypes.h"  // [barista-fork] natural shot descriptor (type + beans), not a stat dump
#include "feedbackstorage.h"
#include "tasksstorage.h"
#include "baristadiagnostics.h"  // [barista-fork] tool-call timeline recorder
#include "../history/recipestorage.h"  // [barista-fork] Recipes 2.0 tools (list_recipes / activate / etc.)
#include "../core/yieldspec.h"          // [barista-fork] update_recipe yield-anchor (ratio clamp / mode)
#include "../history/baristastorage.h"  // [barista-fork] Phase 1 identity: roster for set_active_user

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

namespace {
// [barista-fork] Levenshtein edit distance (small strings — names). Used to fold a misheard-name variant onto
// an existing roster entry so set_active_user doesn't create "Ana"/"Anna" duplicates.
int editDistance(const QString& a, const QString& b) {
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    QVector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// Return the canonical roster name matching `name`, or empty. Exact (case-insensitive) first; then a GUARDED
// near-match (edit distance ≤ 1) only when the longer name is ≥ 4 chars — so "Ana"≈"Anna", "Jon"≈"John",
// "Chris"≈"Kris" fold together, but genuinely distinct short names ("Sam" vs "Pam") never false-match.
QString matchRosterName(const QVector<Barista>& roster, const QString& name) {
    const QString n = name.trimmed();
    if (n.isEmpty()) return QString();
    for (const Barista& b : roster)
        if (b.name.compare(n, Qt::CaseInsensitive) == 0) return b.name;
    for (const Barista& b : roster) {
        if (qMax(b.name.size(), n.size()) < 4) continue;
        if (editDistance(b.name.toLower(), n.toLower()) <= 1) return b.name;
    }
    return QString();
}
}  // namespace

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
        "invent numbers or descriptors. By default the shot it attaches to and the dial "
        "(dose/yield/grind/temp/bean/profile) are recorded automatically — you normally do NOT supply a shot id. "
        "EXCEPTION: to rate or adjust a SPECIFIC PAST shot (\"update my rating for this morning's shot\", \"that "
        "one from last Tuesday was actually bitter\"), first find that shot with query_shots, then pass its "
        "shotId here as shot_id — the rating/taste then lands on THAT shot instead of the just-pulled one.");
    QJsonObject lfSchema;
    lfSchema["type"] = QString("object");
    QJsonObject lfProps;
    lfProps["raw_text"]              = strProp("What the user actually said about the taste/texture, in their own words (required).");
    lfProps["shot_id"]               = intProp("OPTIONAL — the shotId of a SPECIFIC past shot to rate/adjust (from a query_shots result). Omit for the current/just-pulled shot; include ONLY to rate a different, already-recorded shot.");
    lfProps["overall_rating_0to100"]= intProp("How much they liked it, 0-100, ONLY if they gave a clear sense of it (optional).");
    lfProps["acidity"]              = strProp("One of: sour, bright, balanced, flat (optional).");
    lfProps["bitterness"]           = strProp("One of: none, mild, harsh (optional).");
    lfProps["body"]                 = strProp("The shot's body/mouthfeel — one of: thin, medium, heavy (optional). Recorded as the shot's structured taste_body, same as the post-shot taste picker.");
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
    // ("yes" / "do it" / "go ahead") does it call this. ALL fields — dose, yield, temperature, AND grinder —
    // are set immediately (owner decision 2026-07-13: grind is direct-set, no off-machine queue/confirm-later).
    // The NUMERIC fields (dose/yield/ratio/temp) are range-checked (a hallucinated 200 °C never reaches the
    // machine); the grinder setting is grinder-specific free text, so it is applied as given (same as before —
    // the retired queue didn't validate it either). The executor reports applied/rejected so the barista confirms accurately.
    QJsonObject ad;
    ad["name"] = QString("apply_dial_change");
    ad["description"] = QString(
        "Apply an agreed change to the NEXT shot's dial-in. Call this ONLY after you have proposed the change "
        "and the user has clearly approved it (\"yes\", \"do it\", \"go ahead\") — NEVER unprompted, and never "
        "just to acknowledge or to restate unchanged settings. Include ONLY the field(s) that actually change. "
        "ALL fields including the grinder setting are applied right away — do NOT tell the user a grind is merely "
        "'queued' or ask them to set it later; it is set now (they still physically dial their grinder, but the "
        "app has recorded it). The result tells you exactly what was applied or rejected (out of range) — confirm "
        "to the user from that, naturally (\"Done — grind's at 4.4 for the next one\").");
    QJsonObject adSchema;
    adSchema["type"] = QString("object");
    QJsonObject adProps;
    adProps["grinderSetting"] = strProp("New grinder dial setting — applied immediately (the user still physically dials it). Optional.");
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

    // [barista-fork] remember_fact (WRITE) — durable BASIC facts the user tells the barista about themselves or
    // their world, so continuity carries across sessions. Curated, NOT a transcript: the model decides what is a
    // keep-worthy fact. Facts already known are injected each turn as [knownFacts], so the barista shouldn't
    // re-ask them. Scoped to the active user by the executor (never trusted from the model).
    QJsonObject rf;
    rf["name"] = QString("remember_fact");
    rf["description"] = QString(
        "Save a DURABLE BASIC FACT the user tells you about themselves or their world so you remember it in future "
        "conversations — names and relationships (family, friends, pets), preferences, where they live or work, "
        "allergies, an ongoing project or goal. Store ONLY facts the user is clearly telling YOU as part of THIS "
        "conversation. Do NOT store: a transcript or paraphrase of the discussion, small talk or passing/ephemeral "
        "remarks, anything you merely overheard that wasn't addressed to you, or coffee dial-in numbers and shot "
        "ratings (those have their own tools). One fact per call. Phrase it as a short self-contained statement "
        "(\"Oldest daughter Audrey attends University of Washington\", \"Prefers lighter roasts\", \"Has a dog "
        "named Max\"). If the user corrects something, call forget_fact for the old version then remember_fact for "
        "the new. Acknowledge briefly once it's saved (\"Got it — I'll remember that\"). You already receive the "
        "facts you know in your context, so don't re-ask what you've been told.");
    QJsonObject rfSchema;
    rfSchema["type"] = QString("object");
    QJsonObject rfProps;
    rfProps["fact"] = strProp("The fact to remember, as a short self-contained statement (required).");
    rfProps["category"] = strProp("Optional one-word grouping, e.g. \"family\", \"preference\", \"work\", \"pet\", \"home\".");
    rfSchema["properties"] = rfProps;
    rfSchema["required"] = QJsonArray{ QString("fact") };
    rf["input_schema"] = rfSchema;
    tools.append(rf);

    // [barista-fork] forget_fact (WRITE) — the correction/removal path for remember_fact.
    QJsonObject ff;
    ff["name"] = QString("forget_fact");
    ff["description"] = QString(
        "Remove a fact you previously remembered — a correction, or something that's no longer true. Pass a few "
        "words that identify the fact (matched within your stored facts). Confirm briefly what you dropped.");
    QJsonObject ffSchema;
    ffSchema["type"] = QString("object");
    QJsonObject ffProps;
    ffProps["fact"] = strProp("Words identifying the fact to forget, e.g. \"dog named Max\" (required).");
    ffSchema["properties"] = ffProps;
    ffSchema["required"] = QJsonArray{ QString("fact") };
    ff["input_schema"] = ffSchema;
    tools.append(ff);

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

    // [barista-fork] rate_shot (WRITE) is folded into log_tasting_feedback via an OPTIONAL shot_id field:
    // when the user asks to rate/adjust a SPECIFIC past shot ("update my rating for this morning's shot"),
    // the model first finds it with query_shots (whose result carries a shotId) and passes that shotId here.
    // When shot_id is present + valid it OVERRIDES the app-side anchor for the shot-record write, so the user
    // can rate any past shot by voice — not just the just-pulled one. See the executor + the schema field above.

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

    // [barista-fork] Recipes 2.0 — the barista can now know/discuss/use the user's whole-drink recipes.
    // Descriptions carry policy (read every turn) and mirror Fable's design spec §2.
    QJsonObject rlst;
    rlst["name"] = QString("list_recipes");
    rlst["description"] = QString(
        "List the user's saved recipes. A recipe is a whole-drink preset (profile + bean + grind + "
        "dose/yield/temp + optional steam and hot-water blocks) that configures the machine in one step — not a "
        "dial-in tweak. Returns recipes sorted most-recently-used first with: id, name, drink_type, bean (roaster "
        "+ coffee display), has_milk, stale (linked bag no longer in inventory — informational, never a blocker), "
        "shot_count, last_used. Read-only. Use when the user asks what recipes they have, or when you cannot "
        "confidently resolve a spoken recipe reference against the recipe context you were given.");
    QJsonObject rlstSchema;
    rlstSchema["type"] = QString("object");
    QJsonObject rlstProps;
    rlstProps["query"] = strProp("Optional case-insensitive substring matched against recipe name, roaster name, "
                                 "and coffee name. Omit to list all.");
    QJsonObject rlstLimit; rlstLimit["type"] = QString("integer");
    rlstLimit["description"] = QString("Max recipes to return (default 20, max 50).");
    rlstProps["limit"] = rlstLimit;
    rlstSchema["properties"] = rlstProps;
    rlst["input_schema"] = rlstSchema;
    tools.append(rlst);

    QJsonObject rget;
    rget["name"] = QString("get_active_recipe");
    rget["description"] = QString(
        "Return the currently active recipe as a full object (including steam and hot-water blocks), or "
        "{active: false}. Read-only and instant. The recipe context you were given already includes the active "
        "recipe's name — call this only when you need detail: e.g., its steam settings before proposing a switch, "
        "or to answer specific questions about the active drink.");
    QJsonObject rgetSchema;
    rgetSchema["type"] = QString("object");
    rgetSchema["properties"] = QJsonObject{};
    rget["input_schema"] = rgetSchema;
    tools.append(rget);

    QJsonObject ract;
    ract["name"] = QString("activate_recipe");
    ract["description"] = QString(
        "Activate a recipe on the machine. HIGH-IMPACT MACHINE CHANGE: this loads the recipe's profile (replacing "
        "whatever profile is loaded and discarding any unsaved dial-in overrides), rewrites dose, yield, and "
        "temperature, re-routes grind, and — if the recipe includes milk — turns on the steam heater, which then "
        "stays hot for several minutes. NEVER call this tool until the user has explicitly approved, in this "
        "conversation, after you told them which recipe and what will change. Approval for one activation is "
        "approval for that activation only. The result is the ground truth: success=false means the machine was "
        "NOT changed — report the reason honestly and never describe an activation as done unless success=true.");
    QJsonObject ractSchema;
    ractSchema["type"] = QString("object");
    QJsonObject ractProps;
    ractProps["recipe_id"] = intProp("The id of the recipe, taken from the recipe context, list_recipes, or "
                                     "get_active_recipe. Never invent or guess an id.");
    ractSchema["properties"] = ractProps;
    ractSchema["required"] = QJsonArray{ QString("recipe_id") };
    ract["input_schema"] = ractSchema;
    tools.append(ract);

    // [barista-fork] update_recipe — edit a SAVED recipe's fields (does not touch the machine; no activation).
    // Approve-then-apply lives in the model (like activate_recipe): confirm the exact change with the user first.
    const auto numProp = [](const QString& d){ QJsonObject o; o["type"] = QString("number"); o["description"] = d; return o; };
    QJsonObject rupd;
    rupd["name"] = QString("update_recipe");
    rupd["description"] = QString(
        "Change the saved settings of an existing recipe (its stored design — this does NOT activate it or change "
        "the machine). Use it when the user wants to tweak a recipe: dose, grind, temperature, name, yield, "
        "PROFILE, or drink type. "
        "Resolve recipe_id first from get_active_recipe / list_recipes — never invent an id. Send ONLY the fields "
        "you are changing. Yield is ONE anchor: send EITHER yield_g (a fixed gram target) OR ratio (a multiple of "
        "the dose, e.g. 2.0 = 1:2, so the grams follow the weighed dose) — never both, and sending one replaces "
        "the other. Confirm the SPECIFIC change with the user BEFORE calling (approve-then-apply); approval is for "
        "that one edit only. The result is ground truth: report updated:true honestly, never claim a save that "
        "did not return updated:true.");
    QJsonObject rupdSchema;
    rupdSchema["type"] = QString("object");
    QJsonObject rupdProps;
    rupdProps["recipe_id"]  = intProp("The id of the recipe to change (from get_active_recipe or list_recipes). "
                                      "Never invent or guess an id.");
    rupdProps["dose_g"]     = numProp("New dose in grams.");
    rupdProps["grind_setting"] = strProp("New grinder setting (the recipe's own grind); '' clears it.");
    rupdProps["rpm"]        = intProp("New grinder RPM, if the grinder is RPM-based.");
    rupdProps["temperature_offset_c"] = numProp("Signed temperature DELTA in Celsius against the recipe's profile "
                                      "(e.g. -1 for 1C cooler); 0 clears it. This is an OFFSET, not an absolute.");
    rupdProps["title"]      = strProp("New name/title for the recipe.");
    rupdProps["yield_g"]    = numProp("Absolute yield target in grams. Mutually exclusive with ratio; 0 clears "
                                      "the yield. Sending this replaces any stored ratio.");
    rupdProps["ratio"]      = numProp("Yield as a multiple of the dose (2.0 = 1:2; clamped 0.5-6.0) so the gram "
                                      "target follows the weighed dose. Mutually exclusive with yield_g; 0 clears "
                                      "the yield. Sending this replaces any stored absolute yield.");
    rupdProps["profile_title"] = strProp("Change the recipe's PROFILE to this exact profile name (the pressure/flow "
                                      "curve the machine runs). It MUST be a profile the app has — if unsure of the "
                                      "exact title, call list_profiles first and pass the exact title it returns; a "
                                      "name that doesn't match is rejected. "
                                      "The recipe's temperature_offset_c is relative to the profile, so a big profile "
                                      "change may want a fresh temp — mention that. Does NOT reactivate the machine.");
    rupdProps["drink_type"] = strProp("Change the recipe's drink type: one of espresso, filter, americano, "
                                      "long_black, latte, latte_hotwater (a latte with an added hot-water shot), "
                                      "tea, tea_hotwater. Usually you can leave this to derive from the milk/water blocks.");
    rupdProps["has_milk"] = QJsonObject{{"type", QString("boolean")},
        {"description", QString("Turn the recipe's milk/steam on (true) or off (false). Setting any milk field "
                                "REPLACES the whole steam block, so it drops a saved pitcher preset — for a latte "
                                "recipe.")}};
    rupdProps["milk_weight_g"] = numProp("Target milk weight in grams (implies has_milk).");
    rupdProps["steam_temperature_c"] = numProp("Steam/milk target temperature in Celsius (optional).");
    rupdProps["has_water"] = QJsonObject{{"type", QString("boolean")},
        {"description", QString("Turn the recipe's hot-water block on (true) or off (false) — for americano/long "
                                "black. Setting any water field REPLACES the whole water block.")}};
    rupdProps["water_volume_ml"] = numProp("Hot-water volume in mL (implies has_water).");
    rupdProps["water_temperature_c"] = numProp("Hot-water temperature in Celsius (optional).");
    rupdProps["water_order"] = strProp("'before' the shot (long black) or 'after' (americano). Optional.");
    rupdSchema["properties"] = rupdProps;
    rupdSchema["required"] = QJsonArray{ QString("recipe_id") };
    rupd["input_schema"] = rupdSchema;
    tools.append(rupd);

    // [barista-fork] create_recipe — make a NEW saved recipe (its stored design; does NOT activate the machine).
    QJsonObject rnew;
    rnew["name"] = QString("create_recipe");
    rnew["description"] = QString(
        "Create a NEW saved recipe — a whole-drink preset. This ONLY saves the design; it does NOT activate it or "
        "change the machine (tell the user to activate it when they want to use it). Requires a name and a "
        "profile_title (a profile the app has — if unsure of the exact title, call list_profiles first and pass "
        "the exact title; an unknown or missing profile is rejected). For 'same beans as X, different profile', set copy_beans_from_active "
        "true (or pass roaster_name/coffee_name explicitly) so the new recipe carries the beans. Send the dial "
        "fields you know (dose, yield OR ratio, temp offset, grind); omit what you don't. Confirm the details with "
        "the user BEFORE calling (approve-then-apply). The result is ground truth: report created:true + the new "
        "recipe_id only when it returns that.");
    QJsonObject rnewSchema;
    rnewSchema["type"] = QString("object");
    QJsonObject rnewProps;
    rnewProps["name"]          = strProp("Name for the new recipe (required; must be unique among active recipes).");
    rnewProps["profile_title"] = strProp("The exact installed profile name to run. Required unless the drink is "
                                         "hot-water-only. A name that doesn't match an installed profile is rejected.");
    rnewProps["drink_type"]    = strProp("espresso, filter, americano, long_black, latte, latte_hotwater (latte + "
                                         "hot-water shot), tea, or tea_hotwater. Omit to let the app derive it from "
                                         "the milk/water blocks.");
    rnewProps["copy_beans_from_active"] = QJsonObject{{"type", QString("boolean")},
        {"description", QString("When true, inherit the beans (roaster + coffee) from the currently active recipe — "
                                "use this for 'same beans, different profile'. Ignored if roaster_name/coffee_name given.")}};
    rnewProps["roaster_name"]  = strProp("Roaster name for the beans (optional; omit to inherit via copy_beans_from_active).");
    rnewProps["coffee_name"]   = strProp("Coffee/bean name (optional; omit to inherit via copy_beans_from_active).");
    rnewProps["dose_g"]        = numProp("Dose in grams (optional).");
    rnewProps["grind_setting"] = strProp("The recipe's own grinder setting (optional).");
    rnewProps["rpm"]           = intProp("Grinder RPM, if RPM-based (optional; rides with grind_setting).");
    rnewProps["temperature_offset_c"] = numProp("Signed temp DELTA in Celsius vs the profile (optional; 0 = profile temp).");
    rnewProps["yield_g"]       = numProp("Absolute yield target in grams (optional). Mutually exclusive with ratio.");
    rnewProps["ratio"]         = numProp("Yield as a multiple of the dose, e.g. 2.0 = 1:2 (optional; clamped 0.5-6.0). "
                                         "Mutually exclusive with yield_g.");
    rnewProps["has_milk"] = QJsonObject{{"type", QString("boolean")},
        {"description", QString("true adds a milk/steam block (a latte-style drink).")}};
    rnewProps["milk_weight_g"] = numProp("Target milk weight in grams (implies has_milk).");
    rnewProps["steam_temperature_c"] = numProp("Steam/milk target temperature in Celsius (optional).");
    rnewProps["has_water"] = QJsonObject{{"type", QString("boolean")},
        {"description", QString("true adds a hot-water block (americano / long black).")}};
    rnewProps["water_volume_ml"] = numProp("Hot-water volume in mL (implies has_water).");
    rnewProps["water_temperature_c"] = numProp("Hot-water temperature in Celsius (optional).");
    rnewProps["water_order"] = strProp("'before' the shot (long black) or 'after' (americano). Optional.");
    rnewSchema["properties"] = rnewProps;
    rnewSchema["required"] = QJsonArray{ QString("name") };
    rnew["input_schema"] = rnewSchema;
    tools.append(rnew);

    // [barista-fork] clone_recipe — duplicate an existing recipe under a new name (a starting point to tweak).
    QJsonObject rclone;
    rclone["name"] = QString("clone_recipe");
    rclone["description"] = QString(
        "Duplicate an existing recipe under a NEW name — copies all its settings (profile, beans, dial, steam/"
        "water) so the user can tweak the copy without touching the original. Does NOT activate anything. Resolve "
        "recipe_id first. Confirm the new name with the user before calling. Reports created:true + the new recipe_id.");
    QJsonObject rcloneSchema;
    rcloneSchema["type"] = QString("object");
    QJsonObject rcloneProps;
    rcloneProps["recipe_id"] = intProp("The id of the recipe to copy (from list_recipes / get_active_recipe).");
    rcloneProps["new_name"]  = strProp("Name for the new copy (must be unique among active recipes).");
    rcloneSchema["properties"] = rcloneProps;
    rcloneSchema["required"] = QJsonArray{ QString("recipe_id"), QString("new_name") };
    rclone["input_schema"] = rcloneSchema;
    tools.append(rclone);

    // [barista-fork] archive_recipe — hide/unhide a recipe from the main inventory (reversible; keeps history).
    QJsonObject rarch;
    rarch["name"] = QString("archive_recipe");
    rarch["description"] = QString(
        "Archive a recipe (hide it from the main recipe list) or, with archived=false, restore it. Reversible and "
        "keeps all shot history — use this instead of deleting when a recipe has shots or the user just wants it "
        "out of the way. Resolve recipe_id first. Reports updated:true.");
    QJsonObject rarchSchema;
    rarchSchema["type"] = QString("object");
    QJsonObject rarchProps;
    rarchProps["recipe_id"] = intProp("The id of the recipe to archive/restore.");
    rarchProps["archived"]  = QJsonObject{{"type", QString("boolean")},
        {"description", QString("true (default) archives; false restores an archived recipe.")}};
    rarchSchema["properties"] = rarchProps;
    rarchSchema["required"] = QJsonArray{ QString("recipe_id") };
    rarch["input_schema"] = rarchSchema;
    tools.append(rarch);

    // [barista-fork] delete_recipe — permanently remove a recipe (ONLY if it has no shot history).
    QJsonObject rdel;
    rdel["name"] = QString("delete_recipe");
    rdel["description"] = QString(
        "Permanently delete a recipe. This ONLY works when the recipe has NO shots recorded against it (a mistaken "
        "creation); if it has history the delete is refused — archive it instead. Destructive and irreversible, so "
        "confirm clearly with the user before calling. Resolve recipe_id first. Reports deleted:true, or "
        "deleted:false with a reason (e.g. has history) — report that honestly and never claim a delete that "
        "didn't return deleted:true.");
    QJsonObject rdelSchema;
    rdelSchema["type"] = QString("object");
    QJsonObject rdelProps;
    rdelProps["recipe_id"] = intProp("The id of the recipe to delete.");
    rdelSchema["properties"] = rdelProps;
    rdelSchema["required"] = QJsonArray{ QString("recipe_id") };
    rdel["input_schema"] = rdelSchema;
    tools.append(rdel);

    // [barista-fork] list_profiles — see every profile the app can use, and resolve a spoken profile name to its
    // EXACT title for the recipe tools.
    QJsonObject rlp;
    rlp["name"] = QString("list_profiles");
    rlp["description"] = QString(
        "List the PROFILES the app can use (a profile is the pressure/flow curve the machine runs — e.g. a lever "
        "emulation, a filter profile, a flow profile). Use this to (a) tell the user what profiles are available, "
        "and (b) find the EXACT title to pass to create_recipe / update_recipe / activate. Spoken profile names "
        "are often close but not exact (e.g. the user says 'Londinium' but the stored title is 'Londonium'), so "
        "when the user names a profile, look it up here and use the exact title returned — do NOT guess. Pass a "
        "query to search by a title substring. Read-only. Returns count + [{title, editor, drink}].");
    QJsonObject rlpSchema;
    rlpSchema["type"] = QString("object");
    QJsonObject rlpProps;
    rlpProps["query"] = strProp("Optional case-insensitive substring to filter profile titles by "
                                "(e.g. 'lond', 'lever', 'filter', 'flow'). Omit to list them all.");
    rlpSchema["properties"] = rlpProps;
    rlp["input_schema"] = rlpSchema;
    tools.append(rlp);

    QJsonObject rdeact;
    rdeact["name"] = QString("deactivate_recipe");
    rdeact["description"] = QString(
        "Deactivate the currently active recipe. This unlinks the recipe only — the machine keeps its current "
        "profile, dose, yield, and temperature; nothing physical changes and the steam heater is not touched. "
        "Safe to call on a direct user request ('turn off the recipe', 'go freestyle') without a confirmation "
        "exchange. Returns the name of the recipe that was deactivated, or {was_active: false}.");
    QJsonObject rdeactSchema;
    rdeactSchema["type"] = QString("object");
    rdeactSchema["properties"] = QJsonObject{};
    rdeact["input_schema"] = rdeactSchema;
    tools.append(rdeact);

    // [barista-fork] set_active_user — Phase 1 identity: switch who the barista is talking to when they say
    // who they are. The model confirms a NEW name before calling (STT mishears names); a name matching the
    // [Who] roster just switches. Sets the roster active user (dyeBarista), which scopes shot attribution.
    QJsonObject sau;
    sau["name"] = QString("set_active_user");
    sau["description"] = QString(
        "Set who you're currently talking to when they identify themselves by name ('I'm Chris', 'this is Ana', "
        "'Ana's making this one', 'switch to Scott'). Pass their name. Speech can mishear names, so only call this "
        "AFTER you've confirmed a NEW name with them; if the name clearly matches someone you already know (see "
        "the [Who] block), just switch. Switching makes that person the active user.");
    QJsonObject sauSchema;
    sauSchema["type"] = QString("object");
    QJsonObject sauProps;
    sauProps["name"] = strProp("The person's name to switch to, e.g. \"Ana\" or \"Chris\".");
    sauSchema["properties"] = sauProps;
    sauSchema["required"] = QJsonArray{ QString("name") };
    sau["input_schema"] = sauSchema;
    tools.append(sau);

    return tools;
}

// [barista-fork] FAST-PATH web tool definitions (get_weather / get_stock_quote / get_local_news). SEPARATE from
// toolDefinitions() so they can be gated on RequestOptions.webSearch (the umbrella "may reach the internet"
// toggle) rather than the clientTools/query_shots gate. Each maps to one keyless HTTP GET (see BaristaWebTools),
// so the model answers weather / stock / local-news questions in ~1s instead of the ~10s web-search round-trip.
QJsonArray BaristaTools::webToolDefinitions()
{
    QJsonArray tools;
    const auto strProp = [](const QString& d){ QJsonObject o; o["type"] = QString("string"); o["description"] = d; return o; };

    // get_weather — current conditions for a city (open-meteo, keyless).
    QJsonObject gw;
    gw["name"] = QString("get_weather");
    gw["description"] = QString(
        "Get current weather AND a multi-day forecast for a city — fast. Use this INSTEAD of web search for ANY "
        "weather question, including forecasts: current (\"what's the weather in Bellevue\", \"is it raining\", "
        "\"how hot is it\") AND upcoming days (\"what's the forecast\", \"weather for the next few days\", \"how's "
        "the weekend looking\", \"will it rain tomorrow\"). NEVER web-search the weather — this tool already "
        "returns a 4-day forecast, so web results would only be staler. Extract the city and pass it as location. "
        "If they mean HERE / the local area and name no city, omit location and it falls back to their saved home "
        "location; if there's no home location either it will tell you to ask which city. Returns current "
        "temperature, feels-like, condition, wind, humidity, AND a `forecast` array of upcoming days (date, "
        "high/low, condition, precip chance) — answer briefly from that.");
    QJsonObject gwSchema;
    gwSchema["type"] = QString("object");
    QJsonObject gwProps;
    gwProps["location"] = strProp("City to get the weather for, e.g. \"Bellevue\" or \"Paris, France\". Omit for the user's home location (local/around here).");
    gwSchema["properties"] = gwProps;
    gw["input_schema"] = gwSchema;
    tools.append(gw);

    // get_stock_quote — latest price for a ticker (Yahoo Finance, keyless).
    QJsonObject gs;
    gs["name"] = QString("get_stock_quote");
    gs["description"] = QString(
        "Get the latest price for a stock/ETF ticker — fast. Use this INSTEAD of web search whenever the user "
        "asks about a stock price (\"how's Apple doing\", \"what's Tesla at\", \"price of NVDA\"). YOU supply the "
        "ticker symbol (you know AAPL/TSLA/NVDA/etc.); if the user gives a company name, map it to its symbol. "
        "Returns price, change, percent change, and currency — answer briefly from that.");
    QJsonObject gsSchema;
    gsSchema["type"] = QString("object");
    QJsonObject gsProps;
    gsProps["symbol"] = strProp("The ticker symbol to quote, e.g. \"AAPL\" or \"TSLA\" (uppercase). Map a company name to its symbol yourself.");
    gsSchema["properties"] = gsProps;
    gsSchema["required"] = QJsonArray{ QString("symbol") };
    gs["input_schema"] = gsSchema;
    tools.append(gs);

    // get_local_news — recent headlines (Google News RSS, keyless).
    QJsonObject gn;
    gn["name"] = QString("get_local_news");
    gn["description"] = QString(
        "Get recent news headlines — fast. Use this INSTEAD of web search whenever the user asks about the news "
        "or current events (\"what's in the news\", \"any news about the election\", \"local news\"). Pass a "
        "topic in `topic` for a subject search, or a city in `location` for local headlines; for \"news around "
        "here\" with no city, omit both and it uses the user's home location (or tells you to ask). Returns the "
        "top ~5 headlines — summarize a couple briefly.");
    QJsonObject gnSchema;
    gnSchema["type"] = QString("object");
    QJsonObject gnProps;
    gnProps["topic"]    = strProp("A subject/topic to search news for, e.g. \"AI regulation\" or \"Seattle Mariners\" (optional).");
    gnProps["location"] = strProp("A city for LOCAL headlines, e.g. \"Bellevue\" (optional). Omit both topic and location for the user's home-location local news.");
    gnSchema["properties"] = gnProps;
    gn["input_schema"] = gnSchema;
    tools.append(gn);

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

// [barista-fork] Map the model's `body` axis to the canonical structured taste_body value ("thin"|"medium"|
// "heavy") the tap-picker persists (PostShotReviewPage TastePicker). MUST return a canonical value or "" —
// never a pass-through: updateShotMetadataStatic SILENTLY DROPS an out-of-set taste_body, so returning the raw
// word would no-op the write. "syrupy"/"full"/"thick" (a heavy mouthfeel) → "heavy"; "light"/"watery" → "thin";
// the three canonical words pass straight through. Anything else (incl. empty) → "" = no body write, so a shot's
// existing taste_body is preserved. This is the ONLY taste axis the barista's note marker never captured.
static QString baristaTasteBody(const QJsonObject& input)
{
    const QString body = input.value(QStringLiteral("body")).toString().trimmed().toLower();
    if (body == QLatin1String("thin") || body == QLatin1String("light") || body == QLatin1String("watery"))
        return QStringLiteral("thin");
    if (body == QLatin1String("medium"))
        return QStringLiteral("medium");
    if (body == QLatin1String("heavy") || body == QLatin1String("syrupy")
        || body == QLatin1String("full") || body == QLatin1String("thick"))
        return QStringLiteral("heavy");
    return QString();   // unset / unrecognised → no structured body write (preserve any existing value)
}

// [barista-fork] The "Tasted <choice>" note-merge now lives in ShotHistoryStorage::requestApplyTasteToShot
// (notesWithTasteMarkerStatic), which merges against a LIVE read of the shot's notes on the DB thread — so a
// spoken rating can't clobber notes typed after the barista opened. (Was a stale-snapshot merge here.)

// [barista-fork] Run a barista client-tool query against the local shot DB on a background thread and deliver
// the JSON result to `done` on the main thread. query_shots is a READ-ONLY lookup across the user's FULL shot
// history (any roaster/bean, any date range) — the "access to everything" the barista promised. Kept off the
// main thread because withTempDb opens a fresh connection each call and the target is a slow tablet.
namespace {
// [barista-fork] Build the native steamJson from the barista's flat steam params. Returns "" when NO steam field
// is present (→ leave the recipe's steam block untouched). hasMilk defaults true when any steam field is set. The
// block is intentionally minimal (hasMilk drives steam-heater intent; the pitcher program isn't set by voice) —
// the same partial shape the MCP recipe tools accept.
QString baristaBuildSteamJson(const QJsonObject& in) {
    const bool any = in.contains(QStringLiteral("has_milk")) || in.contains(QStringLiteral("milk_weight_g"))
                     || in.contains(QStringLiteral("steam_temperature_c"));
    if (!any) return QString();
    QJsonObject s;
    s[QStringLiteral("hasMilk")] = in.contains(QStringLiteral("has_milk"))
        ? in.value(QStringLiteral("has_milk")).toBool() : true;
    if (in.contains(QStringLiteral("milk_weight_g")))
        s[QStringLiteral("milkWeightG")] = in.value(QStringLiteral("milk_weight_g")).toDouble();
    if (in.contains(QStringLiteral("steam_temperature_c")))
        s[QStringLiteral("temperatureC")] = in.value(QStringLiteral("steam_temperature_c")).toDouble();
    return QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Compact));
}
// [barista-fork] Native hotWaterJson from flat params. "" when no water field present. order = "before"|"after"
// (long black vs americano). Minimal block; matches the partial shape the MCP recipe tools accept.
QString baristaBuildHotWaterJson(const QJsonObject& in) {
    const bool any = in.contains(QStringLiteral("has_water")) || in.contains(QStringLiteral("water_volume_ml"))
                     || in.contains(QStringLiteral("water_temperature_c")) || in.contains(QStringLiteral("water_order"));
    if (!any) return QString();
    QJsonObject w;
    w[QStringLiteral("hasWater")] = in.contains(QStringLiteral("has_water"))
        ? in.value(QStringLiteral("has_water")).toBool() : true;
    if (in.contains(QStringLiteral("water_volume_ml")))
        w[QStringLiteral("volume")] = in.value(QStringLiteral("water_volume_ml")).toInt();
    if (in.contains(QStringLiteral("water_temperature_c")))
        w[QStringLiteral("temperatureC")] = in.value(QStringLiteral("water_temperature_c")).toDouble();
    if (in.contains(QStringLiteral("water_order")))
        w[QStringLiteral("order")] = in.value(QStringLiteral("water_order")).toString();
    return QString::fromUtf8(QJsonDocument(w).toJson(QJsonDocument::Compact));
}
} // namespace

void BaristaTools::executeTool(ShotHistoryStorage* shotHistory, FeedbackStorage* feedback,
                               TasksStorage* tasks,
                               const std::function<QVariantMap(const QVariantMap&, qint64)>& applyDial,
                               const std::function<void()>& endConversation,
                               const std::function<void(const QString&, const QJsonObject&,
                                                        std::function<void(QJsonValue)>)>& webTools,
                               const std::function<QVariantMap()>& getActiveRecipe,
                               const std::function<void(qint64, std::function<void(QJsonObject)>)>& activateRecipe,
                               const std::function<QVariantMap()>& deactivateRecipe,
                               const std::function<void(qint64, const QVariantMap&,
                                                        std::function<void(QJsonObject)>)>& updateRecipe,
                               const std::function<void(const QString&, const QVariantMap&,
                                                        std::function<void(QJsonObject)>)>& recipeOp,
                               const std::function<QJsonArray(const QString&)>& listProfiles,
                               const std::function<void(const QString&)>& setActiveUser,
                               const QVariantMap& anchorSnapshot,
                               const QString& name, const QJsonObject& input,
                               std::function<void(QJsonValue)> done)
{
    BaristaDiagnostics::record(QStringLiteral("tool"), QStringLiteral("call"),
        {{QStringLiteral("name"), name}});
    // [barista-fork] FAST-PATH web tools (get_weather / get_stock_quote / get_local_news). Forwarded to the
    // BaristaWebTools seam, which owns the QNAM and resolves `done` asynchronously. Only reached when the model
    // calls one — and it is only OFFERED these definitions when webSearchEnabled is on (webToolDefinitions() is
    // appended under RequestOptions.webSearch). An empty seam (module off / not wired) yields an error result.
    if (name == QLatin1String("get_weather") || name == QLatin1String("get_stock_quote")
        || name == QLatin1String("get_local_news")) {
        if (!webTools) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("web tools unavailable")}});
            return;
        }
        webTools(name, input, std::move(done));
        return;
    }

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

    // [barista-fork] Recipes 2.0 — list_recipes (READ). Recipes live in the shots.db, so mirror the shot-read
    // tools: load the inventory off the main thread via withTempDb + RecipeStorage::loadInventoryStatic (already
    // MRU-ordered), filter by the optional query app-side, cap at limit, and shape each row per the design spec.
    if (name == QLatin1String("list_recipes")) {
        if (!shotHistory || shotHistory->databasePath().isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("recipe database unavailable")}});
            return;
        }
        const QString dbPath = shotHistory->databasePath();
        const QString query = input.value(QStringLiteral("query")).toString().trimmed().toLower();
        int limit = input.contains(QStringLiteral("limit")) ? input.value(QStringLiteral("limit")).toInt() : 20;
        if (limit <= 0) limit = 20;
        if (limit > 50) limit = 50;
        QThread* thread = QThread::create([=]() {
            QJsonObject result;
            QJsonArray arr;
            int matched = 0;
            const bool dbOk = withTempDb(dbPath, "barista_recipes", [&](QSqlDatabase& db) {
                const QVector<InventoryRecipe> inv = RecipeStorage::loadInventoryStatic(db, /*archived=*/false);
                for (const InventoryRecipe& ir : inv) {
                    const Recipe& r = ir.recipe;
                    if (!query.isEmpty()) {
                        const QString hay = (r.name + QLatin1Char(' ') + r.roasterName + QLatin1Char(' ')
                                             + r.coffeeName).toLower();
                        if (!hay.contains(query))
                            continue;
                    }
                    ++matched;
                    if (arr.size() >= limit)
                        continue;   // keep counting the total match set, but only emit `limit` rows
                    QJsonObject o;
                    o[QStringLiteral("id")] = static_cast<double>(r.id);
                    o[QStringLiteral("name")] = r.name;
                    o[QStringLiteral("drink_type")] = r.drinkType;   // may be empty on legacy rows
                    o[QStringLiteral("bean")] = QString(r.roasterName + QLatin1Char(' ') + r.coffeeName).trimmed();
                    bool hasMilk = false;
                    if (!r.steamJson.isEmpty())
                        hasMilk = QJsonDocument::fromJson(r.steamJson.toUtf8())
                                      .object().value(QStringLiteral("hasMilk")).toBool();
                    o[QStringLiteral("has_milk")] = hasMilk;
                    o[QStringLiteral("stale")] = ir.stale;
                    o[QStringLiteral("shot_count")] = static_cast<double>(ir.shotCount);
                    o[QStringLiteral("last_used")] = static_cast<double>(r.lastUsedEpoch);
                    arr.append(o);
                }
            });
            if (dbOk) {
                result[QStringLiteral("recipes")] = arr;
                result[QStringLiteral("total_matched")] = matched;
            } else if (!result.contains(QStringLiteral("error"))) {
                result[QStringLiteral("error")] = QStringLiteral("recipe database unavailable");
            }
            QMetaObject::invokeMethod(qApp, [done, result]() { done(result); }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return;
    }

    // [barista-fork] get_active_recipe (READ) — synchronous seam into MainController::activeRecipe().
    if (name == QLatin1String("get_active_recipe")) {
        const QVariantMap active = getActiveRecipe ? getActiveRecipe() : QVariantMap{};
        const bool isActive = !active.isEmpty() && active.value(QStringLiteral("id")).toLongLong() > 0;
        QJsonObject out;
        out[QStringLiteral("active")] = isActive;
        if (isActive)
            out[QStringLiteral("recipe")] = QJsonObject::fromVariantMap(active);
        done(out);
        return;
    }

    // [barista-fork] activate_recipe (CONTROL, machine-mutating). The approve-then-apply GATE lives in the model
    // (the tool description forbids calling this before explicit user approval). The executor only forwards to
    // the async seam, which does the pre-flight + activation + result correlation and replies with the ground
    // truth — we hand that straight back so the barista reports success/failure honestly.
    if (name == QLatin1String("activate_recipe")) {
        if (!activateRecipe) {
            done(QJsonObject{{QStringLiteral("success"), false},
                             {QStringLiteral("failure_reason"), QStringLiteral("unavailable")},
                             {QStringLiteral("detail"), QStringLiteral("Recipe activation is unavailable.")}});
            return;
        }
        if (!input.contains(QStringLiteral("recipe_id"))) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("activate_recipe needs recipe_id")}});
            return;
        }
        const qint64 recipeId = input.value(QStringLiteral("recipe_id")).toVariant().toLongLong();
        activateRecipe(recipeId, [done](QJsonObject result) { done(result); });
        return;
    }

    // [barista-fork] update_recipe (edits a saved recipe; no machine mutation). Approve-then-apply is a model
    // (persona) gate. Builds the storage field map (same YieldSpec vocabulary as the MCP recipe_update).
    if (name == QLatin1String("update_recipe")) {
        if (!updateRecipe) {
            done(QJsonObject{{QStringLiteral("success"), false},
                             {QStringLiteral("failure_reason"), QStringLiteral("unavailable")},
                             {QStringLiteral("detail"), QStringLiteral("Recipe editing is unavailable.")}});
            return;
        }
        if (!input.contains(QStringLiteral("recipe_id"))) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("update_recipe needs recipe_id")}});
            return;
        }
        const qint64 recipeId = input.value(QStringLiteral("recipe_id")).toVariant().toLongLong();
        const bool hasYieldG = input.contains(QStringLiteral("yield_g"));
        const bool hasRatio  = input.contains(QStringLiteral("ratio"));
        if (hasYieldG && hasRatio) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral(
                "yield_g and ratio are mutually exclusive — a recipe holds ONE yield anchor. Send exactly one.")}});
            return;
        }
        // Translate the model's snake_case inputs → the storage field names (matches recipeFieldsFromArgs).
        QVariantMap fields;
        if (input.contains(QStringLiteral("dose_g")))
            fields.insert(QStringLiteral("doseG"), input.value(QStringLiteral("dose_g")).toDouble());
        if (input.contains(QStringLiteral("grind_setting")))
            fields.insert(QStringLiteral("grindPinned"), input.value(QStringLiteral("grind_setting")).toString());
        if (input.contains(QStringLiteral("rpm")))
            fields.insert(QStringLiteral("rpmPinned"), input.value(QStringLiteral("rpm")).toVariant().toInt());
        if (input.contains(QStringLiteral("temperature_offset_c")))
            fields.insert(QStringLiteral("tempOffsetC"), input.value(QStringLiteral("temperature_offset_c")).toDouble());
        if (input.contains(QStringLiteral("title")))
            fields.insert(QStringLiteral("name"), input.value(QStringLiteral("title")).toString());
        if (input.contains(QStringLiteral("profile_title")))
            fields.insert(QStringLiteral("profileTitle"), input.value(QStringLiteral("profile_title")).toString());
        if (input.contains(QStringLiteral("drink_type")))
            fields.insert(QStringLiteral("drinkType"), input.value(QStringLiteral("drink_type")).toString());
        {
            const QString steamJson = baristaBuildSteamJson(input);
            if (!steamJson.isEmpty()) fields.insert(QStringLiteral("steamJson"), steamJson);
            const QString waterJson = baristaBuildHotWaterJson(input);
            if (!waterJson.isEmpty()) fields.insert(QStringLiteral("hotWaterJson"), waterJson);
        }
        if (hasYieldG) {
            const double g = input.value(QStringLiteral("yield_g")).toDouble();
            fields.insert(QStringLiteral("yieldValue"), g > 0 ? g : 0.0);
            fields.insert(QStringLiteral("yieldMode"), g > 0 ? QStringLiteral("absolute") : QStringLiteral("none"));
        } else if (hasRatio) {
            const double r = input.value(QStringLiteral("ratio")).toDouble();
            fields.insert(QStringLiteral("yieldValue"), r > 0 ? YieldSpec::clampRatio(r) : 0.0);
            fields.insert(QStringLiteral("yieldMode"), r > 0 ? QStringLiteral("ratio") : QStringLiteral("none"));
        }
        if (fields.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral(
                "update_recipe needs recipe_id plus at least one field to change")}});
            return;
        }
        updateRecipe(recipeId, fields, [done](QJsonObject result) { done(result); });
        return;
    }

    // [barista-fork] create_recipe — build a new recipe's fields and hand off to the app-side recipeOp seam
    // (which resolves the profile, inherits beans if asked, and correlates recipeCreated). No machine mutation.
    if (name == QLatin1String("create_recipe")) {
        if (!recipeOp) {
            done(QJsonObject{{QStringLiteral("success"), false},
                             {QStringLiteral("failure_reason"), QStringLiteral("unavailable")},
                             {QStringLiteral("detail"), QStringLiteral("Recipe creation is unavailable.")}});
            return;
        }
        const QString recipeName = input.value(QStringLiteral("name")).toString().trimmed();
        if (recipeName.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("create_recipe needs a name")}});
            return;
        }
        const bool hasYieldG = input.contains(QStringLiteral("yield_g"));
        const bool hasRatio  = input.contains(QStringLiteral("ratio"));
        if (hasYieldG && hasRatio) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral(
                "yield_g and ratio are mutually exclusive — send exactly one.")}});
            return;
        }
        // Translate snake_case → storage field names; the handler resolves profileTitle / bean inheritance.
        QVariantMap args;
        args.insert(QStringLiteral("name"), recipeName);
        if (input.contains(QStringLiteral("profile_title")))
            args.insert(QStringLiteral("profileTitle"), input.value(QStringLiteral("profile_title")).toString().trimmed());
        if (input.contains(QStringLiteral("drink_type")))
            args.insert(QStringLiteral("drinkType"), input.value(QStringLiteral("drink_type")).toString());
        if (input.value(QStringLiteral("copy_beans_from_active")).toBool())
            args.insert(QStringLiteral("copyBeansFromActive"), true);
        if (input.contains(QStringLiteral("roaster_name")))
            args.insert(QStringLiteral("roasterName"), input.value(QStringLiteral("roaster_name")).toString());
        if (input.contains(QStringLiteral("coffee_name")))
            args.insert(QStringLiteral("coffeeName"), input.value(QStringLiteral("coffee_name")).toString());
        if (input.contains(QStringLiteral("dose_g")))
            args.insert(QStringLiteral("doseG"), input.value(QStringLiteral("dose_g")).toDouble());
        if (input.contains(QStringLiteral("grind_setting")))
            args.insert(QStringLiteral("grindPinned"), input.value(QStringLiteral("grind_setting")).toString());
        if (input.contains(QStringLiteral("rpm")))
            args.insert(QStringLiteral("rpmPinned"), input.value(QStringLiteral("rpm")).toVariant().toInt());
        if (input.contains(QStringLiteral("temperature_offset_c")))
            args.insert(QStringLiteral("tempOffsetC"), input.value(QStringLiteral("temperature_offset_c")).toDouble());
        if (hasYieldG) {
            const double g = input.value(QStringLiteral("yield_g")).toDouble();
            args.insert(QStringLiteral("yieldValue"), g > 0 ? g : 0.0);
            args.insert(QStringLiteral("yieldMode"), g > 0 ? QStringLiteral("absolute") : QStringLiteral("none"));
        } else if (hasRatio) {
            const double r = input.value(QStringLiteral("ratio")).toDouble();
            args.insert(QStringLiteral("yieldValue"), r > 0 ? YieldSpec::clampRatio(r) : 0.0);
            args.insert(QStringLiteral("yieldMode"), r > 0 ? QStringLiteral("ratio") : QStringLiteral("none"));
        }
        {
            const QString steamJson = baristaBuildSteamJson(input);
            if (!steamJson.isEmpty()) args.insert(QStringLiteral("steamJson"), steamJson);
            const QString waterJson = baristaBuildHotWaterJson(input);
            if (!waterJson.isEmpty()) args.insert(QStringLiteral("hotWaterJson"), waterJson);
        }
        recipeOp(QStringLiteral("create"), args, [done](QJsonObject result) { done(result); });
        return;
    }

    // [barista-fork] clone_recipe / archive_recipe / delete_recipe — all ride the recipeOp seam (app-side).
    if (name == QLatin1String("clone_recipe")) {
        if (!recipeOp) { done(QJsonObject{{QStringLiteral("error"), QStringLiteral("Recipe cloning is unavailable.")}}); return; }
        const qint64 sourceId = input.value(QStringLiteral("recipe_id")).toVariant().toLongLong();
        const QString newName = input.value(QStringLiteral("new_name")).toString().trimmed();
        if (sourceId <= 0 || newName.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("clone_recipe needs recipe_id and new_name")}});
            return;
        }
        recipeOp(QStringLiteral("clone"),
                 QVariantMap{{QStringLiteral("sourceId"), sourceId}, {QStringLiteral("newName"), newName}},
                 [done](QJsonObject result) { done(result); });
        return;
    }
    if (name == QLatin1String("archive_recipe")) {
        if (!recipeOp) { done(QJsonObject{{QStringLiteral("error"), QStringLiteral("Recipe archiving is unavailable.")}}); return; }
        const qint64 recipeId = input.value(QStringLiteral("recipe_id")).toVariant().toLongLong();
        if (recipeId <= 0) { done(QJsonObject{{QStringLiteral("error"), QStringLiteral("archive_recipe needs recipe_id")}}); return; }
        const bool archived = input.contains(QStringLiteral("archived"))
                              ? input.value(QStringLiteral("archived")).toBool() : true;
        recipeOp(QStringLiteral("archive"),
                 QVariantMap{{QStringLiteral("recipeId"), recipeId}, {QStringLiteral("archived"), archived}},
                 [done](QJsonObject result) { done(result); });
        return;
    }
    if (name == QLatin1String("delete_recipe")) {
        if (!recipeOp) { done(QJsonObject{{QStringLiteral("error"), QStringLiteral("Recipe deletion is unavailable.")}}); return; }
        const qint64 recipeId = input.value(QStringLiteral("recipe_id")).toVariant().toLongLong();
        if (recipeId <= 0) { done(QJsonObject{{QStringLiteral("error"), QStringLiteral("delete_recipe needs recipe_id")}}); return; }
        recipeOp(QStringLiteral("delete"),
                 QVariantMap{{QStringLiteral("recipeId"), recipeId}},
                 [done](QJsonObject result) { done(result); });
        return;
    }

    // [barista-fork] list_profiles (READ) — the app's usable profiles, optional title-substring filter.
    if (name == QLatin1String("list_profiles")) {
        if (!listProfiles) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("Profile listing is unavailable.")}});
            return;
        }
        const QJsonArray profiles = listProfiles(input.value(QStringLiteral("query")).toString());
        done(QJsonObject{{QStringLiteral("count"), profiles.size()},
                         {QStringLiteral("profiles"), profiles}});
        return;
    }

    // [barista-fork] deactivate_recipe (CONTROL, no machine mutation) — a direct instruction IS the approval.
    if (name == QLatin1String("deactivate_recipe")) {
        const QVariantMap res = deactivateRecipe ? deactivateRecipe() : QVariantMap{};
        done(QJsonObject::fromVariantMap(res));
        return;
    }

    // [barista-fork] set_active_user (Phase 1 identity). Roster lives in shots.db → match/insert OFF the main
    // thread (like the shot-read tools), then hop to the main thread to set the active user (dyeBarista) via the
    // seam. A misheard-name variant folds onto an existing roster entry (matchRosterName); a genuinely new name
    // creates a roster row. The model is instructed to confirm a NEW name before calling this.
    if (name == QLatin1String("set_active_user")) {
        if (!shotHistory || shotHistory->databasePath().isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("user database unavailable")}});
            return;
        }
        const QString rawName = input.value(QStringLiteral("name")).toString().trimmed();
        if (rawName.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("set_active_user needs a name")}});
            return;
        }
        const QString dbPath = shotHistory->databasePath();
        QThread* thread = QThread::create([=]() {
            QString canonical = rawName;
            bool wasKnown = false, created = false;
            const bool dbOk = withTempDb(dbPath, "barista_setuser", [&](QSqlDatabase& db) {
                const QVector<Barista> roster = BaristaStorage::loadRosterStatic(db);
                const QString match = matchRosterName(roster, rawName);
                if (!match.isEmpty()) {
                    canonical = match;
                    wasKnown = true;
                } else {
                    Barista b;
                    b.name = rawName;
                    const qint64 now = QDateTime::currentSecsSinceEpoch();
                    b.createdEpoch = now;
                    b.lastUsedEpoch = now;
                    created = (BaristaStorage::insertStatic(db, b) > 0);
                }
            });
            QJsonObject result;
            if (!dbOk) {
                result[QStringLiteral("error")] = QStringLiteral("user database unavailable");
            } else {
                result[QStringLiteral("switched_to")] = canonical;
                result[QStringLiteral("was_known")] = wasKnown;
                if (created) result[QStringLiteral("created")] = true;
            }
            QMetaObject::invokeMethod(qApp, [setActiveUser, canonical, dbOk, result, done]() {
                if (dbOk && setActiveUser) setActiveUser(canonical);   // set dyeBarista on the main thread
                done(result);
            }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
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

    // [barista-fork] remember_fact (WRITE) — store a durable basic fact. The ACTIVE USER is stamped app-side
    // from the anchor snapshot (m_settings->dye()->dyeBarista()), NEVER trusted from the model, so facts scope
    // to whoever the barista is talking to (no cross-user contamination). The model supplies only fact/category.
    if (name == QLatin1String("remember_fact")) {
        if (!tasks) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("fact storage unavailable")}});
            return;
        }
        const QString fact = input.value(QStringLiteral("fact")).toString().trimmed();
        if (fact.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("remember_fact needs a fact to store")}});
            return;
        }
        QVariantMap fields;
        fields.insert(QStringLiteral("user"), anchorSnapshot.value(QStringLiteral("activeUser")).toString());
        fields.insert(QStringLiteral("fact"), fact);
        fields.insert(QStringLiteral("category"), input.value(QStringLiteral("category")).toString().trimmed());

        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(tasks, &TasksStorage::userFactAdded, tasks,
            [done, conn, fact](qint64 id) {
                QObject::disconnect(*conn);
                if (id > 0)
                    done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("factId"), id},
                                     {QStringLiteral("fact"), fact}});
                else
                    done(QJsonObject{{QStringLiteral("error"), QStringLiteral("failed to save the fact")}});
            });
        tasks->requestAddUserFact(fields);
        return;
    }

    // [barista-fork] forget_fact (WRITE) — remove previously-remembered facts (a correction). Scoped to the
    // active user, same as remember_fact.
    if (name == QLatin1String("forget_fact")) {
        if (!tasks) {
            done(QJsonObject{{QStringLiteral("error"), QStringLiteral("fact storage unavailable")}});
            return;
        }
        const QString fact = input.value(QStringLiteral("fact")).toString().trimmed();
        if (fact.isEmpty()) {
            done(QJsonObject{{QStringLiteral("error"),
                QStringLiteral("forget_fact needs words identifying the fact to remove")}});
            return;
        }
        QVariantMap fields;
        fields.insert(QStringLiteral("user"), anchorSnapshot.value(QStringLiteral("activeUser")).toString());
        fields.insert(QStringLiteral("fact"), fact);

        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(tasks, &TasksStorage::userFactForgotten, tasks,
            [done, conn](int removed) {
                QObject::disconnect(*conn);
                done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("removed"), removed}});
            });
        tasks->requestForgetUserFact(fields);
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

        // [barista-fork] Resolve the target shot ONCE: a valid model-supplied shot_id (rate a specific past
        // shot) overrides the app-side anchor. Used for BOTH the shot-record write (below) and the KB row's
        // shotId provenance, so a past-shot rating attaches its feedback row to the right shot too.
        const qint64 anchorShotId = anchorSnapshot.value(QStringLiteral("shotId")).toLongLong();
        const qint64 explicitShotId = input.value(QStringLiteral("shot_id")).toVariant().toLongLong();
        const qint64 targetShotId = (explicitShotId > 0) ? explicitShotId : anchorShotId;

        // [barista-fork] LAND IT ON THE SHOT RECORD. A spoken rating/taste must fully replace the post-shot tap
        // buttons, which write enjoyment0to100 + a "Tasted <choice>" marker onto the SHOT (shots.db). We do this
        // FIRST because it is the AUTHORITATIVE rating write, and it is the ONLY write for the rate-a-past-shot
        // case below. enjoyment: an explicit overall_rating_0to100 wins; else the tap-button fallback for the
        // derived taste choice. Routed through requestApplyTasteToShot, which does a LIVE read-modify-write of
        // espresso_notes on the serialized DB thread (a spoken rating can NEVER clobber notes typed after the
        // barista opened), so the write is byte-identical to the tap path and PostShotReviewPage round-trips it.
        //
        // RATE A SPECIFIED PAST SHOT. By default targetShotId is the app-side anchor (the current session's
        // latest shot of the current bean). When the user asks to rate/adjust a SPECIFIC past shot ("update my
        // rating for this morning's shot"), the model finds it via query_shots and passes its shotId as shot_id;
        // that OVERRIDES the anchor (resolved above as targetShotId), so the rating lands on THAT shot.
        const QString tasteChoice = baristaTasteChoice(input);
        const QString tasteBody = baristaTasteBody(input);
        if (shotHistory && targetShotId > 0
            && (hasRating || !tasteChoice.isEmpty() || !tasteBody.isEmpty())) {
            const int shotEnjoyment = hasRating ? rating : baristaEnjoymentForTaste(tasteChoice);
            // Also lands the structured taste_balance/taste_body columns (the tap-picker's source of truth), so
            // the barista + the post-shot picker agree; a body-only remark ("it was thin") still records the
            // body axis without touching enjoyment or balance (shotEnjoyment stays 0 → setEnjoyment false).
            shotHistory->requestApplyTasteToShot(targetShotId, shotEnjoyment, shotEnjoyment > 0,
                                                 tasteChoice, tasteBody);
        }

        // [barista-fork] PAST-SHOT rating stops here — do NOT write a feedback-KB row. The KB row's bean/dial
        // provenance comes from the CURRENT-session anchor snapshot, which is the WRONG bean/dial for an earlier
        // shot; a mis-stamped row would surface under the wrong bean in search_tasting_feedback and corrupt the
        // cross-session reasoning the KB exists for. The authoritative rating already landed on the shot record
        // above, which is exactly what a "rate this past shot" ask needs. Confirm to the model and return.
        if (explicitShotId > 0) {
            done(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("ratedShotId"), explicitShotId}});
            return;
        }

        // Field map for FeedbackStorage: provenance (bean/profile/dial/shotId) from the APP-SIDE snapshot, taste
        // content from the (validated) model input. anchorSnapshot keys mirror the column keys. This path is the
        // CURRENT-shot case only (past-shot returned above), so the anchor provenance matches the rated shot.
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
                // [barista-fork] Soften the skip-first-frame flag before the model sees it. A bare
                // `skipFirstFrameDetected: true` gets over-dramatized into an alarming "skipped frame error
                // impacting your shots" — but this detector is LOW-CONFIDENCE and false-positives on
                // preinfusion frames that legitimately hit their target between BLE samples (a benign DE1
                // firmware quirk). Replace the raw boolean with a note that tells the model to keep it in
                // proportion and not raise it unprompted.
                if (o.value(QStringLiteral("skipFirstFrameDetected")).toBool()) {
                    o.remove(QStringLiteral("skipFirstFrameDetected"));
                    o[QStringLiteral("firstFrameNote")] = QStringLiteral(
                        "A possible skipped first frame was flagged, but this is LOW CONFIDENCE and usually "
                        "benign (often a preinfusion frame that hit its target between sensor samples, or a very "
                        "short first frame — a known DE1 quirk, not a shot-ruining error). Do NOT raise it "
                        "proactively or call it a problem; only mention it if the user asks about frames/skips, "
                        "and even then keep it in proportion.");
                    BaristaDiagnostics::record(QStringLiteral("tool"), QStringLiteral("get_shot_detail_skipframe_softened"),
                        {{QStringLiteral("shotId"), shotId}});
                }
                // Speakable descriptor so the barista leads with "that lungo espresso on the
                // Ethiopia beans", not a recitation of dose/yield/ratio/time/grind (the scalars
                // stay in `o` for the barista's own reasoning; the persona says don't read them out).
                const double dDose  = o.value(QStringLiteral("doseWeightG")).toDouble();
                const double dYield = o.value(QStringLiteral("finalWeightG")).toDouble();
                if (dDose > 0 && dYield > 0)
                    o[QStringLiteral("descriptor")] = DrinkTypes::espressoShotDescriptor(
                        dYield / dDose, o.value(QStringLiteral("beanBrand")).toString().trimmed(),
                        o.value(QStringLiteral("beanType")).toString().trimmed());
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
                const int half = static_cast<int>(durs.size() / 2);
                const auto mean = [](const QVector<double>& v, int lo, int hi) {
                    double s = 0; for (int i = lo; i < hi; ++i) s += v[i]; return (hi > lo) ? s / (hi - lo) : 0.0;
                };
                const double olderMean  = mean(durs, 0, half);
                const double recentMean = mean(durs, static_cast<int>(durs.size()) - half, static_cast<int>(durs.size()));
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
                    // A speakable descriptor ("a normale espresso on the Kenya beans") so the
                    // barista refers to a shot naturally instead of reciting doseG/yieldG/ratio/
                    // durationSec/grind. Those numeric fields stay for the barista's OWN reasoning
                    // and follow-up tools — the persona tells it not to read them aloud.
                    if (dose > 0 && yield > 0)
                        s[QStringLiteral("descriptor")] = DrinkTypes::espressoShotDescriptor(
                            yield / dose, q.value(8).toString().trimmed(), q.value(9).toString().trimmed());
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
