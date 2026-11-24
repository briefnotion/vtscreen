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
| `shimmer` | how fast the cell's letter rotates inside its brightness group: 1 (the default) is the normal rate, 0 holds the letter still, 2 is twice as fast. A star could use 0.2 for a slow twinkle and an ember 3. A cell cannot change letter more than once a frame, so at 30 fps nothing above about 2.5 looks different |
| `alt_lum`, `alt_r`, `alt_g`, `alt_b` | the alt colour, the other colour of the pair: brightness 0 to 1 and colour 0 to 255. See below. The default, `alt_lum = 0`, means none |
| `text`, `ch`, `inv` | a letter the scene chooses itself: when `text` is set the cell is drawn as the character `ch` (inverse video if `inv`) in the colour `r`, `g`, `b`, and `lum` and `shimmer` are ignored |

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
character (`char32_t`), so `°`, `─` or `█` work. Every character is taken to be one
column wide: wide characters (Chinese, emoji) and combining marks are not handled. On
Windows the console is switched to UTF-8 while the program runs.

**The status bar.** `Display::bar(picture, info, t, loop, suffix)` writes the default
bar into the bottom row as text cells: the scene's title (and the suffix, which `show`
uses for `[n h]` or `[HOLD]`), the time and the actual and target fps, in inverse video
in the scene's accent colour. `play()` and `show` call it for you. A scene that writes
its own text in the bar row keeps it. Because the bar is cells like the rest, a
cross-fade mixes the two bars' colours; the characters switch at the halfway point.
In a fade, text cells follow that rule: where both pictures have text the colours are
mixed and the letter comes from the picture that is ahead; where only one has text, that
cell is taken as it is.

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
