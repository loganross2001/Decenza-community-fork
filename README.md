# Decenza — community exploratory fork

> ⚠️ **Unofficial fork — this is _not_ the original Decenza.**
>
> All credit for Decenza belongs to **[Kulitorum](https://github.com/Kulitorum)**, the original
> author. This fork stands entirely on their work. For the real project — the full description,
> screenshots, downloads, and build/usage instructions — please go to the original:
>
> ## 👉 Original project: **https://github.com/Kulitorum/Decenza**
>
> This fork is **not affiliated with or endorsed by** the original author and is **not a
> competing project**. I like Decenza and simply wanted to try out a few helpful features and
> share them back, in the hope they might be useful enough to be folded into the original. The
> author is welcome to adopt, adapt, or ignore any of it freely. 🙏

---

## Why this fork exists

A place to explore a small set of **"dial-by-weight"** features on top of Decenza. They've been
offered upstream as a pull request — [Kulitorum/Decenza#1348](https://github.com/Kulitorum/Decenza/pull/1348) —
and this fork just makes them easy to browse and try.

## What this adds (rebased onto the latest upstream `main`)

- **Weight auto-capture** for beans and milk — rest it on the scale, it captures the weight and plays a soft "ding"
- **Weight-timed steaming** — steam time scales proportionally to the measured milk weight, and the DE1 auto-stops there *(concept inspired by [DSx2](https://github.com/Damian-AU/DSx2) — see Acknowledgements)*
- **Capture "ding"** sound — pre-loaded, independent of the accessibility setting
- **Steam-page polish** — purge button, weigh buttons, 0.1 g inputs, scrollable editor
- **Reworked home screen** — three setup shortcuts + a high-contrast bottom stat bar (opinionated; least "drop-in")
- **Quick-select brew ratio** — tap the home-screen ratio to pick Ristretto / Normale / Lungo (with editable preset values in Espresso Setup); style descriptions adapted from [La Marzocco Home](https://home.lamarzoccousa.com/brew-ratios-around-world/)
- **Save last steam session** — record the last milk weight + steam time and adopt them as a pitcher's reference baseline in steam setup
- **Icon-led top status bar** — DE1 status, group/steam temp, scale, and battery icons; **Sleep moved to a clear top-centre button** (no more accidental sleeps where the bottom-left Back button lives)

**Write-ups:** [`fork-notes/SUMMARY.md`](fork-notes/SUMMARY.md) ·
[`fork-notes/DETAILS.md`](fork-notes/DETAILS.md) (design rationale + gotchas) ·
[`fork-notes/changes-vs-1.8.0.patch`](fork-notes/changes-vs-1.8.0.patch) (the isolated diff).

## Acknowledgements & inspiration 🙏

This fork stands on the work and ideas of others, with gratitude:

- **[Kulitorum](https://github.com/Kulitorum)** — the original **[Decenza](https://github.com/Kulitorum/Decenza)** that this fork is built on. All credit for the app itself belongs here.
- **[Damian (Damian-AU)](https://github.com/Damian-AU)** and the **[DSx2 skin](https://github.com/Damian-AU/DSx2)** — the **weight-timed steaming** idea (scaling steam time to the measured milk weight so the machine auto-stops at the right foam point) was **inspired by DSx2's behaviour**. Sincere thanks and appreciation for pioneering the concept. The implementation here is original — **no DSx2 code was used or copied** — but the inspiration is theirs, and DSx2 is well worth checking out: https://github.com/Damian-AU/DSx2

## Building & using

This is the Decenza codebase (rebased onto the latest upstream `main`; the foundation PR has already been merged upstream). For build and usage instructions,
please follow the **original project**: https://github.com/Kulitorum/Decenza — this fork
deliberately does not reproduce the original's description or screenshots.

## License

Unchanged from the original — see [`LICENSE`](LICENSE). These files were modified from the
original on 2026-06-19; the modifications are offered under that same license.
