---
paths:
  - "src/barista/**"
  - "qml/assistant/**"
  - "src/history/coffeebagstorage.*"
  - "src/network/beanbase*"
  - "src/ai/aimanager.*"
---
This subsystem is part of the **feat/barista fork** — the barista AI assistant and the bean/recipe/voice
features it needs. `src/barista/**` and `qml/assistant/**` are fork-exclusive; the bean/recipe/AI seams
(`coffeebagstorage`, `beanbase*`, `aimanager`) are **upstream** files the fork extends — edit them
carefully (they conflict on re-sync; see `.fork/MERGE.md`).

Before building any barista / bean / recipe / voice / AI-extraction capability, grep the reuse index —
much of it already exists (e.g. `add_bag`, `look_up_bean`, `AIManager::extractCoffeeBagDetails`):

    rg '<capability>' .fork/INDEX.tsv

Public fork capabilities carry a `[fork-index]` marker at their definition site; adding a public tool,
storage/seam entry point, or extraction entry point includes adding its marker. `.fork/INDEX.tsv` is
generated from those markers. Decision rationale lives in OpenSpec under the change id named in the
marker — do not restate it in the marker or the index. Full contract: `.fork/FORK.md`.
