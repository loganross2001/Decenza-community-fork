#include "coffeeknowledgebase.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <utility>

namespace {

QString norm(const QString &s) { return s.trimmed().toLower(); }

// A positively-clean trace: the caller reported an even/smooth pour, which is the
// NEGATION of a channeling/gush shape (though still compatible with fast or slow).
// Distinct from a trace we simply can't discriminate — "smooth" is information, not
// absence of it, so it must conflict a channel-family condition rather than sit
// neutral. ("uneven" is NOT clean and never reaches here — traceShapes() already
// maps it to {channel}.)
bool isCleanTrace(const QString &raw)
{
    const QString s = norm(raw);
    return s.contains("smooth") || s.contains("clean") || s.contains("normal");
}

// --- Condition matching ----------------------------------------------------
// A diagnostic's `conditions` describe when it applies. We compare each against
// the caller-supplied context and classify the outcome. A single Conflict means
// the record does not apply and is dropped; Matched counts toward specificity;
// Wildcard ("any") and Unknown (caller gave no value) are neutral.
enum class Match { Wildcard, Unknown, Matched, Conflict };

// The three discriminating trace shapes. "smooth"/"clean"/"normal" carry no
// discriminator (a clean trace is compatible with fast, slow, or on-time), so
// they map to the empty set and never conflict — only channel/fast/slow do.
QSet<QString> traceShapes(const QString &raw)
{
    const QString s = norm(raw);
    QSet<QString> out;
    if (s.contains("channel") || s.contains("jagged") || s.contains("spike")
        || s.contains("erratic") || s.contains("uneven") || s.contains("gush"))
        out.insert(QStringLiteral("channel"));
    if (s.contains("fast") || s.contains("quick") || s.contains("high flow"))
        out.insert(QStringLiteral("fast"));
    if (s.contains("slow") || s.contains("choke") || s.contains("stall"))
        out.insert(QStringLiteral("slow"));
    return out;
}

Match matchTrace(const QString &cond, const QString &user)
{
    if (norm(cond) == QLatin1String("any"))
        return Match::Wildcard;
    const QSet<QString> c = traceShapes(cond);
    if (c.isEmpty())
        return Match::Wildcard;               // e.g. "normal time, smooth" — no discriminator
    if (user.trimmed().isEmpty())
        return Match::Unknown;
    const QSet<QString> u = traceShapes(user);
    if (u.isEmpty()) {
        // A positively-clean trace negates a channel-family condition (e.g. a
        // post-PI gush rule): a smooth pour is evidence AGAINST channeling, so
        // drop the record rather than let it survive on a priority tiebreak.
        // Clean is still compatible with fast/slow, so only the "channel" shape
        // conflicts. Anything genuinely undiscriminated stays neutral.
        if (isCleanTrace(user) && c.contains(QStringLiteral("channel")))
            return Match::Conflict;
        return Match::Unknown;                // caller described a shape we can't discriminate
    }
    return c.intersects(u) ? Match::Matched : Match::Conflict;
}

// Bucket a free string to a single canonical token, or "" if none applies.
QString bucket(const QString &raw, const QList<QPair<QString, QStringList>> &map)
{
    const QString s = norm(raw);
    for (const auto &m : map)
        for (const QString &kw : m.second)
            if (s.contains(kw))
                return m.first;
    return QString();
}

Match matchBucket(const QString &cond, const QString &user,
                  const QList<QPair<QString, QStringList>> &map)
{
    if (norm(cond) == QLatin1String("any") || cond.trimmed().isEmpty())
        return Match::Wildcard;
    const QString c = bucket(cond, map);
    if (c.isEmpty())
        return Match::Wildcard;               // unrecognised condition value — don't over-constrain
    if (user.trimmed().isEmpty())
        return Match::Unknown;
    const QString u = bucket(user, map);
    if (u.isEmpty())
        return Match::Unknown;
    return c == u ? Match::Matched : Match::Conflict;
}

const QList<QPair<QString, QStringList>> kRoastMap = {
    {QStringLiteral("light"), {QStringLiteral("light"), QStringLiteral("nordic")}},
    {QStringLiteral("dark"), {QStringLiteral("dark")}},
    {QStringLiteral("medium"), {QStringLiteral("medium"), QStringLiteral("med")}},
};
const QList<QPair<QString, QStringList>> kRatioMap = {
    {QStringLiteral("wide"), {QStringLiteral("wide"), QStringLiteral("long")}},
    {QStringLiteral("short"), {QStringLiteral("short"), QStringLiteral("tight")}},
    {QStringLiteral("normal"), {QStringLiteral("normal")}},
};
const QList<QPair<QString, QStringList>> kBodyMap = {
    {QStringLiteral("thin"), {QStringLiteral("thin"), QStringLiteral("watery"), QStringLiteral("weak")}},
    {QStringLiteral("heavy"), {QStringLiteral("heavy"), QStringLiteral("syrupy"), QStringLiteral("thick"), QStringLiteral("full")}},
    {QStringLiteral("medium"), {QStringLiteral("medium")}},
};

} // namespace

// ---------------------------------------------------------------------------

const CoffeeKnowledgeBase &CoffeeKnowledgeBase::instance()
{
    static const CoffeeKnowledgeBase kb = [] {
        CoffeeKnowledgeBase inst;
        QFile f(QStringLiteral(":/barista/coffee_knowledge.json"));
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("CoffeeKnowledgeBase: failed to open :/barista/coffee_knowledge.json");
            return inst;
        }
        const QByteArray raw = f.readAll();
        f.close();
        QJsonParseError err {};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning("CoffeeKnowledgeBase: parse error: %s", qUtf8Printable(err.errorString()));
            return inst;
        }
        inst.populate(doc.object());
        return inst;
    }();
    return kb;
}

CoffeeKnowledgeBase CoffeeKnowledgeBase::fromJson(const QByteArray &raw)
{
    CoffeeKnowledgeBase inst;
    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        inst.populate(doc.object());
    return inst;
}

void CoffeeKnowledgeBase::populate(const QJsonObject &root)
{
    m_sources = root.value(QStringLiteral("sources")).toArray();
    m_descriptors = root.value(QStringLiteral("descriptors")).toArray();
    m_levers = root.value(QStringLiteral("levers")).toArray();
    m_causalEdges = root.value(QStringLiteral("causal_edges")).toArray();
    m_diagnostics = root.value(QStringLiteral("diagnostics")).toArray();
    m_traceSignatures = root.value(QStringLiteral("trace_signatures")).toArray();
    m_goals = root.value(QStringLiteral("goals")).toArray();
    for (const QJsonValue &v : std::as_const(m_sources)) {
        const QJsonObject s = v.toObject();
        m_sourceById.insert(s.value(QStringLiteral("id")).toString(), s);
    }
    // "Loaded" means the reasoning collections are present — the diagnostics/descriptors
    // are what the barista actually queries; without them the KB is inert.
    m_loaded = !m_descriptors.isEmpty() && !m_diagnostics.isEmpty();
}

const QJsonObject *CoffeeKnowledgeBase::findDescriptor(const QString &word) const
{
    const QString w = norm(word);
    if (w.isEmpty())
        return nullptr;
    for (const QJsonValue &v : m_descriptors) {
        static thread_local QJsonObject held; // keep the returned pointer valid for the call
        const QJsonObject d = v.toObject();
        if (norm(d.value(QStringLiteral("id")).toString()) == w) { held = d; return &held; }
        // term may be a slash-joined pair like "thin/weak" or "dry/astringent"
        const QStringList terms = d.value(QStringLiteral("term")).toString().split('/');
        for (const QString &t : terms)
            if (norm(t) == w) { held = d; return &held; }
        for (const QJsonValue &sv : d.value(QStringLiteral("synonyms")).toArray())
            if (norm(sv.toString()) == w) { held = d; return &held; }
    }
    return nullptr;
}

QString CoffeeKnowledgeBase::resolveSymptomId(const QString &taste) const
{
    if (const QJsonObject *d = findDescriptor(taste))
        return d->value(QStringLiteral("id")).toString();
    return QString();
}

QJsonArray CoffeeKnowledgeBase::citationsFor(const QJsonArray &provenance) const
{
    QJsonArray out;
    for (const QJsonValue &pv : provenance) {
        const QJsonObject p = pv.toObject();
        const QString sid = p.value(QStringLiteral("source_id")).toString();
        const QJsonObject s = m_sourceById.value(sid);
        QString cite = s.value(QStringLiteral("authors")).toString();
        const QString year = QString::number(s.value(QStringLiteral("year")).toInt());
        if (s.contains(QStringLiteral("year")))
            cite += QStringLiteral(" (%1)").arg(year);
        const QString title = s.value(QStringLiteral("title")).toString();
        if (!title.isEmpty())
            cite += QStringLiteral(". %1").arg(title);
        const QString venue = s.value(QStringLiteral("venue")).toString();
        if (!venue.isEmpty())
            cite += QStringLiteral(". %1").arg(venue);
        QJsonObject o;
        o[QStringLiteral("source_id")] = sid;
        o[QStringLiteral("cite")] = cite.trimmed().isEmpty() ? sid : cite;
        o[QStringLiteral("tier")] = p.value(QStringLiteral("tier"));
        o[QStringLiteral("confidence")] = p.value(QStringLiteral("confidence"));
        o[QStringLiteral("contested")] = p.value(QStringLiteral("contested"));
        if (const QString note = p.value(QStringLiteral("note")).toString(); !note.isEmpty())
            o[QStringLiteral("note")] = note;
        if (const QString url = s.value(QStringLiteral("doi_or_url")).toString(); !url.isEmpty())
            o[QStringLiteral("doi_or_url")] = url;
        out.append(o);
    }
    return out;
}

QJsonObject CoffeeKnowledgeBase::translateTaste(const QString &word) const
{
    const QJsonObject *d = findDescriptor(word);
    if (!d) {
        return QJsonObject {
            {QStringLiteral("found"), false},
            {QStringLiteral("query"), word},
            {QStringLiteral("note"), QStringLiteral("No matching descriptor in the coffee KB. "
                "Ask the user to describe the taste in other words (sour/bright, bitter/harsh, "
                "thin/watery, heavy, dry/astringent, hollow, muddy) or use recommend_next_shot.")},
        };
    }
    QJsonObject out {
        {QStringLiteral("found"), true},
        {QStringLiteral("term"), d->value(QStringLiteral("term"))},
        {QStringLiteral("modality"), d->value(QStringLiteral("modality"))},
        {QStringLiteral("leading_cause"), d->value(QStringLiteral("leading_cause"))},
        {QStringLiteral("extraction_direction"), d->value(QStringLiteral("extraction_direction"))},
        {QStringLiteral("strength_direction"), d->value(QStringLiteral("strength_direction"))},
        {QStringLiteral("contested_alternatives"), d->value(QStringLiteral("contested_alternatives"))},
        {QStringLiteral("citations"), citationsFor(d->value(QStringLiteral("provenance")).toArray())},
    };
    return out;
}

QJsonObject CoffeeKnowledgeBase::recommendNextShot(const QJsonObject &ctx) const
{
    const QString taste = ctx.value(QStringLiteral("taste")).toString();
    const QString roast = ctx.value(QStringLiteral("roast")).toString();
    const QString trace = ctx.value(QStringLiteral("trace")).toString();
    const QString ratio = ctx.value(QStringLiteral("ratio")).toString();
    const QString body = ctx.value(QStringLiteral("body")).toString();

    // Resolve the symptom: prefer the explicit taste word; fall back to a body verdict
    // (a "thin" body IS the symptom when no taste fault is named).
    QString symptomId = resolveSymptomId(taste);
    if (symptomId.isEmpty() && !body.trimmed().isEmpty())
        symptomId = resolveSymptomId(body);

    if (symptomId.isEmpty()) {
        return QJsonObject {
            {QStringLiteral("found"), false},
            {QStringLiteral("note"), QStringLiteral("Could not map the feedback to a known symptom. "
                "Ask for the balance (sour / balanced / bitter) and body (thin / medium / heavy).")},
        };
    }

    // Score every diagnostic for this symptom. A Conflict on any condition drops the record;
    // otherwise specificity = number of Matched conditions. We rank by (specificity desc,
    // priority asc) — more-confirmed rules first, and within a tie the lower priority number
    // (1 = the prep/channeling gate) wins, which encodes "prep before parameters".
    struct Scored { QJsonObject rec; int spec; int priority; bool traceChannelMatched; };
    QList<Scored> ok;
    for (const QJsonValue &dv : m_diagnostics) {
        const QJsonObject rec = dv.toObject();
        if (rec.value(QStringLiteral("symptom_descriptor_id")).toString() != symptomId)
            continue;
        const QJsonObject cond = rec.value(QStringLiteral("conditions")).toObject();

        const Match mTrace = matchTrace(cond.value(QStringLiteral("trace")).toString(), trace);
        const Match mRoast = matchBucket(cond.value(QStringLiteral("roast")).toString(), roast, kRoastMap);
        const Match mRatio = matchBucket(cond.value(QStringLiteral("ratio")).toString(), ratio, kRatioMap);
        const Match mBody = matchBucket(cond.value(QStringLiteral("body")).toString(), body, kBodyMap);

        if (mTrace == Match::Conflict || mRoast == Match::Conflict
            || mRatio == Match::Conflict || mBody == Match::Conflict)
            continue;

        int spec = 0;
        for (Match m : {mTrace, mRoast, mRatio, mBody})
            if (m == Match::Matched) ++spec;

        const bool traceChannel = (mTrace == Match::Matched)
            && traceShapes(cond.value(QStringLiteral("trace")).toString()).contains(QStringLiteral("channel"));
        ok.append({rec, spec, rec.value(QStringLiteral("priority")).toInt(99), traceChannel});
    }

    if (ok.isEmpty()) {
        // Symptom is known but no rule survived — still translate it so the barista can talk.
        QJsonObject out = translateTaste(taste.isEmpty() ? body : taste);
        out[QStringLiteral("found")] = false;
        out[QStringLiteral("note")] = QStringLiteral("No diagnostic rule matched this context; "
            "returning the descriptor science instead. Consider asking for the shot trace or roast.");
        return out;
    }

    std::stable_sort(ok.begin(), ok.end(), [](const Scored &a, const Scored &b) {
        if (a.spec != b.spec) return a.spec > b.spec;
        return a.priority < b.priority;
    });

    const Scored &win = ok.first();
    const int spec = win.spec;
    const QString confidence = spec >= 2 ? QStringLiteral("high")
                             : spec == 1 ? QStringLiteral("med")
                                         : QStringLiteral("low");

    // What context, if supplied, would sharpen a low-confidence call.
    QJsonArray needMore;
    if (spec < 2) {
        if (trace.trimmed().isEmpty()) needMore.append(QStringLiteral("trace"));
        if (roast.trimmed().isEmpty()) needMore.append(QStringLiteral("roast"));
        if (ratio.trimmed().isEmpty()) needMore.append(QStringLiteral("ratio"));
    }

    QJsonObject rec {
        {QStringLiteral("id"), win.rec.value(QStringLiteral("id"))},
        {QStringLiteral("likely_cause"), win.rec.value(QStringLiteral("likely_cause"))},
        {QStringLiteral("change"), win.rec.value(QStringLiteral("change"))},
        {QStringLiteral("hypothesis"), win.rec.value(QStringLiteral("hypothesis"))},
        {QStringLiteral("falsifier"), win.rec.value(QStringLiteral("falsifier"))},
        {QStringLiteral("priority"), win.rec.value(QStringLiteral("priority"))},
    };

    QJsonArray alternatives;
    for (qsizetype i = 1; i < ok.size() && i <= 3; ++i)
        alternatives.append(QJsonObject {
            {QStringLiteral("id"), ok[i].rec.value(QStringLiteral("id"))},
            {QStringLiteral("change"), ok[i].rec.value(QStringLiteral("change"))},
            {QStringLiteral("priority"), ok[i].rec.value(QStringLiteral("priority"))},
        });

    return QJsonObject {
        {QStringLiteral("found"), true},
        {QStringLiteral("symptom"), symptomId},
        {QStringLiteral("recommendation"), rec},
        {QStringLiteral("confidence"), confidence},
        {QStringLiteral("prep_gate"), win.traceChannelMatched},
        {QStringLiteral("need_more_context"), needMore},
        {QStringLiteral("alternatives"), alternatives},
        {QStringLiteral("citations"), citationsFor(win.rec.value(QStringLiteral("provenance")).toArray())},
    };
}

QJsonObject CoffeeKnowledgeBase::planForGoal(const QString &goal, const QString &roast) const
{
    const QString g = norm(goal);
    if (g.isEmpty())
        return QJsonObject {{QStringLiteral("found"), false},
                            {QStringLiteral("note"), QStringLiteral("No goal supplied.")}};

    // Best match: exact id, else substring either direction, else token overlap.
    QJsonObject best;
    int bestScore = 0;
    for (const QJsonValue &gv : m_goals) {
        const QJsonObject rec = gv.toObject();
        const QString id = norm(rec.value(QStringLiteral("id")).toString());
        const QString label = norm(rec.value(QStringLiteral("goal")).toString());
        int score = 0;
        if (id == g || label == g) score = 100;
        else if (label.contains(g) || g.contains(label) || id.contains(g) || g.contains(id)) score = 50;
        else {
            for (const QString &tok : g.split(QRegularExpression(QStringLiteral("\\W+")), Qt::SkipEmptyParts))
                if (tok.size() > 2 && label.contains(tok)) score += 10;
        }
        if (score > bestScore) { bestScore = score; best = rec; }
    }

    if (bestScore == 0)
        return QJsonObject {
            {QStringLiteral("found"), false},
            {QStringLiteral("query"), goal},
            {QStringLiteral("note"), QStringLiteral("No goal map matched. Known goals include: "
                "sweeter, less sharp, rounder/chocolatey, punchier, bigger drink, gentler, like-the-cafe.")},
        };

    QJsonObject out {
        {QStringLiteral("found"), true},
        {QStringLiteral("goal"), best.value(QStringLiteral("goal"))},
        {QStringLiteral("target_region"), best.value(QStringLiteral("target_region"))},
        {QStringLiteral("default_path"), best.value(QStringLiteral("default_path"))},
        {QStringLiteral("citations"), citationsFor(best.value(QStringLiteral("provenance")).toArray())},
    };
    // Roast conditions the path — the same goal can imply opposite moves on light vs dark.
    const QJsonObject rc = best.value(QStringLiteral("roast_conditioned")).toObject();
    const QString rb = bucket(roast, kRoastMap);
    if (!rc.isEmpty()) {
        out[QStringLiteral("roast_conditioned")] = rc;
        if ((rb == QLatin1String("light")) && rc.contains(QStringLiteral("light")))
            out[QStringLiteral("roast_specific_path")] = rc.value(QStringLiteral("light"));
        else if ((rb == QLatin1String("dark")) && rc.contains(QStringLiteral("dark")))
            out[QStringLiteral("roast_specific_path")] = rc.value(QStringLiteral("dark"));
    }
    return out;
}
