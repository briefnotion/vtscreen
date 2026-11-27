// vtscreen.h - draw a picture made of brightness and colour as letters on a
// VT100 terminal. Header only: #include it and compile with g++ -std=c++17.
//
// The split
//
//   the scene (your code)              this header
//   ---------------------              -----------------------------------------
//   setup(cols, rows)                  owns the terminal: size, alternate screen,
//   frame(t) -> for every cell:          raw keys, Ctrl-C, restore on exit
//       brightness 0..1                owns the clock: fps, pacing, --loop, the
//       colour r,g,b                     measured fps, the status line, q to quit
//     + text cells (the readout)       turns brightness into a letter: 16 steps,
//                                        inverse video, shimmer
//                                      rounds colours
//                                      shows a scene's picture on the terminal,
//                                        sending only what changed
//
// A scene never sees a letter, an escape code or a frame rate. Minimal use:
//
//   struct MyScene : vtscreen::Scene {
//       vtscreen::Info info() const override { return {"my scene", 24.0, {255, 140, 40}}; }
//       void setup(int cols, int rows) override { ... }
//       void frame(double t, vtscreen::Picture& F) override { ... }
//   };
//   int main(int argc, char** argv) { MyScene s; return vtscreen::run(s, argc, argv); }
//
// run() understands: --loop  --no-color  --still T  --fps N  --cols N  --rows N
//
// The letters (LEVELS) are grouped by how much ink they carry, in the style of
// the python.animate channel's ASCII shorts: https://www.youtube.com/@python.animate

#pragma once

// On Windows (_WIN32) the terminal, keyboard and timing code uses the Windows
// console API; elsewhere it uses POSIX (termios, poll). The Windows branch needs
// Windows 10 or later, where the console understands escape codes. Defining
// VTSCREEN_FORCE_WINDOWS selects it on any system (for compile-checking only).
#if defined(_WIN32) || defined(VTSCREEN_FORCE_WINDOWS)
#define VTSCREEN_WINDOWS 1
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef VTSCREEN_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX                  // keep std::min and std::max working
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#ifdef near                       // old Windows headers define these as empty macros
#undef near
#endif
#ifdef far
#undef far
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifndef M_PI                      // Visual C++ only defines it with _USE_MATH_DEFINES
#define M_PI 3.14159265358979323846
#endif

// Defined when the library has retained pictures (Picture::retain), so a program can tell.
#define VTSCREEN_RETAINED 1
// Defined when the library has pictures made of pixels (Raster, Picture::place_image, Display::enable_images).
#define VTSCREEN_IMAGES 1

// The one place a letter is worked out for a cell is the hot path of every frame; a compiler does not always inline it
// by itself, and it is a good deal slower when it does not.
#if defined(_MSC_VER)
#define VTSCREEN_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define VTSCREEN_FORCE_INLINE inline __attribute__((always_inline))
#else
#define VTSCREEN_FORCE_INLINE inline
#endif

namespace vtscreen {

// ---------------------------------------------------------------- tuning

// Colours are rounded to this many levels per channel before they are sent, so
// neighbouring cells share a colour code more often (fewer bytes per frame).
#ifndef VTSCREEN_COLOR_LEVELS
#define VTSCREEN_COLOR_LEVELS 16
#endif
inline constexpr int COLOR_LEVELS = VTSCREEN_COLOR_LEVELS;

// Redraw everything now and then, in case the terminal missed something.
inline constexpr double REFRESH_SECONDS = 10.0;

// The letters inside a group rotate once every this many frames, so the shimmer
// speeds up with --fps (2.5 frames = 12 steps a second at the default 30 fps).
inline constexpr double SHIMMER_FRAMES = 2.5;

// Letters grouped by how much ink they carry, dark -> bright.
inline const std::vector<std::string> LEVELS = {
    " ", ".`'", ":;,-~", "=+*^", "ijltrxz/\\|", "oecnuv()", "0O8B&%", "#@$QW"};

// ---------------------------------------------------------------- characters

// A cell holds one Unicode code point (char32_t); text goes in and out as UTF-8.
// Every character is taken to be one column wide: wide characters (Chinese, emoji)
// and combining marks are not handled, and would shift the rest of their line.
namespace detail {
// the next code point of a UTF-8 string, moving i past it; a bad sequence gives '?'
inline char32_t next_utf8(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n;
    char32_t cp;
    if (c >= 0xC2 && c <= 0xDF)      { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
    else return U'?';
    for (int k = 0; k < n; k++) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return U'?';
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    const char32_t least = n == 1 ? 0x80 : (n == 2 ? 0x800 : 0x10000);
    if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return U'?';
    return cp;
}

inline std::u32string to_u32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) out += next_utf8(s, i);
    return out;
}

inline void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) { out += (char)c; return; }
    if (c < 0x800) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

inline std::string to_utf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) append_utf8(out, c);
    return out;
}
}  // namespace detail

// ---------------------------------------------------------------- what a scene gives us

// One cell of a scene's picture. Normally: how bright it is, what colour, and how
// fast its letter shimmers; the Display picks the letter. lum is 0..1; r, g, b are
// 0..255; shimmer is a speed, 1 = the normal rate (the letter inside its group
// changes once every SHIMMER_FRAMES frames), 3 = three times as fast. A shimmer of 0 (or
// less) shows the first letter of the group, the same in every such cell: no scatter, no
// rotation. Any shimmer above 0, however small, shows the varied letters.
//
// The alt colour is the other colour of the pair every cell has. The primary
// colour r, g, b is the letter in a dim cell and the solid block in a bright (inverse)
// cell; the alt colour is what is behind the letter in a dim cell and the letter
// itself in a bright one. alt_lum is its brightness, 0..1: the colour shown is
// alt_r, alt_g, alt_b scaled by alt_lum, and 0 means no alt colour at all (the
// terminal's own colour is left alone, and nothing extra is sent). With --no-color
// it is not used.
//
// A text cell (text = true) is different: it is a letter the scene chose itself.
// The Display draws ch, in inverse video if inv is set, in the colour r, g, b, and
// ignores lum and shimmer. Text is how the readout in the corner is drawn.
// What a text cell can be, besides its letter, colours and inverse flag (PicCell::attrs). The terminal draws them: how (and whether) is up
// to the terminal program. A cell with none of them is drawn as it always was.
constexpr uint8_t CELL_BOLD = 1;         // bold weight
constexpr uint8_t CELL_ITALIC = 2;
constexpr uint8_t CELL_UNDERLINE = 4;    // a plain single underline
constexpr uint8_t CELL_STRIKE = 8;       // strikethrough
constexpr uint8_t CELL_BLINK = 16;       // the terminal's own blink
// A letter that takes two columns (Chinese, an emoji ...) is two cells: the first, with CELL_WIDE and the letter, and the second, with
// CELL_TAIL, which draws nothing itself (the same colours as the first make it look right). A half of a pair without the other is drawn as a
// blank, so a picture cut in the middle of one (a slide, a dissolve) is safe.
constexpr uint8_t CELL_WIDE = 32;
constexpr uint8_t CELL_TAIL = 64;
constexpr uint8_t CELL_LOOKS = CELL_BOLD | CELL_ITALIC | CELL_UNDERLINE | CELL_STRIKE | CELL_BLINK;   // the ones the terminal is told about

struct PicCell {
    double lum = 0;
    double r = 0, g = 0, b = 0;
    double shimmer = 1;
    double alt_lum = 0;
    double alt_r = 0, alt_g = 0, alt_b = 0;
    bool text = false;
    char32_t ch = U' ';
    bool inv = false;
    uint8_t attrs = 0;                        // CELL_BOLD ... (text cells only)

    bool operator==(const PicCell& o) const {
        return lum == o.lum && r == o.r && g == o.g && b == o.b && shimmer == o.shimmer &&
               alt_lum == o.alt_lum && alt_r == o.alt_r && alt_g == o.alt_g && alt_b == o.alt_b &&
               text == o.text && ch == o.ch && inv == o.inv && attrs == o.attrs;
    }
    bool operator!=(const PicCell& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------- pictures made of pixels
//
// Besides letters, a Picture can carry real pixels: a Raster placed at a cell. A Display that has
// been told the terminal can show Sixel pictures (Display::enable_images) draws them over the
// letters; one that has not simply ignores them, and then its output is exactly what it was
// without them. Nothing here knows where a Raster comes from: a scene fills it pixel by pixel,
// whether by its own code, a drawing library or a decoded picture.
//
// A pixel with a of 0 draws nothing (what is below shows, letters included); any other a draws
// the pixel in its colour. There is no half way: Sixel has none.
struct Pixel {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Pixel& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Pixel& o) const { return !(*this == o); }
};

struct Raster {
    int w = 0, h = 0;
    std::vector<Pixel> px;                    // w * h, row by row

    void init(int W, int H) {
        w = W > 0 ? W : 0;
        h = H > 0 ? H : 0;
        px.assign((size_t)w * (size_t)h, Pixel());
    }
    void clear() { std::fill(px.begin(), px.end(), Pixel()); }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    Pixel& at(int x, int y) { return px[(size_t)y * w + x]; }
    const Pixel& at(int x, int y) const { return px[(size_t)y * w + x]; }
    // Out of range is left alone.
    void set(int x, int y, int r, int g, int b, int a = 255) {
        if (!inside(x, y)) return;
        Pixel& p = at(x, y);
        p.r = (uint8_t)r; p.g = (uint8_t)g; p.b = (uint8_t)b; p.a = (uint8_t)a;
    }
};

// A Raster put on a Picture: its top left corner is at the cell (row, col).
//
// letter_rate: how often a letter under its pixels may change. A letter written into a cell takes that cell's pixels with
// it, so when letters change under a picture (shimmer, a moving shade) every such cell has to be written and then drawn
// again, which costs a lot more than the letter did, and shimmering letters under thin lines make it happen all the time.
//   -1 (the default): as often as they like; the picture is sent again whenever a letter under it is written.
//   a number of 0 or more: at most that many times a second for each cell that has pixels of this picture in it (the cells
//      take their turns at different moments, so they do not all change in the same frame). 0 means never while the
//      picture is on the cell. A cell that has to wait shows its old letter, colour and all, so a low rate lags: the content
//      under a picture follows what the scene says, a little behind. Text cells (Picture::text) are never held back.
// The letters catch up when the picture leaves the cell or this changes.
struct ImagePlacement {
    std::shared_ptr<const Raster> raster;
    int row = 0, col = 0;
    double letter_rate = -1;
};

namespace detail {
// Every Picture has a number of its own, so a Display can tell which picture it showed last.
inline uint64_t next_picture_serial() {
    static std::atomic<uint64_t> n(0);
    return ++n;
}
}  // namespace detail

// What a scene draws on: a grid of PicCells, rows * cols, row by row, the same
// shape as a VtScreen (which holds the finished letters), so it is the whole
// terminal. The Display turns it into letters. The last row is the status bar.
//
// Retained mode. Usually a program fills the whole picture again every frame, and the
// Display looks at every cell. A program that keeps its picture between frames and only
// changes what has changed can say so (retain()): the Picture then notes which cells were
// written (touched), and the Display only looks at those, and at the cells that shimmer
// (their letter changes with time), and skips the rest. The picture is the same either way,
// and so is what is sent; only the work differs.
//
// The rules of retained mode:
//   - write cells through at(), set() or text(), never straight into `cell`: that cannot be
//     noticed. (A program that does write into `cell` should not call retain(), or should call
//     touch_all() afterwards.)
//   - at() counts as a write: it may be a false alarm (the cell may end up as it was), which
//     costs one cheap check and nothing else. set() and text() only count as a write when
//     the cell really is different, so a program that sets the same thing every frame costs nothing.
//   - init() starts over (every cell is new); a Picture that is copied or moved into is new too.
//   - the flags are used up by a Display's render, so skipped frames lose nothing.
// Without retain() nothing is noted and every cell is looked at, as it always was.
struct Picture {
    int rows = 0, cols = 0;
    std::vector<PicCell> cell;

    bool retained = false;                    // see above
    uint64_t serial = detail::next_picture_serial();

    // How many fades deep this picture is (fade() sets it): a picture made by fading pictures that were made by fading is one
    // deeper, so each level dissolves the pictures made of pixels in an order of its own and the shares multiply.
    uint32_t fade_level = 0;

    // Pictures made of pixels, drawn over the letters in this order (see Raster). The Raster is
    // looked at when the picture is shown, so a scene can keep one and change it between frames.
    std::vector<ImagePlacement> images;
    void place_image(std::shared_ptr<const Raster> raster, int row, int col, double letter_rate = -1) {
        if (raster) images.push_back({std::move(raster), row, col, letter_rate});
    }
    void clear_images() { images.clear(); }

    // Bookkeeping for retained mode; changed by a Display's render even for a const Picture.
    mutable bool all_touched = true;                     // every cell is new (init, a copy, a resize)
    mutable std::vector<unsigned char> touched;          // per cell: written since a render last looked
    mutable std::vector<unsigned char> animated;         // per cell: shimmering, so its letter moves with time
    mutable std::vector<unsigned char> row_touched;      // per row: a cell in it is touched
    mutable std::vector<int> animated_in_row;            // per row: how many of its cells are animated

    Picture() = default;
    // A copy has a number of its own, and every cell of it is new.
    Picture(const Picture& o) : rows(o.rows), cols(o.cols), cell(o.cell), retained(o.retained), fade_level(o.fade_level), images(o.images) {}
    Picture& operator=(const Picture& o) {
        if (this != &o) { rows = o.rows; cols = o.cols; cell = o.cell; retained = o.retained; fade_level = o.fade_level; images = o.images; forget_flags(); }
        return *this;
    }
    Picture(Picture&& o) noexcept
        : rows(o.rows), cols(o.cols), cell(std::move(o.cell)), retained(o.retained), fade_level(o.fade_level), images(std::move(o.images)) { o.forget_flags(); }
    Picture& operator=(Picture&& o) noexcept {
        if (this != &o) {
            rows = o.rows; cols = o.cols; cell = std::move(o.cell); retained = o.retained; fade_level = o.fade_level; images = std::move(o.images);
            forget_flags(); o.forget_flags();
        }
        return *this;
    }

    // Size it and clear every cell to the default. R is the rows of the picture
    // proper (what setup() was given); the status bar's row comes underneath, so
    // rows ends up R + 1.
    void init(int R, int C) {
        rows = R + 1; cols = C;
        cell.assign((size_t)(R + 1) * C, PicCell());
        images.clear();                           // starting over: no pictures made of pixels either
        fade_level = 0;
        forget_flags();
    }
    int bar_row() const { return rows - 1; }

    // Keep this picture between frames and note what is written (see above).
    void retain() { retained = true; all_touched = true; }
    // Say that a cell, or the whole picture, was changed some other way than through at(), set() or text().
    void touch(int i, int j) const {
        ensure_flags();
        touched[(size_t)i * cols + j] = 1;
        row_touched[(size_t)i] = 1;
    }
    void touch_all() const { all_touched = true; }

    PicCell& at(int i, int j) {
        if (retained) touch(i, j);
        return cell[(size_t)i * cols + j];
    }
    const PicCell& at(int i, int j) const { return cell[(size_t)i * cols + j]; }

    // Put a cell there. In retained mode it only counts as a write if it is different.
    void set(int i, int j, const PicCell& v) {
        PicCell& c = cell[(size_t)i * cols + j];
        if (c == v) return;
        c = v;
        if (retained) touch(i, j);
    }

    // Write s (UTF-8) as text cells along row, starting at col, over whatever is
    // there. The default colour is the readout grey. What does not fit on the
    // picture is left off.
    void text(int row, int col, const std::string& s, double r = 170, double g = 170,
              double b = 170, bool inv = false) {
        if (row < 0 || row >= rows) return;
        const std::u32string u = detail::to_u32(s);
        for (size_t k = 0; k < u.size(); k++) {
            const int j = col + (int)k;
            if (j < 0 || j >= cols) continue;
            PicCell c;
            c.text = true; c.ch = u[k]; c.inv = inv;
            c.r = r; c.g = g; c.b = b;
            set(row, j, c);
        }
    }

    // For the Display: size the flags to the picture (a picture whose size changed is all new).
    void ensure_flags() const {
        if (touched.size() != cell.size() || row_touched.size() != (size_t)rows) {
            touched.assign(cell.size(), 0);
            animated.assign(cell.size(), 0);
            row_touched.assign((size_t)rows, 0);
            animated_in_row.assign((size_t)rows, 0);
            all_touched = true;
        }
    }
    // For the Display: a render has looked at everything that was touched.
    void consume() const {
        ensure_flags();
        std::fill(touched.begin(), touched.end(), (unsigned char)0);
        std::fill(row_touched.begin(), row_touched.end(), (unsigned char)0);
        all_touched = false;
    }

private:
    void forget_flags() const {
        all_touched = true;
        touched.clear(); animated.clear(); row_touched.clear(); animated_in_row.clear();
    }
};

struct Info {
    const char* title;     // left end of the status line
    double duration;       // seconds; without --loop the show stops here
    int accent[3];         // colour of the status line
    // How the letter inside a group is chosen. false: a fixed diagonal shuffle
    // (what the campfire, moon, fireworks and black hole use). true: a hash of
    // position and time, which has no diagonal pattern in large flat areas
    // (what the thunderstorm uses).
    bool hash_letters = false;
};

class Scene {
public:
    virtual ~Scene() = default;
    virtual Info info() const = 0;
    // Called once the size is known, and again each time the terminal is resized
    // (so it must start the scene's state over). rows is the picture only: the
    // status line takes one more row underneath.
    virtual void setup(int cols, int rows) = 0;
    // Fill F for time t (seconds). A pure function of t is easiest to reason about.
    virtual void frame(double t, Picture& F) = 0;

    // The scene's own picture, kept between frames so a Display can be pointed at
    // it (and, later, two of them mixed). cycle(t) refreshes it for time t.
    Picture screen;
    void cycle(double t) { frame(t, screen); }
};

// ---------------------------------------------------------------- mixing two pictures

namespace detail {
inline uint32_t pixel_hash(int x, int y, uint32_t seed) {
    uint32_t h = ((uint32_t)x * 0x9E3779B1u) ^ (((uint32_t)y + 0x7F4A7C15u) * 0x85EBCA77u) ^ (seed * 0xC2B2AE3Du);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

// A picture made of pixels, thinned to the fraction keep (0 to 1) of its pixels. Each pixel has a place of its own in a fixed
// order (a hash of where it is and `seed`, which tells pictures apart), and the ones kept are the first keep of them: as keep
// goes up, the pixels that were there stay and more come, and as it goes down, the same ones go last. That makes a fade a
// dissolve. A keep of 1 gives the picture itself, and of 0 or less nothing (an empty placement, with no raster).
inline ImagePlacement thinned(const ImagePlacement& p, double keep, uint32_t seed) {
    if (!p.raster || keep <= 0.0) return ImagePlacement();
    if (keep >= 1.0) return p;
    auto r = std::make_shared<Raster>(*p.raster);
    const uint32_t limit = (uint32_t)(keep * 4294967295.0);
    for (int y = 0; y < r->h; y++)
        for (int x = 0; x < r->w; x++) {
            Pixel& q = r->at(x, y);
            if (q.a != 0 && pixel_hash(x, y, seed) >= limit) q = Pixel();
        }
    ImagePlacement out = p;
    out.raster = std::move(r);
    return out;
}
}  // namespace detail

// Mix two pictures of the same size into out: pct 0 gives a, 1 gives b, in
// between is a cross-fade. Brightness, colour and shimmer are mixed cell by cell,
// before any letters are picked, so the Display picks the letters once for the
// mixture. Text cannot be mixed, so a cell that is text in either picture takes
// the letter from the picture that is ahead (a up to the halfway point, b after
// it). If both are text the colours are mixed as well; if only one is, the cell
// is that picture's cell as it is. The pictures made of pixels (Picture::place_image) dissolve:
// each thins out by the share it is not ahead by, the pixels that go first coming back last, with
// a's under b's (a picture whose weight is 1 is used as it is, one of 0 is left out).
inline void fade(const Picture& a, const Picture& b, double pct, Picture& out) {
    pct = pct < 0 ? 0 : (pct > 1 ? 1 : pct);
    const double wa = 1.0 - pct, wb = pct;
    out.rows = a.rows; out.cols = a.cols;
    out.cell.resize(a.cell.size());
    {   // pixels cannot be mixed either, so the pictures dissolve: each thins out by the share it is not ahead by (a under b)
        std::vector<ImagePlacement> mixed_images;
        const uint32_t level = std::max(a.fade_level, b.fade_level) + 1;          // a deeper fade dissolves in an order of its own
        for (size_t k = 0; k < a.images.size(); k++) {
            ImagePlacement t = detail::thinned(a.images[k], wa, level * 100003u + 1000u + (uint32_t)k);
            if (t.raster) mixed_images.push_back(std::move(t));
        }
        for (size_t k = 0; k < b.images.size(); k++) {
            ImagePlacement t = detail::thinned(b.images[k], wb, level * 100003u + 2000u + (uint32_t)k);
            if (t.raster) mixed_images.push_back(std::move(t));
        }
        out.images = std::move(mixed_images);
        out.fade_level = level;
    }
    out.touch_all();                          // every cell is written below
    for (size_t i = 0; i < a.cell.size(); i++) {
        const PicCell& p = a.cell[i];
        const PicCell& q = b.cell[i];
        PicCell& o = out.cell[i];
        if (p.text || q.text) {
            o = pct < 0.5 ? p : q;
            if (p.text && q.text) {
                o.r = p.r * wa + q.r * wb;
                o.g = p.g * wa + q.g * wb;
                o.b = p.b * wa + q.b * wb;
                o.alt_lum = p.alt_lum * wa + q.alt_lum * wb;
                o.alt_r = p.alt_r * wa + q.alt_r * wb;
                o.alt_g = p.alt_g * wa + q.alt_g * wb;
                o.alt_b = p.alt_b * wa + q.alt_b * wb;
            }
            continue;
        }
        o = PicCell();
        o.lum = p.lum * wa + q.lum * wb;
        o.r = p.r * wa + q.r * wb;
        o.g = p.g * wa + q.g * wb;
        o.b = p.b * wa + q.b * wb;
        o.shimmer = p.shimmer * wa + q.shimmer * wb;
        o.alt_lum = p.alt_lum * wa + q.alt_lum * wb;
        o.alt_r = p.alt_r * wa + q.alt_r * wb;
        o.alt_g = p.alt_g * wa + q.alt_g * wb;
        o.alt_b = p.alt_b * wa + q.alt_b * wb;
    }
}

// ---------------------------------------------------------------- letters and colours

namespace detail {
inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// a colour channel (0..255) as the nearest whole number, not rounded to colour levels
inline int byte(double v) { return (int)(clampd(v, 0, 255) + 0.5); }

// the codes that take a terminal's text attributes from `pen` to `want` (only the CELL_LOOKS bits count); pen becomes want's
inline void sgr_attrs(std::string& out, unsigned char& pen, unsigned char want) {
    const unsigned char diff = (unsigned char)((pen ^ want) & CELL_LOOKS);
    if (!diff) return;
    if (diff & CELL_BOLD) out += (want & CELL_BOLD) ? "\x1b[1m" : "\x1b[22m";
    if (diff & CELL_ITALIC) out += (want & CELL_ITALIC) ? "\x1b[3m" : "\x1b[23m";
    if (diff & CELL_UNDERLINE) out += (want & CELL_UNDERLINE) ? "\x1b[4m" : "\x1b[24m";
    if (diff & CELL_STRIKE) out += (want & CELL_STRIKE) ? "\x1b[9m" : "\x1b[29m";
    if (diff & CELL_BLINK) out += (want & CELL_BLINK) ? "\x1b[5m" : "\x1b[25m";
    pen = (unsigned char)((pen & ~CELL_LOOKS) | (want & CELL_LOOKS));
}

inline bool stdout_is_tty() {
#ifdef VTSCREEN_WINDOWS
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}
}

// round one colour channel (0..255) to COLOR_LEVELS evenly spaced values
inline int quant(double v) {
    const double step = 255.0 / (COLOR_LEVELS - 1);
    return (int)(std::floor(detail::clampd(v, 0, 255) / step + 0.5) * step + 0.5);
}

// 16 brightness steps. 0 .. 0.5 uses groups 0..7 as they are; 0.5 .. 1 uses
// groups 7..0 drawn in inverse video, so the hottest cells are solid colour.
// The group picks the letters; position and time pick one, so it shimmers.
// shimmer is the cell's speed (PicCell::shimmer): 1 = the normal rate, 3 = three times as
// fast. 0 (or below) is the first letter of the group, the same everywhere; above 0 the
// letter is picked by position (and moves with time at that speed).
inline char pick_char(double lum, int i, int j, double t, int fps, bool& inv,
                      bool hash = false, double shimmer = 1.0) {
    int n = (int)LEVELS.size();
    int step = std::min(std::max((int)(lum * 2 * n), 0), 2 * n - 1);
    inv = step >= n;
    const std::string& g = LEVELS[inv ? 2 * n - 1 - step : step];
    if (shimmer <= 0) return g[0];                 // still: the group's first letter, the same in every cell
    const long ticks = (long)(t * fps / SHIMMER_FRAMES * shimmer);
    if (hash) {
        uint32_t h = (uint32_t)i * 73856093u ^ (uint32_t)j * 19349663u ^
                     (uint32_t)ticks * 83492791u;
        // Mix the bits before taking the remainder: the low bits of the products above
        // repeat every few cells, which tiled the letters of a flat area.
        h ^= h >> 16; h *= 0x7feb352dU;
        h ^= h >> 15; h *= 0x846ca68bU;
        h ^= h >> 16;
        return g[h % g.size()];
    }
    long shuffle = (long)i * 7 + (long)j * 13 + ticks;
    return g[shuffle % (long)g.size()];
}

// The whole frame as one block of text, every cell written (used for --still).
// vt_inv false: no inverse video codes, so the output is plain text.
inline std::string render_plain(const Picture& F, int R, int C, double t, int fps,
                                bool color, bool erase = true, bool vt_inv = true,
                                bool hash_letters = false) {
    std::string out;
    out.reserve((size_t)R * C * (color ? 12 : 2));
    char esc[40];
    for (int i = 0; i < R; i++) {
        int last[3] = {-1, -1, -1};
        unsigned char pen_attrs = 0;           // bold, italic ... currently on
        bool on = false;                       // inverse video currently on
        bool alt_on = false;                   // a background colour currently set
        int lastalt[3] = {-1, -1, -1};
        for (int j = 0; j < C; j++) {
            const PicCell& pc = F.cell[i * C + j];
            bool inv;
            char32_t ch;
            unsigned char attrs = 0;
            bool wide = false;
            if (pc.text) {
                ch = pc.ch; inv = pc.inv; attrs = (unsigned char)(pc.attrs & CELL_LOOKS);
                if (pc.attrs & CELL_WIDE) {            // two columns: only if its other half is there, otherwise a blank
                    wide = j + 1 < C && F.cell[i * C + j + 1].text && (F.cell[i * C + j + 1].attrs & CELL_TAIL);
                    if (!wide) ch = U' ';
                } else if (pc.attrs & CELL_TAIL) {
                    ch = U' ';                         // (a tail whose head is there is skipped with it, below: this one is on its own)
                }
            }
            else ch = pick_char(pc.lum, i, j, t, fps, inv, hash_letters, pc.shimmer);
            inv = inv && vt_inv;
            if (!vt_inv) attrs = 0;                    // plain text: no codes at all
            if (attrs != pen_attrs) detail::sgr_attrs(out, pen_attrs, attrs);
            if (inv != on) { out += inv ? "\x1b[7m" : "\x1b[27m"; on = inv; }
            if (color) {                                  // the alt colour (background)
                const bool alt = pc.alt_lum > 0;
                const double al = detail::clampd(pc.alt_lum, 0, 1);
                int ap[3] = {0, 0, 0};
                if (alt) {
                    if (pc.text) { ap[0] = detail::byte(pc.alt_r * al); ap[1] = detail::byte(pc.alt_g * al); ap[2] = detail::byte(pc.alt_b * al); }
                    else { ap[0] = quant(pc.alt_r * al); ap[1] = quant(pc.alt_g * al); ap[2] = quant(pc.alt_b * al); }
                }
                if (alt && (!alt_on || ap[0] != lastalt[0] || ap[1] != lastalt[1] || ap[2] != lastalt[2])) {
                    std::snprintf(esc, sizeof esc, "\x1b[48;2;%d;%d;%dm", ap[0], ap[1], ap[2]);
                    out += esc;
                    alt_on = true;
                    std::memcpy(lastalt, ap, sizeof lastalt);
                } else if (!alt && alt_on) {
                    out += "\x1b[49m";
                    alt_on = false;
                }
            }
            if (ch == U' ' && !inv && !(attrs & (CELL_UNDERLINE | CELL_STRIKE))) { out += ' '; continue; }
            if (color) {
                int px[3];
                if (pc.text) { px[0] = detail::byte(pc.r); px[1] = detail::byte(pc.g); px[2] = detail::byte(pc.b); }
                else { px[0] = quant(pc.r); px[1] = quant(pc.g); px[2] = quant(pc.b); }
                if (px[0] != last[0] || px[1] != last[1] || px[2] != last[2]) {
                    std::snprintf(esc, sizeof esc, "\x1b[38;2;%d;%d;%dm", px[0], px[1], px[2]);
                    out += esc;
                    std::memcpy(last, px, sizeof last);
                }
            }
            detail::append_utf8(out, ch);
            if (wide) j++;                                 // its second column is drawn with it
        }
        if (color || on || pen_attrs) out += "\x1b[0m";
        if (erase) out += "\x1b[K";
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------- a screen: what a scene draws on
//
// A VtScreen is plain data: a grid of cells, each a letter, an inverse flag and
// a colour. It knows nothing about terminals. Every scene has its own, and a
// Display (below) shows whichever one it is given.
struct VtCell {
    char32_t ch = U' ';
    bool inv = false;
    unsigned char r = 0, g = 0, b = 0;
    bool alt = false;                         // an alt colour is set (see PicCell)
    unsigned char ar = 0, ag = 0, ab = 0;     // it, already scaled by its brightness
    unsigned char attrs = 0;                  // CELL_BOLD ... (see PicCell)
    unsigned char pad_[3] = {0, 0, 0};        // always zero: no padding is left to the compiler (checked below)

    // Sixteen bytes and no hidden padding (checked below), so equal cells are equal bytes: one compare for all of it.
    bool operator==(const VtCell& o) const { return std::memcmp(this, &o, sizeof(VtCell)) == 0; }
    bool operator!=(const VtCell& o) const { return !(*this == o); }
    // does the cell draw anything beyond its background (a plain space does not)
    bool visible() const { return ch != U' ' || inv || (attrs & (CELL_UNDERLINE | CELL_STRIKE)); }
};
static_assert(sizeof(VtCell) == sizeof(char32_t) + 3 * 4 * sizeof(unsigned char), "VtCell must have no hidden padding");

inline VtCell make_cell(char32_t ch, bool inv, int r, int g, int b, bool alt = false,
                        int ar = 0, int ag = 0, int ab = 0, unsigned char attrs = 0) {
    // a plain space has no visible letter colour (one that is underlined or struck through has)
    if (ch == U' ' && !inv && !(attrs & (CELL_UNDERLINE | CELL_STRIKE))) r = g = b = 0;
    VtCell k;
    k.ch = ch; k.inv = inv; k.attrs = attrs;
    k.r = (unsigned char)r; k.g = (unsigned char)g; k.b = (unsigned char)b;
    if (alt) {
        k.alt = true;
        k.ar = (unsigned char)ar; k.ag = (unsigned char)ag; k.ab = (unsigned char)ab;
    }
    return k;
}

struct VtScreen {
    int rows = 0, cols = 0;
    std::vector<VtCell> cell;                 // rows * cols, row by row

    void init(int R, int C) { rows = R; cols = C; cell.assign((size_t)R * C, VtCell()); }
    void set(int i, int j, char32_t ch, bool inv, int r, int g, int b, bool alt = false,
             int ar = 0, int ag = 0, int ab = 0, unsigned char attrs = 0) {
        cell[(size_t)i * cols + j] = make_cell(ch, inv, r, g, b, alt, ar, ag, ab, attrs);
    }
    const VtCell& at(int i, int j) const { return cell[(size_t)i * cols + j]; }
};

// ---------------------------------------------------------------- options and terminal basics

struct Options {
    bool loop = false;        // keep going after duration: the clock just runs on
    bool color = true;
    bool still = false;       // print one frame as text and exit
    double still_t = 0;
    int fps = 30;
    int cols = 0, rows = 0;   // 0: use the terminal's size
};

// The size of a character cell in pixels, if the terminal can show Sixel pictures. The program
// that runs a scene finds that out (vtscreen does not ask the terminal) and says so before the
// Display is opened; open() then turns pictures made of pixels on. 0: they are ignored.
inline int g_image_cell_w = 0, g_image_cell_h = 0;
inline void set_image_support(int cell_w, int cell_h) {
    g_image_cell_w = cell_w > 0 ? cell_w : 0;
    g_image_cell_h = cell_h > 0 ? cell_h : 0;
}

inline volatile sig_atomic_t g_stop = 0;
inline void on_sigint(int) { g_stop = 1; }
// The terminal sends SIGWINCH when its window changes size. The handler can only set
// a flag; Display::resized() reads it from the main loop.
inline volatile sig_atomic_t g_resized = 0;
inline void on_sigwinch(int) { g_resized = 1; }

inline void write_all(const std::string& s) {
    const char* p = s.data();
    size_t left = s.size();
#ifdef VTSCREEN_WINDOWS
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    while (left > 0) {
        DWORD want = (DWORD)(left > 65536 ? 65536 : left), n = 0;
        if (!WriteFile(h, p, want, &n, nullptr) || n == 0) break;
        p += n;
        left -= (size_t)n;
    }
#else
    while (left > 0) {
        ssize_t n = ::write(STDOUT_FILENO, p, left);
        if (n <= 0) break;
        p += n;
        left -= (size_t)n;
    }
#endif
}

// The status line text (UTF-8), exactly cols characters wide. actual = frames really drawn per
// second over the last second (< 0: not known yet).
inline std::string status_text(const Info& info, int cols, double t, bool loop,
                               double actual, int fps) {
    char fpstext[32];
    if (actual >= 0) std::snprintf(fpstext, sizeof fpstext, "%.1f/%d fps", actual, fps);
    else             std::snprintf(fpstext, sizeof fpstext, "--/%d fps", fps);
    char right[80];
    if (loop)
        std::snprintf(right, sizeof right, "running  %5.1fs  %s", t, fpstext);
    else
        std::snprintf(right, sizeof right, "running  %4.1fs / %.0fs  %s", t, info.duration, fpstext);
    const std::u32string left = detail::to_u32(info.title);
    const std::u32string r32 = detail::to_u32(right);
    int pad = std::max(1, cols - (int)left.size() - (int)r32.size());
    std::u32string line = left + std::u32string((size_t)pad, U' ') + r32;
    line.resize((size_t)cols, U' ');
    return detail::to_utf8(line);
}

#if 0   // retired: the status line is now drawn by the Display
static std::string status(int cols, double t, bool color, bool loop) {
    char right[64];
    if (loop)
        std::snprintf(right, sizeof right, "running  %5.1fs  %d fps", t, FPS);
    else
        std::snprintf(right, sizeof right, "running  %4.1fs / %.0fs  %d fps", t, DURATION, FPS);
    std::string left = "campfire (C++)";
    int pad = std::max(1, cols - (int)left.size() - (int)std::strlen(right));
    std::string line = (left + std::string(pad, ' ') + right).substr(0, cols);
    if (!color) return line;
    char head[48];
    std::snprintf(head, sizeof head, "\x1b[7;38;2;%d;%d;%dm", ACCENT[0], ACCENT[1], ACCENT[2]);
    return std::string(head) + line + "\x1b[0m";
}
#endif

// ---------------------------------------------------------------- Sixel
//
// Sixel is how a terminal is told to draw pixels: the picture goes out as text, six pixel rows at
// a time, one colour at a time. detail::sixel_encode makes that text from a block of pixels.
namespace detail {

// The Sixel text (ESC P ... ESC \) for the w x h pixels at base, whose rows are stride pixels
// apart. A pixel with a of 0 is left out (the picture is drawn without touching what is there).
// A picture can use 255 colours: with more, the 255 most used are kept, each shown by the average
// of the colours that fell in its bin of 8 levels per channel, and every other pixel takes the
// nearest of those. Long runs of one letter are written as a count.
inline std::string sixel_encode(const Pixel* base, int stride, int w, int h) {
    auto key = [](const Pixel& p) { return ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b; };
    const size_t n = (size_t)w * (size_t)h;
    std::string s = "\x1bP0;1;0q\"1;1;" + std::to_string(w) + ";" + std::to_string(h);
    if (w <= 0 || h <= 0) return s + "\x1b\\";

    // 1. which colours
    std::vector<uint32_t> keys;                  // sorted, at most 255 while it is exact
    bool exact = true;
    uint32_t previous = 0xffffffffu;
    for (int y = 0; y < h && exact; y++) {
        const Pixel* row = base + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            if (row[x].a == 0) continue;
            const uint32_t k = key(row[x]);
            if (k == previous) continue;
            previous = k;
            auto at = std::lower_bound(keys.begin(), keys.end(), k);
            if (at != keys.end() && *at == k) continue;
            if (keys.size() >= 255) { exact = false; break; }
            keys.insert(at, k);
        }
    }

    std::vector<Pixel> palette;
    std::vector<int> index(n, -1);               // -1: not drawn
    if (exact) {
        for (uint32_t k : keys) palette.push_back({(uint8_t)(k >> 16), (uint8_t)(k >> 8), (uint8_t)k, 255});
        previous = 0xffffffffu;
        int last = -1;
        for (int y = 0; y < h; y++) {
            const Pixel* row = base + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                if (row[x].a == 0) continue;
                const uint32_t k = key(row[x]);
                if (k != previous) { previous = k; last = (int)(std::lower_bound(keys.begin(), keys.end(), k) - keys.begin()); }
                index[(size_t)y * w + x] = last;
            }
        }
    } else {
        struct Bin { uint32_t count = 0, r = 0, g = 0, b = 0; };
        std::vector<Bin> bins(32768);
        auto bin_of = [](const Pixel& p) { return ((p.r >> 3) << 10) | ((p.g >> 3) << 5) | (p.b >> 3); };
        for (int y = 0; y < h; y++) {
            const Pixel* row = base + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                if (row[x].a == 0) continue;
                Bin& b = bins[(size_t)bin_of(row[x])];
                b.count++; b.r += row[x].r; b.g += row[x].g; b.b += row[x].b;
            }
        }
        std::vector<int> used;
        for (int k = 0; k < 32768; k++) if (bins[(size_t)k].count) used.push_back(k);
        std::stable_sort(used.begin(), used.end(), [&](int a, int b) { return bins[(size_t)a].count > bins[(size_t)b].count; });
        const size_t keep = std::min<size_t>(255, used.size());
        auto average = [&](int k) {
            const Bin& b = bins[(size_t)k];
            return Pixel{(uint8_t)(b.r / b.count), (uint8_t)(b.g / b.count), (uint8_t)(b.b / b.count), 255};
        };
        std::vector<int> pal_of_bin(32768, -1);
        for (size_t i = 0; i < keep; i++) { palette.push_back(average(used[i])); pal_of_bin[(size_t)used[i]] = (int)i; }
        for (size_t i = keep; i < used.size(); i++) {          // the rest: the nearest kept colour
            const Pixel c = average(used[i]);
            int best = 0, best_d = 1 << 30;
            for (size_t j = 0; j < keep; j++) {
                const int dr = c.r - palette[j].r, dg = c.g - palette[j].g, db = c.b - palette[j].b;
                const int d = dr * dr + dg * dg + db * db;
                if (d < best_d) { best_d = d; best = (int)j; }
            }
            pal_of_bin[(size_t)used[i]] = best;
        }
        for (int y = 0; y < h; y++) {
            const Pixel* row = base + (size_t)y * stride;
            for (int x = 0; x < w; x++)
                if (row[x].a != 0) index[(size_t)y * w + x] = pal_of_bin[(size_t)bin_of(row[x])];
        }
    }

    // 2. the colours (Sixel counts them in percent)
    auto pct = [](int v) { return (v * 100 + 127) / 255; };
    for (size_t i = 0; i < palette.size(); i++)
        s += "#" + std::to_string(i) + ";2;" + std::to_string(pct(palette[i].r)) + ";" +
             std::to_string(pct(palette[i].g)) + ";" + std::to_string(pct(palette[i].b));

    // 3. bands of six pixel rows, one pass per colour used in the band. Each colour's row is kept
    //    between bands (all '?', which is "no pixels") and only the stretch from its first to its
    //    last pixel is visited and cleared.
    std::vector<std::string> rows(palette.size());
    std::vector<char> used(palette.size(), 0);
    std::vector<int> first_x(palette.size(), 0), last_x(palette.size(), 0);
    for (int top = 0; top < h; top += 6) {
        std::vector<size_t> order;
        for (int b = 0; b < 6 && top + b < h; b++) {
            const int* row_index = &index[(size_t)(top + b) * w];
            for (int x = 0; x < w; x++) {
                const int c = row_index[x];
                if (c < 0) continue;
                if (!used[(size_t)c]) {
                    used[(size_t)c] = 1;
                    if (rows[(size_t)c].empty()) rows[(size_t)c].assign((size_t)w, '?');
                    first_x[(size_t)c] = last_x[(size_t)c] = x;
                    order.push_back((size_t)c);
                } else {
                    if (x < first_x[(size_t)c]) first_x[(size_t)c] = x;
                    if (x > last_x[(size_t)c]) last_x[(size_t)c] = x;
                }
                rows[(size_t)c][(size_t)x] = (char)(rows[(size_t)c][(size_t)x] + (1 << b));
            }
        }
        std::sort(order.begin(), order.end());
        bool first = true;
        for (size_t c : order) {
            std::string& row = rows[c];
            const size_t lo = (size_t)first_x[c], hi = (size_t)last_x[c] + 1;
            if (!first) s += "$";                  // back to the left edge of the same band
            first = false;
            s += "#" + std::to_string(c);
            if (lo > 3) s += "!" + std::to_string(lo) + "?";
            else s.append(lo, '?');
            for (size_t i = lo; i < hi;) {
                size_t j = i;
                while (j < hi && row[j] == row[i]) j++;
                if (j - i > 3) s += "!" + std::to_string(j - i) + row[i];
                else s.append(row, i, j - i);
                i = j;
            }
            std::fill(row.begin() + (std::ptrdiff_t)lo, row.begin() + (std::ptrdiff_t)hi, '?');
            used[c] = 0;
        }
        if (top + 6 < h) s += "-";                 // the next band
    }
    return s + "\x1b\\";
}

}  // namespace detail

// ---------------------------------------------------------------- the display: pictures onto the terminal
//
// The Display is the renderer. Given a Picture (brightness and colour for every
// cell) it picks the letters (16 steps, inverse video, shimmer), compares them
// with what the terminal is showing (a VtScreen of its own), sends only the
// cells that differ, and updates its copy. It tracks the terminal's "pen" (the
// last colour and inverse flag sent) and cursor, so codes and cursor moves are
// only sent when they have to be. It does not matter which Picture is shown from
// one frame to the next: the first frame after a switch sends the differences.
//
// It keeps two screens between frames: `target`, the letters the terminal should show, and
// `shown`, the letters it was last sent. Whenever a cell of `target` changes, that cell and
// its row are flagged; the flags are only a hint to where to look, so sending only has to
// visit the flagged cells (and a frame with nothing flagged sends nothing and costs nothing
// to scan). A cell that is not flagged is the same in both screens. With a retained Picture
// (see Picture) the letters of untouched, still cells are not even worked out again.
class Display {
public:
    bool color = true;            // send colour codes
    bool hash_letters = false;    // how the letter inside a group is chosen
    int  fps = 30;                // the shimmer speed follows this
    bool quit_on_q = true;        // q (or Q) quits; a program that lets the person type text turns it off

    // pic_rows is the height of the pictures it will be given (Picture::init's R);
    // the status bar is one more row underneath, as in a Picture.
    void init(int pic_rows, int cols) {
        shown.init(pic_rows + 1, cols);
        target.init(pic_rows + 1, cols);
        changed_.assign((size_t)(pic_rows + 1) * (size_t)cols, 0);
        row_changed_.assign((size_t)(pic_rows + 1), 0);
        any_changed_ = false;
        have_last_ = false;
        image_reset();
    }

    // Say that the terminal can draw Sixel pictures and how big one of its character cells is in
    // pixels (the program has to find that out, for example by asking the terminal). From now on
    // the pictures made of pixels that a Picture carries (Picture::place_image) are drawn over the
    // letters; the letters under them stay, and show through where a picture has nothing. Until
    // this is called, they are ignored. Call it before the first frame, with 0 to turn it off.
    void enable_images(int cell_w, int cell_h) {
        img_cw_ = (cell_w > 0 && cell_h > 0) ? cell_w : 0;
        img_ch_ = img_cw_ > 0 ? cell_h : 0;
        image_reset();
    }
    bool images_enabled() const { return img_cw_ > 0; }

    // The terminal changed size: cols x rows is the whole terminal, as in open().
    // The buffers are made again at the new size, the terminal is cleared and the
    // next frame sends everything. The pictures given from now on must be the new
    // size (Picture::init(rows - 1, cols), which the scenes do from their setup()).
    void resize(int cols, int rows) {
        cols_ = cols; rows_ = rows;
        init(rows - 1, cols);
        write_all(refresh());
    }

    // clear the terminal and forget everything: the next render sends it all
    std::string refresh() {
        for (VtCell& k : shown.cell) k = VtCell();
        // the terminal is blank now: every cell that is not blank has to be sent again
        any_changed_ = false;
        if (changed_.size() == target.cell.size()) {
            for (int i = 0; i < target.rows; i++) {
                row_changed_[(size_t)i] = 0;
                for (int j = 0; j < target.cols; j++) {
                    const size_t idx = (size_t)i * target.cols + j;
                    const bool differs = target.cell[idx] != VtCell();
                    changed_[idx] = differs;
                    if (differs) { row_changed_[(size_t)i] = 1; any_changed_ = true; }
                }
            }
        }
        pen_inv = 0; pr = pg = pb = -1; pen_alt = false; par = pag = pab = -1; pen_attrs = 0;
        cur_r = cur_c = -1;
        image_reset();                            // the clear took the pictures too
        return "\x1b[0m\x1b[H\x1b[2J";
    }

    // pic: a whole screen, as made by Picture::init (the bar row included). t:
    // seconds (drives the shimmer). Returns the bytes to write to the terminal.
    std::string render(const Picture& pic, double t) {
        const bool images = img_cw_ > 0 && color;
        render_t_ = t;
        if (images) prepare_images(pic);            // what the pictures ask for, before the letters (some letters are held)
        to_letters(pic, t);
        std::string out = send();
        if (images) out += send_images();
        return out;
    }

    // Write the default status bar into the bottom row of pic: the scene's title
    // (plus title_suffix, for a program to add its own hint), then the time and the
    // measured / target fps, in the scene's accent colour, in inverse video (plain
    // with --no-color). It is text cells like any other, so two bars cross-fade.
    // A scene that wrote its own text in the bar row keeps it.
    void bar(Picture& pic, const Info& info, double t, bool loop,
             const std::string& title_suffix = "") const {
        const int row = pic.bar_row();
        const Picture& looking = pic;              // a look, not a write
        for (int j = 0; j < pic.cols; j++)
            if (looking.at(row, j).text) return;
        const std::string title = std::string(info.title) + title_suffix;
        Info shown_info = info;
        shown_info.title = title.c_str();
        pic.text(row, 0, status_text(shown_info, pic.cols, t, loop, actual_, fps),
                 info.accent[0], info.accent[1], info.accent[2], color);
    }

    // ---- the terminal session: the part a program's main loop talks to ----
    //
    //   Display d;  d.hash_letters = ...;  d.open(options, cols, rows);
    //   (set up the scenes)  d.start();
    //   while (!d.stopping()) { t = d.time(); ...fill a picture...;
    //                           d.show(picture, t, info, loop);  d.end_frame(); }
    //
    // The terminal is put back when the Display goes out of scope.

    // Switch to the alternate screen, hide the cursor, read keys one at a time,
    // catch Ctrl-C. cols x rows is the whole terminal (see resolve_size()); the
    // picture is one row shorter, the status line takes the last row. Takes
    // color and fps from o.
    void open(const Options& o, int cols, int rows) {
        color = o.color;
        fps = o.fps;
        cols_ = cols; rows_ = rows;
#ifdef VTSCREEN_WINDOWS
        hout_ = GetStdHandle(STD_OUTPUT_HANDLE);
        hin_ = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        out_console_ = GetConsoleMode(hout_, &mode) != 0;
        if (out_console_) {                    // let the console understand escape codes
            old_out_mode_ = mode;
            if (!SetConsoleMode(hout_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                std::fprintf(stderr, "this console does not understand terminal escape codes; "
                                     "use Windows 10 or later (Windows Terminal works best)\n");
                std::exit(1);
            }
        }
        old_cp_ = GetConsoleOutputCP();        // letters beyond ASCII go out as UTF-8
        SetConsoleOutputCP(CP_UTF8);
        tty_in = GetConsoleMode(hin_, &mode) != 0;
        if (tty_in) {                          // have the console report a resized window too
            old_in_mode_ = mode;
            SetConsoleMode(hin_, mode | ENABLE_WINDOW_INPUT);
        }
        term_size(term_cols_, term_rows_);
        std::signal(SIGINT, on_sigint);
        std::signal(SIGTERM, on_sigint);
        // a high-resolution timer for the wait between frames (Windows 10 1803 and
        // later), else an ordinary one, which is coarser (about 15 ms)
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);

        write_all("\x1b[?1049h\x1b[?25l\x1b[2J");
#else
        tty_in = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_tio) == 0;
        struct sigaction sa{};
        sa.sa_handler = on_sigint;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sa.sa_handler = on_sigwinch;                // window resized (see resized())
        sigaction(SIGWINCH, &sa, nullptr);
        term_size(term_cols_, term_rows_);

        write_all("\x1b[?1049h\x1b[?25l\x1b[2J");
        if (tty_in) {
            termios raw = old_tio;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
        }
#endif
        init(rows - 1, cols);                  // the picture; the status line goes underneath
        write_all(refresh());
        // The program said (set_image_support) that the terminal can draw Sixel pictures: use them (not without colour).
        if (g_image_cell_w > 0 && g_image_cell_h > 0 && o.color) enable_images(g_image_cell_w, g_image_cell_h);
        is_open = true;
    }

    void close() {
        if (!is_open) return;
        write_all("\x1b[0m\x1b[?25h\x1b[?1049l");
#ifdef VTSCREEN_WINDOWS
        if (out_console_) SetConsoleMode(hout_, old_out_mode_);
        if (old_cp_) SetConsoleOutputCP(old_cp_);
        if (tty_in) {
            SetConsoleMode(hin_, old_in_mode_);
            FlushConsoleInputBuffer(hin_);              // keys typed meanwhile are not for the shell
        }
        if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
#else
        if (tty_in) tcsetattr(STDIN_FILENO, TCSADRAIN, &old_tio);
#endif
        is_open = false;
    }
    ~Display() { close(); }

    int cols() const { return cols_; }
    int pic_rows() const { return rows_ - 1; }

    // True once if the terminal's window has changed size since open() or the last
    // time this said so, and then cols, rows are its new size (the whole terminal,
    // as resolve_size() gives it). It only reports: nothing is redrawn or set up
    // again here. The flag behind it is set by SIGWINCH on Linux and by the
    // console's window-size event on Windows (read with the keys).
    bool resized(int& cols, int& rows) {
        if (!g_resized) return false;
        g_resized = 0;                          // before asking, so a later change is not lost
        int c, r;
        if (!term_size(c, r) || (c == term_cols_ && r == term_rows_)) return false;
        term_cols_ = cols = c;
        term_rows_ = rows = r;
        return true;
    }

    // Start the clock (call it once the scenes are set up, so t starts at 0).
    void start() {
        start_ = clk::now();
        n_ = 0; fps_frames_ = 0; fps_start_ = 0; actual_ = -1; last_refresh_ = 0;
    }
    double time() const { return std::chrono::duration<double>(clk::now() - start_).count(); }
    bool stopping() const { return g_stop != 0; }
    void stop() { g_stop = 1; }

    // Draw a picture on the terminal now: refresh everything if it is due, then
    // render() and write. Call bar() on the picture first to give it a status bar.
    void show(const Picture& pic, double t) {
        std::string pre;
        if (t - last_refresh_ >= REFRESH_SECONDS) { pre = refresh(); last_refresh_ = t; }
        write_all(pre + render(pic, t));
        fps_frames_++;
        if (t - fps_start_ >= 1.0) { actual_ = fps_frames_ / (t - fps_start_); fps_frames_ = 0; fps_start_ = t; }
    }

    // The next key pressed since the last call (other than q, which quits unless quit_on_q is
    // false), or 0 if there is none. Keys are collected by end_frame().
    char next_key() {
        if (keys_.empty()) return 0;
        char c = keys_.front();
        keys_.erase(0, 1);
        return c;
    }

    // Wait until the next frame is due, reading the keyboard meanwhile (q quits).
    void end_frame() {
        const double dt = 1.0 / fps;
        n_++;
        double wait = (n_ * dt) - time();
        if (wait < -1.0)                       // fell far behind: resync
            n_ = (long)(time() / dt);
#ifdef VTSCREEN_WINDOWS
        const double due = n_ * dt;
        for (;;) {
            double remaining = due - time();
            if (remaining <= 0 || g_stop) break;
            HANDLE objs[2];
            DWORD count = 0;
            if (timer_) {
                LARGE_INTEGER rel;
                rel.QuadPart = -(LONGLONG)(remaining * 1e7);    // negative: relative, in 100 ns units
                SetWaitableTimer(timer_, &rel, 0, nullptr, nullptr, FALSE);
                objs[count++] = timer_;
            }
            if (tty_in) objs[count++] = hin_;                    // wake early when a key arrives
            DWORD ms = timer_ ? INFINITE : (DWORD)(remaining * 1000 + 0.5);
            if (count > 0) WaitForMultipleObjects(count, objs, FALSE, ms);
            else Sleep(ms);
            read_keys();
        }
        read_keys();
#else
        // A signal (the window being resized, say) cuts a wait short; wait out the rest
        // so the frames keep their pace.
        if (tty_in) {
            for (;;) {
                pollfd pfd{STDIN_FILENO, POLLIN, 0};
                double remaining = n_ * dt - time();
                int ms = remaining > 0 ? (int)(remaining * 1000) : 0;
                int rc = poll(&pfd, 1, ms);
                if (rc > 0) {
                    char c;
                    while (read(STDIN_FILENO, &c, 1) == 1) {
                        if (quit_on_q && (c == 'q' || c == 'Q')) g_stop = 1;
                        else keys_.push_back(c);
                    }
                } else if (rc < 0 && errno == EINTR && !g_stop && n_ * dt - time() > 0.001) {
                    continue;
                }
                break;
            }
        } else {
            for (double rem = n_ * dt - time(); rem > 0 && !g_stop; rem = n_ * dt - time())
                usleep((useconds_t)(rem * 1e6));
        }
#endif
    }

private:
    using clk = std::chrono::steady_clock;
    bool is_open = false, tty_in = false;
#ifdef VTSCREEN_WINDOWS
    HANDLE hout_ = nullptr, hin_ = nullptr, timer_ = nullptr;
    DWORD old_out_mode_ = 0, old_in_mode_ = 0;
    UINT old_cp_ = 0;
    bool out_console_ = false;

    // the console window's size now; false if it cannot be asked
    bool term_size(int& c, int& r) const {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(hout_, &info)) return false;
        c = info.srWindow.Right - info.srWindow.Left + 1;
        r = info.srWindow.Bottom - info.srWindow.Top + 1;
        return c > 0 && r > 0;
    }

    // take every key press waiting in the console: q quits, the rest are kept. A
    // window-size record only raises the flag that resized() looks at.
    void read_keys() {
        if (!tty_in) return;
        INPUT_RECORD rec[64];
        DWORD avail = 0, got = 0;
        while (GetNumberOfConsoleInputEvents(hin_, &avail) && avail > 0) {
            if (!ReadConsoleInputW(hin_, rec, 64, &got)) break;
            for (DWORD i = 0; i < got; i++) {
                if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) { g_resized = 1; continue; }
                if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown) continue;
                wchar_t w = rec[i].Event.KeyEvent.uChar.UnicodeChar;
                if (w <= 0 || w >= 128) continue;
                char c = (char)w;
                if (quit_on_q && (c == 'q' || c == 'Q')) g_stop = 1;
                else keys_.push_back(c);
            }
        }
    }
#else
    termios old_tio{};

    // the terminal window's size now; false if it cannot be asked
    static bool term_size(int& c, int& r) {
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return false;
        c = ws.ws_col; r = ws.ws_row;
        return true;
    }
#endif
    int term_cols_ = 0, term_rows_ = 0;           // the size resized() last saw
    int cols_ = 0, rows_ = 0;
    std::string keys_;                            // keys pressed, not yet read
    clk::time_point start_ = clk::now();
    long n_ = 0;                                  // frames due so far
    long fps_frames_ = 0;                         // frames drawn in the current second
    double fps_start_ = 0, actual_ = -1, last_refresh_ = 0;

    VtScreen shown;                               // what the terminal is showing
    VtScreen target;                              // the picture as letters, this frame
    // Pictures made of pixels (see enable_images). The terminal wipes a cell's pixels when a letter is
    // written into it, so what is kept here is, per cell, whether there are pixels in it now.
    int img_cw_ = 0, img_ch_ = 0;                 // pixels per cell; 0: off
    std::vector<Pixel> img_on_;                   // the whole screen in pixels: what the terminal shows of the pictures
    std::vector<Pixel> img_want_;                 // the same size: what the pictures ask for, put together
    std::vector<unsigned char> img_ink_;          // per cell: there are picture pixels in it on the terminal
    std::vector<unsigned char> written_;          // per cell: a letter was written into it since the pictures were last looked at
    size_t ink_count_ = 0;                        // how many cells have ink
    std::vector<unsigned char> img_hold_;         // per cell: its letter is held back this frame (see ImagePlacement::letter_rate)
    std::vector<float> hold_rate_;                // per cell: how often a held cell's letter may change (see ImagePlacement::letter_rate)
    std::vector<long> hold_slot_;                 // per cell: the time slot of its last allowed change
    double render_t_ = 0;                         // the time of the frame being rendered
    bool hold_any_ = false;                       // img_hold_ has a cell set
    bool img_active_ = false;                     // prepare_images found pictures to do: send_images has work
    int img_i0_ = 0, img_i1_ = -1, img_j0_ = 0, img_j1_ = -1;   // the cells they cover, and the ones with pixels now
    std::vector<unsigned char> changed_;          // per cell: target may differ from shown (a hint)
    std::vector<unsigned char> row_changed_;      // per row: a cell in it is flagged
    bool any_changed_ = false;                    // anything is flagged
    // What the last render was done with: when any of it changes, every letter is worked out again.
    bool have_last_ = false, last_color_ = true, last_hash_ = false;
    int last_fps_ = 30;
    uint64_t last_serial_ = 0;                    // the Picture it was done for
    int pen_inv = 0, pr = -1, pg = -1, pb = -1;   // what the terminal will use next
    unsigned char pen_attrs = 0;                  // ... its text attributes (CELL_BOLD ...)
    bool pen_alt = false;                         // ... and its background colour (none, or
    int par = -1, pag = -1, pab = -1;             //     this one)
    int cur_r = -1, cur_c = -1;                   // where its cursor is (-1: unknown)

    // brightness and colour -> letter, inverse flag and rounded colour, for one cell
    VTSCREEN_FORCE_INLINE void letter(const PicCell& pc, int i, int j, double t, VtCell& out) const {
        const bool alt = color && pc.alt_lum > 0;
        const double al = detail::clampd(pc.alt_lum, 0, 1);
        if (pc.text) {                  // a letter the scene chose: as it is
            out = make_cell(pc.ch, pc.inv, detail::byte(pc.r), detail::byte(pc.g),
                            detail::byte(pc.b), alt, detail::byte(pc.alt_r * al),
                            detail::byte(pc.alt_g * al), detail::byte(pc.alt_b * al), pc.attrs);
            return;
        }
        bool inv;
        char ch = pick_char(pc.lum, i, j, t, fps, inv, hash_letters, pc.shimmer);
        int px[3] = {0, 0, 0};
        if (color) {
            px[0] = quant(pc.r); px[1] = quant(pc.g); px[2] = quant(pc.b);
        }
        out = make_cell(ch, inv, px[0], px[1], px[2], alt, quant(pc.alt_r * al),
                        quant(pc.alt_g * al), quant(pc.alt_b * al));
    }

    // The picture as letters in target, flagging what is different. A retained Picture that is the same picture,
    // the same size and shown with the same settings as last time only has its touched cells and its shimmering
    // cells looked at; anything else, the whole picture.
    void to_letters(const Picture& F, double t) {
        const int R = target.rows, C = target.cols;
        const bool same_settings = have_last_ && last_color_ == color && last_fps_ == fps && last_hash_ == hash_letters;
        if (F.retained) F.ensure_flags();
        const bool whole = !F.retained || F.all_touched || !same_settings || F.serial != last_serial_ ||
                           F.rows != R || F.cols != C;
        VtCell* const tgt = target.cell.data();
        unsigned char* const chg = changed_.data();
        unsigned char* const row_chg = row_changed_.data();
        const PicCell* const src = F.cell.data();
        const bool retained = F.retained, partial = !whole;
        bool any = any_changed_;
        if (retained && whole) {
            std::fill(F.animated.begin(), F.animated.end(), (unsigned char)0);
            std::fill(F.animated_in_row.begin(), F.animated_in_row.end(), 0);
        }
        // One loop for every case (the whole picture, or only what was touched or shimmers), so that the one place a
        // letter is worked out is easy for the compiler to inline.
        for (int i = 0; i < R; i++) {
            if (partial && !F.row_touched[(size_t)i] && F.animated_in_row[(size_t)i] == 0) continue;
            size_t idx = (size_t)i * C;
            for (int j = 0; j < C; j++, idx++) {
                if (partial && !F.touched[idx] && !F.animated[idx]) continue;
                VtCell k;
                letter(src[idx], i, j, t, k);
                if (tgt[idx] != k) {
                    // a half of a wide letter: what the other half draws depends on this one, so look at it again when sending
                    if (((tgt[idx].attrs | k.attrs) & (CELL_WIDE | CELL_TAIL)) != 0) {
                        if (j > 0) chg[idx - 1] = 1;
                        if (j + 1 < C) chg[idx + 1] = 1;
                    }
                    tgt[idx] = k; chg[idx] = 1; row_chg[i] = 1; any = true;
                }
                if (retained) {
                    const bool moves = !src[idx].text && src[idx].shimmer > 0;     // does its letter change with time
                    if (moves != (F.animated[idx] != 0)) {
                        F.animated[idx] = moves;
                        F.animated_in_row[(size_t)i] += moves ? 1 : -1;
                    }
                }
            }
        }
        any_changed_ = any;
        if (F.retained) F.consume();
        have_last_ = true; last_color_ = color; last_fps_ = fps; last_hash_ = hash_letters;
        last_serial_ = F.serial;
    }

    void image_reset() {
        img_on_.clear(); img_want_.clear(); img_ink_.clear(); written_.clear(); img_hold_.clear(); hold_rate_.clear(); hold_slot_.clear();
        ink_count_ = 0;
        hold_any_ = false; img_active_ = false;
    }

    // May the held letter of cell idx change now? Each cell has a turn letter_rate times a second, at a moment of its own.
    bool letter_due(size_t idx) {
        const double rate = hold_rate_[idx];
        if (rate <= 0.0) return false;
        const double phase = (double)((idx * 2654435761u) >> 8 & 1023u) / 1024.0;
        const long slot = (long)std::floor(render_t_ * rate + phase);
        if (slot == hold_slot_[idx]) return false;
        hold_slot_[idx] = slot;
        return true;
    }

    // A letter was written into cell (i, j): any picture pixels it had are gone.
    void note_letter(int i, int j) {
        if (ink_count_ == 0) return;
        const size_t idx = (size_t)i * shown.cols + j;
        if (idx < written_.size()) written_[idx] = 1;
    }

    // Write the letter of cell (i, j) again, which takes the picture pixels out of it. A half of a
    // wide letter is written through its first half, which clears both. Returns the first cell written.
    int wipe_cell(std::string& out, int i, int j) {
        VtCell k = drawn(i, j);
        int head = j;
        if ((k.attrs & CELL_TAIL) && j > 0) head = j - 1;
        // A held letter is written again as it is on the terminal now, not as the picture of this frame has it.
        const size_t hidx = (size_t)i * shown.cols + head;
        const bool held = hidx < img_hold_.size() && img_hold_[hidx];
        put(out, i, head, held ? shown.cell[hidx] : drawn(i, head));
        const int cols = shown.cols;
        for (int x = head; x <= head + 1 && x < cols; x++) {
            if (x != head && !(drawn(i, head).attrs & CELL_WIDE)) break;
            const size_t idx = (size_t)i * cols + x;
            if (img_ink_[idx]) { img_ink_[idx] = 0; ink_count_--; }
        }
        return head;
    }

    // Put the pictures together for this frame (what the terminal should show of them), before the letters are chosen:
    // some letters may be held back under them (see ImagePlacement::letter_rate).
    void prepare_images(const Picture& pic) {
        img_active_ = false;
        hold_any_ = false;
        if (pic.images.empty() && ink_count_ == 0) return;
        const int rows = shown.rows, cols = shown.cols, cw = img_cw_, ch = img_ch_;
        const int W = cols * cw, H = rows * ch;
        const int pic_rows = rows - 1;
        if (img_ink_.size() != (size_t)rows * cols) {
            img_on_.assign((size_t)W * H, Pixel());
            img_want_.assign((size_t)W * H, Pixel());
            img_ink_.assign((size_t)rows * cols, 0);
            written_.assign((size_t)rows * cols, 0);
            img_hold_.assign((size_t)rows * cols, 0);
            hold_rate_.assign((size_t)rows * cols, -1.0f);
            hold_slot_.assign((size_t)rows * cols, -1000000000L);
            ink_count_ = 0;
        }

        // 1. the cells the pictures cover, and the ones that have pixels now
        int i0 = rows, i1 = -1, j0 = cols, j1 = -1;
        auto cover = [&](int i, int j) { i0 = std::min(i0, i); i1 = std::max(i1, i); j0 = std::min(j0, j); j1 = std::max(j1, j); };
        for (const ImagePlacement& p : pic.images) {
            if (!p.raster || p.raster->w <= 0 || p.raster->h <= 0) continue;
            const int a = std::max(0, p.row), b = std::min(pic_rows - 1, p.row + (p.raster->h + ch - 1) / ch - 1);
            const int c = std::max(0, p.col), d = std::min(cols - 1, p.col + (p.raster->w + cw - 1) / cw - 1);
            if (a <= b && c <= d) { cover(a, c); cover(b, d); }
        }
        if (ink_count_ > 0)
            for (int i = 0; i < rows; i++)
                for (int j = 0; j < cols; j++)
                    if (img_ink_[(size_t)i * cols + j]) cover(i, j);
        if (i1 < 0) return;                                       // nothing to draw and nothing to take off
        img_i0_ = i0; img_i1_ = i1; img_j0_ = j0; img_j1_ = j1;
        img_active_ = true;

        // 2. put the pictures together, inside that block of cells, and note the cells a holding picture has pixels in
        for (int y = i0 * ch; y < (i1 + 1) * ch; y++)
            std::fill(img_want_.begin() + (std::ptrdiff_t)y * W + j0 * cw, img_want_.begin() + (std::ptrdiff_t)y * W + (j1 + 1) * cw, Pixel());
        std::fill(hold_rate_.begin(), hold_rate_.end(), -1.0f);       // -1: no picture that holds has pixels here
        std::fill(img_hold_.begin(), img_hold_.end(), (unsigned char)0);
        for (const ImagePlacement& p : pic.images) {
            if (!p.raster) continue;
            const Raster& r = *p.raster;
            for (int y = 0; y < r.h; y++) {
                const int py = p.row * ch + y;
                if (py < 0 || py >= pic_rows * ch) continue;
                for (int x = 0; x < r.w; x++) {
                    const int px = p.col * cw + x;
                    if (px < 0 || px >= W) continue;
                    const Pixel& q = r.at(x, y);
                    if (q.a == 0) continue;
                    img_want_[(size_t)py * W + px] = Pixel{q.r, q.g, q.b, 255};
                    if (p.letter_rate >= 0) {
                        float& r = hold_rate_[(size_t)(py / ch) * cols + (size_t)(px / cw)];
                        r = std::max(r, (float)p.letter_rate);                                  // the most generous of the pictures here
                    }
                }
            }
        }

        // 3. which letters are held: under a holding picture's pixels, where the picture is already drawn, except text
        for (int i = i0; i <= i1; i++)
            for (int j = j0; j <= j1; j++) {
                const size_t idx = (size_t)i * cols + j;
                if (hold_rate_[idx] >= 0.0f && img_ink_[idx] && i < pic.rows && j < pic.cols && !pic.at(i, j).text) { img_hold_[idx] = 1; hold_any_ = true; }
            }
    }

    // Draw the pictures made of pixels that prepare_images put together, after the letters. Only the cells whose
    // pixels differ from what the terminal shows are sent, and cells where a letter was just
    // written (which wiped them) are sent again. Neighbouring cells go together as one picture.
    // The last row is the status bar: no pictures go there.
    std::string send_images() {
        std::string out;
        if (!img_active_) return out;
        const int cols = shown.cols, cw = img_cw_, ch = img_ch_;
        const int W = cols * cw;
        const int i0 = img_i0_, i1 = img_i1_, j0 = img_j0_, j1 = img_j1_;

        auto tile = [&](const std::vector<Pixel>& v, int i, int j) { return &v[(size_t)(i * ch) * W + (size_t)j * cw]; };
        auto tile_empty = [&](int i, int j) {
            for (int y = 0; y < ch; y++) {
                const Pixel* p = tile(img_want_, i, j) + (size_t)y * W;
                for (int x = 0; x < cw; x++) if (p[x].a != 0) return false;
            }
            return true;
        };
        auto tile_same = [&](int i, int j) {
            for (int y = 0; y < ch; y++)
                if (std::memcmp(tile(img_on_, i, j) + (size_t)y * W, tile(img_want_, i, j) + (size_t)y * W, (size_t)cw * sizeof(Pixel)) != 0) return false;
            return true;
        };

        // 3. row by row: first take pixels out of cells that must lose or change theirs, then send
        for (int i = i0; i <= i1; i++) {
            for (int j = j0; j <= j1; j++) {                      // a letter written here took its pixels
                const size_t idx = (size_t)i * cols + j;
                if (written_[idx] && img_ink_[idx]) { img_ink_[idx] = 0; ink_count_--; }
            }
            for (int j = j0; j <= j1; j++) {
                const size_t idx = (size_t)i * cols + j;
                if (img_ink_[idx] && (tile_empty(i, j) || !tile_same(i, j))) wipe_cell(out, i, j);
            }
            // Writing a letter can write the short gap before it again too (see put()), which takes the pixels of those cells.
            for (int j = j0; j <= j1; j++) {
                const size_t idx = (size_t)i * cols + j;
                if (written_[idx] && img_ink_[idx]) { img_ink_[idx] = 0; ink_count_--; }
            }
            for (int j = j0; j <= j1;) {
                const size_t idx = (size_t)i * cols + j;
                const bool need = !tile_empty(i, j) && (!img_ink_[idx] || !tile_same(i, j));
                if (!need) { j++; continue; }
                int end = j;
                while (end <= j1 && !tile_empty(i, end) && (!img_ink_[(size_t)i * cols + end] || !tile_same(i, end))) end++;
                char esc[40];
                std::snprintf(esc, sizeof esc, "\x1b[%d;%dH", i + 1, j + 1);
                out += esc;
                out += detail::sixel_encode(tile(img_want_, i, j), W, (end - j) * cw, ch);
                for (int x = j; x < end; x++) {
                    for (int y = 0; y < ch; y++) {
                        const Pixel* from = tile(img_want_, i, x) + (size_t)y * W;
                        std::copy(from, from + cw, img_on_.begin() + (std::ptrdiff_t)(i * ch + y) * W + (std::ptrdiff_t)x * cw);
                    }
                    if (!img_ink_[(size_t)i * cols + x]) { img_ink_[(size_t)i * cols + x] = 1; ink_count_++; }
                }
                cur_r = cur_c = -1;                               // where a picture leaves the cursor is not trusted
                j = end;
            }
        }
        std::fill(written_.begin(), written_.end(), (unsigned char)0);
        return out;
    }

    // The cell as it is drawn: the target's, except that half of a wide letter without its other half is a blank (keeping its background).
    VtCell drawn(int i, int j) const {
        VtCell k = target.at(i, j);
        bool ok = true;
        if (k.attrs & CELL_WIDE) ok = j + 1 < target.cols && (target.at(i, j + 1).attrs & CELL_TAIL) != 0;
        else if (k.attrs & CELL_TAIL) ok = j > 0 && (target.at(i, j - 1).attrs & CELL_WIDE) != 0 && !(target.at(i, j - 1).attrs & CELL_TAIL);
        if (!ok) k = make_cell(U' ', false, 0, 0, 0, k.alt, k.ar, k.ag, k.ab);
        return k;
    }

    // write cell k at row i, column j (the terminal's cursor and pens are moved as needed), and note it as shown
    void put(std::string& out, int i, int j, const VtCell& k) {
        char esc[40];
        const size_t idx = (size_t)i * shown.cols + j;
        const int cols = shown.cols;
        // A short gap of unchanged cells is cheaper to write again than to
        // skip with a cursor move, when no colour or inverse code is needed.
        if (cur_r == i && cur_c >= 0 && j >= cur_c && j - cur_c <= 3) {
            bool free = true;
            for (int x = cur_c; x < j && free; x++) {
                const VtCell& u = shown.at(i, x);
                free = !(u.attrs & (CELL_WIDE | CELL_TAIL)) &&
                       (int)u.inv == pen_inv &&
                       (u.attrs & CELL_LOOKS) == pen_attrs &&
                       (!u.visible() || !color || (pr == u.r && pg == u.g && pb == u.b)) &&
                       (!color || (u.alt ? pen_alt && par == u.ar && pag == u.ag && pab == u.ab
                                         : !pen_alt));
            }
            if (free) {
                for (int x = cur_c; x < j; x++) { detail::append_utf8(out, shown.at(i, x).ch); note_letter(i, x); }
                cur_c = j;
            }
        }
        if (cur_r == i && cur_c == j) {}
        else if (cur_r == i && cur_c >= 0 && cur_c < j) {
            int g = j - cur_c;
            if (g == 1) out += "\x1b[C";
            else { std::snprintf(esc, sizeof esc, "\x1b[%dC", g); out += esc; }
        } else {
            std::snprintf(esc, sizeof esc, "\x1b[%d;%dH", i + 1, j + 1);
            out += esc;
        }
        if (pen_inv != (int)k.inv) { out += k.inv ? "\x1b[7m" : "\x1b[27m"; pen_inv = k.inv; }
        if ((k.attrs & CELL_LOOKS) != pen_attrs) detail::sgr_attrs(out, pen_attrs, k.attrs);
        if (color) {
            if (k.alt) {
                if (!pen_alt || par != k.ar || pag != k.ag || pab != k.ab) {
                    std::snprintf(esc, sizeof esc, "\x1b[48;2;%d;%d;%dm", k.ar, k.ag, k.ab);
                    out += esc;
                    pen_alt = true; par = k.ar; pag = k.ag; pab = k.ab;
                }
            } else if (pen_alt) {
                out += "\x1b[49m";
                pen_alt = false;
            }
        }
        if (color && k.visible() && (pr != k.r || pg != k.g || pb != k.b)) {
            std::snprintf(esc, sizeof esc, "\x1b[38;2;%d;%d;%dm", k.r, k.g, k.b);
            out += esc;
            pr = k.r; pg = k.g; pb = k.b;
        }
        detail::append_utf8(out, (k.attrs & CELL_TAIL) ? U' ' : k.ch);
        shown.cell[idx] = k;
        note_letter(i, j);
        if (k.attrs & CELL_WIDE) {
            note_letter(i, j + 1);
            // it takes two columns: its second half is shown with it. Where the terminal's cursor is now is not trusted (terminals differ on
            // how wide some letters are), so the next write moves it for certain.
            shown.cell[idx + 1] = drawn(i, j + 1);
            changed_[idx + 1] = 0;
            cur_r = cur_c = -1;
        } else {
            cur_r = i; cur_c = j + 1;
            if (cur_c >= cols) cur_r = cur_c = -1;   // pending wrap: position unknown
        }
    }

    // compare the flagged cells of target with shown and write the differences
    std::string send() {
        std::string out;
        if (!any_changed_) return out;                  // nothing is flagged: nothing to look at, nothing to send
        const int rows = shown.rows, cols = shown.cols;
        bool kept = false;                              // a held letter is still flagged
        for (int i = 0; i < rows; i++) {
            if (!row_changed_[(size_t)i]) continue;
            row_changed_[(size_t)i] = 0;
            for (int j = 0; j < cols; j++) {
                const size_t idx = (size_t)i * cols + j;
                if (!changed_[idx]) continue;
                changed_[idx] = 0;
                const VtCell k = drawn(i, j);
                if (k == shown.cell[idx]) continue;
                if (hold_any_ && img_hold_[idx] && !((k.attrs | shown.cell[idx].attrs) & (CELL_WIDE | CELL_TAIL))) {
                    if (!letter_due(idx)) {
                        changed_[idx] = 1; row_changed_[(size_t)i] = 1; kept = true;     // not now: the picture over it keeps it as it is
                        continue;
                    }
                    img_hold_[idx] = 0;                                                  // its turn: written like any other
                }
                if ((k.attrs & CELL_TAIL) && j > 0) {
                    // the second half of a wide letter changed and its first half did not: the first half draws both, so draw it again
                    put(out, i, j - 1, drawn(i, j - 1));
                    continue;
                }
                put(out, i, j, k);
            }
        }
        any_changed_ = kept;
        return out;
    }
};

// ---------------------------------------------------------------- the player

// Has the terminal changed size, and should the program follow? If so, true, and
// cols, rows are the new size to use: the terminal's, except that --cols or --rows
// keep their dimension fixed. A size under the minimum is not followed (cols, rows
// stay as they were), and neither is one that is no change. Call it once a frame.
inline bool follow_resize(Display& display, const Options& o, int& cols, int& rows) {
    int c, r;
    if (!display.resized(c, r)) return false;
    if (o.cols) c = o.cols;
    if (o.rows) r = o.rows;
    if (c < 40 || r < 20 || (c == cols && r == rows)) return false;
    cols = c; rows = r;
    return true;
}

inline void play(Scene& scene, const Options& o, int cols, int rows) {
    const Info info = scene.info();
    Display display;
    display.hash_letters = info.hash_letters;
    display.open(o, cols, rows);
    scene.setup(cols, rows - 1);
    display.start();
    while (!display.stopping()) {
        double t = display.time();
        if (follow_resize(display, o, cols, rows)) {
            display.resize(cols, rows);
            scene.setup(cols, rows - 1);
        }
        // With --loop the clock just keeps running: the scene carries on
        // from where it was instead of restarting at t = 0.
        if (t >= info.duration && !o.loop) break;
        scene.cycle(t);
        display.bar(scene.screen, info, t, o.loop);
        display.show(scene.screen, t);
        display.end_frame();
    }
}

inline void usage(const char* me) {
    std::fprintf(stderr,
        "usage: %s [--loop] [--no-color] [--still T] [--fps N] [--cols N] [--rows N]\n", me);
}

// Read the command line into o. Returns -1 to carry on, otherwise the exit code
// the program should end with (0 for --help, 2 for a bad option).
inline int parse_options(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--loop") o.loop = true;
        else if (a == "--no-color") o.color = false;
        else if (a == "--still" && i + 1 < argc) { o.still = true; o.still_t = std::atof(argv[++i]); }
        else if (a == "--fps" && i + 1 < argc) o.fps = std::min(std::max(std::atoi(argv[++i]), 1), 240);
        else if (a == "--cols" && i + 1 < argc) o.cols = std::atoi(argv[++i]);
        else if (a == "--rows" && i + 1 < argc) o.rows = std::atoi(argv[++i]);
        else { usage(argv[0]); return a == "--help" || a == "-h" ? 0 : 2; }
    }
    return -1;
}

// The terminal's size, or the options' --cols / --rows. Prints a message and
// returns false if it is smaller than 40 x 20.
inline bool resolve_size(const Options& o, int& cols, int& rows) {
    int tcols = 100, trows = 40;               // used when the output is not a terminal
#ifdef VTSCREEN_WINDOWS
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        tcols = info.srWindow.Right - info.srWindow.Left + 1;
        trows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col != 0) { tcols = ws.ws_col; trows = ws.ws_row; }
#endif
    cols = o.cols ? o.cols : tcols;
    rows = o.rows ? o.rows : trows;
    if (cols < 40 || rows < 20) {
        std::fprintf(stderr, "terminal too small (%dx%d); need at least 40x20\n", cols, rows);
        return false;
    }
    return true;
}

// Everything a one-scene program's main() needs: read the options, find the
// terminal size, then play the scene (or print one still frame).
inline int run(Scene& scene, int argc, char** argv) {
    Options o;
    int rc = parse_options(argc, argv, o);
    if (rc >= 0) return rc;
    int cols, rows;
    if (!resolve_size(o, cols, rows)) return 1;

    if (o.still) {
        const Info info = scene.info();
        scene.setup(cols, rows - 1);
        Picture F;
        scene.frame(o.still_t, F);
        std::string s = render_plain(F, rows - 1, cols, o.still_t, o.fps, false, false,
                                     detail::stdout_is_tty(), info.hash_letters);
        std::fwrite(s.data(), 1, s.size(), stdout);
        return 0;
    }
    play(scene, o, cols, rows);
    return 0;
}

}  // namespace vtscreen
