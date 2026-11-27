// Tests for the Sixel decoder (vtscreen::sixel_decode). The rules they check:
//   - what the encoder writes comes back as the pixels it was given
//   - the hand-checked cases of the Sixel format: colours (RGB, HLS, the default sixteen, wrapping), repeats, carriage return
//     and new line, raster attributes, pixel shape, transparency, the limits
//   - nothing hostile makes it fail, hang or grow past its limits
//
//   tests/run_tests.sh        builds this with the strict flags and the sanitizers and runs it
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../vtscreen.h"

using namespace vtscreen;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

namespace {

// The data of a Sixel picture: what is between "ESC P ... q" and "ESC \".
std::string payload(const std::string& whole) {
    const size_t q = whole.find('q');
    std::string s = whole.substr(q + 1);
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, "\x1b\\") == 0) s.resize(s.size() - 2);
    return s;
}

bool near(const Pixel& a, int r, int g, int b, int tolerance = 3) {
    return a.a == 255 && std::abs(a.r - r) <= tolerance && std::abs(a.g - g) <= tolerance && std::abs(a.b - b) <= tolerance;
}

Raster decode(const std::string& data, int p2 = 1, bool* ok = nullptr, const SixelLimits& lim = SixelLimits()) {
    Raster r;
    const bool good = sixel_decode(data, p2, r, lim);
    if (ok) *ok = good;
    return r;
}

}  // namespace

int main() {
    // ---- what the encoder writes comes back
    {
        std::mt19937 rng(3);
        for (int trial = 0; trial < 300; trial++) {
            const int w = 1 + (int)(rng() % 50), h = 1 + (int)(rng() % 50);
            std::vector<Pixel> px((size_t)w * h);
            const int colours = 1 + (int)(rng() % 12);
            for (Pixel& p : px) {
                if (rng() % 4 == 0) { p = Pixel(); continue; }
                const int k = (int)(rng() % (unsigned)colours);
                p = Pixel{(uint8_t)(k * 20), (uint8_t)(255 - k * 20), (uint8_t)(k * 7), 255};
            }
            bool ok = false;
            Raster r = decode(payload(detail::sixel_encode(px.data(), w, w, h)), 1, &ok);
            CHECK(ok && r.w == w && r.h == h);
            int bad = 0;
            for (size_t k = 0; k < px.size() && ok && k < r.px.size(); k++) {
                if ((px[k].a != 0) != (r.px[k].a != 0)) bad++;
                else if (px[k].a != 0 && !near(r.px[k], px[k].r, px[k].g, px[k].b)) bad++;
            }
            CHECK(bad == 0);
        }
        // More than 255 colours (the encoder keeps the most used): the same shape, and colours close.
        Raster many;
        many.init(60, 40);
        for (int y = 0; y < 40; y++) for (int x = 0; x < 60; x++) many.set(x, y, 50, 100, 150);
        for (int k = 0; k < 400; k++) many.set((k * 7) % 60, (k * 13) % 40, (k * 5) % 256, (k * 11) % 256, (k * 17) % 256);
        bool ok = false;
        Raster back = decode(payload(detail::sixel_encode(many.px.data(), 60, 60, 40)), 1, &ok);
        CHECK(ok && back.w == 60 && back.h == 40);
        int plain = 0, plain_ok = 0;
        for (size_t k = 0; k < many.px.size() && ok; k++) if (many.px[k] == Pixel{50, 100, 150, 255}) { plain++; plain_ok += near(back.px[k], 50, 100, 150, 8); }
        CHECK(plain > 1500 && plain_ok == plain);
    }

    // ---- colours
    {
        bool ok = false;
        Raster r = decode("#0;2;100;0;0~", 1, &ok);                           // a red column, six pixels
        CHECK(ok && r.w == 1 && r.h == 6);
        for (int y = 0; y < 6; y++) CHECK(near(r.at(0, y), 255, 0, 0));
        r = decode("#1;2;0;100;0~", 1, &ok);                                  // defined and selected together
        CHECK(ok && near(r.at(0, 3), 0, 255, 0));
        r = decode("#1;2;0;0;100#1~", 1, &ok);                                // defined, then selected again
        CHECK(ok && near(r.at(0, 0), 0, 0, 255));
        r = decode("#2;2;50;50;50~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 128, 128, 128, 2));
        // the default colours: register 1 is a blue (20 20 80 per cent), 7 and 15 greys
        r = decode("#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 51, 51, 204, 2));
        r = decode("#15~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 204, 204, 204, 2));
        r = decode("#0~", 1, &ok);                                            // register 0 starts black
        CHECK(ok && near(r.at(0, 0), 0, 0, 0));
        r = decode("~", 1, &ok);                                              // no colour chosen: register 0
        CHECK(ok && near(r.at(0, 0), 0, 0, 0));
        r = decode("#200~", 1, &ok);                                          // not defined: black
        CHECK(ok && near(r.at(0, 0), 0, 0, 0));
        // HLS: the DEC hue puts blue at 0, red at 120, green at 240; lightness 100 is white, saturation 0 grey
        r = decode("#1;1;0;50;100#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 0, 0, 255, 3));
        r = decode("#1;1;120;50;100#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 255, 0, 0, 3));
        r = decode("#1;1;240;50;100#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 0, 255, 0, 3));
        r = decode("#1;1;0;100;100#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 255, 255, 255, 3));
        r = decode("#1;1;120;50;0#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 128, 128, 128, 3));
        // registers wrap: 300 is 44 in 256 registers
        r = decode("#300;2;100;100;0#44~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 255, 255, 0));
        r = decode("#44;2;100;100;0#300~", 1, &ok);                           // selected by a number that wraps, without defining it
        CHECK(ok && near(r.at(0, 0), 255, 255, 0));
        // a smaller set of registers
        SixelLimits few;
        few.colours = 16;
        r = decode("#17;2;0;100;100#1~", 1, &ok, few);                        // 17 wraps to 1
        CHECK(ok && near(r.at(0, 0), 0, 255, 255));
        // percentages past 100 are held; a definition of a kind we do not know leaves the register alone
        r = decode("#1;2;500;0;0#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 255, 0, 0));
        r = decode("#1;2;0;100;0#1;9;1;2;3#1~", 1, &ok);
        CHECK(ok && near(r.at(0, 0), 0, 255, 0));
    }

    // ---- the shape of the picture
    {
        bool ok = false;
        Raster r = decode("#1!10~", 1, &ok);                                  // a repeat
        CHECK(ok && r.w == 10 && r.h == 6);
        r = decode("#1~-~", 1, &ok);                                          // a new line: a second band
        CHECK(ok && r.w == 1 && r.h == 12 && near(r.at(0, 11), 51, 51, 204, 2));
        r = decode("#1~~~-#2;2;100;0;0~", 1, &ok);                            // the second band only has one column: the rest is transparent
        CHECK(ok && r.w == 3 && r.h == 12 && r.at(0, 6).a == 255 && r.at(1, 6).a == 0 && r.at(2, 11).a == 0);
        // a carriage return: the next colour goes over the same band; only the bits that are set are drawn
        r = decode("#1;2;100;0;0~~#2;2;0;100;0$@@", 1, &ok);                  // '@' is the top pixel only
        CHECK(ok && r.w == 2 && near(r.at(0, 0), 0, 255, 0) && near(r.at(1, 0), 0, 255, 0) && near(r.at(0, 1), 255, 0, 0) && near(r.at(1, 5), 255, 0, 0));
        // which bits go where: '?' none, '@' the top, 'A' the second row, '~' all six
        r = decode("#1;2;100;100;100@A~?", 1, &ok);
        CHECK(ok && r.w == 4 && r.h == 6);
        CHECK(r.at(0, 0).a == 255 && r.at(0, 1).a == 0);
        CHECK(r.at(1, 0).a == 0 && r.at(1, 1).a == 255 && r.at(1, 2).a == 0);
        for (int y = 0; y < 6; y++) CHECK(r.at(2, y).a == 255 && r.at(3, y).a == 0);     // (the '?' column is part of the width, undrawn)
        // a repeat with a count of 0 or 1; a repeat of something that is not a sixel; a repeat cut off
        r = decode("#1!1~!0~", 1, &ok);
        CHECK(ok && r.w == 2);                                                 // (a count of 0 means once)
        r = decode("#1~!5#2~", 1, &ok);                                       // '!5' followed by '#': a repeat of nothing, skipped
        CHECK(ok && r.w == 2);
        r = decode("#1~!", 1, &ok);
        CHECK(ok && r.w == 1);
    }

    // ---- raster attributes: the size, and the shape of a pixel
    {
        bool ok = false;
        Raster r = decode("\"1;1;10;12#1~", 1, &ok);                          // bigger than the data: the rest is transparent
        CHECK(ok && r.w == 10 && r.h == 12 && r.at(0, 0).a == 255 && r.at(5, 0).a == 0 && r.at(0, 11).a == 0);
        r = decode("\"1;1;10;12#1~", 0, &ok);                                 // ... or the background (register 0), when P2 says so
        CHECK(ok && r.w == 10 && r.h == 12 && near(r.at(5, 0), 0, 0, 0) && near(r.at(0, 11), 0, 0, 0) && near(r.at(0, 0), 51, 51, 204, 2));
        r = decode("#0;2;0;100;0\"1;1;4;6#1~", 2, &ok);                       // P2 of 2 is the same as 0; register 0 may be redefined
        CHECK(ok && near(r.at(3, 3), 0, 255, 0));
        r = decode("\"1;1;2;6#1~~~~~", 1, &ok);                               // smaller than the data: cut there
        CHECK(ok && r.w == 2 && r.h == 6);
        r = decode("\"2;1;1;6#1~", 1, &ok);                                   // a pixel twice as tall as it is wide: every row twice
        CHECK(ok && r.w == 1 && r.h == 12);
        r = decode("\"1;2;1;6#1~", 1, &ok);                                   // twice as wide: every column twice
        CHECK(ok && r.w == 2 && r.h == 6);
        r = decode("\"3;1;2;6#1~~", 1, &ok);                                  // 3:1
        CHECK(ok && r.w == 2 && r.h == 18);
        r = decode("\"1;1;1;6#1~", 1, &ok);
        CHECK(ok && r.w == 1 && r.h == 6);
        r = decode("\"0;0;0;0#1~", 1, &ok);                                   // zeros mean nothing was said
        CHECK(ok && r.w == 1 && r.h == 6);
        r = decode("\"#1~", 1, &ok);                                          // an empty one
        CHECK(ok && r.w == 1);
    }

    // ---- nothing to draw
    {
        bool ok = true;
        Raster r = decode("", 1, &ok);
        CHECK(!ok && r.w == 0 && r.h == 0);
        for (const char* nothing : {"#", "!", "-----", "$$$", "#1;2;", "????", "\"", "#1;2;10;20;30", "   %&'()*+,./:<=>", "\n\r\x1b"}) {
            ok = true;
            r = decode(nothing, 1, &ok);
            CHECK(!ok && r.w == 0 && r.px.empty());
        }
        // a picture that fails leaves the raster it was given empty, not as it was
        Raster used;
        used.init(5, 5);
        CHECK(!sixel_decode("#1;2;", 1, used) && used.w == 0 && used.h == 0 && used.px.empty());
        // '?' columns only (undrawn, but they say how wide it is): a picture with nothing in it is still nothing
        ok = true;
        r = decode("???", 1, &ok);
        CHECK(!ok);
    }

    // ---- the limits
    {
        SixelLimits small;
        small.max_width = 64; small.max_height = 36;
        bool ok = false;
        Raster r = decode("#1!99999999~", 1, &ok, small);                     // an enormous repeat is cut at the width
        CHECK(ok && r.w == 64 && r.h == 6);
        std::string lines = "#1";
        for (int i = 0; i < 100; i++) lines += "~-";
        r = decode(lines, 1, &ok, small);                                     // too many lines: cut at the height
        CHECK(ok && r.w == 1 && r.h == 36);
        r = decode("\"1;1;100000;100000#1~", 1, &ok, small);                  // a declared size beyond the limits is held to them
        CHECK(ok && r.w <= 64 && r.h <= 36);
        // numbers that are too big are numbers, not overflow
        r = decode("#99999999999999999999;2;100;0;0#1~!99999999999999999999~", 1, &ok, small);
        CHECK(ok && r.w <= 64);
        // a flood of new lines does not overflow the line count
        std::string flood(200000, '-');
        r = decode("#1~" + flood + "~", 1, &ok, small);
        CHECK(ok && r.h <= 36);
        // data after the limits is skipped without error
        r = decode("#1!64~~~~~~~~~#2~", 1, &ok, small);
        CHECK(ok && r.w == 64);
    }

    // ---- cut off, and hostile
    {
        std::mt19937 rng(9);
        const std::string alphabet = "#;!\"-$0123456789?@A~}|{abc \n\r\x1b";
        long total = 0;
        for (int trial = 0; trial < 6000; trial++) {
            std::string d;
            for (int j = 0, n = (int)(rng() % 200); j < n; j++) d += alphabet[rng() % alphabet.size()];
            SixelLimits lim;
            lim.max_width = 1 + (int)(rng() % 90); lim.max_height = 1 + (int)(rng() % 90); lim.colours = 1 + (int)(rng() % 300);
            Raster r;
            const bool ok = sixel_decode(d, (int)(rng() % 3), r, lim);
            CHECK(!ok || (r.w > 0 && r.h > 0 && r.px.size() == (size_t)r.w * r.h));
            CHECK(r.w <= lim.max_width * 8 && r.h <= lim.max_height * 8);              // (the shape of a pixel may repeat rows and columns, within the limits)
            total += ok;
        }
        CHECK(total > 1000);
        // every prefix of a real picture decodes or says it cannot, never fails
        std::string whole = "\"1;1;20;12#0;2;0;0;0#1;2;100;0;0#1!10~$#2;2;0;100;0!5~-#3;1;120;50;100!20~";
        for (size_t cut = 0; cut <= whole.size(); cut++) { Raster r; (void)sixel_decode(std::string_view(whole).substr(0, cut), 1, r); }
        // a big one decodes in reasonable time and is as big as it says
        std::string big = "\"1;1;4096;4096#1;2;100;0;0";
        for (int b = 0; b < 683; b++) big += "!4096~-";
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        Raster r = decode(big, 1, &ok);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ok && r.w == 4096 && r.h == 4096 && near(r.at(4095, 4095), 255, 0, 0) && r.at(0, 4095).a == 255);
        CHECK(ms < 30000);
        std::printf("a 4096x4096 picture (%zu bytes of data) decoded in %.0f ms\n", big.size(), ms);
    }

    if (failures == 0) std::printf("all sixel decoder tests passed\n");
    return failures ? 1 : 0;
}
