// Tests for retained mode. The rule they check: a retained Picture and an ordinary one with the same contents
// make a Display send exactly the same bytes, frame after frame, whatever is done to them. The ordinary Display
// path is the old one (checked byte for byte against the library as it was before retained mode, on every scene).
//
//   tests/run_tests.sh        builds this with the strict flags and the sanitizers and runs it
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../vtscreen.h"
#include "pinned_sequence.h"

using namespace vtscreen;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

namespace {

// A terminal, as far as the Display's output goes: it applies the bytes (cursor moves, inverse, colours, clear, text) to a
// grid. Comparing that grid with what a fresh Display sends for the same picture tests the whole chain against what a
// person would see, without relying on two paths agreeing with each other.
struct Model {
    int rows = 0, cols = 0;
    std::vector<VtCell> grid;
    int cr = 0, cc = 0;
    bool inv = false, has_bg = false;
    int fr = 0, fg = 0, fb = 0, br = 0, bg = 0, bb = 0;
    bool broken = false;                       // something the Display should never send

    void init(int r, int c) { rows = r; cols = c; grid.assign((size_t)r * c, VtCell()); cr = cc = 0; inv = has_bg = false; fr = fg = fb = br = bg = bb = 0; broken = false; }

    void sgr(const std::string& params) {
        std::vector<int> v;
        size_t i = 0;
        while (i <= params.size()) {
            size_t e = params.find(';', i);
            if (e == std::string::npos) e = params.size();
            v.push_back(params.substr(i, e - i).empty() ? 0 : std::atoi(params.substr(i, e - i).c_str()));
            i = e + 1;
        }
        for (size_t k = 0; k < v.size(); k++) {
            if (v[k] == 0) { inv = false; has_bg = false; fr = fg = fb = 0; }
            else if (v[k] == 7) inv = true;
            else if (v[k] == 27) inv = false;
            else if (v[k] == 49) has_bg = false;
            else if (v[k] == 38 && k + 4 < v.size() + 0 && v[k + 1] == 2) { fr = v[k + 2]; fg = v[k + 3]; fb = v[k + 4]; k += 4; }
            else if (v[k] == 48 && k + 4 < v.size() + 0 && v[k + 1] == 2) { has_bg = true; br = v[k + 2]; bg = v[k + 3]; bb = v[k + 4]; k += 4; }
            else broken = true;
        }
    }

    void feed(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = (unsigned char)s[i];
            if (c == '\n') { cr++; cc = 0; i++; continue; }
            if (c == 0x1b) {
                if (i + 1 >= s.size() || s[i + 1] != '[') { broken = true; return; }
                size_t j = i + 2;
                while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
                if (j >= s.size()) { broken = true; return; }
                const std::string params = s.substr(i + 2, j - (i + 2));
                const char f = s[j];
                if (f == 'H') {
                    if (params.empty()) { cr = 0; cc = 0; }
                    else { const size_t semi = params.find(';'); cr = std::atoi(params.substr(0, semi).c_str()) - 1; cc = std::atoi(params.substr(semi + 1).c_str()) - 1; }
                } else if (f == 'J') { if (params == "2") std::fill(grid.begin(), grid.end(), VtCell()); else broken = true; }
                else if (f == 'C') cc += params.empty() ? 1 : std::atoi(params.c_str());
                else if (f == 'K') { for (int x = cc; x < cols; x++) if (cr >= 0 && cr < rows) grid[(size_t)cr * cols + x] = VtCell(); }
                else if (f == 'm') sgr(params);
                else broken = true;
                i = j + 1;
                continue;
            }
            char32_t cp; size_t len;
            if (c < 0x80) { cp = c; len = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
            else { cp = c & 0x07; len = 4; }
            for (size_t k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
            i += len;
            if (cr < 0 || cr >= rows || cc < 0 || cc >= cols) { broken = true; continue; }
            VtCell k;
            k.ch = cp; k.inv = inv;
            if (!(cp == U' ' && !inv)) { k.r = (unsigned char)fr; k.g = (unsigned char)fg; k.b = (unsigned char)fb; }
            if (has_bg) { k.alt = true; k.ar = (unsigned char)br; k.ag = (unsigned char)bg; k.ab = (unsigned char)bb; }
            grid[(size_t)cr * cols + cc] = k;
            cc++;
        }
    }
};

// An ordinary picture and a retained one, kept exactly the same by doing everything to both.
struct Twin {
    Picture imm, ret;
    void init(int R, int C) { imm.init(R, C); ret.init(R, C); ret.retain(); }
};

// shimmer_one_in: how many written cells shimmer (one in this many; 0 = none)
PicCell random_cell(std::mt19937& rng, int shimmer_one_in = 10) {
    PicCell c;
    static const double shimmers[] = {0.5, 1.0, 1.0, 3.0};
    c.lum = (double)(rng() % 1001) / 1000.0;
    c.r = (double)(rng() % 256); c.g = (double)(rng() % 256); c.b = (double)(rng() % 256);
    c.shimmer = (shimmer_one_in > 0 && rng() % (unsigned)shimmer_one_in == 0) ? shimmers[rng() % 4] : 0.0;     // most cells are still
    if (rng() % 4 == 0) { c.alt_lum = (double)(rng() % 101) / 100.0; c.alt_r = (double)(rng() % 256); c.alt_g = (double)(rng() % 256); c.alt_b = (double)(rng() % 256); }
    if (rng() % 5 == 0) { c.text = true; c.ch = (rng() % 3 == 0) ? U'█' : (char32_t)(U'a' + rng() % 26); c.inv = rng() % 4 == 0; }
    return c;
}

std::string random_text(std::mt19937& rng) {
    static const char* words[] = {"status", "centre 37.690N", "driving, following", "\xE2\x94\x82 box \xE2\x94\x82", "heading \xC2\xB0 E", "x", "rain"};
    return words[rng() % 7];
}

struct Run {
    std::mt19937 rng;
    int R, C;
    Twin t1, t2;
    Display a, b;                      // a shows the ordinary pictures, b the retained ones
    Model ma, mb;                      // what a person would see on each terminal
    Info info{"retained test", 0.0, {90, 160, 220}};
    int shimmer_one_in = 10;
    int frame = 0;
    long mismatches = 0;
    long frames_with_output = 0, quiet_frames = 0;

    Run(unsigned seed, int rows, int cols) : rng(seed), R(rows), C(cols) {
        t1.init(R, C); t2.init(R, C);
        a.init(R, C); b.init(R, C);
        ma.init(R + 1, C); mb.init(R + 1, C);
    }

    Twin& twin(int which) { return which == 0 ? t1 : t2; }

    void edit(Twin& t, double now) {
        const int i = (int)(rng() % (unsigned)(R + 1)), j = (int)(rng() % (unsigned)C);
        switch (rng() % 14) {
            case 0: case 1: case 2: {                         // write a cell through at()
                const PicCell v = random_cell(rng, shimmer_one_in);
                t.imm.at(i, j) = v; t.ret.at(i, j) = v;
                break;
            }
            case 3: {                                          // change one field through at()
                const double lum = (double)(rng() % 1001) / 1000.0;
                t.imm.at(i, j).lum = lum; t.ret.at(i, j).lum = lum;
                break;
            }
            case 4: (void)t.imm.at(i, j); (void)t.ret.at(i, j); break;    // a false alarm: touched, not changed
            case 5: {                                          // text
                const std::string s = random_text(rng);
                const double r = (double)(rng() % 256), g = (double)(rng() % 256), bb = (double)(rng() % 256);
                const bool inv = rng() % 3 == 0;
                t.imm.text(i, j, s, r, g, bb, inv); t.ret.text(i, j, s, r, g, bb, inv);
                break;
            }
            case 6: {                                          // set()
                const PicCell v = random_cell(rng, shimmer_one_in);
                t.imm.set(i, j, v); t.ret.set(i, j, v);
                break;
            }
            case 7: {                                          // set() to what is there already
                const PicCell v = t.imm.cell[(size_t)i * C + j];
                t.imm.set(i, j, v); t.ret.set(i, j, v);
                break;
            }
            case 8: {                                          // shimmer on or off
                const double sh = (shimmer_one_in > 0 && rng() % 4 == 0) ? 1.5 : 0.0;
                t.imm.at(i, j).shimmer = sh; t.ret.at(i, j).shimmer = sh;
                break;
            }
            case 9: {                                          // a whole row, through at()
                const PicCell v = random_cell(rng, shimmer_one_in);
                for (int x = 0; x < C; x++) { t.imm.at(i, x) = v; t.ret.at(i, x) = v; }
                break;
            }
            case 10: {                                         // the status bar, as a program does
                a.bar(t.imm, info, now, true, " [1/2]"); b.bar(t.ret, info, now, true, " [1/2]");
                break;
            }
            case 11: {                                         // an old style program: straight into the cells, then says so
                if (rng() % 3 != 0) break;
                const PicCell v = random_cell(rng, shimmer_one_in);
                t.imm.cell[(size_t)i * C + j] = v; t.ret.cell[(size_t)i * C + j] = v;
                t.ret.touch(i, j);
                break;
            }
            case 12: {                                         // erase a cell back to the default
                const PicCell v;
                t.imm.set(i, j, v); t.ret.set(i, j, v);
                break;
            }
            default: break;
        }
    }

    // one frame; returns false on a mismatch
    bool step() {
        const double now = 1.0 + (frame + (rng() % 20 == 0 ? (int)(rng() % 40) : 0)) / 30.0;
        // rare things that touch everything or change how the Display works
        const unsigned rare = rng() % 400;
        // (colour is chosen once when a program starts, so it is not changed in the middle of a run here)
        if (rare == 1) { a.hash_letters = b.hash_letters = !a.hash_letters; }
        else if (rare == 2) { a.fps = b.fps = (a.fps == 30 ? 15 : 30); }
        else if (rare == 3) { const std::string x = a.refresh(), y = b.refresh(); if (x != y) { report("refresh"); return false; } ma.feed(x); mb.feed(y); }
        else if (rare == 4) { Twin& t = twin((int)(rng() % 2)); const PicCell v = random_cell(rng, shimmer_one_in);   // a whole new picture, written straight in
            for (size_t k = 0; k < t.imm.cell.size(); k++) { t.imm.cell[k] = v; t.ret.cell[k] = v; }
            t.ret.touch_all(); }
        else if (rare == 5) { t1.imm = t2.imm; t1.ret = t2.ret; }                                      // copy one picture into the other
        else if (rare == 6) { std::swap(t1.imm, t2.imm); std::swap(t1.ret, t2.ret); }
        else if (rare == 7) {                                                                          // a new size
            R = 8 + (int)(rng() % 20); C = 20 + (int)(rng() % 60);
            t1.init(R, C); t2.init(R, C); a.init(R, C); b.init(R, C);
            ma.init(R + 1, C); mb.init(R + 1, C);
            const std::string x = a.refresh(), y = b.refresh();
            if (x != y) { report("refresh after resize"); return false; }
            ma.feed(x); mb.feed(y);
        }

        const int edits = (rng() % 3 == 0) ? 0 : (int)(rng() % 30);
        const int which = (rng() % 5 == 0) ? 1 : 0;           // mostly the first picture; now and then the other
        for (int k = 0; k < edits; k++) edit(twin(which), now);

        std::string oa, ob;
        const Picture* shown_picture;
        Picture mixed_a, mixed_b;
        if (rng() % 15 == 0) {                                 // a cross-fade between the two pictures, into a picture of its own
            mixed_b.retain();
            const double pct = (double)(rng() % 101) / 100.0;
            fade(t1.imm, t2.imm, pct, mixed_a);
            fade(t1.ret, t2.ret, pct, mixed_b);
            oa = a.render(mixed_a, now); ob = b.render(mixed_b, now);
            shown_picture = &mixed_a;
        } else {
            oa = a.render(twin(which).imm, now);
            ob = b.render(twin(which).ret, now);
            shown_picture = &twin(which).imm;
        }
        frame++;
        if (!oa.empty()) frames_with_output++; else quiet_frames++;
        if (oa != ob) { report("frame"); return false; }

        // and what a person sees: each terminal must show what a fresh Display would send for this picture
        ma.feed(oa); mb.feed(ob);
        Display fresh;
        fresh.init(R, C);
        fresh.color = a.color; fresh.fps = a.fps; fresh.hash_letters = a.hash_letters;
        Model expected;
        expected.init(R + 1, C);
        expected.feed(fresh.render(*shown_picture, now));
        // the other expected screen does not go through the Display at all: render_plain turns the picture into text
        // with its own code, and the model reads that
        Model plain;
        plain.init(R + 1, C);
        plain.feed(render_plain(*shown_picture, shown_picture->rows, C, now, a.fps, a.color, false, true, a.hash_letters));
        if (plain.broken) { report("render_plain output not understood by the model"); return false; }
        if (plain.grid != expected.grid) { report("a fresh Display and render_plain disagree"); return false; }
        if (ma.broken || mb.broken) { report("the output has something the Display should never send"); return false; }
        if (ma.grid != expected.grid) {
            report("terminal A does not show the picture");
            for (size_t k = 0; k < ma.grid.size(); k++)
                if (ma.grid[k] != expected.grid[k]) {
                    const VtCell& x = ma.grid[k]; const VtCell& y = expected.grid[k];
                    std::printf("  first difference at row %zu col %zu: shows U+%04X inv=%d fg=%d,%d,%d alt=%d bg=%d,%d,%d ; should show U+%04X inv=%d fg=%d,%d,%d alt=%d bg=%d,%d,%d\n",
                                k / (size_t)C, k % (size_t)C, (unsigned)x.ch, x.inv, x.r, x.g, x.b, x.alt, x.ar, x.ag, x.ab,
                                (unsigned)y.ch, y.inv, y.r, y.g, y.b, y.alt, y.ar, y.ag, y.ab);
                    break;
                }
            return false;
        }
        if (mb.grid != expected.grid) { report("terminal B does not show the picture"); return false; }
        return true;
    }

    void report(const char* what) {
        mismatches++;
        std::printf("MISMATCH (%s) frame %d, %d x %d\n", what, frame, R, C);
    }
};

}  // namespace

int main() {
    // ---- the bytes sent are the bytes the library has always sent: these numbers were made by the library as it was
    // before retained mode (see pinned_sequence.h), on a fixed random sequence, and must not change
    {
        struct { bool color; int fps; bool hash; unsigned seed; uint64_t expected_hash; size_t expected_bytes; int expected_frames; } pins[] = {
            {true, 30, false, 11, 0xa8d806149fabac80ull, 158039, 111},
            {false, 30, false, 12, 0x1e064603a27d1507ull, 41440, 112},
            {true, 12, true, 13, 0xfb94c916877d9a6aull, 88943, 97},
            {true, 30, true, 14, 0x8f3f5b88fced4781ull, 149452, 112},
        };
        for (const auto& pin : pins) {
            const PinnedResult r = pinned_run(pin.color, pin.fps, pin.hash, pin.seed);
            if (r.hash != pin.expected_hash || r.bytes != pin.expected_bytes || r.frames_with_output != pin.expected_frames)
                std::printf("pinned run (color %d, fps %d, hash %d, seed %u): hash %016llx, %zu bytes, %d frames; expected %016llx, %zu, %d\n",
                            pin.color, pin.fps, pin.hash, pin.seed, (unsigned long long)r.hash, r.bytes, r.frames_with_output,
                            (unsigned long long)pin.expected_hash, pin.expected_bytes, pin.expected_frames);
            CHECK(r.hash == pin.expected_hash && r.bytes == pin.expected_bytes && r.frames_with_output == pin.expected_frames);
        }
    }

    // ---- the rule: for every kind of edit, retained and ordinary pictures give the same bytes
    long total_frames = 0, quiet = 0, busy = 0;
    for (unsigned seed = 1; seed <= 40; seed++) {
        const int rows = 6 + (int)(seed % 13), cols = 20 + (int)(seed * 7 % 70);
        Run run(seed * 7919u, rows, cols);
        run.a.color = run.b.color = (seed % 3 != 0);              // some runs without colour
        if (seed % 2 == 0) run.shimmer_one_in = 0;                // half the runs have nothing that shimmers: many quiet frames
        for (int f = 0; f < 500; f++) {
            if (!run.step()) { failures++; std::printf("  (seed %u)\n", seed); break; }
        }
        total_frames += run.frame; quiet += run.quiet_frames; busy += run.frames_with_output;
    }
    std::printf("%ld random frames: %ld sent something, %ld sent nothing\n", total_frames, busy, quiet);
    CHECK(busy > 3000 && quiet > 1500);                        // the run really had both kinds of frame

    // ---- the skipping is real: a cell written behind the picture's back (straight into `cell`, no note made) is not
    // noticed by a retained picture until it is touched. An ordinary picture notices at once.
    {
        Display a, b;
        a.init(10, 30); b.init(10, 30);
        Picture imm, ret;
        imm.init(10, 30); ret.init(10, 30); ret.retain();
        for (int i = 0; i < 11; i++) for (int j = 0; j < 30; j++) {
            PicCell c; c.lum = 0.4; c.r = 200; c.g = 150; c.b = 100; c.shimmer = 0;
            imm.set(i, j, c); ret.set(i, j, c);
        }
        CHECK(a.render(imm, 1.0) == b.render(ret, 1.0));      // the first frame: everything
        CHECK(a.render(imm, 1.1).empty() && b.render(ret, 1.1).empty());   // still: nothing sent, by either
        PicCell hot; hot.lum = 0.95; hot.r = 255; hot.g = 40; hot.b = 40; hot.shimmer = 0;
        imm.cell[5 * 30 + 7] = hot; ret.cell[5 * 30 + 7] = hot;             // no note made
        const std::string oa = a.render(imm, 1.2), ob = b.render(ret, 1.2);
        CHECK(!oa.empty());                                    // the ordinary picture shows it
        CHECK(ob.empty());                                     // the retained one did not look: it was never told
        ret.touch(5, 7);                                       // now it is told
        const std::string oc = b.render(ret, 1.3);
        CHECK(oc == oa);                                       // and catches up with exactly what the other sent
        CHECK(b.render(ret, 1.4).empty());
        // touch_all is the way to say "I wrote all over it"
        for (size_t k = 0; k < ret.cell.size(); k++) { imm.cell[k].lum = 0.1; ret.cell[k].lum = 0.1; }
        CHECK(b.render(ret, 1.5).empty());
        ret.touch_all();
        CHECK(b.render(ret, 1.6) == a.render(imm, 1.6));
    }

    // ---- shimmering cells are looked at every frame without being touched; still cells are not
    {
        Display a, b;
        a.init(6, 20); b.init(6, 20);
        Picture imm, ret;
        imm.init(6, 20); ret.init(6, 20); ret.retain();
        for (int i = 0; i < 7; i++) for (int j = 0; j < 20; j++) {
            PicCell c; c.lum = 0.3 + 0.01 * j; c.r = 180; c.g = 160; c.b = 90; c.shimmer = (i == 2) ? 1.0 : 0.0;
            imm.set(i, j, c); ret.set(i, j, c);
        }
        int sent = 0;
        for (int f = 0; f < 60; f++) {
            const double t = 1.0 + f / 30.0;
            const std::string x = a.render(imm, t), y = b.render(ret, t);
            CHECK(x == y);
            if (f > 0 && !x.empty()) sent++;
        }
        CHECK(sent > 10);                                      // row 2 shimmers: letters change with time, untouched
        // a cell that stops shimmering stops being looked at; one that starts, starts
        imm.at(2, 5).shimmer = 0; ret.at(2, 5).shimmer = 0;
        imm.at(4, 6).shimmer = 2.0; ret.at(4, 6).shimmer = 2.0;
        for (int f = 60; f < 120; f++) {
            const double t = 1.0 + f / 30.0;
            CHECK(a.render(imm, t) == b.render(ret, t));
        }
        // no shimmer anywhere: a quiet screen sends nothing at all
        for (int i = 0; i < 7; i++) for (int j = 0; j < 20; j++) { imm.at(i, j).shimmer = 0; ret.at(i, j).shimmer = 0; }
        CHECK(a.render(imm, 5.0) == b.render(ret, 5.0));
        for (int f = 0; f < 10; f++) { CHECK(b.render(ret, 5.1 + f / 30.0).empty()); CHECK(a.render(imm, 5.1 + f / 30.0).empty()); }
    }

    // ---- a Display that changes how it works looks at everything again
    {
        Display a, b;
        a.init(6, 20); b.init(6, 20);
        Picture imm, ret;
        imm.init(6, 20); ret.init(6, 20); ret.retain();
        for (int i = 0; i < 7; i++) for (int j = 0; j < 20; j++) {
            PicCell c; c.lum = 0.2 + 0.03 * j; c.r = 100 + 5 * j; c.g = 90; c.b = 200; c.shimmer = 0.0;
            imm.set(i, j, c); ret.set(i, j, c);
        }
        CHECK(a.render(imm, 1.0) == b.render(ret, 1.0));
        a.color = b.color = false;                              // nothing in the picture changed, only how it is shown
        const std::string x = a.render(imm, 1.1), y = b.render(ret, 1.1);
        CHECK(x == y && !x.empty());
        a.hash_letters = b.hash_letters = true;
        CHECK(a.render(imm, 1.2) == b.render(ret, 1.2));
        a.fps = b.fps = 12;
        CHECK(a.render(imm, 1.3) == b.render(ret, 1.3));
    }

    // ---- pictures: copies, moves, serials, the rules of retained mode
    {
        Picture p;
        p.init(4, 10);
        p.retain();
        CHECK(p.retained && p.all_touched);
        p.consume();
        CHECK(!p.all_touched);
        p.at(1, 2).lum = 0.5;
        CHECK(p.touched[1 * 10 + 2] && p.row_touched[1] && !p.touched[0]);
        p.set(2, 2, p.at(2, 2));                                  // at() above is a write; this set is not (same cell)
        p.consume();
        CHECK(!p.touched[1 * 10 + 2] && !p.row_touched[1]);
        const PicCell same = std::as_const(p).at(3, 3);
        p.set(3, 3, same);                                         // a cell set to what it is: not a write
        CHECK(!p.touched[3 * 10 + 3]);
        PicCell other = same; other.lum = 0.77;
        p.set(3, 3, other);
        CHECK(p.touched[3 * 10 + 3] && p.row_touched[3]);
        const Picture& cp = p;
        (void)cp.at(0, 0);                                         // a const look is not a write
        CHECK(!p.touched[0]);
        p.text(0, 0, "ab");
        CHECK(p.touched[0] && p.touched[1] && !p.touched[2]);
        p.consume();
        p.text(0, 0, "ab");                                        // the same text again: not a write
        CHECK(!p.touched[0] && !p.touched[1] && !p.row_touched[0]);

        Picture copy(p);
        CHECK(copy.serial != p.serial && copy.all_touched && copy.retained);
        CHECK(copy.cell.size() == p.cell.size());
        p.consume();
        Picture other_pic;
        const uint64_t keep = other_pic.serial;
        other_pic = p;                                             // copying into a picture makes all of it new
        CHECK(other_pic.serial == keep && other_pic.all_touched);
        Picture moved(std::move(copy));
        CHECK(moved.all_touched && moved.serial != copy.serial);
        p.consume();
        std::swap(p, moved);
        CHECK(p.all_touched && moved.all_touched);

        Picture a1, a2;
        CHECK(a1.serial != a2.serial);
        // init starts over
        p.consume();
        p.init(3, 5);
        CHECK(p.all_touched && p.rows == 4 && p.cols == 5);
        // an ordinary picture notes nothing and costs nothing
        Picture plain;
        plain.init(3, 5);
        plain.at(1, 1).lum = 0.5;
        CHECK(!plain.retained && plain.touched.empty() && plain.row_touched.empty());
    }

    // ---- fade into a retained picture: the whole output is new
    {
        Picture a, b, out;
        a.init(3, 6); b.init(3, 6);
        out.init(3, 6); out.retain(); out.consume();
        CHECK(!out.all_touched);
        fade(a, b, 0.5, out);
        CHECK(out.all_touched);
    }

    // ---- the bar's look at its row does not count as a write
    {
        Display d;
        d.init(5, 30);
        Picture p;
        p.init(5, 30); p.retain();
        Info info{"bar", 0.0, {200, 100, 50}};
        d.bar(p, info, 1.0, true);                                 // writes the bar
        p.consume();
        const Picture before = p;
        d.bar(p, info, 1.0, true);                                 // same time: the same text: nothing is a write
        bool any = false;
        for (unsigned char v : p.touched) if (v) any = true;
        CHECK(!any);
        (void)before;
    }

    if (failures == 0) std::printf("all retained mode tests passed\n");
    return failures == 0 ? 0 : 1;
}
