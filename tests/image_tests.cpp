// Tests for pictures made of pixels (Raster, Picture::place_image, Display::enable_images and the Sixel encoder).
//
// The rules they check:
//   - the encoder's text, decoded again, is the pixels it was given (by a decoder written here, not the encoder's idea of itself)
//   - a Display that has not been told about pictures sends exactly what it sent before they existed
//   - a Display that has been told draws them: after every frame a pretend terminal that follows the bytes (including the
//     rule that writing a letter into a cell wipes that cell's pixels, as measured in Windows Terminal) shows the same letters
//     as a letters-only Display, and the pixels the pictures ask for, and nothing else
//   - only what changed is sent
//
//   tests/run_tests.sh        builds this with the strict flags and the sanitizers and runs it
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../vtscreen.h"

using namespace vtscreen;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

namespace {

// ---- a decoder for the Sixel the library writes ----

struct Decoded {
    int w = 0, h = 0;
    std::vector<Pixel> px;                     // a is 255 where the picture drew, 0 where it left alone
    bool ok = false;
};

// The picture that starts at the DCS at s[at]. end is set to just past its ST.
Decoded decode_sixel(const std::string& s, size_t at, size_t* end = nullptr) {
    Decoded d;
    size_t i = at;
    if (s.compare(i, 2, "\x1bP") != 0) return d;
    const size_t q = s.find('q', i);
    if (q == std::string::npos) return d;
    i = q + 1;
    auto number = [&](size_t& k) {
        int v = 0;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9') v = v * 10 + (s[k++] - '0');
        return v;
    };
    if (i >= s.size() || s[i] != '"') return d;
    i++;
    number(i); i++; number(i); i++;
    d.w = number(i); i++;
    d.h = number(i);
    d.px.assign((size_t)d.w * d.h, Pixel());
    std::map<int, Pixel> pal;
    int cur = 0, x = 0, y = 0;
    auto put = [&](char ch, int times) {
        for (int t = 0; t < times; t++, x++)
            for (int b = 0; b < 6; b++)
                if (((ch - 63) >> b & 1) && x < d.w && y + b < d.h) d.px[(size_t)(y + b) * d.w + x] = pal[cur];
    };
    while (i < s.size()) {
        const char c = s[i];
        if (c == '#') {
            i++;
            cur = number(i);
            if (i < s.size() && s[i] == ';') {
                i++; number(i); i++;
                const int r = number(i); i++;
                const int g = number(i); i++;
                const int b = number(i);
                pal[cur] = Pixel{(uint8_t)(r * 255 / 100), (uint8_t)(g * 255 / 100), (uint8_t)(b * 255 / 100), 255};
            }
        } else if (c == '!') {
            i++;
            const int n = number(i);
            put(s[i++], n);
        } else if (c == '$') { x = 0; i++; }
        else if (c == '-') { x = 0; y += 6; i++; }
        else if (c == '\x1b') { i += 2; break; }
        else if (c >= 63 && c <= 126) { put(c, 1); i++; }
        else i++;
    }
    d.ok = true;
    if (end) *end = i;
    return d;
}

int dist(const Pixel& a, const Pixel& b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

// ---- a pretend terminal that understands what a Display sends, Sixel included ----
//
// Letters go into cells (a letter written into a cell wipes the pixels in it, as in Windows Terminal); a Sixel picture is
// drawn at the cursor's cell and leaves alone the pixels it does not draw; clearing the screen clears pixels too.
struct Model {
    int rows = 0, cols = 0, cw = 0, ch = 0;
    std::vector<char32_t> letter;              // rows * cols
    std::vector<unsigned char> wide_head;      // rows * cols: the cell is the first half of a wide letter
    std::vector<Pixel> px;                     // (cols * cw) x (rows * ch)
    int cr = 0, cc = 0;
    bool cursor_known = true;
    bool broken = false;                       // something the Display should never send
    long sixels = 0;                           // pictures received

    void init(int r, int c, int cell_w, int cell_h) {
        rows = r; cols = c; cw = cell_w; ch = cell_h;
        letter.assign((size_t)r * c, U' ');
        wide_head.assign((size_t)r * c, 0);
        px.assign((size_t)r * ch * c * cw, Pixel());
        cr = cc = 0; cursor_known = true; broken = false; sixels = 0;
    }
    int W() const { return cols * cw; }

    void wipe(int i, int j) {
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) px[(size_t)(i * ch + y) * W() + (size_t)j * cw + x] = Pixel();
    }

    void feed(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = (unsigned char)s[i];
            if (c == 0x1b) {
                if (i + 1 >= s.size()) { broken = true; return; }
                if (s[i + 1] == 'P') {                                       // a Sixel picture
                    size_t end = 0;
                    Decoded d = decode_sixel(s, i, &end);
                    if (!d.ok || !cursor_known) { broken = true; return; }          // a picture always follows a cursor move
                    sixels++;
                    for (int y = 0; y < d.h; y++)
                        for (int x = 0; x < d.w; x++) {
                            const Pixel& p = d.px[(size_t)y * d.w + x];
                            if (p.a == 0) continue;
                            const int X = cc * cw + x, Y = cr * ch + y;
                            if (X < 0 || Y < 0 || X >= W() || Y >= rows * ch) continue;
                            px[(size_t)Y * W() + X] = p;
                        }
                    cursor_known = false;                                    // where a picture leaves the cursor is not trusted
                    i = end;
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
                    if (params == "2") { std::fill(letter.begin(), letter.end(), U' '); std::fill(px.begin(), px.end(), Pixel()); std::fill(wide_head.begin(), wide_head.end(), (unsigned char)0); }
                    else broken = true;
                } else if (f == 'C') {
                    if (!cursor_known) broken = true;
                    cc += params.empty() ? 1 : std::atoi(params.c_str());
                } else if (f == 'm') {
                    // colours and looks do not matter here
                } else broken = true;
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
            if (!cursor_known || cr < 0 || cr >= rows || cc < 0 || cc >= cols) { broken = true; continue; }
            const bool wide = cp >= 0x4E00 && cp <= 0x9FFF;                  // the Chinese letters used in the tests
            if (cc > 0 && wide_head[(size_t)cr * cols + cc - 1]) { broken = true; continue; }      // half of a wide letter written over
            wide_head[(size_t)cr * cols + cc] = wide;
            if (cc + 1 < cols && wide_head[(size_t)cr * cols + cc + 1] && !wide) wide_head[(size_t)cr * cols + cc + 1] = wide_head[(size_t)cr * cols + cc + 1];
            letter[(size_t)cr * cols + cc] = cp;
            wipe(cr, cc);
            if (wide && cc + 1 < cols) { letter[(size_t)cr * cols + cc + 1] = U' '; wipe(cr, cc + 1); cc++; }
            cc++;
        }
    }
};

// ---- pictures ----

const int CW = 6, CH = 12;                     // a small character cell, so tests run fast

PicCell plain_cell(std::mt19937& rng) {
    PicCell c;
    c.lum = (double)(rng() % 1001) / 1000.0;
    c.r = (double)(rng() % 256); c.g = (double)(rng() % 256); c.b = (double)(rng() % 256);
    c.shimmer = rng() % 4 == 0 ? 1.0 : 0.0;
    return c;
}

// The pixels the pictures on pic ask for, put together, as the Display should end up showing them.
std::vector<Pixel> wanted(const Picture& pic, int cols) {
    const int rows = pic.rows;
    const int W = cols * CW;
    std::vector<Pixel> w((size_t)W * rows * CH, Pixel());
    for (const ImagePlacement& p : pic.images) {
        if (!p.raster) continue;
        for (int y = 0; y < p.raster->h; y++)
            for (int x = 0; x < p.raster->w; x++) {
                const int X = p.col * CW + x, Y = p.row * CH + y;
                if (X < 0 || Y < 0 || X >= W || Y >= (rows - 1) * CH) continue;     // not off the screen, not in the bar's row
                const Pixel& q = p.raster->at(x, y);
                if (q.a != 0) w[(size_t)Y * W + X] = Pixel{q.r, q.g, q.b, 255};
            }
    }
    return w;
}

// How many pixels are drawn where they should not be, or not drawn where they should (never allowed), and how many
// are drawn in a colour further than `tolerance` from the one asked for.
struct Mismatch { long shape = 0, colour = 0; long total() const { return shape + colour; } };
Mismatch compare_pixels(const Model& m, const std::vector<Pixel>& want, int tolerance) {
    Mismatch bad;
    for (size_t k = 0; k < want.size(); k++) {
        const bool drawn_want = want[k].a != 0, drawn_have = m.px[k].a != 0;
        if (drawn_want != drawn_have) bad.shape++;
        else if (drawn_want && dist(want[k], m.px[k]) > tolerance) bad.colour++;
    }
    return bad;
}
long pixel_mismatches(const Model& m, const std::vector<Pixel>& want, int tolerance) { return compare_pixels(m, want, tolerance).total(); }

std::shared_ptr<Raster> make_raster(std::mt19937& rng, int w, int h, int colours) {
    auto r = std::make_shared<Raster>();
    r->init(w, h);
    const int style = (int)(rng() % 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool on = false;
            switch (style) {
                case 0: on = true; break;                                           // a solid block
                case 1: { const double dx = x - w / 2.0, dy = y - h / 2.0; on = dx * dx + dy * dy <= (std::min(w, h) / 2.0) * (std::min(w, h) / 2.0); break; }   // a disc
                case 2: on = (x + y) % 5 == 0 || x % 7 == 0; break;                 // thin lines
                default: on = rng() % 3 != 0; break;                                // noise with holes
            }
            if (!on) continue;
            if (colours > 255) r->set(x, y, (int)(rng() % 256), (int)(rng() % 256), (int)(rng() % 256));
            else { const int k = (int)(rng() % (unsigned)colours); r->set(x, y, 30 + k * 9 % 200, 200 - k * 13 % 180, 10 + k * 37 % 240); }
        }
    return r;
}

}  // namespace

int main() {
    // ---- the encoder ----
    {
        // Exact colours, holes, and every size of picture round-trip.
        std::mt19937 rng(1);
        for (int trial = 0; trial < 300; trial++) {
            const int w = 1 + (int)(rng() % 40), h = 1 + (int)(rng() % 40);
            std::vector<Pixel> px((size_t)w * h);
            const int colours = 1 + (int)(rng() % 12);
            for (Pixel& p : px) {
                if (rng() % 4 == 0) { p = Pixel(); continue; }                     // a hole
                const int k = (int)(rng() % (unsigned)colours);
                p = Pixel{(uint8_t)(k * 20), (uint8_t)(255 - k * 20), (uint8_t)(k * 7), 255};
            }
            const std::string s = detail::sixel_encode(px.data(), w, w, h);
            Decoded d = decode_sixel(s, 0);
            CHECK(d.ok && d.w == w && d.h == h);
            int bad = 0;
            for (size_t k = 0; k < px.size() && d.ok; k++) {
                if ((px[k].a != 0) != (d.px[k].a != 0)) bad++;
                else if (px[k].a != 0 && dist(px[k], d.px[k]) > 3) bad++;          // Sixel counts colours in percent
            }
            CHECK(bad == 0);
        }
        // A block of a bigger picture (its rows are further apart than it is wide).
        Raster big;
        big.init(50, 30);
        for (int y = 0; y < 30; y++) for (int x = 0; x < 50; x++) big.set(x, y, x * 5, y * 8, 100);
        const std::string s = detail::sixel_encode(&big.at(10, 7), big.w, 12, 9);
        Decoded d = decode_sixel(s, 0);
        CHECK(d.ok && d.w == 12 && d.h == 9);
        int bad = 0;
        for (int y = 0; y < 9 && d.ok; y++) for (int x = 0; x < 12; x++) if (dist(d.px[(size_t)y * 12 + x], big.at(10 + x, 7 + y)) > 3) bad++;
        CHECK(bad == 0);
        // Nothing to draw, and nothing at all.
        std::vector<Pixel> holes(20, Pixel());
        const std::string empty = detail::sixel_encode(holes.data(), 5, 5, 4);
        CHECK(empty.size() < 40 && decode_sixel(empty, 0).ok);
        CHECK(detail::sixel_encode(holes.data(), 5, 0, 0).size() < 30);
        // Long runs are counted, not written out: a 600 pixel line is a few bytes.
        std::vector<Pixel> line(600 * 6, Pixel());
        for (int x = 0; x < 600; x++) line[(size_t)2 * 600 + x] = Pixel{255, 0, 0, 255};
        CHECK(detail::sixel_encode(line.data(), 600, 600, 6).size() < 120);
        // And so is the empty stretch before a colour's first pixel.
        std::vector<Pixel> dot(600 * 6, Pixel());
        dot[(size_t)2 * 600 + 400] = Pixel{255, 0, 0, 255};
        CHECK(detail::sixel_encode(dot.data(), 600, 600, 6).size() < 120);
        // More than 255 colours: the most used are kept, so a big plain area stays right among a few hundred stray colours.
        Raster many;
        many.init(60, 40);
        for (int y = 0; y < 40; y++) for (int x = 0; x < 60; x++) many.set(x, y, 50, 100, 150);
        std::mt19937 noise(5);
        for (int k = 0; k < 400; k++) many.set((int)(noise() % 60), (int)(noise() % 40), (int)(noise() % 256), (int)(noise() % 256), (int)(noise() % 256));
        Decoded m = decode_sixel(detail::sixel_encode(many.px.data(), 60, 60, 40), 0);
        CHECK(m.ok);
        int plain = 0, plain_ok = 0, worst = 0;
        for (size_t k = 0; k < many.px.size() && m.ok; k++) {
            CHECK(m.px[k].a != 0);                                             // every pixel is drawn
            worst = std::max(worst, dist(many.px[k], m.px[k]));
            if (many.px[k] == Pixel{50, 100, 150, 255}) { plain++; plain_ok += dist(many.px[k], m.px[k]) <= 4; }
        }
        CHECK(plain > 1500 && plain_ok == plain);
        // A smooth ramp of more than 255 colours stays close (the ones not kept take their nearest neighbour).
        Raster ramp;
        ramp.init(100, 10);
        for (int y = 0; y < 10; y++) for (int x = 0; x < 100; x++) ramp.set(x, y, x * 2 + y % 3, 40 + x, 200 - x * 2);
        Decoded rd = decode_sixel(detail::sixel_encode(ramp.px.data(), 100, 100, 10), 0);
        int ramp_worst = 0;
        for (size_t k = 0; k < ramp.px.size() && rd.ok; k++) ramp_worst = std::max(ramp_worst, dist(ramp.px[k], rd.px[k]));
        CHECK(rd.ok && ramp_worst <= 16);
    }

    // ---- a Picture carries pictures along ----
    {
        Picture p;
        p.init(10, 20);
        auto r = std::make_shared<Raster>();
        r->init(5, 5);
        p.place_image(r, 2, 3);
        p.place_image(nullptr, 0, 0);                                    // nothing: ignored
        CHECK(p.images.size() == 1);
        Picture q(p);
        CHECK(q.images.size() == 1 && q.images[0].raster == r && q.images[0].row == 2 && q.images[0].col == 3);
        Picture m(std::move(q));
        CHECK(m.images.size() == 1);
        Picture a;
        a = p;
        CHECK(a.images.size() == 1);
        a.clear_images();
        CHECK(a.images.empty() && p.images.size() == 1);
        // init() starts a picture over, pictures included.
        Picture fresh;
        fresh.init(5, 10);
        fresh.place_image(r, 1, 1);
        fresh.init(5, 10);
        CHECK(fresh.images.empty());
        // A fade dissolves the pictures: 0 is a's, 1 is b's, in between each is thinned by the share it is not ahead by.
        {
            auto solid = [](int w, int h, int r, int g, int b) {
                auto ras = std::make_shared<Raster>();
                ras->init(w, h);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) ras->set(x, y, r, g, b);
                return ras;
            };
            auto count = [](const ImagePlacement& ip) { long n = 0; for (const Pixel& px : ip.raster->px) n += px.a != 0; return n; };
            auto ra = solid(40, 30, 200, 0, 0), rb = solid(40, 30, 0, 0, 200);
            Picture x, y, out;
            x.init(10, 20); y.init(10, 20);
            x.place_image(ra, 1, 1);
            y.place_image(rb, 2, 2);
            fade(x, y, 0.0, out);
            CHECK(out.images.size() == 1 && out.images[0].raster == ra && out.images[0].row == 1 && out.images[0].col == 1);   // a itself
            fade(x, y, 1.0, out);
            CHECK(out.images.size() == 1 && out.images[0].raster == rb && out.images[0].row == 2 && out.images[0].col == 2);   // b itself
            fade(x, y, 0.5, out);
            CHECK(out.images.size() == 2);                                   // both are there, thinned
            if (out.images.size() == 2) {
                CHECK(count(out.images[0]) > 540 && count(out.images[0]) < 660);        // about half of 1200 each
                CHECK(count(out.images[1]) > 540 && count(out.images[1]) < 660);
                CHECK(out.images[0].row == 1 && out.images[1].row == 2);                  // a under b
            }
            fade(x, y, 0.25, out);
            if (out.images.size() == 2) CHECK(count(out.images[0]) > 810 && count(out.images[0]) < 990 && count(out.images[1]) > 210 && count(out.images[1]) < 390);
            // What is kept only grows for the picture coming in and only shrinks for the one going out, pixel by pixel; colours are kept.
            Picture lo, hi;
            fade(x, y, 0.3, lo);
            fade(x, y, 0.6, hi);
            if (lo.images.size() == 2 && hi.images.size() == 2) {
                int bad = 0;
                for (size_t k = 0; k < ra->px.size(); k++) {
                    if (hi.images[0].raster->px[k].a != 0 && lo.images[0].raster->px[k].a == 0) bad++;      // a: what is there later was there before
                    if (lo.images[1].raster->px[k].a != 0 && hi.images[1].raster->px[k].a == 0) bad++;      // b: what was there before is still there
                    if (lo.images[0].raster->px[k].a != 0 && lo.images[0].raster->px[k] != ra->px[k]) bad++;
                    if (hi.images[1].raster->px[k].a != 0 && hi.images[1].raster->px[k] != rb->px[k]) bad++;
                }
                CHECK(bad == 0);
            }
            // The two dissolve in orders of their own: over the same place at the halfway point they cover about three quarters of it
            // together (half and half, overlapping at random), not just the same half twice.
            {
                Picture p1, p2, mid;
                p1.init(10, 20); p2.init(10, 20);
                p1.place_image(ra, 1, 1);
                p2.place_image(rb, 1, 1);
                fade(p1, p2, 0.5, mid);
                long both = 0;
                if (mid.images.size() == 2)
                    for (size_t k = 0; k < ra->px.size(); k++) both += (mid.images[0].raster->px[k].a != 0 || mid.images[1].raster->px[k].a != 0);
                CHECK(both > 780 && both < 1020);                                  // of 1200: about 900
            }
            for (const Pixel& px : ra->px) CHECK(px.a != 0);                // the originals are not touched
            for (const Pixel& px : rb->px) CHECK(px.a != 0);
            Picture again;
            fade(x, y, 0.3, again);
            CHECK(again.images.size() == 2 && again.images[0].raster->px == lo.images[0].raster->px && again.images[1].raster->px == lo.images[1].raster->px);   // the same every time
            // Only one of them has pictures; and pictures with nothing in them.
            Picture none;
            none.init(10, 20);
            fade(x, none, 0.5, out);
            CHECK(out.images.size() == 1 && count(out.images[0]) > 540 && count(out.images[0]) < 660);
            fade(none, y, 0.5, out);
            CHECK(out.images.size() == 1 && out.images[0].raster != rb);
            fade(none, none, 0.5, out);
            CHECK(out.images.empty());
            Picture empty_raster;
            empty_raster.init(10, 20);
            empty_raster.place_image(std::make_shared<Raster>(), 1, 1);
            fade(empty_raster, none, 0.5, out);
            CHECK(out.images.size() == 1 && out.images[0].raster->px.empty());
            // A fade of a fade (the show stacks them): weights multiply, and out may be a picture used before.
            Picture z;
            z.init(10, 20);
            z.place_image(solid(40, 30, 0, 200, 0), 3, 3);
            Picture first, second;
            fade(x, y, 0.5, first);
            fade(first, z, 0.5, second);
            long total = 0;
            for (const ImagePlacement& ip : second.images) total += count(ip);
            CHECK(second.images.size() == 3 && total > 1080 && total < 1320);   // 600 * 0.5 + 600 * 0.5 + 1200 * 0.5 = 1200, give or take
            // The shares really multiply: what the first fade kept is thinned again by the second (a second pass in the same
            // order would keep all of it).
            if (second.images.size() == 3 && first.images.size() == 2) {
                CHECK(count(second.images[0]) < count(first.images[0]) * 6 / 10 && count(second.images[0]) > count(first.images[0]) * 4 / 10);
                CHECK(count(second.images[1]) < count(first.images[1]) * 6 / 10 && count(second.images[1]) > count(first.images[1]) * 4 / 10);
            }
            CHECK(first.fade_level == 1 && second.fade_level == 2 && z.fade_level == 0 && x.fade_level == 0);
            Picture copy = second;
            CHECK(copy.fade_level == 2);
            copy.init(10, 20);
            CHECK(copy.fade_level == 0);
            // The percentage is held between 0 and 1.
            fade(x, y, -3.0, out);
            CHECK(out.images.size() == 1 && out.images[0].raster == ra);
            fade(x, y, 9.0, out);
            CHECK(out.images.size() == 1 && out.images[0].raster == rb);
        }
        // Raster basics: out of range is ignored; init of a nonsense size is empty.
        Raster z;
        z.init(-3, 4);
        CHECK(z.w == 0 && z.px.empty());
        z.init(3, 3);
        z.set(-1, 0, 1, 2, 3); z.set(3, 0, 1, 2, 3); z.set(0, 5, 1, 2, 3);
        z.set(1, 1, 9, 8, 7);
        CHECK(z.at(1, 1) == (Pixel{9, 8, 7, 255}));
        z.clear();
        CHECK(z.at(1, 1).a == 0);
    }

    // ---- pictures are ignored until the Display is told: the output is what it always was ----
    {
        std::mt19937 rng(7);
        const int R = 14, C = 40;
        Display with, without;
        with.init(R, C); without.init(R, C);                             // enable_images is never called on `with`
        CHECK(!with.images_enabled());
        auto raster = make_raster(rng, 30, 40, 5);
        long sent = 0;
        for (int frame = 0; frame < 60; frame++) {
            Picture a, b;
            a.init(R, C); b.init(R, C);
            for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { const PicCell c = plain_cell(rng); a.set(i, j, c); b.set(i, j, c); }
            b.place_image(raster, (int)(rng() % 10), (int)(rng() % 30));
            const std::string x = with.render(b, frame * 0.1), y = without.render(a, frame * 0.1);
            CHECK(x == y);
            sent += (long)x.size();
        }
        CHECK(sent > 1000);
        // Told 0 (or a nonsense size), pictures stay off.
        with.enable_images(0, 12);
        CHECK(!with.images_enabled());
        with.enable_images(6, -1);
        CHECK(!with.images_enabled());
        with.enable_images(6, 12);
        CHECK(with.images_enabled());
        with.enable_images(0, 0);
        CHECK(!with.images_enabled());
    }

    // ---- specific behaviour, on a pretend terminal ----
    {
        const int R = 10, C = 30;
        auto letters = [&](Picture& p, char ch, double shimmer) {
            p.init(R, C);
            for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c; c.text = true; c.ch = (char32_t)ch; c.r = c.g = c.b = 200; (void)shimmer; p.set(i, j, c); }
        };
        auto box = [&](int w, int h, int r, int g, int b) {
            auto ras = std::make_shared<Raster>();
            ras->init(w, h);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) ras->set(x, y, r, g, b);
            return ras;
        };
        auto count_sixels = [](const std::string& s) { size_t n = 0, at = 0; while ((at = s.find("\x1bP0;1;0q", at)) != std::string::npos) { n++; at++; } return (int)n; };

        Display d;
        d.init(R, C);
        d.enable_images(CW, CH);
        Model m;
        m.init(R + 1, C, CW, CH);
        Picture p;
        letters(p, 'x', 0);
        auto red = box(3 * CW, 2 * CH, 255, 0, 0);                        // 3 cells wide, 2 high
        p.place_image(red, 2, 4);
        std::string s1 = d.render(p, 0);
        m.feed(s1);
        CHECK(!m.broken);
        CHECK(pixel_mismatches(m, wanted(p, C), 3) == 0);
        CHECK(count_sixels(s1) == 2);                                      // one picture per row of cells: the three cells of a row go together

        // Nothing changed: nothing is sent.
        CHECK(d.render(p, 1).empty());
        CHECK(d.render(p, 2).empty());

        // A letter changes under the picture: that cell is written and its picture sent again; the rest is left alone.
        PicCell c; c.text = true; c.ch = U'Q'; c.r = c.g = c.b = 200;
        p.set(2, 5, c);                                                    // inside the red block
        std::string s2 = d.render(p, 3);
        m.feed(s2);
        CHECK(!m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);
        CHECK(m.letter[(size_t)2 * C + 5] == U'Q');
        CHECK(count_sixels(s2) == 1);
        { Decoded one = decode_sixel(s2, s2.find("\x1bP")); CHECK(one.w == CW && one.h == CH); }    // one cell, not the whole block

        // A letter and the picture over it both change: the letter is written once, not twice, and the picture goes after it.
        PicCell c3 = c; c3.ch = U'W';
        p.set(3, 5, c3);
        auto red3 = std::make_shared<Raster>(*red);
        red3->set(CW + 2, CH + 2, 10, 20, 30);                             // a pixel in the cell (3, 5), whose letter changed
        Picture pw = p;
        pw.clear_images();
        pw.place_image(red3, 2, 4);
        std::string sw = d.render(pw, 3.5);
        m.feed(sw);
        CHECK(!m.broken && pixel_mismatches(m, wanted(pw, C), 3) == 0);
        { size_t n = 0, at = 0; while ((at = sw.find('W', at)) != std::string::npos) { n++; at++; } CHECK(n == 1); }
        p = pw;
        p.clear_images();
        p.place_image(red, 2, 4);
        m.feed(d.render(p, 3.6));
        CHECK(!m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);

        // The letter changes next to the picture: nothing about the picture is sent.
        PicCell c2 = c; c2.ch = U'Z';
        p.set(2, 8, c2);
        std::string s3 = d.render(p, 4);
        m.feed(s3);
        CHECK(count_sixels(s3) == 0 && !m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);

        // The picture changes in one cell: only that cell is sent.
        auto red2 = std::make_shared<Raster>(*red);
        for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) red2->set(CW + x, CH + y, 0, 255, 0);    // the middle cell of the lower row
        Picture p2 = p;
        p2.clear_images();
        p2.place_image(red2, 2, 4);
        std::string s4 = d.render(p2, 5);
        m.feed(s4);
        CHECK(!m.broken && pixel_mismatches(m, wanted(p2, C), 3) == 0);
        CHECK(count_sixels(s4) == 1);
        { Decoded one = decode_sixel(s4, s4.find("\x1bP")); CHECK(one.w == CW && one.h == CH); }

        // The picture moves: the cells it leaves get their letters back (which clears the pixels) and the new ones are drawn.
        Picture p3 = p2;
        p3.clear_images();
        p3.place_image(red2, 5, 12);
        std::string s5 = d.render(p3, 6);
        m.feed(s5);
        CHECK(!m.broken && pixel_mismatches(m, wanted(p3, C), 3) == 0);
        for (int i = 2; i < 4; i++) for (int j = 4; j < 7; j++) CHECK(m.letter[(size_t)i * C + j] == (i == 2 && j == 5 ? U'Q' : i == 3 && j == 5 ? U'W' : U'x'));

        // The picture goes away: every pixel is cleared and the letters are all there.
        Picture p4 = p3;
        p4.clear_images();
        std::string s6 = d.render(p4, 7);
        m.feed(s6);
        CHECK(!m.broken && pixel_mismatches(m, wanted(p4, C), 3) == 0 && count_sixels(s6) == 0);
        CHECK(d.render(p4, 8).empty());                                    // and then it is quiet again

        // The status bar's row is never drawn in, and a picture that hangs off the screen is cut.
        Picture p5;
        letters(p5, 'x', 0);
        auto tall = box(4 * CW, 20 * CH, 0, 0, 255);
        p5.place_image(tall, 7, 27);
        p5.place_image(box(2 * CW, 2 * CH, 255, 255, 0), -1, -1);         // half off the top left
        std::string s7 = d.render(p5, 9);
        m.feed(s7);
        CHECK(!m.broken && pixel_mismatches(m, wanted(p5, C), 3) == 0);
        for (int x = 0; x < C * CW; x++) CHECK(m.px[(size_t)(R * CH + 3) * m.W() + x].a == 0);    // the bar's row (the last one) has no pixels
        // One that is wholly outside draws nothing and does no harm.
        Picture p6;
        letters(p6, 'x', 0);
        p6.place_image(box(CW, CH, 9, 9, 9), 100, 100);
        p6.place_image(box(CW, CH, 9, 9, 9), -50, -50);
        m.feed(d.render(p6, 10));
        CHECK(!m.broken && pixel_mismatches(m, wanted(p6, C), 3) == 0);
        // An empty raster, and a null one.
        Picture p7;
        letters(p7, 'x', 0);
        auto none = std::make_shared<Raster>();
        p7.place_image(none, 1, 1);
        p7.images.push_back({nullptr, 1, 1});
        m.feed(d.render(p7, 11));
        CHECK(!m.broken && pixel_mismatches(m, wanted(p7, C), 3) == 0);
    }
    {
        // Holding letters under a picture: they do not change while the picture is on them, so nothing is wiped and nothing is
        // sent again; text cells still change; and the letters catch up when the picture goes.
        const int R = 8, C = 20;
        auto letters_of = [&](char ch) {
            Picture p;
            p.init(R, C);
            for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c; c.lum = 0.5; c.shimmer = 1.0; c.r = c.g = c.b = 150; c.text = false; (void)ch; p.set(i, j, c); }
            return p;
        };
        auto line = std::make_shared<Raster>();                           // a thin line across 4 cells of one row
        line->init(4 * CW, CH);
        for (int x = 0; x < line->w; x++) line->set(x, 5, 255, 255, 0);
        auto count_sixels = [](const std::string& s) { size_t n = 0, at = 0; while ((at = s.find("\x1bP0;1;0q", at)) != std::string::npos) { n++; at++; } return (int)n; };

        for (int hold = 0; hold < 2; hold++) {
            Display d;
            d.init(R, C);
            d.enable_images(CW, CH);
            Model m;
            m.init(R + 1, C, CW, CH);
            Picture p = letters_of('a');
            p.place_image(line, 3, 5, hold != 0 ? 0.0 : -1.0);
            m.feed(d.render(p, 0.0));                                       // the first frame draws letters and pixels
            CHECK(!m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);
            // The letters under the picture change (a different brightness gives a different letter): with hold, nothing is sent.
            Picture q = letters_of('b');
            for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c = q.at(i, j); c.lum = 0.9; q.set(i, j, c); }
            q.place_image(line, 3, 5, hold != 0 ? 0.0 : -1.0);
            std::string s = d.render(q, 1.0);
            m.feed(s);
            CHECK(!m.broken && pixel_mismatches(m, wanted(q, C), 3) == 0);
            Display plain;                                                  // what the letters should be, with no picture
            plain.init(R, C);
            Model mp;
            mp.init(R + 1, C, CW, CH);
            Picture q0 = letters_of('b');
            for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c = q0.at(i, j); c.lum = 0.9; q0.set(i, j, c); }
            mp.feed(plain.render(letters_of('a'), 0.0));
            mp.feed(plain.render(q0, 1.0));
            int held = 0, differ = 0;
            for (int i = 0; i < R; i++)
                for (int j = 0; j < C; j++) {
                    const bool under = i == 3 && j >= 5 && j < 9;
                    if (m.letter[(size_t)i * C + j] != mp.letter[(size_t)i * C + j]) { differ++; held += under; }
                }
            if (hold) {
                CHECK(count_sixels(s) == 0);                                // nothing about the picture was sent
                CHECK(differ == 4 && held == 4);                           // exactly the 4 cells under the line kept their letter
            } else {
                CHECK(count_sixels(s) > 0 && differ == 0);                 // without hold they changed, and the line went on again
            }
            // The picture goes: the letters catch up at once, the pixels are gone.
            Picture r = q0;
            m.feed(d.render(r, 2.0));
            mp.feed(plain.render(r, 2.0));
            bool same = true;
            for (size_t k = 0; k < m.letter.size(); k++) if (m.letter[k] != mp.letter[k]) same = false;
            CHECK(same && !m.broken && pixel_mismatches(m, wanted(r, C), 3) == 0);
        }
        // The picture stays but stops holding: the letters catch up (even though nothing else about the frame changed).
        {
            Display d;
            d.init(R, C);
            d.enable_images(CW, CH);
            Model m;
            m.init(R + 1, C, CW, CH);
            Picture a = letters_of('a');
            a.place_image(line, 3, 5, 0.0);
            m.feed(d.render(a, 0.0));
            Picture b = letters_of('b');
            for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c = b.at(i, j); c.lum = 0.9; b.set(i, j, c); }
            Picture b_hold = b, b_free = b;
            b_hold.place_image(line, 3, 5, 0.0);
            b_free.place_image(line, 3, 5, -1.0);
            m.feed(d.render(b_hold, 1.0));                                  // the letters under the line hold
            m.feed(d.render(b_hold, 2.0));                                  // still holding, nothing else changes
            Display plain;
            plain.init(R, C);
            Model mp;
            mp.init(R + 1, C, CW, CH);
            mp.feed(plain.render(letters_of('a'), 0.0));
            mp.feed(plain.render(b, 1.0));
            mp.feed(plain.render(b, 2.0));
            int differ = 0;
            for (size_t k = 0; k < m.letter.size(); k++) differ += m.letter[k] != mp.letter[k];
            CHECK(differ == 4);
            m.feed(d.render(b_free, 3.0));                                  // no longer holding: the same letters as before, nothing new asked of the letters
            mp.feed(plain.render(b, 3.0));
            bool same = true;
            for (size_t k = 0; k < m.letter.size(); k++) if (m.letter[k] != mp.letter[k]) same = false;
            CHECK(same && !m.broken && pixel_mismatches(m, wanted(b_free, C), 3) == 0);
        }
        // A rate: a cell under the picture may change that many times a second, each cell at a moment of its own.
        for (double rate : {-1.0, 0.0, 3.0, 10.0}) {
            Display d;
            d.init(R, C);
            d.enable_images(CW, CH);
            Model m;
            m.init(R + 1, C, CW, CH);
            std::vector<int> changes(4, 0);
            std::vector<char32_t> last(4, U' ');
            int frames_some = 0;                                            // frames in which some of the four cells changed but not all
            const int frames = 90;                                          // 3 seconds at 30 a second
            for (int f = 0; f < frames; f++) {
                Picture p = letters_of('a');
                for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c = p.at(i, j); c.lum = (double)((f + i * 3 + j) % 14) / 14.0; p.set(i, j, c); }   // every letter changes every frame
                p.place_image(line, 3, 5, rate);
                m.feed(d.render(p, f / 30.0));
                CHECK(!m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);
                int now = 0;
                for (int k = 0; k < 4; k++) {
                    const char32_t ch = m.letter[(size_t)3 * C + 5 + (size_t)k];
                    if (f > 0 && ch != last[(size_t)k]) { changes[(size_t)k]++; now++; }
                    last[(size_t)k] = ch;
                }
                if (now > 0 && now < 4) frames_some++;
            }
            for (int k = 0; k < 4; k++) {
                if (rate < 0) CHECK(changes[(size_t)k] > 70);                   // as often as the scene says
                else if (rate == 0) CHECK(changes[(size_t)k] == 0);             // never
                else CHECK(changes[(size_t)k] >= (int)(rate * 3) - 3 && changes[(size_t)k] <= (int)(rate * 3) + 3);
            }
            if (rate == 3.0 || rate == 10.0) CHECK(frames_some > 0);            // not all in the same frame
        }
        // Two pictures on the same cells: the more generous rate is the one that counts.
        {
            Display d;
            d.init(R, C);
            d.enable_images(CW, CH);
            Model m;
            m.init(R + 1, C, CW, CH);
            int changes = 0;
            char32_t last = U' ';
            for (int f = 0; f < 90; f++) {
                Picture p = letters_of('a');
                for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { PicCell c = p.at(i, j); c.lum = (double)((f + i * 3 + j) % 14) / 14.0; p.set(i, j, c); }
                p.place_image(line, 3, 5, 1.0);
                p.place_image(line, 3, 5, 15.0);
                m.feed(d.render(p, f / 30.0));
                const char32_t ch = m.letter[(size_t)3 * C + 5];
                if (f > 0 && ch != last) changes++;
                last = ch;
            }
            CHECK(changes >= 35 && changes <= 50);                          // about 15 a second for 3 seconds, not about 3
        }
        // A text cell under a holding picture is not held; a cell the picture has not drawn yet is not held either.
        {
            Display d;
            d.init(R, C);
            d.enable_images(CW, CH);
            Model m;
            m.init(R + 1, C, CW, CH);
            Picture p = letters_of('a');
            PicCell t; t.text = true; t.ch = U'A'; t.r = t.g = t.b = 220;
            p.set(3, 6, t);
            p.place_image(line, 3, 5, 0.0);
            m.feed(d.render(p, 0.0));
            CHECK(!m.broken && m.letter[(size_t)3 * C + 6] == U'A');       // drawn the first time, though a picture is over it
            PicCell t2 = t; t2.ch = U'B';
            Picture q = p;
            q.set(3, 6, t2);
            std::string s = d.render(q, 1.0);
            m.feed(s);
            CHECK(!m.broken && m.letter[(size_t)3 * C + 6] == U'B');       // text changes under the picture still show
            CHECK(pixel_mismatches(m, wanted(q, C), 3) == 0);
        }
    }
    {
        // Pictures on top of each other: the later one is above. Holes show what is below, a picture or letters.
        const int R = 8, C = 20;
        Display d;
        d.init(R, C);
        d.enable_images(CW, CH);
        Model m;
        m.init(R + 1, C, CW, CH);
        Picture p;
        p.init(R, C);
        for (int j = 0; j < C; j++) { PicCell c; c.text = true; c.ch = U'o'; c.r = c.g = c.b = 100; p.set(1, j, c); }
        auto under = std::make_shared<Raster>(), over = std::make_shared<Raster>();
        under->init(4 * CW, 2 * CH); over->init(4 * CW, 2 * CH);
        for (int y = 0; y < under->h; y++) for (int x = 0; x < under->w; x++) under->set(x, y, 255, 0, 0);
        for (int y = 0; y < over->h; y++) for (int x = 0; x < over->w; x++) if ((x / 3 + y / 3) % 2 == 0) over->set(x, y, 0, 0, 255);
        p.place_image(under, 1, 2);
        p.place_image(over, 1, 3);                                         // half over the first
        m.feed(d.render(p, 0));
        CHECK(!m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);
        // The other order.
        Picture q = p;
        q.clear_images();
        q.place_image(over, 1, 3);
        q.place_image(under, 1, 2);
        m.feed(d.render(q, 1));
        CHECK(!m.broken && pixel_mismatches(m, wanted(q, C), 3) == 0);
    }
    {
        // A wide letter under a picture: writing it clears the pixels of both its cells.
        const int R = 6, C = 20;
        Display d;
        d.init(R, C);
        d.enable_images(CW, CH);
        Model m;
        m.init(R + 1, C, CW, CH);
        auto build = [&](char32_t ch) {
            Picture p;
            p.init(R, C);
            PicCell head, tail;
            head.text = true; head.ch = ch; head.attrs = CELL_WIDE; head.r = head.g = head.b = 200;
            tail.text = true; tail.ch = U' '; tail.attrs = CELL_TAIL; tail.r = tail.g = tail.b = 200;
            p.set(2, 5, head); p.set(2, 6, tail);
            return p;
        };
        auto block = std::make_shared<Raster>();
        block->init(5 * CW, 3 * CH);
        for (int y = 0; y < block->h; y++) for (int x = 0; x < block->w; x++) block->set(x, y, 10, 220, 10);
        Picture p = build(U'你');
        p.place_image(block, 1, 3);
        m.feed(d.render(p, 0));
        CHECK(!m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);
        Picture q = build(U'好');                                      // another wide letter: both its cells are written
        q.place_image(block, 1, 3);
        std::string s = d.render(q, 1);
        m.feed(s);
        CHECK(!m.broken && pixel_mismatches(m, wanted(q, C), 3) == 0);
        CHECK(m.letter[(size_t)2 * C + 5] == U'好');
        // Only the second half changes (the first half does not): the first half draws both, and the pixels come back.
        Picture r = build(U'好');
        PicCell tail2 = r.at(2, 6);
        tail2.r = tail2.g = tail2.b = 150;
        r.set(2, 6, tail2);
        r.place_image(block, 1, 3);
        m.feed(d.render(r, 2));
        CHECK(!m.broken && pixel_mismatches(m, wanted(r, C), 3) == 0);
    }
    {
        // The picture covers only the second half of a wide letter: it can only be taken off by writing the first half (which
        // writes both), never by writing the second half alone.
        const int R = 6, C = 20;
        Display d;
        d.init(R, C);
        d.enable_images(CW, CH);
        Model m;
        m.init(R + 1, C, CW, CH);
        Picture p;
        p.init(R, C);
        PicCell head, tail;
        head.text = true; head.ch = U'\u4F60'; head.attrs = CELL_WIDE; head.r = head.g = head.b = 200;
        tail.text = true; tail.ch = U' '; tail.attrs = CELL_TAIL; tail.r = tail.g = tail.b = 200;
        p.set(2, 5, head); p.set(2, 6, tail);
        auto one = std::make_shared<Raster>();
        one->init(CW, CH);
        for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) one->set(x, y, 255, 100, 0);
        Picture q = p;
        q.place_image(one, 2, 6);                                          // over the tail only
        m.feed(d.render(q, 0));
        CHECK(!m.broken && pixel_mismatches(m, wanted(q, C), 3) == 0);
        m.feed(d.render(p, 1));                                            // taken away
        CHECK(!m.broken && pixel_mismatches(m, wanted(p, C), 3) == 0);
        q = p;
        q.place_image(one, 2, 6);
        m.feed(d.render(q, 2));
        CHECK(!m.broken && pixel_mismatches(m, wanted(q, C), 3) == 0);
        Picture q2 = p;
        auto two = std::make_shared<Raster>(*one);
        two->set(1, 1, 0, 0, 255);
        q2.place_image(two, 2, 6);                                         // changed under the tail
        m.feed(d.render(q2, 3));
        CHECK(!m.broken && pixel_mismatches(m, wanted(q2, C), 3) == 0);
    }
    {
        // No colour (--no-color): letters only, pictures left out.
        const int R = 8, C = 20;
        Options o;
        o.color = false;
        Display d;
        d.init(R, C);
        d.color = false;
        d.enable_images(CW, CH);
        Picture p;
        p.init(R, C);
        auto block = std::make_shared<Raster>();
        block->init(CW, CH);
        block->set(1, 1, 255, 0, 0);
        p.place_image(block, 1, 1);
        CHECK(d.render(p, 0).find("\x1bP") == std::string::npos);
    }

    // ---- a fade shown on a Display: at every step the screen has the pixels the faded picture asks for ----
    {
        const int R = 12, C = 30;
        Display d;
        d.init(R, C);
        d.enable_images(CW, CH);
        Model m;
        m.init(R + 1, C, CW, CH);
        std::mt19937 rng(11);
        Picture a, b;
        a.init(R, C); b.init(R, C);
        for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) { a.set(i, j, plain_cell(rng)); b.set(i, j, plain_cell(rng)); }
        a.place_image(make_raster(rng, 80, 60, 4), 1, 2);
        b.place_image(make_raster(rng, 70, 70, 5), 3, 8);
        b.place_image(make_raster(rng, 40, 40, 3), 6, 18);
        Picture mix;
        long sent = 0;
        for (int step = 0; step <= 20; step++) {
            fade(a, b, step / 20.0, mix);
            const std::string s = d.render(mix, step * 0.1);
            sent += (long)s.size();
            m.feed(s);
            CHECK(!m.broken);
            CHECK(pixel_mismatches(m, wanted(mix, C), 3) == 0);
        }
        CHECK(sent > 2000);
    }

    // ---- the whole thing at random: after every frame the terminal shows the right letters and the right pixels ----
    {
        long frames = 0, with_pixels = 0, exact_frames = 0, refreshed = 0, held_cells = 0;
        for (unsigned seed = 1; seed <= 12; seed++) {
            std::mt19937 rng(seed * 7919);
            const int R = 8 + (int)(rng() % 8), C = 24 + (int)(rng() % 20);
            Display with, without;
            with.init(R, C); without.init(R, C);
            with.enable_images(CW, CH);
            Model mw, mo;
            mw.init(R + 1, C, CW, CH);
            mo.init(R + 1, C, CW, CH);
            Picture letters;
            letters.init(R, C);
            struct Item { std::shared_ptr<Raster> raster; int row, col; double rate; };
            static const double rate_choices[5] = {-1.0, -1.0, 0.0, 3.0, 40.0};
            std::vector<Item> items;
            std::vector<char> prev_cell_ink((size_t)R * C, 0);
            for (int frame = 0; frame < 160; frame++) {
                // letters: some change, some shimmer in time
                for (int k = 0, n = (int)(rng() % 25); k < n; k++) letters.set((int)(rng() % (unsigned)(R + 1)), (int)(rng() % (unsigned)C), plain_cell(rng));
                if (rng() % 25 == 0) {                                     // a line of text, some wide letters
                    PicCell tc; tc.text = true; tc.ch = U'T'; tc.r = tc.g = tc.b = 220;
                    const int row = (int)(rng() % (unsigned)R), col = (int)(rng() % (unsigned)(C - 6));
                    for (int k = 0; k < 6; k++) letters.set(row, col + k, tc);
                }
                // the pictures: add, remove, move, change
                if (rng() % 12 == 0 && items.size() < 4) {
                    const int colours = rng() % 5 == 0 ? 400 : 1 + (int)(rng() % 6);
                    items.push_back({make_raster(rng, 6 + (int)(rng() % 40), 6 + (int)(rng() % 40), colours), (int)(rng() % (unsigned)(R + 6)) - 3, (int)(rng() % (unsigned)(C + 6)) - 3, rate_choices[rng() % 5]});
                }
                if (!items.empty() && rng() % 15 == 0) items.erase(items.begin() + (std::ptrdiff_t)(rng() % items.size()));
                for (Item& it : items) {
                    const unsigned r = (unsigned)(rng() % 10);
                    if (r == 0) { it.row += (int)(rng() % 3) - 1; it.col += (int)(rng() % 3) - 1; }
                    else if (r == 1) it.raster = make_raster(rng, 6 + (int)(rng() % 40), 6 + (int)(rng() % 40), 1 + (int)(rng() % 6));
                    else if (r == 2) for (int k = 0; k < 20; k++) it.raster->set((int)(rng() % 40), (int)(rng() % 40), (int)(rng() % 256), 0, 100);
                }
                Picture withp = letters;
                for (const Item& it : items) withp.place_image(it.raster, it.row, it.col, it.rate);
                const bool many_colours = [&] { for (const Item& it : items) { std::map<uint32_t, int> k; for (const Pixel& p : it.raster->px) if (p.a) k[((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b]++; if (k.size() > 120) return true; } return false; }();
                const double t = frame * 0.07;
                std::string pre_w, pre_o;
                if (frame % 53 == 52) { pre_w = with.refresh(); pre_o = without.refresh(); refreshed++; }     // the clear a player does now and then
                // Cells whose letters are held back this frame: a picture that holds has pixels in them (the most generous rate of those
                // that do counts), they had pixels last frame (and a clear has not just taken them), and they are not text. A rate of 0
                // keeps the letter exactly; another rate lets it change or not, whichever its turn says.
                std::vector<char> held(mw.letter.size(), 0);                // 0 free, 1 kept as it was, 2 either
                std::vector<double> cell_rate(mw.letter.size(), -1.0);
                if (pre_w.empty())
                    for (const Item& it : items) {
                        if (it.rate < 0) continue;
                        for (int y = 0; y < it.raster->h; y++)
                            for (int x = 0; x < it.raster->w; x++) {
                                if (it.raster->at(x, y).a == 0) continue;
                                const int X = it.col * CW + x, Y = it.row * CH + y;
                                if (X < 0 || Y < 0 || X >= C * CW || Y >= R * CH) continue;
                                const size_t k = (size_t)(Y / CH) * C + (size_t)(X / CW);
                                cell_rate[k] = std::max(cell_rate[k], it.rate);
                            }
                    }
                for (size_t k = 0; k < held.size(); k++)
                    if (cell_rate[k] >= 0 && prev_cell_ink[k] && !withp.at((int)(k / (size_t)C), (int)(k % (size_t)C)).text) held[k] = cell_rate[k] == 0 ? 1 : 2;
                const std::vector<char32_t> before = mw.letter;
                mw.feed(pre_w + with.render(withp, t));
                mo.feed(pre_o + without.render(letters, t));
                frames++;
                CHECK(!mw.broken && !mo.broken);
                // the same letters as a Display that knows nothing of pictures, except the held ones, which stayed as they were
                bool same = true;
                for (size_t k = 0; k < mw.letter.size(); k++) {
                    bool bad = false;
                    if (held[k] == 1) { if (mw.letter[k] != before[k]) bad = true; held_cells++; }
                    else if (held[k] == 2) { if (mw.letter[k] != before[k] && mw.letter[k] != mo.letter[k]) bad = true; held_cells++; }
                    else if (mw.letter[k] != mo.letter[k]) bad = true;
                    if (bad && same) std::printf("seed %u frame %d cell (%zu,%zu): held=%d with='%c' before='%c' without='%c' prev_ink=%d\n", seed, frame, k / (size_t)C, k % (size_t)C,
                                                 (int)held[k], (char)mw.letter[k], (char)before[k], (char)mo.letter[k], (int)prev_cell_ink[k < prev_cell_ink.size() ? k : 0]);
                    if (bad) same = false;
                }
                CHECK(same);
                // and the pixels the pictures ask for, nothing more and nothing less
                const std::vector<Pixel> want = wanted(withp, C);
                const Mismatch bad = compare_pixels(mw, want, many_colours ? 80 : 3);
                if (bad.shape != 0) { std::printf("seed %u frame %d: %ld pixels drawn or left out wrongly (%ld of the wrong colour)\n", seed, frame, bad.shape, bad.colour); failures++; break; }
                if (bad.colour != 0 && !many_colours) { std::printf("seed %u frame %d: %ld pixels of the wrong colour with few colours\n", seed, frame, bad.colour); failures++; break; }
                bool any = false;
                for (const Pixel& p : want) if (p.a) { any = true; break; }
                with_pixels += any;
                // which cells have pixels now (the next frame's "had pixels last frame")
                for (int i = 0; i < R; i++)
                    for (int j = 0; j < C; j++) {
                        bool ink = false;
                        for (int y = 0; y < CH && !ink; y++) for (int x = 0; x < CW; x++) if (want[(size_t)(i * CH + y) * (C * CW) + (size_t)j * CW + x].a) { ink = true; break; }
                        prev_cell_ink[(size_t)i * C + j] = ink;
                    }
                exact_frames += !many_colours;
            }
        }
        std::printf("%ld random frames (%ld with pixels, %ld exact colours, %ld refreshed, %ld held letters): letters and pixels as asked\n", frames, with_pixels, exact_frames, refreshed, held_cells);
        CHECK(with_pixels > frames / 3);
        CHECK(held_cells > 1000);
    }

    if (failures == 0) std::printf("all image tests passed\n");
    return failures ? 1 : 0;
}
