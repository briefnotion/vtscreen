// Tests for drawings made of shapes (Picture::place_drawing, Display::enable_regis, and their use as pixels).
//
// They need vtpen's headers, and its tests/regis_decode.h (a small ReGIS reader written from the DEC manual), on the include
// path:   g++ -std=c++17 -I<vtpen>/include -I<vtpen>/tests vector_tests.cpp
// tests/run_tests.sh does that when it finds vtpen (next to this repository, or in $VTPEN).
//
// The rules they check:
//   - a Display that has not been told about ReGIS or Sixel sends exactly what it sent without the drawings
//   - told about Sixel only, a drawing is shown as the pixels vtpen makes of it, the same bytes as a Raster of those pixels
//   - told about ReGIS, a pretend terminal that follows the bytes (a letter written into a cell wipes that cell's ReGIS pixels,
//     as measured in xterm) shows the same letters as a letters-only Display, and the drawing's pixels, and nothing else
//   - only what changed is sent: a changed or moved drawing is erased then drawn, never the whole ReGIS screen wiped;
//     a letter written under a drawing makes it drawn again; a frame with nothing new sends nothing
//   - fade() carries the drawings, and a drawing is drawn as ReGIS while it is at least half there
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "../vtscreen.h"

#ifndef VTSCREEN_VECTORS
int main() {
    std::printf("vtpen's headers are not on the include path: the vector tests are skipped\n");
    return 0;
}
#else

#include "regis_decode.h"

using namespace vtscreen;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

namespace {

const int CW = 6, CH = 12;                        // a small character cell, so tests run fast
const int R = 10, C = 60;                         // the picture; the status row is under it
const int SW = C * CW, SH = (R + 1) * CH;         // the ReGIS screen the Display is told about: the whole terminal

// ---- a pretend terminal that understands what a Display sends, ReGIS included ----
//
// Letters go into cells and a letter written into a cell wipes the ReGIS pixels in it; a ReGIS string is read by the reader from
// vtpen's tests; clearing the screen clears the pixels and the ReGIS colour map too (the Display must send the colours again);
// Sixel pictures are counted and skipped.
struct Model {
    std::vector<char> letter;
    registest::Screen plane;
    int cr = 0, cc = 0;
    bool cursor_known = true, broken = false;
    long regis_strings = 0, sixels = 0;

    void init() {
        letter.assign((size_t)(R + 1) * C, ' ');
        plane = registest::Screen();
        plane.grow(SW, SH);
        cr = cc = 0; cursor_known = true; broken = false; regis_strings = sixels = 0;
    }
    void wipe(int i, int j) {
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++) plane.px[(size_t)(i * CH + y) * plane.w + (size_t)j * CW + x] = 0;
    }
    void feed(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = (unsigned char)s[i];
            if (c == 0x1b) {
                if (i + 1 >= s.size()) { broken = true; return; }
                if (s[i + 1] == 'P') {
                    const size_t end = s.find("\x1b\\", i);
                    if (end == std::string::npos) { broken = true; return; }
                    if (s.compare(i, 4, "\x1bP1p") == 0) {
                        registest::read_into(plane, s.substr(i, end + 2 - i));
                        if (!plane.ok) { std::printf("  (the ReGIS reader said: %s)\n", plane.error.c_str()); broken = true; return; }
                        regis_strings++;
                    } else { sixels++; cursor_known = false; }
                    i = end + 2;
                    continue;
                }
                if (s[i + 1] != '[') { broken = true; return; }
                size_t j = i + 2;
                while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
                if (j >= s.size()) { broken = true; return; }
                const std::string params = s.substr(i + 2, j - (i + 2));
                const char f = s[j];
                if (f == 'H') {
                    if (params.empty()) { cr = 0; cc = 0; }
                    else {
                        const size_t semi = params.find(';');
                        cr = std::atoi(params.substr(0, semi).c_str()) - 1;
                        cc = std::atoi(params.substr(semi + 1).c_str()) - 1;
                    }
                    cursor_known = true;
                } else if (f == 'J') {
                    if (params != "2") { broken = true; return; }
                    std::fill(letter.begin(), letter.end(), ' ');
                    std::fill(plane.px.begin(), plane.px.end(), (uint8_t)0);
                    plane.defined.fill(false);
                } else if (f == 'C') {
                    if (!cursor_known) broken = true;
                    cc += params.empty() ? 1 : std::atoi(params.c_str());
                } else if (f != 'm') broken = true;
                i = j + 1;
                continue;
            }
            i++;
            if (!cursor_known || cr < 0 || cr >= R + 1 || cc < 0 || cc >= C) { broken = true; continue; }
            letter[(size_t)cr * C + cc] = (char)c;
            wipe(cr, cc);
            cc++;
        }
    }
};

// ---- pictures ----

void letters(Picture& p, std::mt19937& rng, int how_many) {          // a few cells change; the rest stay
    for (int k = 0; k < how_many; k++) {
        PicCell c;
        c.text = true;
        c.ch = (char32_t)('a' + rng() % 26);
        c.r = c.g = c.b = 200;
        p.set((int)(rng() % R), (int)(rng() % C), c);
    }
}
void all_letters(Picture& p, char ch) {
    p.init(R, C);
    for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c; c.text = true; c.ch = (char32_t)ch; c.r = c.g = c.b = 200; p.set(i, j, c); }
}

const vtpen::Rgb COLOURS[] = {{255, 80, 80}, {80, 255, 80}, {90, 140, 255}, {255, 230, 60}, {240, 240, 240}};

std::shared_ptr<const vtpen::Drawing> make_drawing(std::mt19937& rng) {
    auto d = std::make_shared<vtpen::Drawing>(100, 60);
    const int n = 2 + (int)(rng() % 4);
    for (int k = 0; k < n; k++) {
        d->set_colour(COLOURS[rng() % 5]);
        d->set_thickness(1 + (double)(rng() % 3));
        const double a = (double)(rng() % 100), b = (double)(rng() % 60), c2 = (double)(rng() % 100), e = (double)(rng() % 60);
        switch (rng() % 4) {
            case 0: d->line({a, b}, {c2, e}); break;
            case 1: d->circle(a, b, 5 + (double)(rng() % 20)); break;
            case 2: d->fill_rectangle(std::min(a, c2), std::min(b, e), 5 + std::abs(a - c2) / 3, 5 + std::abs(b - e) / 3); break;
            default: d->polyline({{a, b}, {c2, e}, {(a + c2) / 2, 59 - b}}); break;
        }
    }
    return d;
}

// The drawing as pixels, the way a Display does it (so the same bytes can be asked of a Raster).
std::shared_ptr<Raster> as_raster(const vtpen::Drawing& d, int rows, int cols, int edge) {
    vtpen::Canvas canvas(cols * CW, rows * CH, d.width(), d.height());
    canvas.set_edge_levels(edge);
    d.draw(canvas);
    auto r = std::make_shared<Raster>();
    r->init(cols * CW, rows * CH);
    const vtpen::Rgb bg = canvas.background();
    for (int y = 0; y < r->h; y++) for (int x = 0; x < r->w; x++) { const vtpen::Rgb p = canvas.row(y)[x]; if (!(p == bg)) r->at(x, y) = Pixel{p.r, p.g, p.b, 255}; }
    return r;
}

struct Place { std::shared_ptr<const vtpen::Drawing> d; int row, col, rows, cols; };

// What the ReGIS screen should hold: every placement drawn on its own, put together (ink only: which colour register a
// colour gets depends on what was drawn first).
std::vector<char> expected_ink(const std::vector<Place>& places) {
    registest::Screen e;
    e.grow(SW, SH);
    for (const Place& p : places) {
        vtpen::RegisOptions o;
        o.surface_width = SW; o.surface_height = SH;
        vtpen::Regis r(o);
        registest::read_into(e, r.draw(*p.d, {p.col * CW, p.row * CH, p.cols * CW, p.rows * CH}));
    }
    std::vector<char> ink((size_t)SW * SH, 0);
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) ink[(size_t)y * SW + x] = e.at(x, y) != 0;
    return ink;
}
long ink_differences(const Model& m, const std::vector<char>& want) {
    long bad = 0;
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) bad += (m.plane.at(x, y) != 0) != (want[(size_t)y * SW + x] != 0);
    return bad;
}
long ink_count(const Model& m) {
    long n = 0;
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) n += m.plane.at(x, y) != 0;
    return n;
}

}  // namespace

int main() {
    std::mt19937 rng(11);

    // ---- a Picture carries drawings along ----
    {
        auto d = make_drawing(rng);
        Picture p;
        p.init(R, C);
        p.place_drawing(d, 1, 2, 3, 4);
        p.place_drawing(nullptr, 0, 0, 3, 3);                           // nothing: ignored
        p.place_drawing(d, 0, 0, 0, 3);                                 // no cells: ignored
        p.place_drawing(d, 0, 0, 3, -1);
        CHECK(p.vectors.size() == 1 && p.vectors[0].drawing == d && p.vectors[0].weight == 1.0 && p.vectors[0].cols == 4);
        Picture copy(p);
        CHECK(copy.vectors.size() == 1);
        Picture assigned;
        assigned = p;
        CHECK(assigned.vectors.size() == 1);
        Picture moved(std::move(copy));
        CHECK(moved.vectors.size() == 1);
        Picture move_assigned;
        move_assigned = std::move(assigned);
        CHECK(move_assigned.vectors.size() == 1);
        p.clear_vectors();
        CHECK(p.vectors.empty());
        p.place_drawing(d, 0, 0, 2, 2);
        p.init(R, C);                                                    // starting over: no drawings either
        CHECK(p.vectors.empty());
    }

    // ---- what a scene needs to know, and the Display's softness starts from the global ----
    {
        vtscreen::set_image_support(0, 0);
        vtscreen::set_regis_support(0, 0, 0, 0);
        CHECK(vtscreen::vector_cell_w() == 0 && vtscreen::vector_cell_h() == 0);
        vtscreen::set_image_support(7, 15);
        CHECK(vtscreen::vector_cell_w() == 7 && vtscreen::vector_cell_h() == 15);
        vtscreen::set_regis_support(9, 19, 800, 480);
        CHECK(vtscreen::vector_cell_w() == 9 && vtscreen::vector_cell_h() == 19);          // ReGIS's cell, when it has one
        vtscreen::set_regis_support(9, 19, 800, 0);                                         // not a usable screen: off
        CHECK(vtscreen::vector_cell_w() == 7 && vtscreen::vector_cell_h() == 15);
        vtscreen::set_image_support(0, 0);
        vtscreen::g_vector_edge_levels = 3;
        Display e;
        CHECK(e.vector_edge_levels == 3);
        vtscreen::g_vector_edge_levels = 2;
        Display f;
        CHECK(f.vector_edge_levels == 2);
    }

    // ---- fade carries the drawings, with their share ----
    {
        auto da = make_drawing(rng), db = make_drawing(rng);
        Picture a, b, out;
        all_letters(a, 'a'); all_letters(b, 'b');
        a.place_drawing(da, 1, 1, 3, 3);
        b.place_drawing(db, 4, 4, 3, 3);
        fade(a, b, 0.0, out);
        CHECK(out.vectors.size() == 1 && out.vectors[0].drawing == da && out.vectors[0].weight == 1.0);
        fade(a, b, 1.0, out);
        CHECK(out.vectors.size() == 1 && out.vectors[0].drawing == db && out.vectors[0].weight == 1.0);
        fade(a, b, 0.25, out);
        CHECK(out.vectors.size() == 2 && out.vectors[0].drawing == da && std::abs(out.vectors[0].weight - 0.75) < 1e-9 &&
              out.vectors[1].drawing == db && std::abs(out.vectors[1].weight - 0.25) < 1e-9);
        CHECK(out.vectors[0].seed != out.vectors[1].seed);
        Picture out2;
        fade(out, b, 0.5, out2);                                        // a fade of a fade multiplies the shares
        CHECK(out2.vectors.size() == 3 && std::abs(out2.vectors[0].weight - 0.375) < 1e-9 && std::abs(out2.vectors[2].weight - 0.5) < 1e-9);
        Picture none;
        all_letters(none, 'n');
        fade(none, none, 0.5, out);                                     // a result that had drawings does not keep them
        CHECK(out.vectors.empty());
    }

    // ---- not told: ignored, the bytes are what they were ----
    {
        Display with, without;
        with.init(R, C); without.init(R, C);
        CHECK(!with.regis_enabled() && !with.images_enabled());
        long sent = 0;
        for (int frame = 0; frame < 40; frame++) {
            Picture a, b;
            all_letters(a, 'a' + frame % 3); all_letters(b, 'a' + frame % 3);
            letters(a, rng, 20);
            b = a;
            b.place_drawing(make_drawing(rng), (int)(rng() % 5), (int)(rng() % 20), 3, 10);
            const std::string x = with.render(b, frame * 0.1), y = without.render(a, frame * 0.1);
            CHECK(x == y);
            sent += (long)x.size();
        }
        CHECK(sent > 500);
        with.enable_regis(0, CH, SW, SH);
        CHECK(!with.regis_enabled());
        with.enable_regis(CW, CH, SW, 0);
        CHECK(!with.regis_enabled());
        with.enable_regis(CW, CH, SW, SH);
        CHECK(with.regis_enabled());
        with.enable_regis(0, 0, 0, 0);
        CHECK(!with.regis_enabled());
    }

    // ---- told about Sixel only: the drawing is shown as pixels, the same bytes as a Raster of them ----
    {
        Display drawn, pixels;
        drawn.init(R, C); pixels.init(R, C);
        drawn.enable_images(CW, CH); pixels.enable_images(CW, CH);
        long pictures = 0;
        std::shared_ptr<const vtpen::Drawing> d = make_drawing(rng);
        for (int frame = 0; frame < 40; frame++) {
            Picture a, b;
            all_letters(a, 'x'); all_letters(b, 'x');
            letters(a, rng, 15);
            b = a;
            if (frame % 7 == 0) d = make_drawing(rng);                  // sometimes a new drawing, else the same one again
            const int row = frame % 4, col = (frame * 3) % 20;
            const double rate = frame % 3 == 0 ? 2.0 : -1;                // the letters under it held back, or not
            a.place_drawing(d, row, col, 4, 12, rate);
            b.place_image(as_raster(*d, 4, 12, drawn.vector_edge_levels), row, col, rate);
            const std::string x = drawn.render(a, frame * 0.1), y = pixels.render(b, frame * 0.1);
            CHECK(x == y);
            pictures += x.find("\x1bP0;1;0q") != std::string::npos;
        }
        CHECK(pictures > 20);
        // The edge softness is the Display's to set.
        Display soft, hard;
        soft.init(R, C); hard.init(R, C);
        soft.enable_images(CW, CH); hard.enable_images(CW, CH);
        soft.vector_edge_levels = 4; hard.vector_edge_levels = 1;
        Picture p;
        all_letters(p, 'x');
        p.place_drawing(d, 1, 1, 4, 12);
        CHECK(soft.render(p, 0) != hard.render(p, 0));
        // A drawing that is only half there is drawn as fewer pixels than a whole one.
        Display whole, half;
        whole.init(R, C); half.init(R, C);
        whole.enable_images(CW, CH); half.enable_images(CW, CH);
        Picture pw, ph;
        all_letters(pw, 'x'); all_letters(ph, 'x');
        pw.place_drawing(d, 1, 1, 4, 12);
        ph.place_drawing(d, 1, 1, 4, 12);
        ph.vectors[0].weight = 0.4; ph.vectors[0].seed = 5;
        CHECK(half.render(ph, 0).size() < whole.render(pw, 0).size());
        // Nothing drawn when the Display has no colour.
        Display mono, plain;
        mono.init(R, C); plain.init(R, C);
        mono.color = false; plain.color = false;
        mono.enable_images(CW, CH);
        CHECK(mono.render(pw, 0) == plain.render(pw, 0));
        // The same drawing in another size, or with another edge softness, is worked out again, not taken from before: every
        // frame is what a Raster made at that size and softness gives.
        {
            Display x, y;
            x.init(R, C); y.init(R, C);
            x.enable_images(CW, CH); y.enable_images(CW, CH);
            struct Step { int rows, cols, edge; };
            const Step steps[] = {{4, 12, 2}, {4, 12, 2}, {6, 20, 2}, {6, 20, 4}, {6, 20, 4}, {4, 12, 4}, {4, 12, 1}, {2, 8, 1}, {6, 20, 2}};
            int frame = 0, compared = 0;
            for (const Step& st : steps) {
                Picture a, b;
                all_letters(a, 'x'); all_letters(b, 'x');
                x.vector_edge_levels = st.edge;
                a.place_drawing(d, 1, 1, st.rows, st.cols);
                b.place_image(as_raster(*d, st.rows, st.cols, st.edge), 1, 1);
                const std::string sx = x.render(a, frame * 0.1), sy = y.render(b, frame * 0.1);
                CHECK(sx == sy);
                compared += sx.empty() ? 0 : 1;
                frame++;
            }
            CHECK(compared >= 6);
        }
    }

    // ---- told about ReGIS: on a pretend terminal ----
    {
        Display d;
        d.init(R, C);
        d.enable_regis(CW, CH, SW, SH);
        Model m;
        m.init();
        auto drawing = make_drawing(rng), other = make_drawing(rng);
        Picture p;
        all_letters(p, 'x');
        p.place_drawing(drawing, 2, 5, 4, 20);
        std::vector<Place> want{{drawing, 2, 5, 4, 20}};
        std::string s = d.render(p, 0);
        m.feed(s);
        CHECK(!m.broken && m.regis_strings == 1);
        CHECK(ink_count(m) > 100 && ink_differences(m, expected_ink(want)) == 0);
        CHECK(s.find("S(A[0,0][359,131])") != std::string::npos && s.find("S(M") != std::string::npos);   // size and colours, once
        const size_t first = s.size();

        // Nothing changed: nothing is sent.
        CHECK(d.render(p, 1).empty());
        CHECK(d.render(p, 2).empty());

        // A letter written under the drawing: the drawing is drawn again, on top, not erased first.
        Picture q = p;
        PicCell c; c.text = true; c.ch = U'Z'; c.r = c.g = c.b = 250;
        q.set(3, 8, c);
        s = d.render(q, 3);
        m.feed(s);
        CHECK(!m.broken && s.find("W(V,P1)") != std::string::npos && s.find("W(E,P1)") == std::string::npos && s.find("S(E)") == std::string::npos);
        CHECK(s.find("S(M") == std::string::npos);                                                      // the colours are already there
        CHECK(ink_differences(m, expected_ink(want)) == 0 && m.letter[(size_t)3 * C + 8] == 'Z');
        CHECK(s.size() < first + 50 && s.size() > 100);

        // A letter in the very last cell of the drawing's block, or the very first, counts; one cell beyond either does not.
        // (The drawing is at rows 2 to 5 and columns 5 to 24.)
        for (const auto& at : std::vector<std::pair<int, int>>{{5, 24}, {2, 5}, {5, 5}, {2, 24}}) {
            Picture e = q;
            e.set(at.first, at.second, c);
            q = e;
            s = d.render(q, 3.5);
            m.feed(s);
            CHECK(!m.broken && s.find("W(V,P1)") != std::string::npos);
        }
        for (const auto& at : std::vector<std::pair<int, int>>{{6, 24}, {1, 5}, {5, 25}, {2, 4}}) {
            PicCell c2 = c; c2.ch = U'Y';
            Picture e = q;
            e.set(at.first, at.second, c2);
            q = e;
            s = d.render(q, 3.6);
            m.feed(s);
            CHECK(!m.broken && s.find("\x1bP1p") == std::string::npos);
        }
        p = q;
        // A letter written outside the drawing's cells: the drawing is left alone.
        Picture q2 = q;
        q2.set(8, 50, c);
        s = d.render(q2, 4);
        m.feed(s);
        CHECK(!m.broken && s.find("\x1bP1p") == std::string::npos && s.size() < 60);
        CHECK(ink_differences(m, expected_ink(want)) == 0);

        // A drawing that changes is not erased with ReGIS (an erased ReGIS pixel is a black one, which hides the letter under
        // it): the letters its lines were over are written again, which takes the lines out, and the new one is drawn on top, in one
        // ReGIS string. The whole screen is not wiped.
        auto dcs_count = [](const std::string& x) { size_t n = 0, at = 0; while ((at = x.find("\x1bP1p", at)) != std::string::npos) { n++; at++; } return n; };
        Picture r = q2;
        r.clear_vectors();
        r.place_drawing(other, 2, 5, 4, 20);
        want = {{other, 2, 5, 4, 20}};
        s = d.render(r, 5);
        m.feed(s);
        CHECK(!m.broken && s.find("W(V,P1)") != std::string::npos && s.find("W(E,P1)") == std::string::npos && s.find("S(E)") == std::string::npos);
        CHECK(ink_differences(m, expected_ink(want)) == 0);
        CHECK(dcs_count(s) == 1 && s.find("\x1bP1p") > 20);                                                // letters first, then one ReGIS string

        // The same drawing moved.
        Picture mv = r;
        mv.clear_vectors();
        mv.place_drawing(other, 4, 30, 4, 20);
        want = {{other, 4, 30, 4, 20}};
        s = d.render(mv, 6);
        m.feed(s);
        CHECK(!m.broken && s.find("W(V,P1)") != std::string::npos && s.find("W(E,P1)") == std::string::npos && dcs_count(s) == 1);
        CHECK(ink_differences(m, expected_ink(want)) == 0);

        // Two drawings side by side; one changes, the other is not touched.
        Picture two = mv;
        two.place_drawing(drawing, 1, 2, 3, 12);
        want = {{other, 4, 30, 4, 20}, {drawing, 1, 2, 3, 12}};
        s = d.render(two, 7);
        m.feed(s);
        CHECK(!m.broken && ink_differences(m, expected_ink(want)) == 0);
        Picture three = two;
        three.vectors[1].drawing = other;
        want = {{other, 4, 30, 4, 20}, {other, 1, 2, 3, 12}};
        s = d.render(three, 8);
        m.feed(s);
        CHECK(!m.broken && ink_differences(m, expected_ink(want)) == 0);
        // Everything that changed in a frame goes in one ReGIS string.
        CHECK(dcs_count(s) == 1 && s.find("W(E,P1)") == std::string::npos);

        // A drawing is drawn as ReGIS while at least half of it is there (it cannot be half drawn).
        for (const double weight : {1.0, 0.9, 0.5, 0.49, 0.1}) {
            Display w;
            w.init(R, C);
            w.enable_regis(CW, CH, SW, SH);
            Picture pw;
            all_letters(pw, 'x');
            pw.place_drawing(other, 2, 5, 4, 20);
            pw.vectors[0].weight = weight;
            CHECK((w.render(pw, 0).find("W(V,P1)") != std::string::npos) == (weight >= 0.5));
        }
        // No colour: nothing is drawn (ReGIS draws in colours the letters' colour codes would not match).
        Display mono, plain;
        mono.init(R, C); plain.init(R, C);
        mono.color = false; plain.color = false;
        mono.enable_regis(CW, CH, SW, SH);
        CHECK(mono.render(r, 0) == plain.render(r, 0));

        // The drawings go: the letters they were over are written again, and that is all (no ReGIS at all).
        Picture gone = three;
        gone.clear_vectors();
        s = d.render(gone, 9);
        m.feed(s);
        CHECK(!m.broken && !s.empty() && s.find("\x1bP1p") == std::string::npos && ink_count(m) == 0);
        CHECK(d.render(gone, 10).empty());

        // The terminal was cleared (refresh): the drawing and its colours are sent again.
        d.render(three, 11);
        m.feed(d.refresh());
        CHECK(ink_count(m) == 0);
        s = d.render(three, 12);
        m.feed(s);
        CHECK(!m.broken && s.find("S(M") != std::string::npos && ink_differences(m, expected_ink(want)) == 0);

        // Only the part of a drawing that lies on the ReGIS screen is drawn, and a drawing off it is nothing.
        Display small;
        small.init(R, C);
        small.enable_regis(CW, CH, 120, 60);                              // a screen of 20 x 5 cells
        Model ms;
        ms.init();
        Picture off;
        all_letters(off, 'x');
        off.place_drawing(drawing, 0, 10, 4, 20);                         // half of it is past the right edge
        off.place_drawing(other, 6, 0, 3, 10);                            // below the bottom
        ms.feed(small.render(off, 0));
        CHECK(!ms.broken && ink_count(ms) > 0);
        for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) if (ms.plane.at(x, y) && (x >= 120 || y >= 60)) { CHECK(false); y = SH; break; }
    }

    // ---- many random frames: the letters are a letters-only Display's, the drawings' pixels are the drawings' ----
    {
        Display d, plain;
        d.init(R, C); plain.init(R, C);
        d.enable_regis(CW, CH, SW, SH);
        Model m, mp;
        m.init(); mp.init();
        std::vector<std::shared_ptr<const vtpen::Drawing>> pool;
        for (int k = 0; k < 4; k++) pool.push_back(make_drawing(rng));
        Picture p, bare;
        all_letters(p, ' ');
        bare = p;
        long bad_ink = 0, broken = 0, letters_differ = 0, with_drawings = 0, bytes = 0, frames_empty = 0;
        for (int frame = 0; frame < 300; frame++) {
            letters(p, rng, (int)(rng() % 3 == 0 ? 0 : 1 + rng() % 40));
            bare = p;
            p.clear_vectors();
            std::vector<Place> want;
            const int n = (int)(rng() % 3);                                // 0, 1 or 2 drawings, side by side so they do not touch
            for (int k = 0; k < n; k++) {
                Place pl{pool[rng() % pool.size()], (int)(rng() % 5), k * 30 + (int)(rng() % 8), 2 + (int)(rng() % 4), 10 + (int)(rng() % 15)};
                p.place_drawing(pl.d, pl.row, pl.col, pl.rows, pl.cols);
                want.push_back(pl);
            }
            const std::string s = d.render(p, frame * 0.05);
            m.feed(s);
            mp.feed(plain.render(bare, frame * 0.05));
            bytes += (long)s.size();
            frames_empty += s.empty();
            broken += m.broken || mp.broken;
            letters_differ += m.letter != mp.letter;
            bad_ink += ink_differences(m, expected_ink(want));
            with_drawings += !want.empty();
        }
        CHECK(broken == 0);
        CHECK(letters_differ == 0);
        CHECK(bad_ink == 0);
        CHECK(with_drawings > 100 && bytes > 5000 && frames_empty > 5);
        std::printf("300 random frames (%ld with drawings, %ld bytes, %ld frames with nothing to send): letters and ReGIS pixels as asked\n",
                    with_drawings, bytes, frames_empty);
    }

    // ---- without colour: left out, unless the program says its pictures suit that ----
    {
        auto drawing = make_drawing(rng);
        Picture p;
        all_letters(p, 'x');
        p.place_drawing(drawing, 2, 5, 4, 20);
        // as ReGIS
        Display left_out, shown, plain;
        left_out.init(R, C); shown.init(R, C); plain.init(R, C);
        left_out.color = shown.color = plain.color = false;
        shown.pictures_without_colour = true;
        left_out.enable_regis(CW, CH, SW, SH);
        shown.enable_regis(CW, CH, SW, SH);
        CHECK(left_out.render(p, 0) == plain.render(p, 0));                      // as before: nothing
        Model m;
        m.init();
        const std::string s = shown.render(p, 0);
        m.feed(s);
        CHECK(!m.broken && m.regis_strings == 1 && s.find("\x1b[38;") == std::string::npos && s.find("\x1b[48;") == std::string::npos);   // no colour codes for the letters
        CHECK(ink_differences(m, expected_ink({{drawing, 2, 5, 4, 20}})) == 0 && ink_count(m) > 50);
        // a Display that has colour does not need the flag
        Display with_colour;
        with_colour.init(R, C);
        with_colour.enable_regis(CW, CH, SW, SH);
        CHECK(with_colour.render(p, 0).find("\x1bP1p") != std::string::npos);
        // as Sixel pixels
        Display sixel_left_out, sixel_shown, sixel_raster;
        sixel_left_out.init(R, C); sixel_shown.init(R, C); sixel_raster.init(R, C);
        sixel_left_out.color = sixel_shown.color = sixel_raster.color = false;
        sixel_shown.pictures_without_colour = sixel_raster.pictures_without_colour = true;
        sixel_left_out.enable_images(CW, CH); sixel_shown.enable_images(CW, CH); sixel_raster.enable_images(CW, CH);
        Display plain_again;
        plain_again.init(R, C);
        plain_again.color = false;
        CHECK(sixel_left_out.render(p, 0) == plain_again.render(p, 0));
        Picture q;
        all_letters(q, 'x');
        q.place_image(as_raster(*drawing, 4, 20, sixel_shown.vector_edge_levels), 2, 5);
        const std::string a = sixel_shown.render(p, 0), b = sixel_raster.render(q, 0);
        CHECK(a == b && a.find("\x1bP0;1;0q") != std::string::npos);
    }

    // ---- ReGIS and Sixel both: drawings go as ReGIS, pictures made of pixels as Sixel ----
    {
        Display d;
        d.init(R, C);
        d.enable_images(CW, CH);
        d.enable_regis(CW, CH, SW, SH);
        Model m;
        m.init();
        auto drawing = make_drawing(rng);
        auto ras = std::make_shared<Raster>();
        ras->init(3 * CW, 2 * CH);
        for (int y = 0; y < ras->h; y++) for (int x = 0; x < ras->w; x++) ras->set(x, y, 255, 0, 0);
        Picture p;
        all_letters(p, 'x');
        p.place_drawing(drawing, 5, 20, 3, 10);
        p.place_image(ras, 1, 40);
        m.feed(d.render(p, 0));
        CHECK(!m.broken && m.sixels > 0 && m.regis_strings == 1);
        Display only_pixels;                                               // the same, without the drawing
        only_pixels.init(R, C);
        only_pixels.enable_images(CW, CH);
        Picture q;
        all_letters(q, 'x');
        q.place_image(ras, 1, 40);
        Model mq;
        mq.init();
        mq.feed(only_pixels.render(q, 0));
        CHECK(m.sixels == mq.sixels);                                      // the drawing was not drawn as pixels as well
        CHECK(ink_differences(m, expected_ink({{drawing, 5, 20, 3, 10}})) == 0);
    }

    // ---- colours: every drawing's colours are in the map from the first frame ----
    {
        Display d;
        d.init(R, C);
        d.enable_regis(CW, CH, SW, SH);
        Model m;
        m.init();
        auto red = std::make_shared<vtpen::Drawing>(10, 10), blue = std::make_shared<vtpen::Drawing>(10, 10);
        red->set_colour({255, 0, 0}); red->fill_rectangle(0, 0, 10, 10);
        blue->set_colour({0, 0, 255}); blue->fill_rectangle(0, 0, 10, 10);
        Picture p;
        all_letters(p, 'x');
        p.place_drawing(red, 1, 1, 3, 5);                                  // blue is only in the second drawing
        p.place_drawing(blue, 1, 20, 3, 5);
        const std::string s = d.render(p, 0);
        m.feed(s);
        CHECK(!m.broken);
        CHECK(s.find("S(M1(AH120L50S100)2(AH0L50S100))") != std::string::npos || s.find("S(M1(AH0L50S100)2(AH120L50S100))") != std::string::npos);
        const int a = m.plane.at(1 * CW + 3, 1 * CH + 3), b = m.plane.at(20 * CW + 3, 1 * CH + 3);
        CHECK(a != 0 && b != 0 && a != b);                                 // two different colour registers
    }

    if (failures == 0) std::printf("all vector tests passed\n");
    return failures ? 1 : 0;
}

#endif
