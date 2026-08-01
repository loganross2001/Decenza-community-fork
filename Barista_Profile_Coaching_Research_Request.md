# Research Request for Claude Fable 5 — Profile & Pre-infusion Coaching for the Decenza Barista

*For the Decenza barista assistant (Decent Espresso DE1). This is a **targeted extension** of the earlier "Coffee Intelligence" brief (`Barista_Coffee_Intelligence_Research_Request.md`) that produced our current knowledge base. It is a research brief, not a build spec — the result is handed to engineering to extend a defensible KB.*

---

## 0. Why this, and why now — the gap we found

The barista already has a grounded brain: `resources/barista/coffee_knowledge.json`, built from the prior brief — a perception→cause lexicon, an 11-lever causal graph, 12 diagnostics, trace signatures, and goal→dial maps, every fact carrying `{source_id, tier, confidence, contested, note}`. Three tools query it (`translate_taste`, `recommend_next_shot`, `plan_for_goal`).

But in real use the assistant **tunnels on grind** and rarely reaches for the DE1's signature capability — the **pressure/flow profile and pre-infusion**. We audited the KB and confirmed *why*, mechanically:

- **`flow_profile` appears in ZERO diagnostics, ZERO goals, ZERO trace-signatures, and ZERO causal-edges.** It is a defined lever the reasoning engine literally never uses.
- **`preinfusion` appears in NO diagnostic and NO goal** — only in 2 background causal-edges. Since `recommend_next_shot` and `plan_for_goal` rank *over diagnostics and goals*, they **cannot surface a pre-infusion move to the user**.
- The 12 diagnostics mobilize grind / ratio / dose / roast (temperature in only 3, and 0 of 7 goals). Grind is the statistically dominant single lever, so "change one thing" almost always resolves to grind.

The owner's mandate, restated for this gap:

> "When I plan a shot I'm thinking about grind, dose, temperature, **and profile** — but the assistant only talks about grind. I want it to reason across every variable the DE1 gives us, and to treat the profile as a first-class lever, not something fixed."

The DE1 is *defined* by programmable pressure/flow and pre-infusion. A barista brain that never touches them is coaching a lever-machine as if it were a spring lever bolted to one curve. **This brief fills exactly that hole** — the profile/pre-infusion science, encoded as drop-in KB rows in our existing schema.

---

## 1. What we already have — extend it, don't re-derive it

Do **not** rebuild the perception→cause lexicon, the extraction-vs-strength honesty boundary, the source-trust rubric, or the general diagnostic method — the prior brief settled those and they are in the KB. Anchor on them.

**The machine (DE1) measures, per shot, as time series:** water **pressure** (bar), **flow** (ml/s), group **temperature** (°C), **weight/yield** (g), elapsed **time** (s). Every shot runs a **profile**: a sequence of frames, each a pressure *or* flow target, including a **pre-infusion / bloom** phase, and often a **declining-pressure** or **flow-capped** tail. The app's Recipe Editor expresses profiles as these frames (pressure-type, flow-type, with stop limits) — so any recommended profile change must be **expressible as a frame move a home user can make**, not an abstract ideal.

**The human provides, per shot:** a **balance** verdict (`sour|balanced|bitter`), a **body** verdict (`thin|medium|heavy`), an **enjoyment** score (0–100), free-text **notes**, plus roast/freshness/origin where known.

**The KB schema you will populate (add rows, do not change logic).** Return records byte-compatible with these existing shapes:

```jsonc
// diagnostics[]  — a taste+trace+context condition → ONE change as a checkable hypothesis
{ "id": "...", "symptom_descriptor_id": "sour|bitter|thin|...",
  "conditions": { "trace": "...", "roast": "light|medium|dark|any", "ratio": "...", "water": "any", "body": "any" },
  "likely_cause": "...", "change": "<free-text lever move>", "hypothesis": "<predicted, measurable shift>",
  "falsifier": "<what result would prove this wrong>", "priority": <int, 1 = prep/channel gate>,
  "provenance": [ { "source_id": "...", "tier": "T1|T2|T3", "confidence": "high|med|low", "contested": false, "note": "" } ] }

// goals[]  — a human goal → target region + roast-conditioned dialing path
{ "id": "...", "goal": "...", "target_region": "...", "default_path": "...",
  "roast_conditioned": { "light": "...", "dark": "..." }, "provenance": [ ... ] }

// trace_signatures[]  — a shape in the pressure/flow curve → meaning → which lever
{ "id": "...", "signature": "...", "meaning": "...", "class": "prep|profile|...",
  "next_change": "...", "provenance": [ ... ] }

// causal_edges[]  — lever → outcome, with direction/magnitude/interactions
{ "id": "...", "from_lever": "flow_profile|preinfusion", "to_outcome": "body|evenness|clarity|sweetness|...",
  "direction": "up|down", "magnitude": <1-3>, "interactions": [ "..." ], "provenance": [ ... ] }

// sources[]  — full citation for anything referenced
{ "id": "...", "authors": "...", "title": "...", "venue": "...", "year": <int>, "doi_or_url": "...", "tier": "T1|T2|T3" }
```

**Honesty rails (from the KB `_meta.rails`, still binding):**
- **R1 — no refractometer by default:** never emit an EY% or TDS% number; reason about *direction* only. (Note: a DiFluid R1/R2 path exists, so a rule may say "…if a TDS reading is available"; but default reasoning must not need one.)
- **R2 — grind scale uncalibrated:** relative steps, never microns.
- **R3 — no water/PSD assay** unless the user reports it.
- **Prep gate outranks profile:** a channeled puck is fixed with distribution/tamp FIRST (priority 1). Profile can *compensate* for imperfect prep only up to a ceiling — never let a profile move become a crutch for a bad puck. Say where that ceiling is.

---

## 2. The core research questions (profile-specific)

### PQ1 — The decision boundary: when does a PROFILE or PRE-INFUSION move beat grind/dose/ratio/temp?
This is the crux. Given a taste report + the shot's own trace + bean/roast context, **when is the right next change a profile change rather than a grind/ratio/dose/temp change?** What symptoms, traces, and goals *uniquely* call for a profile move — and what does a profile move achieve that the extraction levers cannot? Give the discriminators (the questions/traces that break the tie), so a diagnostic can fire on "this is a profile problem, not a grind problem."

### PQ2 — Reading the DE1 curve for a profile verdict (extend `trace_signatures`)
Our trace signatures today are prep-class only (e.g. a mid-ramp pressure notch = channel healed). Add the **profile-class** reads: pressure **spike** at the start of the ramp; flow that **never caps / gushes**; **early gush right after pre-infusion** (puck not saturated); **fast pressure bleed** (loose puck / too coarse for the profile); **choke-then-gush**; a clean **declining-pressure** tail vs a flat 9-bar wall. For each: the signature, what it means for the cup, and the specific **profile frame change** it implies — distinguishing a *profile fault* from a *grind/prep* fault. Flag peer-established vs strong-community-consensus vs speculative.

### PQ3 — Pre-infusion / bloom science (fill `preinfusion` diagnostics + goals)
What do **longer / gentler / higher-or-lower pressure** pre-infusion changes actually do — wetting and puck saturation, evenness, channeling mitigation, body, and the taste outcome — and **roast-conditioned** (light roasts are denser/harder to wet; dark roasts degas and gush)? Produce the diagnostic *conditions* that should trigger a pre-infusion recommendation (e.g. "sour + early-gush trace + light roast → extend/soften pre-infusion before touching grind"), each as a checkable hypothesis with a falsifier.

### PQ4 — Profile archetypes → bean/roast/goal (fill `flow_profile` diagnostics + goals)
Catalog the **profile archetypes** a DE1 user can actually run and when each helps: flat **9-bar**, **declining pressure** (lever/spring emulation), **blooming pre-infusion**, **flow-profiled** (capped flow), **low-pressure / allongé**, **high-then-decline**. Map each to the **bean, roast, and stated goal** it serves — especially the outcomes profile owns: **body/mouthfeel, clarity, sweetness via smoother extraction, taming a bright light roast, and channeling mitigation.** Where does the *same* goal imply *opposite* profile moves on a light vs dark roast? Return these as `goals` (with `roast_conditioned` paths that name a profile move) and as `diagnostics`.

### PQ5 — Profile ↔ other-lever interactions and ordering (extend `causal_edges`)
Map `flow_profile` and `preinfusion` to the outcomes they move (**body, evenness, clarity, sweetness, extraction uniformity**) with direction, relative magnitude, and the **interactions**: when a profile change substitutes for a grind change vs when it can't; the lever **ordering** for different goals (does profile come before or after grind/ratio when the goal is *body* rather than *balance*?); and the honest ceiling where profile stops compensating for prep or grind.

### PQ6 — Body & mouthfeel specifically — the outcomes the extraction levers can't reach
The owner's felt gap is partly **texture/body**, which grind and ratio move only crudely. What does the evidence (vs community consensus) actually support about **profile/pre-infusion → tactile body and mouthfeel**? Be explicit about what is grounded science, what is credible practitioner consensus (Rao, Gagné, the Decent diaspora), and what is folklore.

### PQ7 — Skeptic pass: profiling is genuinely contested
Pressure/flow-profiling benefits are one of the most **contested** areas in coffee — strong community enthusiasm, thinner peer-reviewed support than for grind/ratio/temp. Separate **grounded** from **contested rule-of-thumb** from **DE1-diaspora folklore**, and mark every profile record's `contested` flag and `tier` accordingly. A profile diagnostic resting on community consensus must say so (T3/contested), not masquerade as T1.

---

## 3. Trusted sources (real citations; separate science from folklore)

Extend the prior canon with the profile-literate ones: **peer-reviewed** where it exists (the flow/pressure terms in *Cameron et al., Matter 2020*; any *Food Chemistry / Food Research International / Scientific Reports* work touching pressure/flow/pre-infusion — be honest that this literature is thin); **named authorities** — **Jonathan Gagné** (*Coffee ad Astra* profiling analyses, *The Physics of Filter Coffee*), **Scott Rao** (pressure profiling & pre-infusion writing), **Barista Hustle / Matt Perger** (pressure profiling, pre-infusion), **Illy & Viani** (percolation/pre-infusion physics); **DE1-specific** (Tier 3, flag as hypothesis) — **Decent Espresso** documentation, **John Weiss** profile guides, and the Decent diaspora's profile archetypes. The DE1 community is where profile *practice* lives — admissible only as flagged, verify-against-Tier-1/2 signal.

**Rigor rules (same standard as the prior brief):**
1. **Cite every substantive claim** to a real, identifiable source. **No fabricated citations.**
2. **Adversarial pass:** grounded science vs contested rule-of-thumb vs folk-coaching, explicitly (see PQ7).
3. **Respect the data limits (R1–R3)** and the **prep-gate precedence** — flag every conclusion that leans on a channel we don't have; give the honestly-degraded version.
4. **Computable / expressible:** every recommendation must reduce to a **frame move a user can make in the Recipe Editor** (a pressure or flow target, a pre-infusion length/pressure, a decline) or a plain question — not an abstract ideal.
5. **Roast/bean dependence is first-class** — where a profile rule flips with roast or freshness, say so.

---

## 4. What to return — drop-in KB rows + rationale

Deliver, in the **exact schema of §1** so we merge by adding rows to `coffee_knowledge.json`:

1. **New `diagnostics`** whose `change` mobilizes `flow_profile` and `preinfusion` (PQ1–PQ4) — each with conditions, likely_cause, a predicted **hypothesis**, a **falsifier**, a priority (prep-gate stays 1), and provenance. These are the rows that let `recommend_next_shot` surface a profile move at all.
2. **New / revised `goals`** (PQ4) whose `default_path` / `roast_conditioned` name profile moves — especially body, clarity, "rounder/silkier," "tame this bright light roast," and channeling-driven goals.
3. **New profile-class `trace_signatures`** (PQ2) — the DE1-curve reads that tell the AI a profile change is warranted, each with the frame change it implies.
4. **New `causal_edges`** (PQ5) — `flow_profile`→{body, evenness, clarity, sweetness, uniformity} and `preinfusion`→{…}, with direction, magnitude, interactions, and the compensation ceiling.
5. **New `sources`** for everything cited, with tier.
6. A short **decision-boundary write-up** (PQ1): prose the persona can compress — "reach for the profile when …, reach for grind when …" — so we can also rebalance the system prompt away from grind-first.
7. An honest **"what we could NOT ground"** section: which profile claims are community-only, where the science is genuinely thin, and which rules should ship as `contested:true` / T3.

**The goal:** hand this to engineering and be able to add rows that let the barista say — grounded and cited — *"that ran sharp and thin on a light roast and gushed right out of pre-infusion; before we touch grind, let's soften and lengthen the pre-infusion so the puck saturates — it should even out and gain body, and if it turns hollow we lengthened too far,"* every profile verdict traceable to peer-reviewed science or a named authority, and honestly flagged where it rests on Decent-diaspora consensus rather than proof.
