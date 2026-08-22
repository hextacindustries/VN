# Cross-Platform Visual Novel Engine — Research & Architecture Plan

> **Status:** DRAFT — research agents in flight (PC-98 production techniques, FSN/VN script
> architecture, cross-platform target matrix). Sections marked ⏳ pending research.

## Step 0 — Install the `gauntlet-loop` skill ✅ DONE (commit `17f69ef`, pushed)

Installed to `.claude/skills/gauntlet-loop/` as `SKILL.md` (byte-identical to upstream) +
`LICENSE` + `NOTICE.md`. Upstream's `assets/` is a README banner at repo root, not skill
content, so it was not vendored. Active on next session start.

<details><summary>Original plan</summary>

Source: <https://github.com/robonuggets/gauntlet-loop> — CC BY 4.0, by Jay E (RoboNuggets);
technique by Matt Shumer. It is a genuine Claude Code skill, not a plugin.

Upstream layout is `.claude/skills/gauntlet-loop/SKILL.md` (+ `assets/`), so it installs by copy:

```bash
git clone --depth 1 https://github.com/robonuggets/gauntlet-loop /tmp/.../gauntlet-loop
mkdir -p /home/user/VN/.claude/skills
cp -r /tmp/.../gauntlet-loop/.claude/skills/gauntlet-loop /home/user/VN/.claude/skills/
cp /tmp/.../gauntlet-loop/LICENSE /home/user/VN/.claude/skills/gauntlet-loop/LICENSE   # CC BY 4.0 attribution
```

Then commit to `claude/gauntlet-loop-skill-install-czeily`. Verify by confirming
`.claude/skills/gauntlet-loop/SKILL.md` has valid YAML frontmatter and that `/gauntlet-loop`
appears in the skill list on next session start.

**What it does:** turns a goal into one paste-ready prompt that makes an agent (1) pick a *named,
fetchable, comparable* quality bar, (2) split work into small pieces, (3) run a builder and a
separate harsh critic with fresh context on each, (4) compare blind against the bar, (5) loop with
no round limit until the critic picks our work over the bar. I read SKILL.md before proposing this
— it is a prompt-engineering workflow with no tool escalation or network side effects.

**Why it fits this project:** "FSN is the gold standard" is already a named, fetchable, comparable
bar. This skill is a natural driver for the demo script and for the PC-98 art pass, where
"is this actually good enough" is otherwise unfalsifiable.

</details>

---

## Context

`hextacindustries/vn` is an empty repository. The goal is a **new visual novel engine**, built
from scratch, with two intertwined requirements:

1. **Radical portability.** One engine that genuinely runs on desktop PC, Raspberry Pi, web
   browsers, and **smartwatches** (Wear OS + watchOS) — confirmed as a hard ship target, not a
   stretch goal. This constraint is the architectural driver: it eliminates every existing VN
   engine (Ren'Py, KiriKiri, Godot, Love2D, TyranoBuilder) because each carries a runtime far too
   heavy for a watch, and it forces a small dependency-free core.

2. **FSN-class narrative machinery.** *Fate/stay night* is the stated gold standard: three
   route-locked branches, dozens of bad endings with an in-fiction recovery mechanic, persistent
   cross-playthrough unlock state, and roughly a million words of script. The **demo** does not
   need that word count — it needs a vertical slice that exercises every one of those mechanics
   end to end, proving the engine could carry an FSN-sized script.

The demo's presentation target is the **NEC PC-9801 aesthetic**: 640×400, 16 colors from a 4096
palette, hand-dithered art, FM-synth music. This is not only nostalgia — it is a genuine
engineering asset. A 4-bit indexed framebuffer at 640×400 is 128 KB, software-renderable at full
speed on any device in the matrix including a watch, and FM synthesis produces music measured in
kilobytes rather than megabytes. **The retro aesthetic and the portability requirement are the
same decision.**

### Confirmed direction (from user)

| Question | Decision |
|---|---|
| Smartwatch target | **Must actually ship on watch** — portable core, software renderer, thin native shells |
| Demo scope | **Engine proves the mechanics** — short vertical slice exercising every FSN-class feature |
| Script format | **Custom DSL → bytecode VM** — compact, snapshot-able, portable |
| Text / i18n | **English-first, i18n-ready** — all strings in external locale tables, CJK addable later |

---

## Part 1 — Research findings: PC-98 era production ✅

### 1.1 Display geometry — and the pixel-aspect question, answered honestly

| Parameter | Value |
|---|---|
| Standard mode | **640×400, 16 colors, 4 bitplanes** |
| Text cells | **8×16** hankaku (ASCII) · **16×16** zenkaku (kanji) → 80×25 grid |
| Horizontal scan | **24.83 kHz** (400-line); 15.98 kHz (200-line); 31.47 kHz on PC-9821 |
| Vertical refresh | **≈56.4 Hz** |
| Scanning | **Non-interlaced / progressive** in standard 400-line mode |
| GDC | Dual NEC µPD7220 (one text, one graphics), 2.5 or 5 MHz |

**Pixel aspect ratio is genuinely contested — decide deliberately.** 640×400 is 16:10, and the
signal/design intent was **square pixels** (DOSBox-X's maintainers: PC-98 "was designed to display
a 640×400 image at its native 16:10 aspect ratio," unlike IBM PCs). But most period CRTs were 4:3
tubes, which stretches the image vertically ~20% (PAR = 1.333/1.6 = **0.8333**).
**Decision: author and render at 1:1 square pixels — that is what artists saw in Multi Paint —
and offer an optional 1.2× vertical stretch as a "period CRT" toggle.**

**Scanlines: my premise was right, and this matters.** At 640×400 progressive there is no
240p-style black-gap scanline structure. The authentic look is a dense, flat, *digital* raster.
**Do not ship a scanline overlay** — it is the single most common way modern "PC-98 style" work
gives itself away as inauthentic.

### 1.2 Color

- **16 simultaneous colors**, indexed by 4 bitplanes. **Analog palette = 4 bits/channel = 12-bit
  RGB = 4096 total.** (Several web sources claim "64 steps per channel" — that is wrong; 4096 = 16³.)
- Confirmed in shipping code: palette ports `0xA8` (index), `0xAA` (G), `0xAC` (R), `0xAE` (B),
  `0x6A` (analog-mode enable). **4-bit → 8-bit expansion is `value × 17`** — confirmed in
  AliceSoft's VSP decoder, so this is not a guess.
- Earlier machines: 8-color digital (3-bit). Later PC-9821 PEGC: 256 colors — a *distinct, later,
  less PC-98-looking* style. **16 colors is our target.**

### 1.3 Planar VRAM — the reason the UI looks the way it does

| Plane | Address | Channel |
|---|---|---|
| 0 | `0xA8000` | B |
| 1 | `0xB0000` | R |
| 2 | `0xB8000` | G |
| 3 | `0xE0000` | I (intensity) |

Each plane is 640×400 bits = **32,000 B**; 4 planes = **128,000 B/page**; 2 pages = 256 KB VRAM.

**One byte = 8 horizontal pixels in one plane.** Every fill, blit and window frame is cheapest on
**8-pixel horizontal boundaries** — which is *why* PC-98 message-window frames and CG blit regions
align to multiples of 8. This is a visual tell, not merely an optimization, and we should honor it.

Accelerators: **GRCG** (ports `0x7C` mode, `0x7E` tile; write one byte, hardware fans it across up
to 4 planes with a per-plane mask) made solid fills and window clears fast. **EGC** (PC-9801VX,
1986) added raster ops, bit-block transfer and a barrel shifter so VRAM→VRAM copies escaped 8-px
alignment — the standard sprite/scroll/window-restore path.

Text is a **separate hardware plane composited over graphics** (chars at `0xA0000`, attributes at
`0xA2000`; kanji font ROM ~128 KB holding 2,965 JIS L1 kanji + 611 symbols at 16×16). Games chose
between the free-but-grid-locked hardware text layer and **blitting glyphs into graphics VRAM** —
the latter costs more but lets text sit inside a drawn frame and participate in fades. Most
commercial ADV message windows did the latter. **We do the same: text is rendered into the
framebuffer, not composited by a separate layer.**

### 1.4 The number that explains everything

**One uncompressed 640×400×16-color screen is exactly 128,000 bytes** → ~9 CGs per 1.2 MB floppy.
A 200-CG VN would have needed 25+ floppies. *Every* image format below exists to beat that number,
and DOS's 640 KB conventional-memory ceiling explains the rest of the architecture.

### 1.5 Art technique — what actually makes the look

The constraint chain: 16 colors + 12-bit palette + no per-pixel alpha ⇒ **all tonal range comes
from spatial patterning, and the whole image shares one 16-entry palette.** Because the palette is
per-image, **each CG defines its own color mood** — PC-98 art reads as a series of distinct moods
rather than a unified game palette.

- **Dithering is structured, never noisy.** Checkerboard 2×2 (50%) is the workhorse; ordered/Bayer
  2×2, 4×4, 8×8 for skies and large gradients. The hallmark of *hand* dithering is **graded
  density** — artists vary 25%/50%/75% along a form so two colors read as a four-step ramp, and
  taper density toward an edge. **Floyd–Steinberg error diffusion is the wrong look** (noisy,
  non-repeating speckle).
- **Anti-aliasing is manual, sparse, asymmetric.** Line art is a hard 1-px contour in a *dark hue
  of the local color*, rarely pure black. AA is usually a **single** intermediate pixel drawn from
  the existing ramp — there is no palette budget for dedicated AA colors. Hence the crisp,
  cut-out, "digital" edges.
- **Skin:** 3–5 step ramp, dithering only between adjacent steps, hard cel terminator, and shadows
  **hue-shifted toward magenta/purple** rather than desaturated.
- **Hair:** hard-edged specular band ("angel ring") 1–3 px wide, solid and **un-dithered**; strands
  as 1-px dark lines over the mid tone.
- **Highlights** on eyes/lips are solid near-white blobs with no AA — "wet gloss" costs one palette
  slot and reads as high production value.
- The heavy **cyan/magenta/purple/teal** presence is real, and falls out of 12-bit quantization
  making saturated secondaries cheap plus needing a mid-value that dithers against both skin and
  hair. *(Observable across the corpus; not a documented doctrine — UNVERIFIED as explicit intent.)*

**Tools — with corrections to my own assumptions.** **Multi Paint** (C-Lab, 1992, ¥14,800) was the
de-facto standard, native format MAG; **Z's STAFF KID98** the other common one (ZIM).
**"Dante 2" is *RPG Tsukūru Dante98 II*, an RPG construction tool, not a paint program.**
**MASL and Mariko: UNVERIFIED — do not cite.** **PiCTOR `.PIC` is an American format** (John
Bridges, GRASP) and must not be conflated with the Japanese Pic.

### 1.6 Image formats worth stealing from

- **MAG / MAKIchan Graphics** (`MAKI02`, by Woddy_RINN) — the BBS-standard and Multi Paint's native
  format. 2-D LZSS-ish: **flag A** omits 4-pixel units identical to the previous scanline; **flag
  B** is a 4-bit code into ~15 fixed relative 2-D copy offsets; novel units stored raw, XOR for
  near-matches.
- **Pi** — algorithmically superior to MAG and explicitly "used by Japanese software houses who
  needed the most efficient available image compression to fit games on fewer diskettes." Touhou
  PC-98 links "Pi loader by BERO."
- **VSP (AliceSoft) is the best-documented and most instructive.** Hard-bounded to 640×400×16
  (`x ≤ 80` in 8-px units, `y ≤ 400`). Palette = 16 × 3 bytes **BGR**, nibble **×17**. Pixels are
  **4 bitplanes stored column-major in 8-px-wide columns**. Opcodes include copy-from-previous-
  column, RLE, and — the clever part — **`0x03`/`0x04`/`0x05` copy from plane 0/1/2 optionally
  XORed with a mask**, exploiting the fact that *in dithered 16-color art the bitplanes are highly
  correlated*. **This is a compression trick specific to dithered planar art and we should
  implement it** — it is directly relevant to our 75 MB watch budget.

### 1.7 Sound

| | PC-9801-26K | PC-9801-86 |
|---|---|---|
| Chip | **YM2203 (OPN)** | **YM2608 (OPNA)** |
| FM | 3 ch × 4 op | **6 ch × 4 op** |
| SSG | 3 ch + noise | 3 ch + noise |
| Rhythm | — | **6 ch from ROM samples** |
| ADPCM | — | **1 ch, up to 256 KB** |
| Output | **mono** | **stereo** |

- 26K era: 6 total voices → lead + bass + one accompaniment, SSG faking chords via fast arpeggios
  and noise percussion. The dense, busy writing style is a direct artifact of the voice count.
- 86/OPNA era: percussion moves to rhythm ROM, freeing SSG for melody; ADPCM gives **one** real
  sample voice.
- **The 256 KB ADPCM budget is why floppy-era PC-98 VNs are essentially unvoiced** — at ~8 KB/sec
  that is ~30 seconds of resident audio. Full voice arrives only with CD-ROM (Policenauts, 1994).
  *(Caveat: some -86 revisions may have omitted ADPCM buffer RAM — the doujin "ちびおと" add-on
  board exists to add it. Don't assume every board had 256 KB.)*
- **PMD (Professional Music Driver, KAJA)** was the dominant driver: **MML text → MC.EXE → binary
  → resident TSR** streaming register writes on a timer IRQ. Extensions **`.M`** (OPN), **`.M2`**
  (OPNA), **`.MZ`** (PPZ8). It mattered because it was consumer-priced and consumer-licensed, so
  small studios shipped commercial games without writing their own driver — and it is why PC-98
  soundtracks survive as *data* rather than only recordings. **Our music pipeline should mirror
  this shape: MML-ish source → compiled register-event stream → tiny resident player.**

### 1.8 How the games were actually built

- Hand-written 16-bit x86 for anything touching VRAM/GRCG/EGC; **Borland Turbo C/C++** for the rest
  (ReC98 establishes Turbo C++ 4.0J for the PC-98 Touhou games).
- **master.lib** (Koizuka et al., all assembly, C/Pascal linkage, open source) was the de-facto
  standard runtime — **74% of the code in TH05's OP.EXE**. Closest thing to a documented "standard
  PC-98 game runtime."
- **AliceSoft "System" is the standout precedent for *our* design** — explicitly "a series of
  OS/VM hybrids to cope with porting their games across different computer platforms" (PC-88,
  PC-98, MSX, X68000, FM Towns, DOS, Windows 3.1), shipped commercially from **1989**. *A
  cross-platform bytecode VM for VNs is not a new idea; it is the proven one.* System 1 used a
  literal book metaphor — "pages" containing "verbs" and "objects." Files: `.ALD` archives holding
  `.SCO` bytecode, `.AIN` metadata, VSP/PMS/QNT images.
- **The ADV script language** (documented via the `xsys35c` decompiler) is the most concrete
  surviving picture of a period VN script, and its choices are worth copying:
  - Text in single quotes; `R` = line break; `A` = wait for input.
  - Menus: `$label$'option text'$ … ]`.
  - **Variables are 16-bit unsigned, 0–65535, with *saturating* arithmetic** — addition/
    multiplication clamp at 65535, subtraction at 0. **No wraparound.** Exactly right for affection
    counters and flags, and we should copy this.
  - Jumps `@label:`, subroutine `\label:`/`\0:`, and **cross-file page jumps `&#page.adv:` /
    calls `%#page.adv:`** — the mechanism that let a scenario span many small files so only the
    current chapter stayed resident in 640 KB. *This is the ancestor of our chunked loading.*
  - **Routes are emergent from conditionals on a flat flag array — there is no "route" concept in
    the engine.** Save = variable array + current page/label + call stack.

### 1.9 Landmarks that matter to our design

- **YU-NO (élf, 1996)** — the important one. **A.D.M.S.** made the branch graph a *diegetic,
  navigable UI object* (reading right-to-left). The **jewel save system**: the in-fiction Reflector
  is powered by jewels; spending one drops a **bookmark node onto the divergence map** that can be
  warped to later, recovering the jewel on return — and conventional multi-slot saving is
  deliberately restricted, turning save-scumming into a resource-managed traversal puzzle.
  **Engine consequence: persist not just current flags but a set of full restorable world-states
  keyed to nodes in an explicit authored graph, plus completion tracking over that graph.** This is
  materially different from the flag-array model and is the direct ancestor of 999's flowchart.
- **EVE Burst Error (C's Ware, 1995)** — two protagonist scenarios running *simultaneously*, player
  switches at will, progress in one gates the other. Requires **two independent program counters +
  flag sets + cross-scenario gating**. Harder than a branch tree; a good stress test for our VM.
- **Doukyuusei (élf, 1992)** — a **simulation loop with a time/place state machine** gating scene
  scripts, not a branching tree at all. Different engine shape worth not painting ourselves out of.
- **Shizuku (Leaf, 1996-01-26)** — **the origin of the term "visual novel"**: Leaf chose "visual"
  because "sound novel" was Chunsoft's trademark. *Kizuato* (1996) then *To Heart* (1997) followed.
- **Corrections:** there was **no 1988 PC-9801 Snatcher** (PC-8801mkII SR and MSX2 only) —
  treat "PC-98 Snatcher" as false. **No evidence for a C's Ware "LIPS" engine** — I made that name
  up in my prompt; do not cite it. **To Heart's PC-98 build is UNVERIFIED** (sources conflict with
  Windows 95/98).

### 1.10 Reference material for implementation

Neko Project II / **np2kai** (actively maintained, ports to Linux/Win/macOS/Emscripten/Android/
iOS/RPi/libretro) and **np2debug** (breakpoints, Shift-JIS memory view) for reverse-engineering.
**DOSBox-X** PC-98 mode has explicit GRCG/EGC toggles. Documentation: *Undocumented 9801/9821*
Vol.1–2, FreeBSD(98)'s `pc98-arch.html` and `gdc.c`, **ReC98** (deepest English-language PC-98
hardware writing), and **master.lib**'s source + `doc/master.txt`.

## Part 2 — Research findings: FSN & landmark VN script architecture ✅

### 2.1 Fate/stay night runs on KiriKiri2 + KAG3 + TJS2 — confirmed

Four layers: **KiriKiri2** (C++ — layer compositor, XP3 archive VFS, audio, *and* the TJS2 bytecode
VM), **TJS2** (a JS-like language), **KAG3** (an ADV framework written *in* TJS2), and **KAGParser**
(the `.ks` tokenizer, which is **native C++, not TJS**). That last split matters: the tag language
is parsed by a small explicit machine, which is precisely what makes its state serializable.

FSN's own script idiom, from the Baka-Tsuki translation instructions:

```
*page120|
@textoff
@bg storage="bg_10a" rule="crossfade" time=800
@texton
　　"...Emiya-kun."[l][r]
@pg
*page121|
```

**Every text page carries a label with a caption (`*label|caption`), and in KAG a captioned label
is a save point.** That is how a 2004 engine gets "save anywhere" without arbitrary mid-statement
snapshots — it doesn't. It puts an anchor on every page. The same label is simultaneously the save
key, the read-tracking key, and the jump key.

Two more KAG facts worth stealing outright:
- **Patches are a stacked overlay VFS.** `patch.xp3`, `patch_lang_english.xp3` etc. mount over the
  base archives and shadow files by path. Both the official English release and the fan TL use it.
  You never rebuild base data. This is the most translation-friendly feature of any engine surveyed.
- **`[backlay]` → mutate the back page → `[trans]` → `[wt]`.** KAG keeps two full layer pages, fore
  and back; you mutate the invisible one, then cross-dissolve. Every visual change is atomic, and
  **skip mode becomes trivially correct** (run transitions with `time=0`).

### 2.2 Scale — give it a budget, not a trivia answer

Word-count figures for FSN are genuinely contested (~750k–1M English words; a claimed 3.85M JP
characters is implausible). **All UNVERIFIED, ±30%.** What is *not* in doubt, and is what actually
constrains the engine: **a few megabytes of prose across hundreds of script files, tens of thousands
of individually-labelled pages, and a read-flag registry with one entry per page.**

That last item is the one that bites. KAG stores read-flags as one system variable per label
(`sf.trail_first_start`), producing a dictionary with tens of thousands of entries serialized into a
single file loaded at startup. **RealLive's approach is dramatically better: a `dynamic_bitset` per
scenario, one bit per bytecode position.** We use the bitset.

### 2.3 Route locking and the gating model

Structure: shared Prologue → **Fate** → **UBW** → **Heaven's Feel**, railroaded in that order.
5 real endings; **40 Tiger Dojo segments** as the bad-end set. Clearing Fate makes new choices
*appear* in the shared early section that route you to UBW; clearing UBW unlocks HF.

KAG gives exactly two scopes, and the gate is necessarily built from them:

- **`f.*`** — game flags, serialized *into each save*. Correct for per-playthrough counters.
- **`sf.*`** — system variables, one profile-global store outside saves, shared by all slots.
  Correct for route-cleared gates, gallery, read-text.

```
[if exp="sf.fate_cleared"]
  [link target=*ubw_branch]…stop her.[endlink]
[endif]
```

**Route unlocks live in profile-global scope; route progress lives in save-local scope.** This is
the single most transferable structural fact in the whole report.

FSN's parameter system is **threshold-on-accumulator**, not a graph of explicit flags: hidden
Saber/Rin counters incremented ±1 by choices, compared at fixed checkpoints (UBW: ≥4 Saber and <8
Rin → Good End; >8 Rin → True End). Points stop accruing after certain days. *(Exact variable names
UNVERIFIED.)*

**Tiger Dojo is the mechanic worth copying.** Each bad end is numbered, explains what you did wrong,
hints at which earlier choice to change, and unlocks a gallery entry. It is **not** an auto-rewind —
the player still loads a save. Design consequence: **a bad end is a first-class, numbered,
gallery-tracked content node**, not a failure state. The engine wants `ending_id` as a persistent
enum, a completion registry, and an "on ending reached" hook that routes into an interstitial.

### 2.4 How KiriKiri actually saves — the central engine answer

`KAGWindow.internalStoreFlags()` builds a dictionary containing message-layer contents and geometry
(fore *and* back), character and background layers, current storage/label/page, the entire `f.`
dictionary, **the Conductor's execution position, call stack, and macro table**, audio state, and
message history. `KAGParser::Store()` (C++) serializes macro definitions, the macro-argument stack,
and the complete call stack — each frame holding storage name, label, line offset, *current line
buffer contents and cursor position*, and conditional state (`ExcludeLevel`, `IfLevel`).

**The scenario is never re-executed. State is reconstructed directly and execution resumes at the
exact parser position.**

> This works because the parser position is itself a value. KAG can save at an arbitrary tag because
> the interpreter is a small explicit machine — file, line, char offset, call stack, if-level,
> macro-arg stack — with **no hidden host-language stack**. *The moment you let script call into a
> host coroutine that yields mid-frame, save-anywhere dies.*

Known cost, straight from the KAG docs: *"Save/load always uses current snapshots, causing potential
incompatibilities when scenario files change — save slots may become inaccessible."* **Snapshot
saves are version-brittle. Budget for it.**

KAG also keeps **two distinct mechanisms** we should keep distinct too:
- **text backlog** — append-only list of rendered strings + voice handles (cheap, clickable replay)
- **state rollback** ("Go Back") — a *ring buffer of full snapshots*, bounded, keyed to user-
  interaction boundaries (`[l]`/`[p]`/`[s]`)

### 2.5 The other engines, and what each contributes

**NScripter** (Tsukihime, Higurashi, Umineko) — BASIC-flavoured, `%` int / `$` string / `?` array
sigils, `*labels`, `if`/`goto`/`gosub`, **no OR operator at all**. Primitive, and it shipped
legendary games because its *presentation* primitives were exactly right. Its save model is a third
strategy: **checkpoint, not save-anywhere** — snapshots taken automatically at the start of each
displayed sentence, with `saveon`/`saveoff`/`savepoint` for author control. Players never notice.
**ONScripter** (GPL, C++/SDL, ports to Linux/Win/macOS/iOS/Android/PSP/Wii/Dreamcast/Switch) is why
the fan-TL scene standardised here — portability drove adoption.

**Higurashi and Umineko are kinetic novels — zero choices, zero branching.** Their entire narrative
innovation (same events, different lens) comes from *ordering standalone script files*, not flags.
**If our engine makes a chapter cheap to ship as an independent, separately-selectable script with
its own completion flag, we have covered both franchises completely.**

**RealLive / rlvm** (Key: Kanon, Air, Clannad) — the compiled-bytecode counterexample. Kepago source
→ `seenXXXX` bytecode in a `SEEN.TXT` archive. Memory model is explicitly banked: `intA–intF`
(2,000 each, **save-local**), `intG`/`intZ` (**profile-global**), `intL` (40, **stack-local** for
parameters), `strS` local / `strM` global. Checkpoints are **declared by the scenario header itself**
(`savepoint_message_`, `savepoint_selcom_`, `savepoint_seentop_`). And the trick worth stealing:
**a savepoint is a diff, not a copy** — rlvm tracks modifications in `original_intA` maps and
reconstructs originals at save time. Critically, **RealLive got an open-source clone because Haeleth
built a lossless round-trip toolchain** (`kprl -d` disassembles to Kepago, `rlc` recompiles).
Siglus didn't document its bytecode, and its games are correspondingly unportable.

**MAGES./SC3** (Steins;Gate) — the most sophisticated VM surveyed, and the strongest argument for
*deliberately underpowering* a script language:
- **Cooperative multithreading.** Each thread runs until it blocks, then yields; threads persist
  across frames.
- **No general-purpose registers.** One accumulator, a loop counter, no locals, no parameters, no
  return-value register. 32 thread-local vars; **8,000 global int32; 6,400 global 1-bit flags**
  (an 800-byte bitfield); call stack **bounded at depth 8**; **16 script buffers** loaded on demand,
  with jumps targeting *buffer IDs, not filenames*.

That entire machine is a couple of kilobytes. **Snapshotting it is trivial by construction.** And
the multithreading is what buys Steins;Gate's phone system: the phone is *a second script thread*.
Model those "choices" properly and they are not a choice statement at all — they're a persistent
concurrent UI mode, an inbox as game state, timed availability windows, and flags read by late-game
endings. **With one linear instruction pointer, non-menu choices are painful; with cheap script
threads and a blocking discipline, they're free.**

**Zero Escape 999 / VLR — the flowchart data model.** 999 on DS had no flowchart and required full
replays; VLR introduced it as a first-class engine feature and the 2017 remaster retrofitted it into
999. Uchikoshi's principle is **"bi-directionality"** — the chart *is* the visualisation of time,
and its visibly locked, tangled paths are themselves narrative.

```
Node {
  id            : stable identifier, survives patches
  kind          : NOVEL | ESCAPE | ENDING | JUNCTION
  predecessors[] / successors[] : (id, condition)
  entry_state   : the canonical state installed on jump
  status        : UNSEEN | AVAILABLE | CLEARED
  lock          : optional flag expression
}
```

The load-bearing property: **jumping to a node is not "load a save" — it is "install this node's
*authored* starting state."** Which yields a hard design rule: **scene entry must be idempotent and
history-independent.** Progress is node-clear bits plus *knowledge* flags at profile scope (VLR's
locked nodes say "you lack information obtained elsewhere"). The topology is authored statically but
visibility is dynamic. As the Fuwanovel analysis puts it: *"Navigation by jump is the feature that
separates built-in flowcharts from simple save management."* **This is YU-NO's A.D.M.S. and jewel
system, generalised and made standard.**

**Ren'Py** — `.rpy` compiled to `.rpyc` (gzipped pickled AST). Its rollback is **not** periodic
snapshots: it is an **undo log over a revertable object graph**. `object`/`list`/`dict`/`set` inside
script stores are replaced with instrumented equivalents that log their own mutations; each statement
gets a `Rollback` record holding a shallow context copy, object-restore tokens and store deltas;
rolling back walks them in reverse and **pushes the RNG back** so re-execution is deterministic.
The save is a ZIP containing a pickle of `(roots, RollbackLog)` — **which is why Ren'Py can roll back
after loading a save, and nothing else surveyed can.** Cost: every value crossing the script boundary
must be revertable or explicitly excluded, and unpicklable objects silently break saves.

**Danganronpa / Ace Attorney** matter only for where the engine boundary sits: in both, gameplay is a
*native subsystem parameterised by data tables*, and the script's job is to set it up, block on it,
and read back a result. **Design the blocking foreign-call primitive deliberately** — it is the seam
every hybrid VN needs, and it is exactly what broke rlvm on Little Busters' baseball minigame
(which lived in a Windows DLL, not bytecode).

### 2.6 Save-anywhere: the five real strategies

| # | Strategy | Who | Requires | Cost |
|---|---|---|---|---|
| 1 | **Serializable-interpreter snapshot** | KiriKiri/KAG | No host stack may span a savepoint | Brittle against script edits |
| 2 | **Author-declared checkpoints** | NScripter, RealLive | Nothing — trivially correct | Not truly save-anywhere |
| 3 | **Undo log over revertable objects** | Ren'Py | Control of value types + deterministic RNG | Unpicklable objects break saves |
| 4 | **Restricted VM with no hidden state** | SC3 (implicitly) | Underpowering the language | Script can't allocate or close over anything |
| 5 | **Replay-from-checkpoint** | (rare; Ren'Py's `forward`) | Perfect determinism, suppressible side effects | Side effects hard to suppress |

**Our recipe: #4 as the foundation** (small explicit VM, no hidden host state — which makes
save-anywhere free *by construction*), **#1 as the save mechanism** (snapshot VM + presentation
state), and **#3's revertable-store idea applied only to the small mutable-variable subset** to get
Ren'Py-quality rollback. Script must never hold a reference to a native object across a yield.

**Version-brittleness must be designed for up front**, since every snapshot engine surveyed has the
same failure: (a) address saves by **stable label ID**, never file+line offset; (b) store a script
content hash and warn; (c) store variables **by name**, not bank index, so adding a flag doesn't
shift everything; (d) ship a documented migration hook.

### 2.7 What makes million-word scripts tractable

1. **Many small script files, loaded on demand.** Never require the whole script resident.
2. **Compile to bytecode** — but then your debugger, diff tool and translators need a decompiler,
   so **ship a lossless round-trip decompiler** (the RealLive lesson).
3. **Separate text from code at the format level, not by convention.** RLdev had `#resource` +
   `#res<id>` string tables in **2004**. FSN had only a convention (`@texton`/`@textoff`) and paid
   for it forever in fan-TL tooling.
4. **Read-flag registry as a bitset** keyed by (script id, offset).
5. **Author-extensible macros**, plus **`cond=` on every tag** — this eliminates thousands of
   `[if]…[endif]` pairs and is why FSN's writers could invent presentation idioms without engine
   changes.
6. **Two syntactic forms**: line-mode directives keep prose readable; inline tags handle ruby,
   colour, waits. FSN uses both.
7. **UTF-8 end to end.** Every legacy engine here is Shift-JIS-bound and the universal fan-TL
   workaround is *hijacking the double-byte space* (RLdev's `-x Western`). KiriKiri "doesn't support
   automatic word wrapping since it was written solely for Japanese." **Word wrapping, proportional
   fonts and per-language line-breaking are engine features, not localisation afterthoughts** —
   ONScripter-EN and Ponscripter exist almost entirely to add them. English runs ~1.5–2× the box
   width of Japanese, so text length must never be layout-critical.

## Part 3 — Research findings: cross-platform target matrix ✅

**Headline: the target set is achievable with one C99 core that software-renders into an indexed
pixel buffer, plus ~6 thin shells. It is achievable with no off-the-shelf engine, and not at all
if GPU rendering is required everywhere.**

### 3.1 The binding constraint is not CPU — it is the watchOS 75 MB app bundle cap

| Constraint | Value | Consequence |
|---|---|---|
| watchOS bundle cap | **75 MB uncompressed**, and **watchOS does not support on-demand resources** | Total art + audio + binary must fit in 75 MB. No streaming escape hatch. |
| Metal on watchOS | **Not available.** Neither is OpenGL ES. | Only rendering primitive is SpriteKit `SKTexture(data:size:)` / `CGImage` from raw bytes — i.e. *hand it a pixel buffer*. |
| C in a watchOS target | **Yes** — `arm64_32-apple-watchos`, C headers import into Swift with zero wrapper | C is the only language with a no-shim path here. |
| JIT / dynamic code | Prohibited | Rules out a WASM JIT; our own bytecode interpreter is fine (Lua-style VMs have shipped for years). |
| Background execution | App suspends on wrist-down; Extended Runtime Sessions are gated to workout/mindfulness/audio | Watch build must be a *glanceable episodic reader*, auto-saving every line. |

A conventional VN (1024×768 JPEG backgrounds) is 300 MB–2 GB and simply cannot exist on watchOS.
**640×400 at 4bpp indexed is ~128 KB raw, ~10–25 KB compressed — roughly 2,000 full-screen images
in 40 MB.** This is the finding that validates the whole premise: *the PC-98 art direction is the
only thing that makes the watch target physically possible.* It is a hard engineering requirement
masquerading as a style choice.

### 3.2 Wear OS — easy, because it is just Android

Full AOSP; the NDK works and Google now *mandates* 64-bit native libs for Wear OS apps. 2 GB RAM,
Snapdragon W5+ Gen 1 (4× Cortex-A53 + Adreno A702), 32 GB storage — **no size problem on this
side**. Round displays: 408×408, 432×432, 466×466. Precedents exist (DOOM on Wear OS via Compose,
Godot 4.3 on a Galaxy Watch 4, Unity porting toolkits).

Three gotchas that matter more than the hardware:
- **Screen sleeps after ~15 s of no interaction.** A reader who studies a line for 40 s goes black.
  Needs `keepScreenOn` or an Ongoing Activity.
- **WebView on Wear OS is unreliable/absent** — this independently kills any "just ship a PWA in a
  WebView" strategy.
- **No SDL-on-Wear-OS port is published by anyone.** It inherits Android support by accident, not
  by test. This is a week-one de-risking task, not a month-six assumption.

### 3.3 Platform-layer verdict

Only one row of the support matrix covers every target:

| Layer | Desktop | Pi | Browser | Wear OS | **watchOS** | RP2040 |
|---|---|---|---|---|---|---|
| SDL2 / SDL3 | ✅ | ✅ | ✅ | ⚠️ untested | ❌ | ❌ |
| raylib | ✅ | ✅ DRM | ✅ | ⚠️ | ❌ | ❌ |
| sokol | ✅ | ⚠️ | ✅ | ⚠️ | ❌ needs Metal | ❌ |
| LVGL | ✅ | ✅ | ⚠️ | ⚠️ | ❌ | ✅ |
| Ren'Py | ✅ | ✅ arm64 only | ❌ | ❌ | ❌ | ❌ |
| Godot 4 | ✅ | ⚠️ | ✅ huge | ⚠️ no crown input | ❌ | ❌ |
| **Own software rasterizer → pixel buffer** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

**So: make the software framebuffer the primary path and treat GPU as an optional accelerator.**
Every shell does exactly one thing — expand indexed→RGB once per frame and hand over a rectangle.
At 640×400 that is a table lookup costing ~0.3–2 ms even on a Cortex-A53.

### 3.4 Language: C99, and the decisive argument is watchOS + Swift

| | WASM | NDK | Static lib into Swift | Bare metal | Size | Verdict |
|---|---|---|---|---|---|---|
| **C99** | ✅ best | ✅ | ✅ **no shim** | ✅ | ~56 KB ref | ✅✅ **chosen** |
| C++17 | ✅ | ✅ | ⚠️ ObjC++ shim + libc++ | ⚠️ | larger | acceptable |
| Rust | ✅ | ✅ | ⚠️ Tier-2 targets + cbindgen | ⚠️ no_std | ~72 KB | strong 2nd |
| Zig | ✅ | ✅ | ⚠️ watchOS UNVERIFIED | ✅ | C-like | risky on Apple |
| Go/TinyGo | ⚠️ 332 KB | ⚠️ | ❌ | ⚠️ | worst | ❌ reject |

Discipline the core will hold to: **no `malloc` after init (arena allocators only)**, no `double`
in the hot path, no libc beyond `stdint/string/math`. This is what simultaneously makes the 75 MB
budget, deterministic WASM heap sizing, and a hypothetical MCU port tractable.

Tooling (script compiler, asset packer) ships to no watch and can be written in anything.

### 3.5 WASM as the portability layer itself — evaluated and rejected

wasm3 (~64 KB) / WAMR could host the core everywhere. Rejected as *primary* architecture:
it buys nothing (our core is already C, which compiles natively for every one of these targets),
watchOS forbids the fast variants (no JIT, no install-time AOT), and it removes ~0% of the shell
work while adding an ABI boundary and a 5–15× interpreter penalty.
**Keep it for v2 as a sandbox for user-authored mods** — memory-safe untrusted code on every
platform including the watch, for 64 KB.

### 3.6 Asset pipeline

- **Images:** author PNG-8 → pack to **QOI** (single ~300-line header, ~3–4× faster decode than
  libpng, no allocations). Raw 4bpp + RLE for the MCU tier.
- **Audio: FM synthesis is the strategic play.** A VGM log driving an emulated YM2608 is **2–20 KB
  per track** versus ~2.5 MB for Ogg. A full OST becomes ~200 KB instead of ~60 MB — this single
  decision is what lets the *same bundle* ship to watchOS, the web, and a Pico. Note `ymfm` is
  C++11; prefer a C OPN core to keep the core C-only *(Nuked-OPNA availability: UNVERIFIED —
  fallback is to isolate ymfm as the one C++ module, or write our own OPNA-subset synth)*.
- **Fonts:** ship a **16×16 bitmap font, never a TTF.** Noto Sans CJK is 15–20 MB per weight = 25%
  of the entire watch budget for one font. Full JIS X 0208 at 16×16 = **220 KB** (6,879 glyphs ×
  32 B) — confirmed. Subset to the ~1,500–2,500 kanji a script actually uses → **~70 KB**. Bitmap
  fonts also deliver the pixel-perfect no-AA aesthetic for free, and avoid HarfBuzz (1 MB+).

### 3.7 Raspberry Pi

Pi 4/5 are overkill (Mesa V3D, GLES 3.1, Vulkan 1.3). **Pi Zero 2 W (4× A53 @ 1 GHz, 512 MB) is
comfortable**: a 128 KB indexed frame expanded to RGB565 at 30 fps is ~15 MB/s against >1 GB/s of
bandwidth — 60×+ headroom. Deploy via KMS/DRM with no X11 for ~2 s boot-to-game.

**RP2040 verdict: no for a real VN, yes as a stunt.** 264 KB SRAM (a 640×400 4bpp buffer is 128 KB
= 48% of all RAM) and 2 MB flash for every asset. *Design so it stays possible — band renderer, no
malloc, integer math — but do not ship it in v1.* The discipline is worth more than the port.

### 3.8 Browser

Emscripten. Use `file_packager` for **per-chapter `.data` bundles** rather than all-or-nothing
`--preload-file`; **IDBFS only for saves** (kilobytes — persisting 300 MB of art in IndexedDB
invites eviction). The indexed-color decision pays off hardest here: **a complete VN can be a ~5 MB
first load** versus 30–50 MB of JPEGs. That is the difference between "playable link" and "nobody
plays it."

---

## Part 4 — Recommended architecture

### 4.0 The one-line thesis

**Three constraints converge on the same answer.** The watch demands a tiny dependency-free core;
save-anywhere demands an interpreter with no hidden host state; a million-word script demands
on-demand chunk loading and an external string table. SC3 shows that *deliberately underpowering the
script VM* satisfies all three at once — a machine with no registers, no locals and no closures is
trivially snapshottable, trivially portable, and trivially small. **We are building AliceSoft's
System engine (1989) with SC3's VM discipline, KAG's presentation model, and 999's flowchart.**

### 4.1 Layer cake

```
┌──────────────────────────────────────────────────────┐
│ vn_vm.c      bytecode VM: 8 cooperative threads,     │  pure C99
│              no registers, bounded call stack        │  no malloc after init
│ vn_state.c   flag banks, snapshot/restore, rollback  │  no double in hot path
│ vn_flow.c    scene graph, node status, flowchart     │  no libc beyond
│ vn_text.c    bitmap layout, wrap, ruby, kinsoku      │    stdint/string/math
│ vn_gfx.c     8bpp indexed software rasterizer        │
│ vn_snd.c     OPNA-subset FM synth + mixer → int16    │
│ vn_pack.c    QOI + planar-4bpp asset decode          │
└───────────────────────┬──────────────────────────────┘
                        │ vn_platform.h — 12 functions
   blit · audio_push · now_ms · poll_input · file_open/read/close
   · save_write/read · log · exit · dpi_scale
```

Shells (200–800 lines each): SDL3 desktop · SDL3/DRM Pi · Emscripten · `ANativeActivity` Wear OS ·
Swift + SpriteKit watchOS · LVGL for the micro tier.

### 4.2 The VM — underpowered on purpose

| Property | Value | Rationale |
|---|---|---|
| Threads | **8 cooperative**, run-until-block | Steins;Gate-style non-menu choices (phone as thread 2) |
| Registers | **1 accumulator + 1 loop counter**. No GP registers, no locals, no closures | Snapshot is free by construction (SC3) |
| Call stack | **depth 16**, per thread | Bounded → fixed-size snapshot |
| `f.` save-local | 4,096 × int32 + 256 × string(64) | Story progress, affection counters |
| `sf.` profile-global | 2,048 × int32 + **8,192 bits** | Route unlocks, gallery, endings, knowledge flags |
| `tf.` transient | 256 × int32, never saved | Scratch |
| Arithmetic | **saturating**, clamp [0, 2³¹−1] | AliceSoft's ADV — no wraparound on affection counters |
| Script buffers | **16**, loaded on demand; jumps target buffer IDs | Never require whole script resident |
| Foreign calls | **explicit blocking primitive only** | The Danganronpa/rlvm-baseball seam, designed deliberately |

Total VM state ≈ **20 KB**. Snapshot = `memcpy`. This is the whole trick.

### 4.3 Script DSL → bytecode

Authors write inline text; **the compiler extracts every display string into a per-locale table**
with a stable ID (`scene_label:ordinal`) plus a source hash for change detection. The script
references IDs only.

```
scene day01_gate  "The School Gate"          # stable ID; save/read/jump key
    bg     school_gate  fade 800
    show   saber  neutral  at center
    narr   The rain had stopped sometime before dawn.
    saber  "You're late again, Shirou."

    choice
        "Apologise."            -> gate_apologise   ; f.saber_pt += 1
        "Say nothing."          -> gate_silent
        "...Stop her."          -> ubw_branch        if sf.fate_cleared
    end

ending  ending_bad_07  kind=BAD  dojo=12
```

- **Two tag forms**, per FSN: line-mode directives (`bg`, `show`) and inline markup for ruby,
  colour, per-char speed.
- **`if` as a modifier on any statement** — kills thousands of `if/endif` pairs.
- **Macros** are author-defined and expand at compile time.
- `ending` is a **first-class statement**: numbered, gallery-tracked, fires the completion hook,
  routes into the Tiger-Dojo-style interstitial.
- Compiler `vnc` emits `.vnb` bytecode + `.vns` string table + `.vnf` flow graph, **and ships a
  lossless round-trip decompiler** (`vnc -d`) — the RealLive lesson, non-negotiable.

### 4.4 Presentation — authentic *and* reflowable

- **Fixed 8-layer stack** (bg, 3 character slots, overlay, text, UI, effect), each a small fixed
  struct → snapshot stays small and fixed-size.
- **Fore/back pages, KAG-style.** Mutate the back page, run a transition, block. Every visual change
  is atomic; **skip mode = run transitions at `time=0`**, correct by construction.
- **Transitions are dithered dissolves and palette animation, not alpha blends.** Crossfading in
  index space is impossible — and doing it with a Bayer threshold mask is exactly what period
  hardware did. Authenticity and portability agree again.
- **Presentation profiles**, because the watch cannot show 640×400: authors declare *semantic*
  positions (`at center`, `at left`), never pixels. Profiles: **full** 640×400 · **watch** ~400×400
  reflowed, ~12–14 chars/line, 4–6 lines, tap-to-advance, crown-scrolls-backlog · **micro** 240×240.
- **Text layout is an engine feature**: wrapping, per-language line-breaking, auto-fit. English runs
  1.5–2× the width of Japanese, so no layout may be length-critical.

### 4.5 Save, rollback, and the flowchart

- **Save = VM snapshot + presentation snapshot** (~36 KB). Addressed by **stable label ID**, never
  file+line. Variables stored **by name**, not bank index. Script content hash embedded; mismatch
  warns and runs a migration hook.
- **Rollback** = bounded ring of snapshots at interaction boundaries (KAG's model), *plus* a
  revertable-store undo log for the mutable-variable subset so rollback survives a load (Ren'Py's
  one unique win).
- **Backlog** is separate and cheap: append-only (speaker, string ID, voice handle), clickable replay.
- **Read registry** = bitset per script buffer, one bit per page. Not named variables.
- **Flow graph** is authored statically, visibility dynamic. Node entry installs the node's
  *authored* state — **scene entry must be idempotent and history-independent.** This gives us YU-NO's
  A.D.M.S., 999's flowchart, and jewel-style bookmark warping from one mechanism.

### 4.6 Assets

Author PNG-8 → pack to **planar 4bpp with VSP's cross-plane XOR opcodes** (bitplanes in dithered
16-color art are highly correlated) with QOI as the general-purpose fallback. Music: MML-ish source →
compiled register-event stream → tiny resident player, mirroring PMD's architecture; **OPNA subset,
6 FM + 3 SSG + rhythm**. Fonts: 16×16 bitmap, subset to glyphs actually used.
**Packaging is an additive overlay VFS** — a language pack or patch is a mounted archive that shadows
by path, never a rebuild (KiriKiri's best idea).

---

## Part 5 — Repository layout & milestones

```
core/        vn_vm.c vn_state.c vn_flow.c vn_text.c vn_gfx.c vn_snd.c vn_pack.c
             include/vn.h  include/vn_platform.h
platform/    sdl3/  web/  wearos/  watchos/  drm/  lvgl/
tools/       vnc/      script compiler + decompiler
             vnpack/   asset packer + budget enforcer
             vnfont/   TTF → subset bitmap font
demo/        script/*.vns  art/  audio/  flow.vnf
tests/       golden/  vm/  fuzz/  routes/
docs/        format-bytecode.md  format-assets.md  dsl-reference.md
```

**Ship order** (each stage is a working build before the next starts):

1. **Desktop SDL3** — core + VM + renderer + a two-scene script. Proves the architecture.
2. **Browser (Emscripten)** — same code, near-zero marginal work, gives a shareable link.
3. **Pi via DRM/KMS** — same code, no X11.
4. **Wear OS** — *but see risk 3: a hello-world `NativeActivity` blitting a bitmap on real hardware
   is week-one work, not stage-4 work.*
5. **watchOS** — Swift shell + C static lib + SpriteKit `SKTexture` from raw bytes.
6. **Micro tier** — only if the no-malloc discipline actually held.

---

## Part 6 — Demo content design

A vertical slice, ~1–3 hours, that exercises **every** FSN-class mechanic. Content is small; the
machinery is complete.

**All demo narrative is original.** The games studied above are structural and technical reference
only — we take architecture and craft standard from them, never characters, names, prose or story.
See `demo/README.md` for the boundary.

| Mechanic | Demo realisation |
|---|---|
| Route locking | 3 routes; B locked until A cleared, C until B — gated on `sf.` flags |
| Threshold-on-accumulator | Two hidden affection counters, saturating, compared at fixed checkpoints |
| Bad ends + recovery | 6 numbered bad ends, each with an interstitial that names the choice to change and unlocks a gallery entry (the mechanic FSN implements as Tiger Dojo) |
| Flowchart | Full node graph, jump-to-any-cleared-node, unseen successors as stubs |
| Non-menu choice | One Steins;Gate-style concurrent thread (a second script thread with a timed window) |
| Kinetic chapter | One Higurashi-style standalone chapter with zero choices — proves file-ordering covers it |
| Save/rollback | Save anywhere, rollback across a load, backlog with voice replay |
| Skip / auto / read-tracking | Skip-to-unread driven by the bitset registry |
| i18n | Ship EN with the string table plumbed and one scene translated to JP as a proof |

Art: 640×400, 16 colors, hand-graded dither, no scanlines, square pixels. Music: OPNA subset.

---

## Part 7 — Verification

- **Golden-frame tests.** Render to PPM at fixed logical times, hash, compare. Catches renderer,
  dither, and text-layout regressions.
- **Headless route walker.** Play every path; assert all 3 routes, all endings, and all 6 bad ends
  are reachable, and that no node is orphaned or unreachable in the flow graph.
- **Save/load round-trip fuzzing.** At every interaction boundary: snapshot → restore → assert the
  next 100 VM steps are bit-identical. This is the test that proves the save architecture.
- **Rollback determinism.** Roll back N steps, replay, assert identical state including RNG.
- **Round-trip decompiler test.** `vnc script.vns → .vnb → vnc -d → .vns'`; assert semantic identity.
- **Budget enforcement as a build failure.** `vnpack` fails the build if the watch bundle exceeds
  its cap or the web first-load exceeds target. **This runs from day one, before a single background
  is drawn** — see risk 2.
- **Platform smoke tests.** Boot, render one scene, take input, save, reload — on each shell.

### Top risks

1. **watchOS App Store review, not watchOS technology.** The tech is fine; a reading-heavy app with
   no wrist-native purpose is the rejection risk, as is an Extended Runtime Session we can't justify.
   *Mitigation: design the watch build as a glanceable episodic reader — one scene per session,
   auto-save every line, instant resume. Validate via TestFlight before writing the full shell.*
2. **The 75 MB budget silently drives art direction, and art gets made before anyone checks.**
   *Mitigation: packer enforces the budget as a build failure from day one; the 640×400/16-color
   constraint goes in the art bible, not the backlog.*
3. **Wear OS is untested territory for native apps.** Everything says it should work; nobody has
   published an SDL-on-Wear-OS port. *Mitigation: week-one hello-world `NativeActivity` on physical
   hardware — confirm it installs from Play internal test, that `keepScreenOn` doesn't get flagged,
   and that rotary input reaches native code.*
