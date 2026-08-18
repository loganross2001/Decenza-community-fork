# Research Request for Claude Fable 5 — Coffee Intelligence & Perception→Science Translation Engine

*For the Decenza barista assistant (Decent Espresso DE1). This is a research brief, not a build spec — hand the result to our engineering side to build a defensible knowledge base and reasoning layer.*

---

## 0. Why this, and why now

The barista assistant already talks, remembers, and records taste feedback. What it does **not** yet have is a *grounded brain*: a rigorous, cited body of coffee knowledge plus a reasoning method that turns **how a shot tasted to a human** into **what to change on the next shot** — and can explain *why* in plain language.

The owner's mandate, in his words:

> "I don't care about the technicalities of making coffee — I want to learn how to make **good** coffee. I want to speak in human terms about human perception and human goals, and have the AI do all of the translation: take what I'm thinking, convert it into the language of science, and come back with strong recommendations."

So the deliverable has two halves that must fit together:

1. **A knowledge base** built only from **trusted, rigorously-vetted, publicly-available** sources — peer-reviewed science first, named authorities second, community signal only as flagged hypotheses. No SEO garbage, no unvetted Reddit/forum folklore presented as fact.
2. **A reasoning method** that uses that KB as a *translation layer* (human perception ⇄ coffee science) and a *planning layer* (this shot's feedback → the single best next change → a predicted, checkable outcome).

---

## 1. What we already have — ground the research in this; don't re-derive it

**The machine (DE1) measures, per shot, as time series:** water **pressure** (bar), **flow** (ml/s), group **temperature** (°C), **weight/yield** (g) and elapsed **time** (s). Shots run a **profile** (per-frame pressure or flow targets, incl. a pre-infusion/bloom phase). We know the **dose** (g in), **yield** (g out), **ratio**, and the **grinder setting** (an arbitrary, grinder-specific number — *not* microns, *not* a particle-size distribution).

**The human provides, per shot, today:** a **balance** verdict (`sour` | `balanced` | `bitter`), a **body** verdict (`thin` | `medium` | `heavy`), an **enjoyment** score (0–100), and free-text **notes**. Plus per-bean/recipe metadata (roast level where known, roast date/freshness where known, origin/process where known).

**What we deliberately do NOT have** (be honest about every conclusion that leans on these):
- **No refractometer / TDS / extraction-yield measurement by default.** We cannot measure EY. *Any* number presented as "extraction yield" without a refractometer is fabricated — the research must respect this and route around it (infer *direction* from palate + traces, never invent a percentage).
- **No calibrated grind scale** (arbitrary units, and it drifts between grinders/burrs).
- **No particle-size distribution, no water-chemistry assay** unless the user reports it.
- **One palate, subjective, adapting, and untrained** — a single noisy sensor that also fatigues and drifts.

The existing taste vocabulary (`balance`, `body`, enjoyment, notes) is the **anchor** the research should extend, not replace. If a richer perceptual vocabulary is warranted, propose it as a superset that degrades cleanly to what we already collect.

---

## 2. The core research questions

### Q1 — The Rosetta Stone: a rigorous perception→cause lexicon
Build the mapping from **human sensory language** to **physical/chemical cause**, as a defensible dictionary. For each common descriptor a home user actually says — *sour, sharp, bright, tart, lemony; bitter, harsh, ashy, burnt; dry, astringent, puckery; thin, watery, weak; muddy, muted, flat; hollow/empty; heavy, syrupy, intense; sweet, chocolatey, round* — give: the leading physical cause(s), the direction of the fault, the **contested** alternative causes, and the sensory-science basis. Distinguish **taste** (sour/bitter/sweet), **mouthfeel/tactile** (body, astringency), and **retronasal aroma** — they have different causes and different fixes. Ground in the **World Coffee Research Sensory Lexicon** and SCA sensory work, not folk analogies.

### Q2 — The causal lever model (the physics/chemistry of the knobs)
Map each controllable lever to the outcomes it moves, with **direction and, where the literature supports it, relative magnitude**: **grind** (the dominant extraction lever), **dose**, **yield/ratio**, **temperature**, **pressure/flow profile & pre-infusion**, **puck prep/distribution**, **basket**, **water chemistry**, **bean freshness & roast level**. Critically: the **interactions** (e.g. grind and ratio both move extraction but not the same way; temperature vs. grind for taming a bright light roast). Produce a causal graph, not a list.

### Q3 — The diagnosis→prescription reasoning method (the coaching core)
This is the heart of the request. Given *(taste report + shot traces + bean context)*, how should the assistant **reason to the single most likely cause and the one best next change**? We want the actual decision method the good practitioners use:
- The **one-variable-at-a-time** discipline and why it matters.
- How to **disambiguate** a symptom with multiple causes (sour could be under-extracted *or* too cold *or* ran too fast *or* channeled — how do the traces + questions break the tie?).
- The order to move levers in (what to fix first, and when to stop).
- How to phrase the change as a **hypothesis with a predicted, checkable result** ("grind two steps finer → shot runs ~4s longer → the sourness should drop and sweetness come up; if it turns bitter/dry instead, we overshot").

### Q4 — Extraction vs. strength, and the honesty boundary
Nail the classic **extraction (EY) vs. strength (TDS)** independence and the **Brewing Control Chart** framing — then translate it to our reality: **we have no refractometer.** What can be honestly inferred about extraction and strength from **palate + DE1 traces + ratio/time alone**, and what genuinely cannot? Where does "sour = under-extracted" break down? Give the honestly-degraded reasoning that never fabricates an EY number, and state plainly the questions we can only answer *if* the user later adds a refractometer.

### Q5 — Reading the shot traces (espresso-specific, because we have them)
DE1 pressure/flow/weight/temperature curves are a real signal most tools lack. What do they reveal, with cited or community-established signatures: **channeling** (flow spikes / early gushing), **pre-infusion** behavior, **chokes vs. gushers**, flow that races or stalls, temperature instability. For each: the trace signature, what it means for the cup, and whether it's a *grind/prep* problem vs. a *profile* problem. Be explicit about which signatures are peer-established vs. strong community consensus vs. speculative.

### Q6 — Goal-directed dialing from a *human* goal
Map the goals a user actually states — *"sweeter," "less sharp," "more chocolatey/rounder," "punchier/more intense," "a bigger/longer drink," "gentler," "more like the café's"* — to a **target region** (extraction/strength/balance) and a **dialing path** to get there. Make it **roast-** and **bean-aware**: a light Nordic roast and a dark espresso roast want different targets and different technique. Where does the same stated goal imply *opposite* lever moves depending on the bean?

### Q7 — The human in the loop: palate calibration & good questioning
The user is our only sensory instrument and it is noisy. From sensory science and coaching practice: how to **weight** subjective feedback, **anchor** it (reference tastes, side-by-sides), detect **palate adaptation/fatigue**, and — most useful — the **clarifying questions** to ask when a user says "it's just… off" or can't name it. What's the minimum, least-annoying set of questions that most reduces diagnostic uncertainty? How should confidence scale with how much the user has told us?

### Q8 — Source-trust framework (the KB inclusion/exclusion policy — an explicit deliverable)
The owner is emphatic about rigor. Produce the **tiering rubric and thresholds** we will use to admit or reject a source, roughly:
- **Tier 1 — Peer-reviewed science** (journals: *Matter*, *J. Agric. Food Chem.*, *Food Chemistry*, *Food Research Int'l*, *Scientific Reports*, etc.). Primary evidence.
- **Tier 2 — Named authorities with a reproducible, methodical track record** (e.g. **Scott Rao**; **Jonathan Gagné** / *Coffee ad Astra* & *The Physics of Filter Coffee*; **Christopher Hendon** & the UO chemistry group; **James Hoffmann**; **Barista Hustle / Matt Perger**; **SCA** standards & lexicons; **Colonna-Dashwood & Hendon**, *Water for Coffee*; **Illy & Viani**, *Espresso Coffee: The Science of Quality*). Strong, citable, but flag where a practitioner claim outruns the science.
- **Tier 3 — Community signal** (Home-Barista, r/espresso, the Decent diaspora, YouTube). **Never primary evidence.** Admissible only as a *flagged hypothesis or trend to verify* against Tier 1/2, and always labeled as such.
Define the concrete **thresholds** (what earns Tier 1 vs 2; what disqualifies a source outright; how to treat a strong-but-single practitioner claim; how to treat genuine expert disagreement). This rubric *is* our curation policy — it must be usable as a filter, not a vibe.

### Q9 — How to encode it so the AI can *reason*, not just recite
Recommend the KB structure the assistant will actually query at shot time: the perception→cause lexicon (Q1), the causal lever graph (Q2), the diagnostic decision rules (Q3), the goal→target maps (Q6), and per-fact **provenance** (source + tier + confidence + "contested?" flag). Keep it small enough to sit in context or be retrieved cheaply, and structured so that every recommendation the assistant makes can cite the fact and tier it rests on.

---

## 3. Trusted sources (require real citations; separate science from folklore)

Anchor in the coffee-science and authoritative-practitioner canon, e.g.: **peer-reviewed journals** — *Matter* (Hendon et al., "Systematically Improving Espresso: Insights from Mass Transfer Modeling," 2020, and the reproducibility/economics follow-up), *Journal of Agricultural and Food Chemistry*, *Food Chemistry*, *Food Research International*, *Scientific Reports*; **Andrea Illy & Rinantonio Viani**, *Espresso Coffee: The Science of Quality*; **Jonathan Gagné**, *The Physics of Filter Coffee* + *Coffee ad Astra*; **Scott Rao**, *Everything But Espresso* / *The Professional Barista's Handbook* / *The Coffee Roaster's Companion*; **James Hoffmann**, *The World Atlas of Coffee* + method work; **Maxwell Colonna-Dashwood & Christopher Hendon**, *Water for Coffee*; **Barista Hustle** (Matt Perger) courses & extraction/strength material; **SCA** — Coffee Brewing Control Chart, cupping protocol, and the **World Coffee Research Sensory Lexicon**; **Ted Lingle**, *The Coffee Brewing Handbook*. For Q7, general **descriptive-sensory-analysis** and perceptual-calibration literature. Use the **Decent Espresso community** only as Tier 3 signal for DE1-specific profiling hypotheses.

**Rigor rules (same standard as our prior Fable briefs):**
1. **Cite every substantive claim** to a real, identifiable source (author/title/where). **No fabricated citations** — if you're unsure a source actually says it, say so.
2. **Adversarial / skeptic pass:** explicitly separate **grounded science** from **contested rules of thumb** from **folk-coaching**. Where the field genuinely disagrees (e.g. temperature's real effect size, pressure-profiling benefits, "sweet spot" extraction ranges), present the disagreement — don't paper over it.
3. **Respect our data limits — be honest.** We have **no refractometer** (never fabricate an EY/TDS number), an **uncalibrated grind scale**, **no particle-size or water assay** unless reported, and **one untrained palate**. Flag every conclusion that depends on a channel or precision we don't have, and give the **honestly-degraded** version.
4. **Prefer measurable, computable definitions over adjectives.** Every "good/bad/too-X" must reduce to something we can compute from §1's channels or elicit from the user in plain language.
5. **Roast/bean dependence is first-class.** Where a rule flips with roast level, origin, or freshness, say so — never present an espresso-roast heuristic as universal.

---

## 4. What to return

1. **The perception→cause lexicon** (Q1) as a structured table: descriptor → sense modality → leading cause → contested alternatives → source/tier.
2. **The causal lever graph** (Q2): lever → outcomes moved (direction, relative magnitude, key interactions) → source/tier.
3. **The diagnostic reasoning method** (Q3, Q4): the decision procedure from *(taste + traces + bean)* to *one next change stated as a checkable hypothesis*, with the disambiguation logic and the no-refractometer honesty boundary built in.
4. **The trace-signature catalog** (Q5): signature → meaning → grind/prep-vs-profile → evidence tier.
5. **The goal→dial maps** (Q6): human goal → target region → roast-aware dialing path.
6. **The human-in-the-loop protocol** (Q7): feedback weighting, anchoring, fatigue detection, and the minimal high-value question set.
7. **The source-trust rubric** (Q8): the tiers, the concrete admit/reject thresholds, and how to treat disagreement — usable directly as our KB curation filter.
8. **The KB encoding recommendation** (Q9): the schema, with per-fact provenance (source, tier, confidence, contested-flag).
9. A **source list** stating what each source substantiates, plus an honest **"what we could NOT ground"** section.

The goal: hand this to engineering and have it be enough to build a barista brain we can **defend** — one that takes "this tastes a bit sharp and thin" and returns "grind two steps finer and pull ~2g longer; here's why, here's what should change, and here's how we'll know if I'm right" — every verdict traceable to peer-reviewed science or a named, trusted authority, and every "do this instead" backed by evidence rather than the internet's loudest opinion.
