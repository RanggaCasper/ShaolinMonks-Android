// Mortal Kombat: Shaolin Monks needs no patch of its code to run: the XDK it calls is all on the host. Its own
// hooks go here, [[hook]] entries in config/game.toml and X86_FUNC replacements of recompiled functions.
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/aspect.h"
#include "common/options.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"
#include "netplay.h"

namespace game {

// the disc has English, French, German, Spanish and Italian; Indonesian is the settings panel's alone
bool LanguageAvailable(const std::string& code) { return KnownLanguage(code); }

// no HUD hook to tell play from a menu: the touch controls stay up
bool HudShown() { return true; }

}  // namespace game

namespace app {

// the console's XC_LANGUAGE the game reads at boot, from the language setting as this run started
uint32_t ConsoleLanguage() {
    const std::string& l = game::BootLanguage();
    if (l == "deu") return 3;
    if (l == "fra") return 4;
    if (l == "esp") return 5;
    if (l == "ita") return 6;
    return 1;  // English, and Indonesian, which the disc lacks
}

}  // namespace app

// Gameplay steps the world by a fixed 1/60 s a frame (0x51003C), so under 60 fps it plays in slow motion. For PAL
// 50 Hz the game steps a frame without drawing it when this is 1 (0x1B0AF0 skips the render and the present),
// which gameplay's frame (0x0CB780) decides each frame, every sixth there. Frames behind the clock are skipped the
// same way, so the world keeps its speed and the frame rate drops instead. With FRAME SKIP off the game's own
// choice stands: slow motion under 60, as the console had it.
void FrameSkip(X86Context&, uint8_t*) {
    using clock = std::chrono::steady_clock;
    constexpr uint32_t kSkipDraw = 0x5CAB98;
    constexpr auto kStep = std::chrono::microseconds(16667);
    // a load, a pause in the background or a stall: no catching up on it
    constexpr auto kMaxLag = std::chrono::milliseconds(250);
    constexpr int kMaxSkips = 3;  // a frame drawn at least every fourth, the controls answer at 15 fps
    static clock::time_point due;
    static int skips = 0;
    auto now = clock::now();
    const bool on = game::Opt().frame_skip;
    bool late = false;
    if (on) {
        // ahead of the clock carries no credit: the pacing at the present keeps 60
        if (due > now || now - due > kMaxLag) due = now;
        late = now - due >= kStep && skips < kMaxSkips;
    } else {
        due = now;  // turned on again, it starts from here
        skips = 0;
    }
    // netplay first: it may wait for the friend's pad. This phone's own clock decides the skip: sharing it made the
    // faster phone skip as often as the slower one, and the game's own PAL skip leaves the world's steps as they are
    netplay::Step();
    if (!on) return;
    if (late) kernel::W32(kSkipDraw, 1);
    skips = kernel::R32(kSkipDraw) ? skips + 1 : 0;
    due += kStep;
}

// The cameras draw a view made for a 4:3 screen whatever the shape it is shown at: the screen's 640 by 480
// (0x1245DC), and 1.2 for gameplay's. Wider settings stretched it. A camera of about 4:3 gets the shown frame's
// aspect instead, the same height of view and more of the sides; render to texture cameras of other shapes keep
// theirs.
void WidenAspect(X86Context& ctx, uint8_t*) {
    if (!game::Ultrawide()) return;
    float aspect;
    std::memcpy(&aspect, &ctx.eax, 4);
    if (aspect < 1.15f || aspect > 1.4f) return;
    aspect = game::Aspect();
    std::memcpy(&ctx.eax, &aspect, 4);
}
