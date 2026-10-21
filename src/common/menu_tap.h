#pragma once
#include <cstdint>
#include <vector>

// The game's own menus, found in what it draws: a column of item images, left aligned, one drawn brighter than the
// rest (the highlight), or the character select's row of portraits, one in full colour and the rest greyed. The touch
// controls tap them (native/touch.cpp).
namespace game {

struct MenuRow {
    float x0, x1;  // guest pixels of the 640x480 frame
    float top, bottom;  // the row's band: halfway to its neighbours
};

struct MenuList {
    std::vector<MenuRow> rows;  // top to bottom, or left to right when horizontal
    int selected = -1;
    bool horizontal = false;  // the character select's portraits: left and right step it
};

// game thread: a screen space textured quad drawn this frame (guest pixels, its diffuse alpha)
void MenuQuad(uint32_t texture, float x0, float y0, float x1, float y1, float r, float g, float b, float a);
// game thread, at each present: the frame's quads become the current menu, or none
void MenuFrame();
// any thread: the menu shown lately; false when there is none
bool CurrentMenu(MenuList& out);

// The button prompts on the screen (src/game/prompts.cpp): an icon and its label, tapped as the button
enum PromptButton { kPromptA, kPromptB, kPromptX, kPromptY, kPromptL, kPromptR, kPromptGrey, kPromptBlack };
struct Prompt {
    int button;  // PromptButton, kPromptGrey never
    float x0, y0, x1, y1;  // guest pixels: the icon and its label
};
// game thread: a textured quad of the screen, its box in guest pixels and its texture coordinates (0 to 1)
void PromptQuad(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1);
void PromptFrame();  // game thread, at each present
bool CurrentPrompts(std::vector<Prompt>& out);  // any thread: false when none were drawn lately
// any thread: the button icons of the last frame, their boxes alone (the Black button's too, which no tap presses)
void CurrentIcons(std::vector<Prompt>& out);

}  // namespace game
