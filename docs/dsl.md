# The VN script DSL

Compile with `vnc source.vn out.vnb out.vnstr`. Disassemble with
`vnc -d out.vnb out.vnstr`.

Display text never lands in the bytecode. Every string is extracted into
`.vnstr` with a stable id and a hash of its source text, so a locale is
a separate file and a retranslation cannot invalidate anyone's save.

## Structure

```
scene <name> "Title"
    <statements>
end
```

Scenes are the unit of everything: a scene name is simultaneously the
jump target, the save anchor, and the read-tracking key. Falling off the
end of a scene ends that thread rather than running into the next scene.

## Statements

| Statement | Meaning |
|---|---|
| `narr <rest of line>` | Narration. The rest of the line is prose; no quotes needed. |
| `<speaker> "text"` | A line of dialogue. |
| `bg <name> [fade <ms>]` | Set the background. |
| `show <char> [<pose>] [at left\|center\|right]` | Place a character. Max 3 on stage. |
| `hide <char>` | Remove a character. |
| `bgm <name>` / `sfx <name>` | Audio. |
| `wait <ms>` | Block this thread for a time. |
| `set <var> = <expr>` | Assign. |
| `add <var> <expr>` | Add, saturating. |
| `if <expr>` / `elif` / `else` / `end` | Conditionals. |
| `choice` … `end` | A menu; see below. |
| `jump <scene>` | Transfer control. |
| `call <scene>` / `return` | Subroutine, depth 16. |
| `spawn <scene>` | Start a cooperative thread. |
| `yield` | Let other threads run; resumes next frame. |
| `ending <name> kind=BAD\|GOOD\|TRUE` | Record a numbered ending. |
| `halt` | Stop the whole machine. |

## Choices

```
choice
    "Apologise."   -> gate_sorry
    "Say nothing." -> gate_silent
    "Stop her."    -> route_b   if sf.route_a_cleared
end
```

A guard makes the option visible but disabled, not hidden. Seeing a
locked door is what tells a player another route exists.

## Variables

Three banks, distinguished by prefix. The scope discipline is the point:

| Prefix | Scope | Use for |
|---|---|---|
| `f.` | save-local; written to every save | story progress, affection counters |
| `sf.` | profile-global; outlives saves | route unlocks, endings seen, gallery |
| `tf.` | transient; never saved | scratch |

Route gates belong in `sf.`, always. Putting one in `f.` means the
unlock dies with the save file.

Variables are 32-bit integers and **arithmetic saturates** — it clamps
at the limits rather than wrapping. A wrapped affection counter is the
classic unreproducible bug three routes later.

Expressions support `+ - *`, comparisons `== != < <= > >=`, and
`and` / `or` / `not`, with parentheses.

## Threads

`spawn` starts a cooperative thread; `yield` hands control on. Only one
thread may own the screen, so a second thread reaching a line waits its
turn rather than overwriting the first. This is the mechanism behind
choices that are not menus — a phone, an inbox, a timer running
alongside the narrative.

Threads resume on the frame after they yield, so a script that yields in
a loop cannot spin the scheduler.

## Saves

A save is the VM's state, which is a flat struct: threads, the save-local
bank, and the stage. Two details make saves survive script edits:

- Code positions are stored as *(scene, offset within that scene)* plus
  a hash of that scene's structure. Editing a **different** scene leaves
  the save valid; editing the scene you saved inside is refused rather
  than silently resuming at a wrong offset.
- Variables are stored by **name**, not slot index, so adding a new
  variable does not shift the existing ones.

The scene hash covers structure only — opcodes, jump targets as
(scene, offset), and numeric operands. Text is excluded, so retranslating
a line never breaks a save.
