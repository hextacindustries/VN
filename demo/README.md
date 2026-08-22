# Demo content

All narrative content in this directory is **original**. Character
names, dialogue, setting and plot are written for this project.

`docs/research.md` studies *Fate/stay night* and other landmark visual
novels at length, but strictly as **structural and technical
reference** — how routes are gated, how saves snapshot interpreter
state, how a bad end is tracked as a first-class content node, and what
a million-word script demands of a format. What we take from those
games is *architecture and craft standard*, never their characters,
names, prose or story.

Concretely, the demo must never ship:

- character names, place names or organisation names from an existing work
- dialogue, narration or plot beats lifted or paraphrased from one
- art, music or UI assets derived from an existing title

Structural mechanics are a different matter and are fair to implement:
route locking, affection counters compared at thresholds, numbered bad
endings with an in-fiction recovery interstitial, and a navigable scene
flowchart are all general VN design patterns, documented across many
games and reimplemented freely throughout the medium.

## Layout

```
assets/    generated fonts and packed art
src/       demo scene code (temporary: replaced by compiled script once
           the DSL and VM land)
```
