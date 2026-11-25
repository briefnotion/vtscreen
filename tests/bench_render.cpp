// What does Display::render cost per frame (letters + compare + building the bytes; no terminal is involved)?
//   g++ -std=c++17 -O2 -o bench tests/bench_render.cpp && ./bench
// Works with any version of the library; the retained rows only run when the library has them.
#include <chrono>
#include <cstdio>
#include <random>
#include "../vtscreen.h"
using namespace vtscreen;
static double now_us() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void fill(Picture& pic, int R, int C, int kind) {
    std::mt19937 rng(1);
    for (int i = 0; i < R + 1; i++) for (int j = 0; j < C; j++) {
        PicCell p;
        p.lum = (double)(rng() % 1000) / 1000.0; p.r = 80 + rng() % 150; p.g = 120 + rng() % 100; p.b = 90 + rng() % 100;
        p.shimmer = kind == 2 ? 1.0 : (kind == 1 ? (rng() % 10 == 0 ? 1.0 : 0.0) : 0.0);
        if (kind == 0 || kind == 3) { p.text = true; p.ch = U'a' + (char32_t)(rng() % 26); }
        pic.at(i, j) = p;
    }
}

int main() {
    const int R = 49, C = 160;
    struct Case { const char* name; int kind; int touch_per_frame; } cases[] = {
        {"all text, nothing changes", 0, 0},
        {"all text, 20 cells change a frame", 0, 20},
        {"map-like: 90% still, 10% shimmering", 1, 0},
        {"map-like, and 20 cells change a frame", 1, 20},
        {"every cell shimmering", 2, 0},
        {"all text, every cell changes every frame", 3, -1}};
    for (const Case& c : cases) {
        for (int mode = 0; mode < 2; mode++) {
#ifndef VTSCREEN_RETAINED
            if (mode == 1) continue;
#endif
            if (mode == 1 && c.touch_per_frame < 0) continue;       // a program that rewrites everything has nothing to keep
            Display d; d.init(R, C); d.fps = 30;
            Picture pic; pic.init(R, C);
#ifdef VTSCREEN_RETAINED
            if (mode == 1) pic.retain();
#endif
            fill(pic, R, C, c.kind);
            std::mt19937 rng(5);
            std::string out = d.render(pic, 0.0);
            const int frames = 400; double total = 0, bytes = 0;
            for (int f = 1; f <= frames; f++) {
                if (c.touch_per_frame < 0) { for (int i = 0; i < R + 1; i++) for (int j = 0; j < C; j++) pic.at(i, j).ch = U'a' + (char32_t)(rng() % 26); }
                for (int k = 0; k < c.touch_per_frame; k++) {
                    const int i = (int)(rng() % (R + 1)), j = (int)(rng() % C);
                    pic.at(i, j).lum = (double)(rng() % 1000) / 1000.0;
                    if (c.kind == 0) pic.at(i, j).ch = U'a' + (char32_t)(rng() % 26);
                }
                const double t0 = now_us();
                out = d.render(pic, f / 30.0);
                total += now_us() - t0; bytes += (double)out.size();
            }
            std::printf("%-44s %-9s %8.1f us per frame, %8.0f bytes\n", c.name, mode ? "retained" : "ordinary", total / frames, bytes / frames);
        }
    }
}
