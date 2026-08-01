# Profile & Pre-infusion Coaching — Research Report
*Companion to `profile_preinfusion_kb_extension.json`. Extends `coffee_knowledge.json`; nothing here re-derives the prior brief's foundations. All citations verified against live sources, July 2026.*

---

## 1. The decision boundary (PQ1) — when profile beats the extraction levers

The extraction levers (grind, dose, ratio, temperature) move the shot's **average**: how much dissolves overall. The profile levers (pre-infusion, pressure/flow shape) move the shot's **dynamics and distribution**: when and how evenly water works the puck. That is the whole boundary, and it yields five concrete discriminators a diagnostic can fire on.

**Reach for the profile when:**

1. **The fault is localized in time, not global.** A harsh *finish* on an otherwise good shot, or a sharp *attack* that settles — the trace usually shows it (late flow rise under flat 9 bar; post-PI gush). Grinding coarser to fix a harsh tail breaks the good start; a declining tail fixes only the tail. Coarser support: Barista Hustle Æ 6.08 (late-shot puck breakdown), Rao's 3-phase model. Tier: T2, contested.
2. **The trace shows a dynamics fault while averages are on target.** Pressure spike past ~9.5 bar, flow that never caps, uncommanded pressure bleed, gush right out of pre-infusion — these are profile-class signatures (see `trace_signatures`) with profile-frame fixes. A clean trace with a bad cup, conversely, routes to the extraction levers.
3. **The goal is texture, evenness, or forgiveness rather than balance or strength.** Balance → grind/ratio/temp first. Strength → ratio, full stop. Body, silkiness, clean finish, shot-to-shot variance → profile owns the plausible mechanism (though the taste evidence is T2/T3 — see §6–7).
4. **Grind is pinned at a limit.** When finer chokes or spikes pressure and coarser gushes, grind is a dead end; archetype switches (extended PI, allongé, blooming) route around it. This is exactly why Rao built allongé and blooming for light roasts (scottrao.com, "How the Decent evolved").
5. **The problem is roast-specific wetting.** Dense light roasts resist saturation → longer/softer PI. Degassing dark roasts saturate fast and over-soften → shorter PI, never bloom. The same PI extension that rescues one harms the other (Decent "All about Blooming espresso" — dark-roast contraindication).

**Reach for grind/ratio/dose/temp when:** the whole cup is off in one direction with a clean trace; the complaint is strength/concentration; the bitterness is roasty (temperature); or any time the prep gate is failing.

**The compensation ceiling (rail, unchanged):** pre-infusion buys margin against *small* distribution noise — that is real and is most of why it works. It cannot rescue bad distribution, clumping grinders, or damaged baskets. Operationalize the ceiling as: **a channel signature that recurs after two shots on a gentled profile is a prep/equipment fault, and every further profile softening to hide it is a crutch.** `trace_choke_then_gush` stays `class: prep`, priority 1 routes to prep, and `diag_pi_recurrent_channeling` only fires with prep explicitly verified.

**Lever ordering by goal (PQ5):**
- *Balance/strength goals:* prep → grind/ratio/temp → profile last (only to clean residual trace faults).
- *Body/texture/finish goals:* prep → get balance acceptable on extraction levers → **profile before further grind/ratio moves**, because pushing grind for texture shifts the balance you just set.
- *Forgiveness/variance goals:* prep → PI length/softness → peak cap (≤8 bar) → ramp speed → only then grind.
- Interaction to encode with every archetype switch: longer PI and lower peak pressures generally need a **finer** grind to hold total resistance; allongé needs a **coarser** one. A profile move is rarely grind-neutral — the diagnostics keep changes single by treating the paired grind step as the *next* shot's move if the falsifier fires.

## 2. Reading the curve (PQ2)
Seven profile-class signatures shipped in `trace_signatures`, each with the frame move it implies and the fault-class boundary (profile vs grind vs prep) stated in `next_change`. Two deserve emphasis:
- `trace_pressure_spike_ramp` is the DE1's most common self-inflicted wound: in flow mode, pressure responds to roughly the **square** of puck-resistance changes (Gagné, "An Espresso Profile that Adapts to your Grind Size"), so a slightly fine grind detonates past 10 bar → secondary compression → muted cup (Decent docs, "Pressure vs. flow profiling"). The fix is a stop-limit or pressure-frame conversion, not a grind panic.
- `trace_flow_never_caps` and `trace_fast_pressure_bleed` are *exoneration* signatures — they let the engine detect from the trace that the profile is fine and the resistance is wrong, which prevents the opposite failure mode of this whole project (profile-tunneling).
- `trace_clean_decline` encodes the reference "good" shape so the engine can say "the profile ran as designed; look elsewhere."

## 3. Pre-infusion science (PQ3)
What's defensible: low-pressure/low-flow saturation before full pressure reduces preferential pathways and channeling — mechanistically supported (Lee et al. 2023 on uneven-extraction physics; Waszkiewicz et al. 2026 on poroelastic compaction), universally held by named authorities (BH Æ 6.08; Rao: best PI = high flow shifting to low flow with modest pressure buildup), and directly falsifiable per shot from the post-PI trace. Encoded as the strongest profile edge (`edge_pi_evenness`, contested:false).

What's honest but messier: PI's *taste* effects. McKeon Aloe's paired-shot data (single rig, unblinded — filed T3) found longer PI → **more sweetness, less syrupy mouthfeel, slightly lower TDS/EY** — partially contradicting the folk claim that PI adds body. Both directions ship contested, and `diag_profile_thin_body`'s falsifier tells the assistant to shorten the PI component first if body *drops*.

Roast conditioning is first-class throughout: light = dense/hard-to-wet → extend/soften PI (the brief's flagship diagnostic, `diag_pi_light_sour_gush`: "sour + early-gush + light roast → saturate before touching grind"); dark = porous/degassing → shorten PI, bloom contraindicated (`diag_pi_dark_bitter_gush`, `edge_pi_dark_gush_bitter`).

## 4. Archetypes (PQ4)
Mapped in `goals` with roast-conditioned paths, all expressible as Recipe Editor frames:
- **Flat 9-bar** — the baseline; its characteristic failure (late flow rise → harsh tail) is what the declining tail fixes.
- **Declining pressure (lever emulation)** — default for body/rounder goals and for dark roasts (fastest-eroding pucks). Body claim: contested T2/T3.
- **"Best overall" (PI → ~9 → decline)** — the default recommendation for "smoother" with no named fault; matches Rao's 3-phase shape and Decent's shipped default.
- **Blooming (PI → 30 s pause → ramp)** — max evenness/extraction on excellent light/medium roasts; hardest to dial; **never on dark** (Buckman, explicit).
- **Allongé / flow-capped fast-flow** — clarity/fruit on ultralight; trades body away, so never offered for a body goal; easiest to dial (Decent diaspora).
- **Low-pressure flat (~6 bar) + long PI** — Colonna-Dashwood's channeling-mitigation practice; feeds `goal_shot_forgiveness`.
- Roast-flip example the brief asked for: *"tame the brightness"* on light = extend PI / switch archetype; the same sensory word on dark = under-extraction → grind/temp, and extending PI would make it worse. Encoded in `goal_tame_bright_light.roast_conditioned`.

## 5. Interactions and honest limits (PQ5)
Eight `causal_edges` for flow_profile/preinfusion → {evenness, clarity, body, sweetness}, each carrying its interaction list (grind coupling, roast flips, the prep ceiling). The best-grounded quantitative anchor: **flow through a puck peaks near ~8 bar and falls above it** — Petracco in Illy & Viani (2005), replicated on the DE1 itself by John Weiss, now explained as poroelastic compaction (Waszkiewicz et al., *Physics of Fluids* 2026), and consistent with Andueza et al. 2002 (11 atm shots scored sensorially worst; 9 atm best). That chain is why "cap the peak ≤8–9 bar" rows carry the KB's only near-T1 profile confidence.

## 6. Body & mouthfeel (PQ6) — sorted honestly
- **Grounded science:** essentially none directly links dynamic profiles to tactile body. Peer-reviewed pressure work is *static* (Andueza 2002: pressure affects foam/texture attributes across 7/9/11 atm — the closest T1 gets).
- **Credible practitioner consensus (T2):** decline profiles read "syrupy/lever-like"; capping spikes restores texture lost to channeling (Rao, BH, Gagné). Plausible mechanisms, unblinded.
- **Folklore (T3, shipped contested):** "pre-infusion adds body" — directly challenged by McKeon's data; "lever profiles = body" is confounded with the finer grind those profiles permit. The KB never promises body from a profile move; it proposes an A/B with a falsifier and a ratio fallback.

## 7. Skeptic pass (PQ7)
The strongest published skeptic is inside the canon: Perger's "Keep It Simple" (Barista Hustle) lists pressure profiling among features demanded "without evidence of necessity" and ranks pump-pressure changes far below basics. The engine should inherit that humility: profile diagnostics sit at priority 2–3, never 1; the prep gate is untouched; and the grind-first instinct the audit flagged is *rebalanced, not inverted*. Tier discipline in the rows: mechanism/evenness claims → T1/T2, contested:false where the physics chain holds; archetype→taste maps → T2/T3, contested:true throughout; anything Decent-diaspora-only → T3. No profile row masquerades above its evidence.

## 8. What we could NOT ground
- **No peer-reviewed, blinded sensory test of any dynamic profile archetype exists** (declining, blooming, allongé, flow-profiled). The entire archetype→taste layer is practitioner consensus. Ship it contested.
- **Pre-infusion's taste effects** (vs its evenness effects) rest on one named unblinded dataset plus community consensus — and they disagree on mouthfeel direction.
- **"Harshness comes from channels, not high extraction"** (Rao) and **"channels pass astringent undissolved particles"** (Gagné) are compelling, coherent hypotheses — not demonstrated. Rows leaning on them say so in provenance notes.
- **Exact numbers** (30 s bloom, 4-bar PI exit, 5–6 bar decline floors) are Decent-diaspora starting points, not optima; every row phrases them as starting frames with taste-based falsifiers, per R1–R3.
- Peer-reviewed flow/pressure work that *does* exist (Cameron 2020; Lee 2023; Waszkiewicz 2026; Andueza 2002/2003) grounds mechanisms and the ≤8–9 bar cap — it does not adjudicate between archetypes.

## 9. Merge notes for engineering
1. Rows are byte-compatible with the §1 schema; remap ID prefixes if house style differs.
2. `symptom_descriptor_id: "muted"` (diag_profile_muted_spike) may need a lexicon descriptor if absent; everything else uses base descriptors.
3. Diagnostic `conditions.trace` references the new trace IDs — confirm the engine matches trace conditions by ID (as the prep-class rows presumably do).
4. Persona rebalance (from PQ1): add to the system prompt the one-liner — *"Reach for the profile when the fault lives in part of the shot, the trace disagrees with the taste, the goal is texture or forgiveness, or grind is pinned; reach for grind when the whole cup is off and the trace is clean; reach for prep before either."*
5. `diag_flow_never_caps_regrind` intentionally outputs a grind change from a profile trace — keep it; it is the guard against over-rotating into profile-tunneling.
