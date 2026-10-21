// Shaolin Monks draws each menu item as an image of its text, all of a column at the same left edge, the highlighted
// one opaque and the others faded (alpha 1.0 and 0.3 on the main menu). Those columns are the menus the touch
// controls let the player tap.
#include "common/menu_tap.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include "common/guest.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace game {

namespace {

struct Item {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    float alpha = 0;
    float grey = 0;  // the diffuse's brightness when its r g b are equal, -1 when tinted
};

float MidX(const Item& it) { return (it.x0 + it.x1) / 2; }
float MidY(const Item& it) { return (it.y0 + it.y1) / 2; }

constexpr float kSameEdge = 3.0f;      // guest pixels two items' left edges may differ by
constexpr float kContrast = 0.25f;     // the brightest item at least this much more opaque than the faintest
constexpr float kMinHighlight = 0.5f;  // a highlight at least this opaque
constexpr int64_t kStaleMs = 250;      // a menu not drawn this long is gone
constexpr size_t kMinRows = 3;  // fewer rows only when they are close, two images that line up are often art

// game thread: this frame's quads by texture and top edge: an item's triangles share both, and one image drawn on
// two rows is two items
std::map<std::pair<uint32_t, int>, Item> g_items;
std::mutex g_lock;
MenuList g_menu;
int64_t g_menu_ms = 0;

// a column of items sharing an edge, or a centre: the menu in it, or an empty list
MenuList Column(std::vector<Item> col) {
    std::sort(col.begin(), col.end(), [](const Item& a, const Item& b) { return a.y0 < b.y0; });
    // the brightest is the highlight, already as it fades in: the menu's crossfade takes a while
    float top = 0, low = 1;
    int lit = -1;
    for (size_t k = 0; k < col.size(); k++) {
        if (col[k].alpha > top) top = col[k].alpha, lit = int(k);
        low = std::min(low, col[k].alpha);
    }
    if (top < kMinHighlight || top - low < kContrast) return {};
    // a menu's rows are evenly spaced, near each other and on the screen; art that happens to line up is not
    float step = col[1].y0 - col[0].y0;
    float height = col[0].y1 - col[0].y0;
    if (step < 8 || (col.size() < kMinRows && step > height * 1.5f)) return {};
    for (size_t k = 0; k < col.size(); k++) {
        if (col[k].y0 < 0 || col[k].y1 > 480 || col[k].x0 < 0 || col[k].x1 > 640) return {};
        // rows stack, a banner behind a label overlaps it
        if (k && col[k].y0 < col[k - 1].y1 - (col[k - 1].y1 - col[k - 1].y0) * 0.75f) return {};
        if (k && std::fabs((col[k].y0 - col[k - 1].y0) - step) > step * 0.25f) return {};
        // one menu's text images are as tall as each other (a name label beside small print is no menu)
        float h = col[k].y1 - col[k].y0;
        if (h < height * 0.75f || h > height * 1.33f) return {};
    }
    MenuList m;
    m.selected = lit;
    for (size_t k = 0; k < col.size(); k++) {
        float c = MidY(col[k]);
        float up = k ? (c - MidY(col[k - 1])) / 2 : (col[k].y1 - col[k].y0) / 2;
        float down = k + 1 < col.size() ? (MidY(col[k + 1]) - c) / 2 : up;
        if (!k) up = down;
        m.rows.push_back({col[k].x0, col[k].x1, c - up, c + down});
    }
    return m;
}

// the menu with the most rows among the columns of left aligned and of centred items
MenuList Find() {
    std::vector<Item> items;
    for (auto& [tex, it] : g_items)
        if (it.grey >= 0.9f) items.push_back(it);
    MenuList best;
    for (bool centred : {false, true}) {
        auto key = [centred](const Item& it) { return centred ? MidX(it) : it.x0; };
        std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) { return key(a) < key(b); });
        for (size_t i = 0; i < items.size();) {
            size_t j = i;
            while (j < items.size() && key(items[j]) - key(items[i]) <= kSameEdge) j++;
            std::vector<Item> col(items.begin() + long(i), items.begin() + long(j));
            i = j;
            if (col.size() < 2 || col.size() <= best.rows.size()) continue;
            MenuList m = Column(std::move(col));
            if (!m.rows.empty()) best = std::move(m);
        }
    }
    return best;
}

// verbose builds: the guest bytes that follow the highlight, scored at each move of it (a stray reading costs a
// candidate one point, not its place), to find the menu's index
void TrackSelection(int selected, const MenuList& m) {
    static std::vector<std::pair<uint32_t, uint16_t>> cand;  // address, misses
    static int last = -1, samples = 0;
    static std::pair<size_t, int> page{};
    if (!logx::kVerbose) return;
    // another page of rows: the search starts over
    std::pair<size_t, int> now_page{m.rows.size(), 0};
    if (now_page != page) {
        page = now_page;
        cand.clear();
        samples = 0;
        last = -1;
    }
    if (selected == last) return;
    last = selected;
    auto at = [](uint32_t va) { return kernel::G<uint8_t>(va); };
    if (!samples) {
        if (selected < 2) return;  // a value rarer than 0 or 1 to start from
        for (uint32_t va = kImageBegin; va < kImageEnd; va++)
            if (at(va) == selected) cand.push_back({va, 0});
        for (uint32_t va = kPhysicalBegin; va < kPhysicalEnd; va++)
            if (at(va) == selected) cand.push_back({va, 0});
    } else {
        for (auto& c : cand)
            if (at(c.first) != uint8_t(selected)) c.second++;
        std::erase_if(cand, [](const auto& c) { return c.second > 1; });
    }
    samples++;
    std::string list;
    int shown = 0;
    for (const auto& c : cand)
        if (!c.second && shown++ < 24) {
            char b[16];
            std::snprintf(b, sizeof b, " %08X", c.first);
            list += b;
        }
    LOGI("menu index search: highlight %d, sample %d, %zu candidates, exact:%s", selected, samples, cand.size(),
         list.c_str());
    LOGI("menu index probe: highlight %d, [598468] %08X [598464] %08X [7545A2] %02X [7545A0] %08X", selected,
         kernel::R32(0x598468), kernel::R32(0x598464), at(0x7545A2), kernel::R32(0x7545A0));
}

// the character select: tall portraits side by side (the back row's, Sub-Zero's and Scorpion's, about 260 rows), one
// in full colour and the others greyed (0.45); the locked characters' silhouettes are black and left out
MenuList FindPortraits() {
    // a portrait is narrower than the backdrop behind them all (640 wide)
    constexpr float kMinHeight = 200, kMinWidth = 120, kMaxWidth = 400, kLit = 0.9f, kGreyed = 0.7f;
    std::vector<Item> p;
    for (auto& [key, it] : g_items)
        if (it.y1 - it.y0 >= kMinHeight && it.x1 - it.x0 >= kMinWidth && it.x1 - it.x0 <= kMaxWidth &&
            it.alpha >= 0.9f)
            p.push_back(it);
    std::sort(p.begin(), p.end(), [](const Item& a, const Item& b) { return a.x0 + a.x1 < b.x0 + b.x1; });
    MenuList m;
    int lit = 0;
    for (size_t k = 0; k < p.size(); k++) {
        if (p[k].grey >= kLit) {
            m.selected = int(k);
            lit++;
        } else if (p[k].grey > kGreyed) {
            return {};  // neither lit nor greyed: not the character select
        }
    }
    if (p.size() < 2 || lit != 1) return {};
    m.horizontal = true;
    for (size_t k = 0; k < p.size(); k++) {
        // the portraits overlap: each takes the screen to halfway to its neighbours' centres
        float c = MidX(p[k]);
        float l = k ? (c + MidX(p[k - 1])) / 2 : p[k].x0;
        float r = k + 1 < p.size() ? (c + MidX(p[k + 1])) / 2 : p[k].x1;
        m.rows.push_back({l, r, std::max(p[k].y0, 0.0f), std::min(p[k].y1, 480.0f)});
    }
    return m;
}

}  // namespace

void MenuQuad(uint32_t texture, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    if (logx::kVerbose && y1 - y0 < 60 && y1 - y0 > 8 && x1 - x0 > 20) {  // DIAG
        static int64_t logged = 0;
        static int n = 0;
        if (NowMs() - logged > 3000) logged = NowMs(), n = 0;
        if (n++ < 40) LOGI("menu quad %08X %.0f,%.0f..%.0f,%.0f rgba %.2f %.2f %.2f %.2f", texture, x0, y0, x1, y1, r, g, b, a);
    }
    // untinted images only: menu text is white, a portrait white or greyed; the art around them is tinted
    if (!texture || a <= 0 || std::fabs(r - g) > 0.02f || std::fabs(g - b) > 0.02f || r < 0.1f) return;
    Item& it = g_items[{texture, int(std::lround(y0 / 4))}];
    it.x0 = std::min(it.x0, x0), it.y0 = std::min(it.y0, y0);
    it.x1 = std::max(it.x1, x1), it.y1 = std::max(it.y1, y1);
    it.alpha = std::max(it.alpha, a);
    it.grey = std::max(it.grey, r);
}

void MenuFrame() {
    MenuList m = Find();
    if (m.rows.empty()) m = FindPortraits();
    g_items.clear();
    if (m.rows.empty()) return;
    // as the highlight moves a few frames read the first row brightest: a highlight counts once kStableFrames
    // frames in a row agree on it
    constexpr int kStableFrames = 5;
    static int raw = -1, stable = -1, same = 0;
    same = m.selected == raw ? same + 1 : 1;
    raw = m.selected;
    if (same >= kStableFrames) stable = raw;
    if (stable < 0 || stable >= int(m.rows.size())) return;
    m.selected = stable;
    if (!m.horizontal) TrackSelection(m.selected, m);
    std::lock_guard lock(g_lock);
    g_menu = std::move(m);
    g_menu_ms = NowMs();
}

bool CurrentMenu(MenuList& out) {
    std::lock_guard lock(g_lock);
    if (g_menu.rows.empty() || NowMs() - g_menu_ms > kStaleMs) return false;
    out = g_menu;
    return true;
}

}  // namespace game
