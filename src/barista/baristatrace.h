#pragma once

#include <QString>
#include <QVector>

class ShotProjection;
class CoffeeKnowledgeBase;
class QJsonObject;

// [barista-fork] The measurement->KB-vocabulary bridge for the barista's opening read.
//
// objectiveTraceShape() (baristatools.cpp) already maps detector verdicts onto the coarse
// 4-value `trace` axis the KB diagnostics match on. This is its richer sibling: it maps the
// same measured verdicts onto the KB's named `trace_signatures` (choke / gusher / flow-exceeds-
// pressure / ...), so the barista can lead with the specific curve fault the machine recorded
// rather than a generic grind guess.
//
// The grounding contract (see docs/barista/Barista_Coaching_Loop_And_Trace_Fable5_DESIGN.md §3):
//   - "measured"  — a detector fired; the app asserts it as fact, with the numbers attached.
//   - "inferred"  — only CONSISTENT WITH the signature (the detector measured something weaker);
//                   handed to the model labelled, to raise as a maybe or not at all.
// A signatureId is ALWAYS one of the KB's trace_signatures ids — the accessor
// CoffeeKnowledgeBase::traceSignature(id) validates that, and a test asserts every id this
// bridge can emit resolves in the shipped KB, so a typo is a build-time failure, never a
// runtime-invented signature.
//
// This is its own translation unit (not an anonymous-namespace helper) so it stays unit-testable
// headlessly and so both consumers — the recommend_next_shot executor and the context builder —
// call one definition. It depends only on ShotProjection + Qt Core.
namespace BaristaTrace {

struct SignatureHit {
    QString signatureId;   // an id in the KB's trace_signatures
    QString grounding;     // "measured" | "inferred"
    QString evidence;      // human-readable, units included — the numbers that earned the hit
};

// Map a MEASURED shot's detector verdicts (s.detectorResults + channeling flag) onto KB trace
// signatures. Empty when the shot ran clean or carries no discriminating fault.
QVector<SignatureHit> objectiveTraceSignatures(const ShotProjection &s);

// Build the `lastShotTraceRead` context field for the barista's opening read: the trace signatures
// matched on `s`, each joined to its KB meaning / next_change / one citation, split into `measured`
// (assert, with evidence) and `inferred` (raise as a maybe, with basis). Empty object when the shot
// carries no signature — so the persona degrades to the palate/bean read. Capped at 2 entries,
// 1 citation each. Pure over (ShotProjection, KB) — headless-testable.
QJsonObject buildLastShotTraceRead(const ShotProjection &s, const CoffeeKnowledgeBase &kb);

}  // namespace BaristaTrace
