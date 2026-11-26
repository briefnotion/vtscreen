// Tests for text attributes (bold, italic, underline, strikethrough, blink) and wide letters (two columns). A small terminal model applies the
// bytes the Display sends and says what a person would see; it is compared with what the pictures say, frame after frame.
//
//   tests/run_tests.sh        builds this with the strict flags and the sanitizers and runs it
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../vtscreen.h"

using namespace vtscreen;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

namespace {

// what is on one cell of the terminal
struct Seen {
    char32_t ch = U' ';
    unsigned looks = 0;                  // CELL_BOLD ... as the terminal was told
    bool inv = false, has_bg = false;
    int fr = 0, fg = 0, fb = 0, br = 0, bg = 0, bb = 0;
    bool wide = false, tail = false;     // the first half of a wide letter, the second
    bool operator==(const Seen& o) const {
        return ch == o.ch && looks == o.looks && inv == o.inv && has_bg == o.has_bg && wide == o.wide && tail == o.tail &&
               (!has_bg || (br == o.br && bg == o.bg && bb == o.bb)) &&
               (!visible() || (fr == o.fr && fg == o.fg && fb == o.fb));
    }
    bool visible() const { return ch != U' ' || inv || (looks & (CELL_UNDERLINE | CELL_STRIKE)); }
};

bool is_wide_letter(char32_t c) { return c == 0x4E2D || c == 0x6587 || c == 0x1F600; }

// a terminal: cursor moves, the pens (colours, inverse, attributes), text. A wide letter takes two columns, and writing over half of one blanks
// the other half (as xterm does). A letter that does not fit in the last column breaks the model (the Display must never send one).
struct Term {
    int rows = 0, cols = 0;
    std::vector<Seen> grid;
    int cr = 0, cc = 0;
    Seen pen;
    bool broken = false;
    bool clear_halves = true;            // writing over half of a wide letter blanks the other half (xterm does; a terminal that does not must still look right)
    std::string why;
    void bad(const std::string& s) { if (!broken) why = s; broken = true; }

    void init(int r, int c) { rows = r; cols = c; grid.assign((size_t)r * c, Seen()); cr = cc = 0; pen = Seen(); broken = false; why.clear(); }

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
            const int x = v[k];
            if (x == 0) { pen = Seen(); }
            else if (x == 1) pen.looks |= CELL_BOLD;
            else if (x == 22) pen.looks &= ~(unsigned)CELL_BOLD;
            else if (x == 3) pen.looks |= CELL_ITALIC;
            else if (x == 23) pen.looks &= ~(unsigned)CELL_ITALIC;
            else if (x == 4) pen.looks |= CELL_UNDERLINE;
            else if (x == 24) pen.looks &= ~(unsigned)CELL_UNDERLINE;
            else if (x == 9) pen.looks |= CELL_STRIKE;
            else if (x == 29) pen.looks &= ~(unsigned)CELL_STRIKE;
            else if (x == 5) pen.looks |= CELL_BLINK;
            else if (x == 25) pen.looks &= ~(unsigned)CELL_BLINK;
            else if (x == 7) pen.inv = true;
            else if (x == 27) pen.inv = false;
            else if (x == 49) pen.has_bg = false;
            else if (x == 38 && k + 4 < v.size() + 0 && v[k + 1] == 2) { pen.fr = v[k + 2]; pen.fg = v[k + 3]; pen.fb = v[k + 4]; k += 4; }
            else if (x == 48 && k + 4 < v.size() + 0 && v[k + 1] == 2) { pen.has_bg = true; pen.br = v[k + 2]; pen.bg = v[k + 3]; pen.bb = v[k + 4]; k += 4; }
            else bad("sgr " + std::to_string(x));
        }
    }

    Seen& at(int r, int c) { return grid[(size_t)r * cols + c]; }

    void blank_half(int r, int c) {          // the other half of a wide letter that was written over
        Seen& x = at(r, c);
        const Seen keep = x;
        x = Seen();
        x.has_bg = keep.has_bg; x.br = keep.br; x.bg = keep.bg; x.bb = keep.bb;
    }

    void put(char32_t cp) {
        if (cr < 0 || cr >= rows || cc < 0 || cc >= cols) { bad("write off the screen"); return; }
        const bool wide = is_wide_letter(cp);
        if (wide && cc + 1 >= cols) { bad("a wide letter in the last column"); return; }
        // writing over half of a wide letter blanks the other half
        if (clear_halves) {
            if (at(cr, cc).tail && cc > 0) blank_half(cr, cc - 1);
            if (at(cr, cc).wide && cc + 1 < cols) blank_half(cr, cc + 1);
            if (wide) {
                if (at(cr, cc + 1).wide && cc + 2 < cols) blank_half(cr, cc + 2);
                if (at(cr, cc + 1).tail) blank_half(cr, cc);
            }
        }
        Seen s = pen;
        s.ch = cp; s.wide = wide; s.tail = false;
        at(cr, cc) = s;
        if (wide) { Seen t = pen; t.ch = U' '; t.wide = false; t.tail = true; at(cr, cc + 1) = t; cc += 2; }
        else cc++;
    }

    void feed(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = (unsigned char)s[i];
            if (c == 0x1b) {
                if (i + 1 >= s.size() || s[i + 1] != '[') { bad("escape"); return; }
                size_t j = i + 2;
                while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
                if (j >= s.size()) { bad("cut escape"); return; }
                const std::string params = s.substr(i + 2, j - (i + 2));
                const char f = s[j];
                if (f == 'H') {
                    if (params.empty()) { cr = 0; cc = 0; }
                    else { const size_t semi = params.find(';'); cr = std::atoi(params.substr(0, semi).c_str()) - 1; cc = std::atoi(params.substr(semi + 1).c_str()) - 1; }
                } else if (f == 'J') { if (params == "2") std::fill(grid.begin(), grid.end(), Seen()); else bad("J"); }
                else if (f == 'C') cc += params.empty() ? 1 : std::atoi(params.c_str());
                else if (f == 'm') sgr(params);
                else bad(std::string("csi ") + f);
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
            put(cp);
        }
    }
};

// what a person should see for a picture of text cells (the rules of the library, written out independently)
std::vector<Seen> expected_for(const Picture& p) {
    std::vector<Seen> out((size_t)p.rows * p.cols);
    for (int i = 0; i < p.rows; i++) {
        for (int j = 0; j < p.cols; j++) {
            const PicCell& c = p.at(i, j);
            Seen s;
            s.ch = c.ch;
            s.looks = c.attrs & CELL_LOOKS;
            s.inv = c.inv;
            s.fr = (int)c.r; s.fg = (int)c.g; s.fb = (int)c.b;
            if (c.alt_lum > 0) { s.has_bg = true; s.br = (int)(c.alt_r * c.alt_lum + 0.5); s.bg = (int)(c.alt_g * c.alt_lum + 0.5); s.bb = (int)(c.alt_b * c.alt_lum + 0.5); }
            if (c.attrs & CELL_WIDE) {
                const bool ok = j + 1 < p.cols && (p.at(i, j + 1).attrs & CELL_TAIL);
                if (ok) {
                    s.wide = true;
                    out[(size_t)i * p.cols + j] = s;
                    Seen t = s; t.ch = U' '; t.wide = false; t.tail = true;
                    out[(size_t)i * p.cols + j + 1] = t;
                    j++;
                    continue;
                }
                s = Seen(); s.has_bg = c.alt_lum > 0;
                if (s.has_bg) { s.br = (int)(c.alt_r * c.alt_lum + 0.5); s.bg = (int)(c.alt_g * c.alt_lum + 0.5); s.bb = (int)(c.alt_b * c.alt_lum + 0.5); }
            } else if (c.attrs & CELL_TAIL) {                     // one that was not taken by a head before it
                s = Seen(); s.has_bg = c.alt_lum > 0;
                if (s.has_bg) { s.br = (int)(c.alt_r * c.alt_lum + 0.5); s.bg = (int)(c.alt_g * c.alt_lum + 0.5); s.bb = (int)(c.alt_b * c.alt_lum + 0.5); }
            }
            out[(size_t)i * p.cols + j] = s;
        }
    }
    return out;
}

bool same(const Term& t, const std::vector<Seen>& want, int rows, int cols, std::string* where = nullptr) {
    if (t.broken) { if (where) *where = "model broken: " + t.why; return false; }
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++) {
            Seen a = t.grid[(size_t)i * cols + j];
            const Seen& b = want[(size_t)i * cols + j];
            if (!(a == b)) { if (where) *where = "cell " + std::to_string(i) + "," + std::to_string(j); return false; }
        }
    return true;
}

PicCell cell(char32_t ch, uint8_t attrs = 0, double r = 200, double g = 100, double b = 50) {
    PicCell c;
    c.text = true; c.ch = ch; c.attrs = attrs; c.r = r; c.g = g; c.b = b;
    return c;
}

// the picture, the display, the terminal: render and apply
struct Rig {
    int R, C;
    Picture pic;
    Display disp;
    Term term;
    std::string last;
    Rig(int rows, int cols, bool retained = true, bool clears = true) : R(rows), C(cols) {
        pic.init(R, C);
        if (retained) pic.retain();
        disp.init(R, C);
        term.init(R + 1, C);
        term.clear_halves = clears;
    }
    void show(double t = 1.0) { last = disp.render(pic, t); term.feed(last); }
    bool matches(std::string* where = nullptr) { return same(term, expected_for(pic), R + 1, C, where); }
};

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

void test_codes() {
    Rig r(2, 20);
    r.pic.set(0, 0, cell(U'a', CELL_BOLD));
    r.pic.set(0, 1, cell(U'b', CELL_ITALIC));
    r.pic.set(0, 2, cell(U'c', CELL_UNDERLINE));
    r.pic.set(0, 3, cell(U'd', CELL_STRIKE));
    r.pic.set(0, 4, cell(U'e', CELL_BLINK));
    r.pic.set(0, 5, cell(U'f', CELL_BOLD | CELL_ITALIC | CELL_UNDERLINE | CELL_STRIKE | CELL_BLINK));
    r.pic.set(0, 6, cell(U'g'));
    r.show();
    CHECK(has(r.last, "\x1b[1m")); CHECK(has(r.last, "\x1b[3m")); CHECK(has(r.last, "\x1b[4m"));
    CHECK(has(r.last, "\x1b[9m")); CHECK(has(r.last, "\x1b[5m"));
    CHECK(has(r.last, "\x1b[22m")); CHECK(has(r.last, "\x1b[23m")); CHECK(has(r.last, "\x1b[24m"));
    CHECK(has(r.last, "\x1b[29m")); CHECK(has(r.last, "\x1b[25m"));
    std::string where;
    CHECK(r.matches(&where));
    if (!where.empty()) std::printf("  %s\n", where.c_str());
    CHECK(r.term.grid[0].looks == CELL_BOLD);
    CHECK(r.term.grid[4].looks == CELL_BLINK);
    CHECK(r.term.grid[6].looks == 0);

    // nothing changes: nothing is sent; one attribute changes: only that cell is written
    r.show(1.1);
    CHECK(r.last.empty());
    r.pic.set(0, 2, cell(U'c', CELL_UNDERLINE | CELL_BOLD));
    r.show(1.2);
    CHECK(!r.last.empty() && r.last.size() < 40);
    CHECK(r.matches());
    r.pic.set(0, 2, cell(U'c', CELL_UNDERLINE));                                   // an attribute alone changes back
    r.show(1.3);
    CHECK(r.matches());
    // no attributes anywhere: no attribute codes in the output
    Rig plain(2, 10);
    plain.pic.set(0, 0, cell(U'x'));
    plain.pic.set(1, 3, cell(U'y'));
    plain.show();
    CHECK(!has(plain.last, "\x1b[1m") && !has(plain.last, "\x1b[3m") && !has(plain.last, "\x1b[4m") && !has(plain.last, "\x1b[22m"));
}

void test_blank_with_lines() {
    // a space that is underlined or struck through is drawn (it has a colour); a plain one is not
    Rig r(1, 8);
    r.pic.set(0, 1, cell(U' ', CELL_UNDERLINE, 10, 200, 30));
    r.pic.set(0, 3, cell(U' ', CELL_STRIKE, 10, 200, 30));
    r.pic.set(0, 5, cell(U' ', CELL_BOLD, 10, 200, 30));                            // bold alone shows nothing on a space
    r.show();
    CHECK(has(r.last, "38;2;10;200;30m"));
    CHECK(r.matches());
    CHECK(r.term.grid[1].looks == CELL_UNDERLINE && r.term.grid[1].fg == 200);
    CHECK(r.term.grid[3].looks == CELL_STRIKE);
    CHECK(r.term.grid[2].looks == 0);
}

void test_wide_basics() {
    Rig r(2, 10);
    PicCell head = cell(0x4E2D, CELL_WIDE | CELL_BOLD), tail = cell(U' ', CELL_TAIL | CELL_BOLD);
    r.pic.set(0, 2, head); r.pic.set(0, 3, tail);
    r.pic.set(0, 4, cell(U'x'));
    r.show();
    std::string where;
    CHECK(r.matches(&where));
    CHECK(r.term.grid[2].wide && r.term.grid[2].ch == 0x4E2D && r.term.grid[3].tail);
    CHECK(r.term.grid[4].ch == U'x');
    CHECK(!r.term.broken);
    // the next write after a wide letter moves the cursor for certain (terminals differ on its width)
    CHECK(has(r.last, "\x1b[1;5H") || has(r.last, "\x1b[1;5"));

    // an emoji (four byte letter)
    r.pic.set(1, 0, cell(0x1F600, CELL_WIDE)); r.pic.set(1, 1, cell(U' ', CELL_TAIL));
    r.show(1.1);
    CHECK(r.matches());

    // nothing changes: nothing sent
    r.show(1.2);
    CHECK(r.last.empty());

    // the head's letter changes: both columns come out again
    r.pic.set(0, 2, cell(0x6587, CELL_WIDE | CELL_BOLD));
    r.show(1.3);
    CHECK(r.matches());
    // only the colour of the second half changes (the first half did not): the pair is drawn again, so the terminal shows the new colour
    PicCell t2 = tail; t2.alt_lum = 1; t2.alt_r = 20; t2.alt_g = 30; t2.alt_b = 40;
    r.pic.set(0, 3, t2);
    r.show(1.4);
    std::string where2;
    CHECK(r.matches(&where2));
    CHECK(!r.last.empty());
    // the second half becomes an ordinary letter: the first half must go (it is blank now)
    r.pic.set(0, 3, cell(U'z'));
    r.show(1.5);
    CHECK(r.matches());
    CHECK(r.term.grid[2].ch == U' ' && !r.term.grid[2].wide);
    CHECK(r.term.grid[3].ch == U'z');
    // and back
    r.pic.set(0, 3, tail);
    r.show(1.6);
    CHECK(r.matches());
    CHECK(r.term.grid[2].wide && r.term.grid[2].ch == 0x6587);                       // the first half comes back when the second does
    // the first half becomes narrow: the second half is blank
    r.pic.set(0, 2, cell(U'n'));
    r.show(1.7);
    CHECK(r.matches());
    CHECK(r.term.grid[3].ch == U' ' && !r.term.grid[3].tail);
}

void test_wide_orphans() {
    // a first half at the last column, a first half whose next cell is not a second half, a second half alone: all blanks, none broken
    Rig r(2, 6);
    r.pic.set(0, 5, cell(0x4E2D, CELL_WIDE));                                         // no room for a second column
    r.pic.set(1, 0, cell(0x4E2D, CELL_WIDE)); r.pic.set(1, 1, cell(U'q'));            // first half, then an ordinary letter
    r.pic.set(1, 3, cell(U' ', CELL_TAIL));                                          // second half alone
    r.pic.set(1, 4, cell(U' ', CELL_TAIL)); r.pic.set(1, 5, cell(0x6587, CELL_WIDE));  // second half then a first half in the last column
    r.show();
    std::string where;
    CHECK(r.matches(&where));
    if (!where.empty()) std::printf("  %s\n", where.c_str());
    CHECK(!r.term.broken);
    CHECK(r.term.grid[5].ch == U' ' && !r.term.grid[5].wide);
    CHECK(r.term.grid[(size_t)1 * 6 + 0].ch == U' ');
    CHECK(r.term.grid[(size_t)1 * 6 + 1].ch == U'q');
    // the missing half arrives later: it comes to life
    r.pic.set(1, 1, cell(U' ', CELL_TAIL));
    r.show(1.1);
    CHECK(r.matches());
    CHECK(r.term.grid[(size_t)1 * 6 + 0].wide);
}

// random pictures of text cells with attributes and wide letters (some of them broken on purpose), changed a little each frame, shown with
// a retained and an ordinary picture: what the terminal shows is what the picture says, and both send the same bytes
void test_random() {
    long frames = 0;
    for (unsigned seed = 1; seed <= 40; seed++) {
        std::mt19937 rng(seed);
        const int R = 3 + (int)(rng() % 5), C = 4 + (int)(rng() % 14);
        const bool clears = seed % 2 == 0;                                   // half the runs: a terminal that leaves the other half of a written-over wide letter alone
        Rig a(R, C, false, clears), b(R, C, true, clears);
        static const char32_t letters[] = {U'a', U'b', U' ', U'#', U'x', U'z', U'-', 0xE9};    // (narrow: a wide letter is only ever drawn with CELL_WIDE, as the library asks)
        for (int f = 0; f < 400; f++) {
            const int edits = 1 + (int)(rng() % 6);
            for (int e = 0; e < edits; e++) {
                const int i = (int)(rng() % (unsigned)R), j = (int)(rng() % (unsigned)C);
                PicCell c = cell(letters[rng() % 8], (uint8_t)(rng() % 32), (double)(rng() % 3 * 100), (double)(rng() % 3 * 100), 50);
                if (rng() % 3 == 0) { c.alt_lum = 1; c.alt_r = (double)(rng() % 3 * 100); c.alt_g = 20; c.alt_b = 30; }
                c.inv = rng() % 6 == 0;
                const unsigned kind = rng() % 6;
                if (kind == 0 && j + 1 < C) {                                       // a proper pair
                    c.ch = rng() % 2 ? 0x4E2D : 0x1F600; c.attrs = (uint8_t)((c.attrs & CELL_LOOKS) | CELL_WIDE);
                    PicCell t = c; t.ch = U' '; t.attrs = (uint8_t)((c.attrs & CELL_LOOKS) | CELL_TAIL);
                    a.pic.set(i, j, c); a.pic.set(i, j + 1, t); b.pic.set(i, j, c); b.pic.set(i, j + 1, t);
                    continue;
                }
                if (kind == 1) { c.attrs |= CELL_WIDE; c.ch = 0x6587; }             // a head on its own (or with whatever follows)
                if (kind == 2) { c.attrs |= CELL_TAIL; c.ch = U' '; }               // a tail on its own
                a.pic.set(i, j, c); b.pic.set(i, j, c);
            }
            const double t = 1.0 + f * 0.05;
            const std::string x = a.disp.render(a.pic, t), y = b.disp.render(b.pic, t);
            a.term.feed(x); b.term.feed(y);
            CHECK(x == y);
            std::string where;
            const bool ok = same(a.term, expected_for(a.pic), R + 1, C, &where) && same(b.term, expected_for(b.pic), R + 1, C, &where);
            if (!ok) { CHECK(ok); std::printf("  seed %u frame %d: %s\n", seed, f, where.c_str()); return; }
            frames++;
        }
    }
    CHECK(frames == 40 * 400);
    std::printf("%ld random frames with attributes and wide letters\n", frames);
}

void test_plain_render() {
    // --still style output: the codes are there, a wide letter is written once, with its second half skipped
    Picture p;
    p.init(1, 8);
    p.set(0, 0, cell(U'a', CELL_BOLD | CELL_UNDERLINE));
    p.set(0, 1, cell(0x4E2D, CELL_WIDE)); p.set(0, 2, cell(U' ', CELL_TAIL));
    p.set(0, 3, cell(U'b'));
    p.set(0, 4, cell(0x6587, CELL_WIDE));                                          // an orphan: a blank
    const std::string s = render_plain(p, p.rows, p.cols, 0, 30, true);
    CHECK(has(s, "\x1b[1m") && has(s, "\x1b[4m"));
    CHECK(has(s, "\xE4\xB8\xAD"));
    CHECK(!has(s, "\xE6\x96\x87"));
    const std::string plain = render_plain(p, p.rows, p.cols, 0, 30, false, true, false);      // no inverse/attribute codes at all
    CHECK(!has(plain, "\x1b[1m") && !has(plain, "\x1b[4m"));
}

void test_fade_keeps_attrs() {
    Picture a, b, out;
    a.init(1, 4); b.init(1, 4);
    a.set(0, 0, cell(U'a', CELL_BOLD)); b.set(0, 0, cell(U'b', CELL_ITALIC));
    fade(a, b, 0.2, out);
    CHECK(out.at(0, 0).ch == U'a' && out.at(0, 0).attrs == CELL_BOLD);
    fade(a, b, 0.8, out);
    CHECK(out.at(0, 0).ch == U'b' && out.at(0, 0).attrs == CELL_ITALIC);
}

void test_cell_layout() {
    CHECK(sizeof(VtCell) == 16);
    VtCell x, y;
    CHECK(x == y);
    y.attrs = CELL_BOLD;
    CHECK(x != y);
    PicCell p, q;
    CHECK(p == q);
    q.attrs = CELL_STRIKE;
    CHECK(p != q);
    const VtCell m = make_cell(U' ', false, 9, 9, 9);
    CHECK(m.r == 0);                                              // a plain space: no colour
    const VtCell u = make_cell(U' ', false, 9, 9, 9, false, 0, 0, 0, CELL_UNDERLINE);
    CHECK(u.r == 9);                                              // an underlined one keeps it
}

}  // namespace

int main() {
    test_cell_layout();
    test_codes();
    test_blank_with_lines();
    test_wide_basics();
    test_wide_orphans();
    test_plain_render();
    test_fade_keeps_attrs();
    test_random();
    if (failures == 0) std::printf("all attribute and wide letter tests passed\n");
    return failures == 0 ? 0 : 1;
}
