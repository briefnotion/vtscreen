# Retained mode, and what a terminal multiplexer needs from vtscreen

Status (2026-11-25): sections 3 to 6 are **implemented** (retained pictures, the changed flags, the early exit), with the
differences listed just below. Sections 7 and 8 (the multiplexer) are still a plan; nothing of them is built.

### What was built, and how it differs from the plan

- **Opt-in, not inferred.** Programs write cells straight into `picture.cell` in hundreds of places (vtshow's scenes), which
  cannot be noticed. So `touched` is only kept for a Picture that has been told `retain()`; any other Picture gets the
  whole pass every frame, exactly as before, so nothing can go stale. (The plan said any non-const `at()` would count as
  touched; that is still true for a retained Picture.)
- **`shown` was kept.** Step 3 of section 6 (replace `shown` by the changed flags) was dropped. The gap logic in `send()`
  reads what the terminal currently has for unchanged cells, and exact byte-for-byte output is easier to guarantee with
  `shown` kept. The changed flags are a *hint* to where to look; a flagged cell is still compared with `shown` before
  anything is written. The saving, 12 bytes a cell, was not worth the risk.
- **`render` does one pass.** The letters are worked out and compared with `target` in one loop, which flags what changed.
  `send()` visits only flagged rows and cells, and returns at once when nothing is flagged.
- **`refresh()` flags every cell that is not blank**, since the terminal has just been cleared.
- **Settings.** A Display whose `color`, `fps` or `hash_letters` changed since the last render works everything out again.
- **Checked** against the library as it was: 1191 frames of every vtshow scene (colour, no colour, 15 fps, cross-fades,
  switching pictures, a resize, refreshes, static text) hash identically; four pinned sequences are in
  `tests/retained_tests.cpp`; 20 000 random frames agree between retained and ordinary pictures and with a terminal model.
- **Measured** (`tests/bench_render.cpp`, 160 x 49, this PC): a static all-text screen 112 us ordinary, 0.2 us retained;
  map-like 339 us to 119 us; everything shimmering unchanged. Ordinary pictures cost the same as before on map-like
  frames and about 10 to 15 us more on all-text frames (before: 96 us). The letter pass must be force-inlined
  (`VTSCREEN_FORCE_INLINE`): without it the compiler left it as a call and ordinary frames were 30 % slower.

## 1. What this is for

Two goals, one design.

1. **Do less work per frame.** Keep the picture and the virtual screen in memory between frames, mark what changed as
   it changes, and only recompute and send what did. A screen where nothing happens should cost nothing.
2. **Be the display half of a tmux-like program.** Several sessions, each on its own thread, each feeding a terminal
   emulator that fills a picture; a parent layer combines those pictures into the real screen. Programs that use
   vtscreen directly (the scenes, vtweather) must keep working, and can also appear as panes with no emulator at all.

## 2. How it works today (the facts the plan starts from)

- A program fills a `Picture` every frame (`Picture::init`, then draws everything). A cell is a `PicCell`, 88 bytes:
  brightness, colour, shimmer, a second colour, and a text flag with a character.
- `Display::show(pic, t)` calls `render`, which calls `to_letters` (every cell becomes a `VtCell`, 12 bytes: a letter,
  an inverse flag, colours) and `send` (compare every cell with `shown`, the copy of what the terminal has, and write
  only the differences, in runs, with cursor jumps).
- `show` accepts any Picture each call. The first frame after a switch just sends the differences.
- Measured on a 160 x 49 screen (microseconds per frame, render only):

  | screen | render |
  |---|---|
  | all text, nothing changes | 105 (sends 0 bytes) |
  | 90 % still cells, 10 % shimmering | 344 |
  | every cell shimmering | 755 |
  | all text, every cell changes | 1100 |

  So a static screen costs 0.1 ms a frame only to find out that nothing changed. The writes are already minimal.

## 3. The model

Three levels, all kept in memory between frames:

    program --writes--> Picture (touched flags) --render--> Screen (changed flags) --send--> terminal
                         "what the program drew"            "what the terminal will show"

- **Picture**: the program's cells. A cell is *touched* when the program writes it. Touched means "may have changed",
  not "has changed" (see 4.1).
- **Screen**: a `VtScreen`, one `VtCell` per terminal cell, every attribute worked out, the letters chosen. A cell is
  *changed* when it differs from what the terminal is showing. This replaces `shown`: the changed flag *is* the
  difference, so there is no second copy to compare with.
- **Terminal**: only the changed cells are written, in runs, row by row.

Render looks only at touched cells (their text copied over, or their letter chosen) and animated cells (shimmer above
0, whose letter changes with time even when nobody touches them). Everything else is skipped.

## 4. Data structures (pseudo code)

```
struct Picture {                          // as today, plus:
    cells[rows * cols]      PicCell
    touched[rows * cols]    byte          // written since a render last looked at it
    row_touched[rows]       byte          // any cell of the row touched
    animated[rows * cols]   byte          // shimmer > 0, kept up to date whenever the cell is touched
    animated_in_row[rows]   int           // so rows with none are skipped
    all_touched             bool          // after init, resize, or when it is shown to a different Display
    serial                  uint64        // which picture this is (a number from a counter)

    PicCell&       at(r, c)               // non-const: counts as touched (conservative, see 4.1)
    const PicCell& at(r, c) const         // const: no mark
    void set(r, c, PicCell v)             // touched only if v differs from what is there
    void touch_all()
}

class Display {                           // as today, plus:
    screen                  VtScreen      // persistent: what the terminal will show
    changed[rows * cols]    byte          // differs from the terminal
    row_changed[rows]       byte
    any_changed             bool          // the overall flag: only when set is anything written
    last_serial             uint64        // the picture rendered last
}
```

### 4.1 "Touched" is conservative on purpose

Programs write cells through `picture.at(r, c).lum = ...` in hundreds of places (the scenes, vtweather). A reference
cannot be watched, so any non-const `at()` marks the cell touched. A false alarm costs one cheap check at render (pick
the letter, compare 12 bytes); a cell is only marked *changed* if the letter really differs. Consequences:

- Existing programs touch every cell every frame, so they cost what they cost today. Nothing breaks.
- A program that keeps its picture and edits only what changed gets the saving. It can be converted piece by piece.
- `set()` is the exact version for new code (the emulator, the compositor, `fade`): it compares first.

## 5. The render pass (pseudo code)

```
render(pic, t):
    if pic.serial != last_serial or pic.all_touched:      // a different picture, or everything is new
        pic.touch_all()
        last_serial = pic.serial

    for row in rows:
        if !pic.row_touched[row] and pic.animated_in_row[row] == 0: continue
        for col in cols:
            i = row * cols + col
            if !pic.touched[i] and !pic.animated[i]: continue
            pc  = pic.cells[i]
            new = pc.text ? text_cell(pc)                 // the letter the program chose, with its attributes
                          : shade_cell(pc, row, col, t)   // pick_char: the letter from brightness and shimmer
            if new != screen[i]:
                screen[i] = new; changed[i] = 1; row_changed[row] = 1; any_changed = true

    pic.clear_touched()                                   // consumed

    if !any_changed: return ""                            // a quiet screen costs the scan of the row flags
    out = ""
    for row with row_changed:
        // as send() does today: runs of changed cells, a cursor jump to the first, short unchanged gaps
        // (up to 3 cells, no colour code needed) written again rather than jumped over
        ...
    clear changed and row_changed; any_changed = false
    return out
```

Rules that keep it correct:

- **Consumed means cleared.** Flags accumulate until a render consumes them, so a skipped frame loses nothing.
- **A switch means everything.** `show(b)` after `show(a)`: b's flags say nothing about what a left on the terminal,
  so the serial differs and the whole picture is treated as touched. (vtshow does this when a fade ends.)
- **Fades.** `fade(a, b, pct, out)` writes `out` with `set()`. `out` is a persistent picture; cells that mix to the
  same value are not touched. During a fade most cells do change, so it costs what it does now. "Several pictures
  put together into one screen" is the same operation again: the output picture is written with `set()`.
- **Resize and garbled output.** `refresh()` still clears the terminal, then marks every screen cell changed. A resize
  makes the grids again at the new size and marks everything. The 10 second refresh stays.
- **The status bar** (`Display::bar`) is written with `set()`, so it is only touched when its text changes.
- **Attributes later.** When more are added (bold, underline, italic, blink, strike, wide characters), they go in the
  `VtCell`. Keep a cell's visible attributes in one small packed value so that "differs" stays a single compare.

## 6. Order of work, each step checked

Each step is checked by rendering the same frames through the old and new code and showing the bytes are identical
(the same method used for the vtshow scenes). Programs and scenes are not edited.

1. Add the flags to `Picture`; `render` still does the full pass. Output identical.
2. Skip cells that are neither touched nor animated. Output identical; measure.
3. Replace `shown` by the changed flags. Output identical; less memory.
4. The "nothing changed" early return, and row flags in the send loop. Measure the static-screen case (105 us).
5. Then, one at a time and only where it pays: convert vtweather's drawing to touch only what changed.

## 7. The multiplexer

### 7.1 Who does what

vtscreen is the display library. A multiplexer needs more than it, so most of it is a **separate program** (call it
`vtmux`) that uses vtscreen:

| part | where |
|---|---|
| pty and process per session, SIGWINCH to the session, keys and focus | vtmux |
| the VT emulator: parses escape sequences, fills a picture | vtmux (its own module) |
| pictures, change tracking, rendering, combining panes | vtscreen |

### 7.2 Everything is a picture source

```
session thread:   program output --> emulator --> Picture   (text cells, with attributes)
native program:   vtscreen program ---------------> Picture   (no emulator: it writes cells directly)

compositor:       for each pane: copy the pane's touched cells into one full-screen Picture, at the pane's place
                  draw borders and the status line into the same Picture
                  display.show(full_screen_picture, t)
```

The chain from section 3 runs end to end: a session writes its picture; only its touched cells are copied into the
big picture (with `set()`, so a cell that did not really change is not touched again); only those reach the screen
and the terminal. A quiet pane costs nothing. A pane that moves, resizes, is covered or uncovered, or changes focus
touches its whole rectangle. Both kinds of pane are the same to the compositor, which is how programs that use vtscreen
natively keep working: they simply fill a Picture as they do now.

### 7.3 Threads

Rule: **a Picture has one writer thread and is never read while it is being written.** Two ways to meet it:

- **A. Publish (recommended).** Each source has a working picture, written only by its own thread, and a published
  picture. At the end of a burst of output (or at most ~60 times a second) its thread copies only the touched rows to
  the published one, and merges the touched flags in; the compositor reads only published pictures. One lock, held for
  microseconds, only at publish, never while parsing. Simple and correct. The cost of a publish is the size of what
  changed.
- **B. Lock-free.** A seqlock per row, or three rotating buffers. No lock, but the touched flags have to be
  accumulated across buffers, which is where triple buffering gets tricky. I would only go there if A shows up in
  a measurement.

### 7.4 What the emulator needs from the cell

- **Attributes**: bold, dim, italic, underline (and styles), blink, reverse, strikethrough, hidden; foreground and
  background as default, 16, 256 or 24-bit colour. These go in the `VtCell`.
- **Wide characters**: a wide character takes two cells; the second is a continuation marker. Combining marks attach
  to the previous cell.
- **Cursor**: position, shape and visibility, drawn by the compositor for the focused pane only.
- **Alternate screen**: a second grid per session; switching to it touches the whole pane.
- **Size**: a `PicCell` is 88 bytes, a `VtCell` 12. A pane picture is fine either way, but **scrollback** (say 10,000
  lines of 200 columns) is 176 MB as `PicCell`s and 24 MB as `VtCell`s. The emulator should write compact text cells,
  and scrollback should never be `PicCell`s. See decision 1.
- **Scrolling**: a scroll shifts every row of the pane. The simple version moves the rows and touches the whole pane,
  so every row of it is rewritten. Later, when a pane's scroll region covers whole terminal rows, the compositor can
  use the terminal's own scroll commands instead. Measure before bothering.

### 7.5 What is probably the real cost

For a terminal multiplexer the hot paths are more likely the pty reads, parsing escape sequences, and the writes
to the terminal than the render pass. The retained design is right for it (a busy pane costs what it changes, a quiet
pane costs nothing), but measure the emulator first once it exists.

## 8. Decisions for you

1. **Cell size.** Make `PicCell` smaller (floats instead of doubles gives about 44 bytes), or add a compact text cell
   type for emulator pictures, or both. Recommend the compact text cell: it is what scrollback needs anyway.
2. **Thread hand-off.** A (publish with one short lock) or B (lock-free). Recommend A.
3. **Touched is conservative.** Any non-const `at()` counts as touched (4.1). Recommend yes: nothing existing breaks.
4. **Where the emulator lives.** Its own repository (`vtmux`), using vtscreen. Recommend yes.
5. **Start.** Steps 1 to 4 of section 6 on vtscreen now, then a `blit` for combining panes, before any emulator.
