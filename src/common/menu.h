#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// the F10 settings panel, drawn by DrawUi (GameInfo::ui)
namespace app {

bool MenuOpen();
bool Capturing();  // a key binding waits for a key
void ToggleMenu();
void DrawUi(double fps);
inline constexpr uint32_t kFpsCapRefresh = 1;  // Settings::fps_cap: just under the display's refresh rate
void PaceFrame();  // app::Pace, with kFpsCapRefresh; game thread, after the present

// the panel's rows, for the game's own options menu
namespace rows {

enum class Kind { Heading, Value, Bind };
struct Row {
    Kind kind;
    int id;  // a bind row's input action
    const char* label;
};
// 0 graphics, 1 touch controls, 2 general, 3 mouse and keyboard, 4 cheats
const Row* Page(int page, size_t* count);

}  // namespace rows

}  // namespace app
