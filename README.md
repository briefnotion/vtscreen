# vtscreen

A small header-only C++ library that draws animations made of letters in a terminal.
A program describes a picture as brightness and colour for every cell; `vtscreen.h` turns
it into letters (16 brightness steps, the top eight in inverse video, with a shimmer),
keeps a copy of what the terminal shows, and sends only what changed. It also owns the
terminal session (alternate screen, keys, Ctrl-C), the clock, the fps counter and the
status bar, and it can mix two pictures for a cross-fade.

It was written for [vtshow](https://github.com/briefnotion/vtshow), a set of scenes and a
show that plays them, and is meant to be used by other programs too.

## Using it

Requirements: Linux or Windows 10 or later, `g++` with C++17 support (or Visual C++), and a
terminal. Nothing else. Put this repo where your project can reach it (as a git submodule,
for example) and include the header:

    #include "vtscreen/vtscreen.h"

    g++ -O2 -std=c++17 -o myprogram myprogram.cpp

The comment at the top of `vtscreen.h` has a short example of a scene and of a `main` that
runs it with `vtscreen::run(scene, argc, argv)`, which understands `--loop`, `--no-color`,
`--still T`, `--fps N`, `--cols N` and `--rows N`.

- Colour is 24-bit. Most desktop terminals and SSH clients support it; the plain Linux text
  console does not, so use `--no-color` there.
- If the window is resized while the program runs, the Display and the scene are set up again
  at the new size and the clock carries on. On Linux this uses the terminal's resize signal
  (`SIGWINCH`), on Windows the console's window-size event. A terminal smaller than 40 x 20 is
  not followed. `--cols` or `--rows` keep that dimension as you gave it.
- On Windows the code uses the console API instead of termios and poll (`#ifdef _WIN32`),
  and switches the console to UTF-8 while the program runs. Windows Terminal is the best
  place to run it; it has been run there (smoothly, with the keys and resizing working).
  `VTSCREEN_FORCE_WINDOWS` selects the Windows branch on any system, for compile-checking only.

## Credit

The look of this project, letters grouped by how much ink they carry and a scene described
as `setup(cols, rows)` once and then `frame(t)` as a pure function of time, comes from the
**python.animate** YouTube channel: **https://www.youtube.com/@python.animate**

The table of letter groups (`LEVELS` in `vtscreen.h`) is the channel's. Everything else
here was written for this project, with Claude. This is an unofficial personal project, not
affiliated with or endorsed by the channel.

## How it fits together

    scene (your code)                  vtscreen.h
    ------------------                 ----------------------------------------
    setup(cols, rows)                  Display: the terminal session (size,
    frame(t, Picture&):                  alternate screen, keys, Ctrl-C), the clock,
      for every cell a brightness        the fps counter, the status bar
      0..1, a colour and a shimmer     letters: 16 steps, the top eight in inverse
      speed, or a letter of its own      video, plus a shimmer that follows --fps
      (text, such as the readout)      colours rounded to 16 levels per channel
                                       compares with what the terminal shows and
                                         sends only the differences
                                       fade(a, b, pct): mix two pictures

A scene never sees a letter or an escape code. Because a `Picture` is just numbers,
two of them can be mixed cell by cell and the letters picked once for the result,
which is how the cross-fade works. Only the scene on screen (and the one fading
out) is computed.

A `Picture` is a grid of `PicCell`s, the same shape and size as the `VtScreen` of
finished letters that the Display keeps: the whole terminal, with the status bar as its
last row. `F.init(R, C)` takes the rows of the picture proper (what `setup()` was given)
and adds the bar row underneath; `F.at(i, j)` reaches a cell, `F.bar_row()` the bar. A
scene fills in, for each cell:

| field | meaning |
|---|---|
| `lum` | brightness, 0 to 1 |
| `r`, `g`, `b` | colour, 0 to 255 |
| `shimmer` | how fast the cell's letter rotates inside its brightness group: 1 (the default) is the normal rate, 2 is twice as fast, and 0 shows the first letter of the group in every such cell (no scatter, no rotation), so a cell that must hold still, and look the same as its neighbours, uses 0. Any value above 0, however small, shows the varied letters, only very slowly. A star could use 0.2 for a slow twinkle and an ember 3. A cell cannot change letter more than once a frame, so at 30 fps nothing above about 2.5 looks different |
| `alt_lum`, `alt_r`, `alt_g`, `alt_b` | the alt colour, the other colour of the pair: brightness 0 to 1 and colour 0 to 255. See below. The default, `alt_lum = 0`, means none |
| `text`, `ch`, `inv` | a letter the scene chooses itself: when `text` is set the cell is drawn as the character `ch` (inverse video if `inv`) in the colour `r`, `g`, `b`, and `lum` and `shimmer` are ignored |
| `attrs` | for a text cell, any of `CELL_BOLD`, `CELL_ITALIC`, `CELL_UNDERLINE`, `CELL_STRIKE`, `CELL_BLINK` added together (`|`), and `CELL_WIDE` / `CELL_TAIL` for a wide letter (see below). Default 0: drawn as before |

`shimmer` only chooses which letter of the group shows. For a cell that dims and
brightens, the scene also varies `lum`. `init()` resets every cell to the defaults
each frame, so a scene only sets what it wants different.

**The alt colour.** Every cell is drawn with two colours: the primary `r, g, b` and an
alt colour. In a dim cell (`lum` below 0.5) the primary is the letter and the alt is the
background behind it. In a bright cell (0.5 and above, drawn in inverse video) the
primary is the solid block and the alt is the letter on it. So a meadow can be a dark
green block with lighter green letters: set `lum` above 0.5, `r, g, b` to the dark green,
and `alt_lum = 1` with `alt_r, alt_g, alt_b` the light green. The colour shown is
`alt_r, alt_g, alt_b` scaled by `alt_lum`, and `alt_lum = 0` (the default) means no alt
colour: the terminal's own colour is left alone and nothing extra is sent, so a scene
that never sets it looks and costs what it always did. The primary decides the letter and
the flip at 0.5; the alt never does. `--no-color` ignores the alt, like all colour.
In a fade the alt's brightness and colour are mixed like the rest, so an alt that one
scene lacks fades toward none.

**Text.** `F.text(row, col, "flame 2.2 m")` writes a string as text cells, in the readout
grey unless you give a colour: `F.text(row, col, s, r, g, b, inv)`. The scenes use it
for the readout in the top-left corner. Text is UTF-8 and each cell holds one Unicode
character (`char32_t`), so `°`, `─` or `█` work. A character is taken to be one column wide
(combining marks are not handled), except where you say otherwise (below). On Windows the
console is switched to UTF-8 while the program runs.

**Attributes.** A text cell can be bold, italic, underlined, struck through or blinking:
`PicCell c; c.text = true; c.ch = U'x'; c.attrs = CELL_BOLD | CELL_UNDERLINE;`. The Display tells
the terminal with the usual codes (`ESC[1m`, `3m`, `4m`, `9m`, `5m` and the matching offs), only when
they change from one cell to the next. How a terminal draws them, or whether it does, is up to the
terminal program (blink in particular is often off). An underlined or struck-through space is
drawn in the cell's colour; any other space has no colour, as before. A cell with no attributes is
drawn exactly as it always was, and a program that uses none sends the same bytes as before.

**Wide characters.** A letter that takes two columns is two cells in the picture: the first has
`CELL_WIDE` and the letter, the second has `CELL_TAIL` (and the same colours, so a pair is drawn with
one pen). The Display writes the first, which fills both columns, and skips the second; after it the
terminal's cursor is moved explicitly, since terminals disagree about how wide some letters are. You
must set `CELL_WIDE` only on a letter the terminal draws two columns wide (you know the width: the
library does not have a table). A half of a pair without the other (a pair cut by the edge of the
picture, or in the last column, or a picture mixed from two) is drawn as a blank, so mixing pictures
cell by cell is safe.

**The status bar.** `Display::bar(picture, info, t, loop, suffix)` writes the default
bar into the bottom row as text cells: the scene's title (and the suffix, which `show`
uses for `[n h]` or `[HOLD]`), the time and the actual and target fps, in inverse video
in the scene's accent colour. `play()` and `show` call it for you. A scene that writes
its own text in the bar row keeps it. Because the bar is cells like the rest, a
cross-fade mixes the two bars' colours; the characters switch at the halfway point.
In a fade, text cells follow that rule: where both pictures have text the colours are
mixed and the letter comes from the picture that is ahead; where only one has text, that
cell is taken as it is.

## Retained pictures

Usually a program fills its whole picture again every frame, and the Display looks at
every cell. A program that keeps its picture between frames and changes only what has
changed can say so: call `picture.retain()` once, after `init()`. The Picture then notes
which cells were written (*touched*), and each frame the Display only works out the
letters of the touched cells and of the cells that shimmer (their letter moves with
time), and skips the rest. What ends up on the screen, and the bytes sent to the
terminal, are exactly the same either way; only the work differs. A screen where nothing
happens costs nothing: no letters are worked out, nothing is compared, nothing is sent.

Measured on a 160 x 49 screen (microseconds a frame for `Display::render`, no terminal;
`tests/bench_render.cpp`):

| screen | ordinary | retained |
|---|---|---|
| all text, nothing changes | 112 | 0.2 |
| all text, 20 cells change a frame | 122 | 13 |
| 90 % still cells, 10 % shimmering | 339 | 119 |
| every cell shimmering | 764 | 763 |

The rules of retained mode:

- Write cells with `at(i, j)`, `set(i, j, cell)` or `text(...)`, not straight into the
  `cell` vector: that cannot be noticed. If a program does write into `cell`, it says so
  with `touch(i, j)` or `touch_all()`.
- `at()` counts as a write even if the cell ends up as it was (a false alarm costs one
  cheap check). `set()` and `text()` only count when the cell is really different, so
  setting the same thing every frame costs nothing. Reading through a `const Picture&`
  is never a write.
- `init()` starts the picture over (every cell is new); so does copying or moving into
  a Picture, and `fade()` into one.
- A Picture can be shown on a different Display or alternate with another Picture; the
  Display notices a picture it has not seen last and looks at all of it.
- A Display whose `color`, `fps` or `hash_letters` changes works every letter out again.
  Set `color` once, before the first frame: cells already on the terminal keep the colour
  they were drawn in.
- `#ifdef VTSCREEN_RETAINED` tells a program the library has this.

Without `retain()` nothing changes: no flags are kept and every cell is looked at, as it
always was (the scenes in vtshow work like this and were not touched). Inside the
Display the letters the terminal should show (`target`) and what it was last sent
(`shown`) are kept between frames; a cell of `target` that changes is flagged, and
sending visits only the flagged cells and rows, so a frame with nothing flagged scans
nothing.

## Tests

`tests/run_tests.sh` builds `tests/retained_tests.cpp` with the strict warning flags and
the address and undefined-behaviour sanitizers, and runs it. It checks:

- that the bytes sent are the bytes the library sent before retained mode, on fixed
  pseudo-random sequences (hashes made with the older library are pinned in the test);
- that, for 20 000 random frames of every kind of edit (writes, text, `set`, false alarms,
  shimmer on and off, direct writes with `touch`, copies, swaps, resizes, refreshes, fades,
  switching pictures, changing `hash_letters` and `fps`), a retained picture and an
  ordinary one make the Display send identical bytes;
- that a small model of a terminal, reading those bytes, shows the same screen as
  `render_plain` (which converts a picture with code of its own) for each frame;
- that skipping is real (a cell written behind the picture's back is not noticed until
  it is touched), shimmer is followed without being touched, and copies, moves, serials and
  the flags behave as described above.

`tests/attr_tests.cpp` (run by the same script) checks attributes and wide letters with a model of a
terminal: the codes sent, nothing sent when nothing changed, underlined spaces, pairs drawn and
redrawn when only one half changes, halves without a partner shown as blanks, `render_plain`, fades,
and 16 000 random frames (retained and ordinary pictures, terminals that do and do not blank the
other half of a wide letter written over) against what the pictures say.

`tests/bench_render.cpp` prints the timings above.

## Writing a scene

Derive from `vtscreen::Scene` (`info()`, `setup(cols, rows)`, `frame(t, Picture&)`),
put its internals in an anonymous namespace, and end the file with a factory like
`make_thunderstorm()` (see `thunderstorm/thunderstorm.cpp`, `bonfire/bonfire.cpp` and
`scenes.h` in the vtshow repo). For a single-scene program, `main` is `vtscreen::run(scene, argc, argv)`.
`setup(cols, rows)` is called again, with the new size, whenever the terminal is resized,
so it must start the scene's state over from scratch each time rather than rely on being
called once; `frame()` stays a pure function of `t`.
The comment at the top of `vtscreen.h` has a short example. Compile a scene with
`-DVTSHOW_NO_MAIN` to leave out its `main` so it can be linked into vtshow's `show`.

## Licence

MIT, see `LICENSE`. The letter table in `vtscreen.h` is the channel's, see Credit above.
