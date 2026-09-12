#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// [barista-fork] The coffee-science knowledge base — the barista's grounded "brain".
//
// Loaded once from the bundled :/barista/coffee_knowledge.json (produced by the Fable research
// brief, 2026-07). It holds a perception->cause lexicon, a causal lever graph, diagnostic
// rules, trace signatures, and goal->dial maps. EVERY fact carries provenance
// {source_id, tier, confidence, contested}, so every recommendation the barista makes can
// cite the peer-reviewed paper or named authority it rests on.
//
// The class is read-only and side-effect-free — pure lookups over cached JSON — and depends
// only on Qt Core, so it links into a DECENZA_BARISTA=OFF build (baristatools.cpp, which is
// compiled unconditionally, calls it) and into the DB-only test binaries.
//
// It powers three barista function-calling tools, mirroring the two halves of the design
// (translate human perception <-> science, then plan the next shot):
//   translate_taste     — a human descriptor word -> the science (modality, cause, direction, citation)
//   recommend_next_shot — taste verdict + shot context -> ONE next change as a cited, checkable hypothesis
//   plan_for_goal       — a human goal -> target region + roast-aware dialing path
//
// Honesty rails from the brief are enforced here, not bolted on: we never emit an extraction
// yield / TDS number (we have no refractometer), we reason in relative grind STEPS, and the
// prep-before-parameters gate wins whenever the trace shows channeling.
class CoffeeKnowledgeBase {
public:
    // The shared, lazily-loaded instance backed by the bundled resource.
    static const CoffeeKnowledgeBase &instance();

    // Build an instance from raw JSON bytes (tests, or an alternate source).
    static CoffeeKnowledgeBase fromJson(const QByteArray &raw);

    bool isLoaded() const { return m_loaded; }

    // Q1 — translation layer. Resolve a user's word (matched against term/synonyms/id,
    // case-insensitive) to its science. Returns { found, term, modality, leading_cause,
    // extraction_direction, strength_direction, contested_alternatives, citations }.
    QJsonObject translateTaste(const QString &word) const;

    // Q3 — planning layer. Given the taste verdict + shot context, return the single best
    // next change as a checkable hypothesis, carrying its citation. `ctx` keys (all optional
    // except taste): taste, roast (light|medium|dark), trace, ratio, body, water.
    // Returns { found, recommendation{change, likely_cause, hypothesis, falsifier, ...},
    // confidence, prep_gate, need_more_context[], alternatives[], citations }.
    QJsonObject recommendNextShot(const QJsonObject &ctx) const;

    // Q6 — goal layer. Map a human goal to a target region and a roast-aware dialing path.
    QJsonObject planForGoal(const QString &goal, const QString &roast) const;

    // Look up one trace signature by id (as emitted by BaristaTrace::objectiveTraceSignatures).
    // Returns { found, id, signature, meaning, class, next_change, citations } — the descriptive
    // vocabulary + provenance for a measured/inferred curve fault. { found:false } for an unknown id.
    QJsonObject traceSignature(const QString &id) const;

private:
    CoffeeKnowledgeBase() = default;
    void populate(const QJsonObject &root);

    const QJsonObject *findDescriptor(const QString &word) const;   // by term / synonym / id
    QString resolveSymptomId(const QString &taste) const;           // user word -> descriptor id
    QJsonArray citationsFor(const QJsonArray &provenance) const;    // source_id -> readable cite

    bool m_loaded = false;
    QJsonArray m_sources;
    QJsonArray m_descriptors;
    QJsonArray m_levers;
    QJsonArray m_causalEdges;
    QJsonArray m_diagnostics;
    QJsonArray m_traceSignatures;
    QJsonArray m_goals;
    QHash<QString, QJsonObject> m_sourceById;
};
