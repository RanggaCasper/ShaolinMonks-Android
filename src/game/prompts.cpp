// The button prompts the game draws, "A Accept", "B Exit", the L and R of a page's tabs: small icons told apart by
// their colour (A green, B red, X blue, Y yellow, L and R grey in pairs). A tap on one, or on its label to the right,
// presses that button (native/touch.cpp).
#include "common/menu_tap.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include "common/guest.h"
#include "core/log.h"

#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {
void DecodeBlocks(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h,
                  std::vector<uint8_t>& out);  // native/vulkan.cpp
}  // namespace gpu

namespace game {

namespace {

// an icon this frame: its box in guest pixels and what it presses
struct Icon {
    float x0, y0, x1, y1;
    int button;  // PromptButton
    bool font = false;  // a glyph of the font's atlas
};

constexpr float kMinIcon = 8, kMaxIcon = 48;  // guest pixels an icon's side measures
constexpr float kLabelReach = 120;            // how far right of its icon a prompt's label is taken to run
constexpr int64_t kStaleMs = 250;
constexpr int64_t kIconsStaleMs = 100;

std::vector<Icon> g_icons;  // game thread: this frame's
std::mutex g_lock;
std::vector<Prompt> g_prompts;
int64_t g_prompts_ms = 0;
std::vector<Prompt> g_icons_shown;  // the last frame's icons
int64_t g_icons_ms = 0;

// a texture's level 0 as BGRA, decoded once: the prompts' icons are the same few small textures every frame
struct Decoded {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bgra;  // empty when the format does not decode to BGRA
    bool blocks = false;        // DXT: the font's atlas, whose button glyphs sit among its letters
};
const Decoded* DecodedTexture(uint32_t header) {
    static std::map<std::pair<uint32_t, uint32_t>, Decoded> cache;
    gpu::TextureDesc d;
    if (!gpu::Describe(header, d) || d.width > 512 || d.height > 512 || d.cube || d.depth > 1) return nullptr;
    auto key = std::make_pair(d.data, kernel::R32(header + 12));
    auto it = cache.find(key);
    if (it != cache.end()) return &it->second;
    if (cache.size() > 64) cache.clear();
    Decoded& out = cache[key];
    gpu::Host host = gpu::Format(d.format).host;
    bool blocks = host == gpu::Host::BC1 || host == gpu::Host::BC2 || host == gpu::Host::BC3;
    if (host != gpu::Host::BGRA8 && !blocks) return &out;
    uint32_t pitch = 0;
    gpu::DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, out.bgra, pitch);
    out.w = d.width;
    out.h = d.height;
    out.blocks = blocks;
    if (blocks) {
        std::vector<uint8_t> raw;
        raw.swap(out.bgra);
        gpu::DecodeBlocks(host, raw, pitch, d.width, d.height, out.bgra);
        pitch = d.width * 4;
    }
    if (pitch != d.width * 4) out.bgra.clear();
    return &out;
}

// the button an icon's texels show, by their average colour where opaque; -1 for none
int Classify(uint32_t header, float u0, float v0, float u1, float v1) {
    const Decoded* t = DecodedTexture(header);
    if (!t || t->bgra.empty()) return -1;
    double r = 0, g = 0, b = 0, n = 0;
    constexpr int kSteps = 9;
    for (int j = 0; j < kSteps; j++)
        for (int i = 0; i < kSteps; i++) {
            float u = u0 + (u1 - u0) * (i + 0.5f) / kSteps, v = v0 + (v1 - v0) * (j + 0.5f) / kSteps;
            int x = std::clamp(int(u * t->w), 0, int(t->w) - 1), y = std::clamp(int(v * t->h), 0, int(t->h) - 1);
            const uint8_t* p = &t->bgra[(size_t(y) * t->w + x) * 4];
            if (p[3] < 128) continue;
            b += p[0], g += p[1], r += p[2], n++;
        }
    if (n < 4) return -1;
    double cover = n / (kSteps * kSteps);
    r /= n * 255, g /= n * 255, b /= n * 255;
    // doubles, as the channels are: a float's rounding up would fail r >= hi for the channel that is the highest
    double hi = std::max({r, g, b}), lo = std::min({r, g, b});
    // the Black button's: a dark grey disc, about the share of its square a circle takes
    bool disc = cover > 0.6 && cover < 0.9;
    if (hi < 0.3f) return hi >= 0.04f && hi - lo < 0.1f && disc ? kPromptBlack : -1;
    // the font's grey glyphs: the Black button's dark disc, the triggers' L and R told apart by their place in its
    // atlas; its letters are pure white and thin, no disc
    if (t->blocks && hi - lo < 0.12f) {
        if (!disc || hi >= 0.8f) return -1;
        if (hi < 0.45f) return kPromptBlack;  // the Black button's disc, darker in the font than the triggers' tabs
        if (std::fabs(v0 - 0.412f) > 0.01f) return -1;
        if (std::fabs(u0 - 0.738f) < 0.01f) return kPromptL;
        if (std::fabs(u0 - 0.820f) < 0.01f) return kPromptR;
        return -1;
    }
    if (hi - lo < 0.12f) return kPromptGrey;
    // a button is a disc: a square filled to its corners is a piece of the HUD's frame (the red around the portrait)
    if (cover > 0.95) return -1;
    if (g >= hi && g > r * 1.3 && g > b * 1.3) return kPromptA;
    // yellow: green near red's; the save slots' brown medallion has much less
    if (r >= hi && g > b * 1.6 && g > r * 0.75) return kPromptY;
    // red, even with the font's white letter over much of the disc; the save slots' brown medallion has more green
    if (r >= hi && g < r * 0.55 && b < r * 0.55) return kPromptB;
    if (b >= hi && b > r * 1.2) return kPromptX;
    return -1;
}

}  // namespace

void PromptQuad(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    float w = x1 - x0, h = y1 - y0;
    if (!texture || w < kMinIcon || h < kMinIcon || w > kMaxIcon || h > kMaxIcon || w > h * 1.6f || h > w * 1.6f)
        return;
    int button = Classify(texture, std::min(u0, u1), std::min(v0, v1), std::max(u0, u1), std::max(v0, v1));
    const Decoded* t = DecodedTexture(texture);
    if (button >= 0) g_icons.push_back({x0, y0, x1, y1, button, t && t->blocks});
}

void PromptFrame() {
    std::vector<Icon> icons;
    icons.swap(g_icons);
    // an icon drawn twice (a shadow, a glow) counts once
    std::sort(icons.begin(), icons.end(), [](const Icon& a, const Icon& b) { return a.x0 < b.x0; });
    std::vector<Icon> unique;
    for (const Icon& i : icons) {
        bool same = std::any_of(unique.begin(), unique.end(), [&](const Icon& u) {
            return std::fabs(u.x0 - i.x0) < 4 && std::fabs(u.y0 - i.y0) < 4;
        });
        if (!same) unique.push_back(i);
    }
    // grey icons are the L and R of a pair on one row; a lone one is no prompt
    std::vector<Prompt> out, shown;
    for (size_t k = 0; k < unique.size(); k++) {
        Icon i = unique[k];
        if (i.button == kPromptGrey) {
            int pair = -1, count = 0;
            for (size_t j = 0; j < unique.size(); j++)
                if (j != k && unique[j].button == kPromptGrey && std::fabs(unique[j].y0 - i.y0) < 8) pair = int(j), count++;
            if (count != 1) continue;
            i.button = unique[size_t(pair)].x0 > i.x0 ? kPromptL : kPromptR;
        }
        shown.push_back({i.button, i.x0, i.y0, i.x1, i.y1});
        if (i.button == kPromptBlack || i.button == kPromptWhite) continue;
        // the label runs right to the next icon on the row
        float reach = i.x1 + kLabelReach;
        for (const Icon& n : unique)
            if (n.x0 > i.x1 && std::fabs(n.y0 - i.y0) < 8) reach = std::min(reach, n.x0 - 4);
        float pad = (i.y1 - i.y0) * 0.25f;
        out.push_back({i.button, i.x0 - pad, i.y0 - pad, reach, i.y1 + pad});
    }
    if (logx::kVerbose && !out.empty()) {
        static int64_t logged = 0;
        if (NowMs() - logged > 2000) {
            logged = NowMs();
            std::string l;
            static const char* kNames[] = {"A", "B", "X", "Y", "L", "R", "grey", "black", "white"};
            for (const Prompt& p : out) {
                char b[64];
                std::snprintf(b, sizeof b, " %s %.0f,%.0f..%.0f,%.0f", kNames[p.button], p.x0, p.y0, p.x1, p.y1);
                l += b;
            }
            LOGI("prompts:%s", l.c_str());
        }
    }
    std::lock_guard lock(g_lock);
    g_icons_shown = std::move(shown);
    g_icons_ms = NowMs();
    if (out.empty()) return;
    g_prompts = std::move(out);
    g_prompts_ms = NowMs();
}

void CurrentIcons(std::vector<Prompt>& out) {
    std::lock_guard lock(g_lock);
    if (NowMs() - g_icons_ms > kIconsStaleMs) out.clear();
    else out = g_icons_shown;
}

bool CurrentPrompts(std::vector<Prompt>& out) {
    std::lock_guard lock(g_lock);
    if (g_prompts.empty() || NowMs() - g_prompts_ms > kStaleMs) return false;
    out = g_prompts;
    return true;
}

}  // namespace game
