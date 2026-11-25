// A fixed sequence of pictures through a Display, hashed. It uses only what vtscreen has always had (Picture::init, at,
// text, Display::init, render, refresh), so it can be run against any version of the library: the numbers pinned in
// retained_tests.cpp were made with the library as it was before retained mode, and the library must keep making them
// (the bytes sent are the same, down to every colour code and cursor move).
//
// Include after vtscreen.h.
#pragma once

#include <cstdint>
#include <random>
#include <string>

struct PinnedResult {
    uint64_t hash = 1469598103934665603ull;
    size_t bytes = 0;
    int frames_with_output = 0;
};

inline void pinned_add(PinnedResult& r, const std::string& out) {
    for (unsigned char c : out) { r.hash ^= c; r.hash *= 1099511628211ull; }
    r.bytes += out.size();
    if (!out.empty()) r.frames_with_output++;
}

inline PinnedResult pinned_run(bool color, int fps, bool hash_letters, unsigned seed) {
    using namespace vtscreen;
    const int R = 14, C = 50;
    std::mt19937 rng(seed);
    Display d;
    d.init(R, C);
    d.color = color; d.fps = fps; d.hash_letters = hash_letters;
    Picture p;
    p.init(R, C);
    PinnedResult result;
    static const char* words[] = {"status", "centre 37.690N", "driving, following", "\xE2\x94\x82 box \xE2\x94\x82", "heading \xC2\xB0 E", "x", "rain"};
    for (int f = 0; f < 120; f++) {
        const double t = 1.0 + f / 30.0;
        const int edits = (f % 5 == 0) ? 0 : (int)(rng() % 40);
        for (int k = 0; k < edits; k++) {
            const int i = (int)(rng() % (unsigned)(R + 1)), j = (int)(rng() % (unsigned)C);
            PicCell& c = p.at(i, j);
            if (rng() % 6 == 0) {
                p.text(i, j, words[rng() % 7], (double)(rng() % 256), (double)(rng() % 256), (double)(rng() % 256), rng() % 4 == 0);
            } else {
                c = PicCell();
                c.lum = (double)(rng() % 1001) / 1000.0;
                c.r = (double)(rng() % 256); c.g = (double)(rng() % 256); c.b = (double)(rng() % 256);
                c.shimmer = (rng() % 3 == 0) ? 1.0 : 0.0;
                if (rng() % 4 == 0) { c.alt_lum = (double)(rng() % 101) / 100.0; c.alt_r = (double)(rng() % 256); c.alt_g = (double)(rng() % 256); c.alt_b = (double)(rng() % 256); }
            }
        }
        std::string out = d.render(p, t);
        if (f == 70) out += d.refresh();
        pinned_add(result, out);
    }
    return result;
}
