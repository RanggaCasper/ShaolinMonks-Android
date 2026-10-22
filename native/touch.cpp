// Android's input on port 0 beside the controllers: the touch controls, and a keyboard when one is plugged in, as an
// Xbox controller. It is src/common/mouse_keyboard.h. SDL's keys arrive as the virtual keys of common/keys.h.
//
// The left stick's side of the screen is the left stick, a drag on the other is the right stick and the buttons press
// the controller's: Shaolin Monks is played and its menus are driven with the pad alone.
#include "common/mouse_keyboard.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/aspect.h"
#include "common/guest.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/menu_tap.h"
#include "common/options.h"
#include "common/translations.h"
#include "host.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace input {

namespace {

std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<int> g_touch_dx{0};  // the touch screen's look, scaled by its own sensitivity
std::atomic<int> g_touch_dy{0};
std::atomic<uint64_t> g_capture_ms{0};  // PressedKey
std::atomic<bool> g_settle{false};
std::atomic<uint64_t> g_confirm_until{0};
std::atomic<bool> g_menu{false};
std::atomic<bool> g_keyboard{false};  // the keyboard or mouse used last
std::atomic<bool> g_touch{true};      // the touch controls used last, not a controller or the keys
std::atomic<bool> g_text{false};
std::atomic<bool> g_text_shown{false};  // the on-screen keyboard, main thread
std::mutex g_chars_lock;
std::u16string g_chars;  // typed, not yet taken
constexpr size_t kMaxChars = 64;
std::atomic<int> g_wheel{0};
constexpr uint64_t kTapMs = 80;
constexpr uint64_t kCaptureMs = 250;  // PressedKey asked this recently: a key capture is up
// The wheel binds as two keys, codes no key has. A notch holds what is bound to it for one tap.
constexpr int kWheelUp = 0x0E;
constexpr int kWheelDown = 0x0F;
std::atomic<uint64_t> g_wheel_ms[2] = {};  // the last notch up, down
// the keys and mouse buttons held, by virtual key
std::array<std::atomic<bool>, 256> g_down{};

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

// what an action presses: 0-7 analog A B X Y black white LT RT, 8-15 the buttons bits, 16-19 the move keys,
// 20-23 the look keys
struct Target {
    Action action;
    int button;
};
constexpr int kHostKey = -2;
constexpr int kWalk = -3;
constexpr int kFirstButtonBit = 8;
constexpr int kStart = 12;
constexpr int kFirstMoveKey = 16;
constexpr int kFirstLookKey = 20;
constexpr int kStickMax = 32767;
// the controller's buttons and sticks, each a key of its own
constexpr Target kActions[] = {
    {{"move_up", "LEFT STICK UP", "W"}, 16},
    {{"move_down", "LEFT STICK DOWN", "S"}, 17},
    {{"move_left", "LEFT STICK LEFT", "A"}, 18},
    {{"move_right", "LEFT STICK RIGHT", "D"}, 19},
    {{"walk", "WALK", "Alt"}, kWalk},
    {{"button_a", "A", "Space,K"}, 0},
    {{"button_b", "B", "L"}, 1},
    {{"button_x", "X", "J"}, 2},
    {{"button_y", "Y", "I"}, 3},
    {{"button_black", "BLACK", "O"}, 4},
    {{"button_white", "WHITE", "U"}, 5},
    {{"trigger_left", "LEFT TRIGGER", "Q"}, 6},
    {{"trigger_right", "RIGHT TRIGGER", "E"}, 7},
    {{"dpad_up", "DPAD UP", "Up"}, 8},
    {{"dpad_down", "DPAD DOWN", "Down"}, 9},
    {{"dpad_left", "DPAD LEFT", "Left"}, 10},
    {{"dpad_right", "DPAD RIGHT", "Right"}, 11},
    {{"start", "START", "Enter,Escape"}, kStart},
    {{"back", "BACK", "Backspace,Tab"}, 13},
    {{"thumb_left", "LEFT THUMB", "C"}, 14},
    {{"thumb_right", "RIGHT THUMB", "V"}, 15},
    {{"settings_panel", "SETTINGS PANEL", "F10"}, kHostKey},
    {{"look_up", "RIGHT STICK UP", "Numpad8"}, 20},
    {{"look_down", "RIGHT STICK DOWN", "Numpad5"}, 21},
    {{"look_left", "RIGHT STICK LEFT", "Numpad4"}, 22},
    {{"look_right", "RIGHT STICK RIGHT", "Numpad6"}, 23},
};
constexpr size_t kCount = std::size(kActions);
constexpr size_t HostAction(std::string_view id) {
    size_t i = 0;
    while (kActions[i].action.id != id) i++;
    return i;
}
constexpr size_t kPanelAction = HostAction("settings_panel");
struct Name {
    int vk;
    const char* name;
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

// held from before the game had the input, these wait for release as the bound keys do
constexpr int kFixed[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};

using Binds = std::array<std::vector<int>, kCount>;
std::mutex g_bind_lock;
Binds g_binds;

int Vk(const std::string& name) {
    for (const Name& n : kNames)
        if (strcasecmp(n.name, name.c_str()) == 0) return n.vk;
    return 0;
}

const char* NameOf(int vk) {
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    return nullptr;
}

std::vector<int> Parse(const std::string& list) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        if (int vk = Vk(list.substr(start, comma - start))) out.push_back(vk);
        start = comma + 1;
    }
    return out;
}

std::string Join(const std::vector<int>& vks) {
    std::string s;
    for (int vk : vks) {
        if (!s.empty()) s += ',';
        s += NameOf(vk);
    }
    return s;
}

bool Active() { return host::Foreground() && !app::MenuOpen(); }
bool Wheel(int vk) { return vk == kWheelUp || vk == kWheelDown; }
bool Down(int vk) {
    if (Wheel(vk)) return NowMs() - g_wheel_ms[vk == kWheelDown] < kTapMs;
    return vk > 0 && vk < 256 && g_down[size_t(vk)];
}
bool Mouse(int vk) {
    bool button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    return button || Wheel(vk);
}
bool Play() { return !g_menu; }  // no game hook sets a menu: always in play
bool GameDown(int vk) { return Down(vk) && !(g_menu && vk == VK_DELETE); }
bool KeyCaptureUp() { return NowMs() - g_capture_ms <= kCaptureMs; }

bool HostKeyBound(size_t action, int vk) {
    if (KeyCaptureUp()) return false;
    std::lock_guard lock(g_bind_lock);
    const auto& b = g_binds[action];
    return std::find(b.begin(), b.end(), vk) != b.end();
}

// keys held from before the game had the input wait for release; true once none is held
bool UpdateSettle(const Binds& binds) {
    if (!g_settle) return true;
    for (auto& b : binds)
        for (int vk : b)
            if (Down(vk)) return false;
    for (int vk : kFixed)
        if (Down(vk)) return false;
    g_settle = false;
    return true;
}

struct Held {
    bool used = false;
    bool walk = false;
    int stick[8] = {};  // move forward back left right, look up down left right
};

Held PressActions(const Binds& binds, bool play, Pad& p) {
    Held held;
    auto pressed = [play](int vk) { return GameDown(vk) && (play || !Wheel(vk)); };
    static bool pause_held;
    static bool pause_backs;
    bool pause = false;
    for (size_t i = 0; i < kCount; i++) {
        int b = kActions[i].button;
        if (b == kHostKey || !std::any_of(binds[i].begin(), binds[i].end(), pressed)) continue;
        held.used = true;
        if (g_text && b != kStart) continue;
        if (b == kWalk) {
            held.walk = true;
        } else if (b >= kFirstLookKey && !play) {
            continue;
        } else if (b >= kFirstMoveKey) {
            held.stick[b - kFirstMoveKey] = 1;
        } else if (b < kFirstButtonBit) {
            p.analog[b] = 0xFF;
        } else if (b == kStart) {
            if (!pause_held) pause_backs = g_menu;
            pause = true;
            if (!pause_backs) p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
            else if (g_menu) p.analog[1] = 0xFF;
        } else {
            p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
        }
    }
    pause_held = pause;
    return held;
}

// SDL's scancodes as the virtual keys (common/keys.h) the bindings name, by position as on a US keyboard
int VkOf(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
        case SDL_SCANCODE_0: return '0';
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return VK_SHIFT;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return VK_MENU;
        case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
        case SDL_SCANCODE_KP_PLUS: return VK_ADD;
        case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        default: return 0;
    }
}

int VkOfButton(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return VK_LBUTTON;
        case SDL_BUTTON_RIGHT: return VK_RBUTTON;
        case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
        case SDL_BUTTON_X1: return VK_XBUTTON1;
        case SDL_BUTTON_X2: return VK_XBUTTON2;
        default: return 0;
    }
}

}  // namespace

void ReleaseCursor() { g_settle = true; }

void Keyboard(Pad& p) {
    if (!Active()) {
        g_settle = true;
        return;
    }
    Binds binds;
    {
        std::lock_guard lock(g_bind_lock);
        binds = g_binds;
    }
    if (!UpdateSettle(binds)) return;
    bool play = Play();
    bool fixed_down = std::any_of(std::begin(kFixed), std::end(kFixed), Down);
    Held held = PressActions(binds, play, p);
    bool confirm = !play && !g_text && (Down(VK_RETURN) || Down(VK_SPACE));
    if (confirm || NowMs() < g_confirm_until) p.analog[0] = 0xFF;
    if ((!play && !g_text && Down(VK_BACK)) || (g_menu && Down(VK_RBUTTON))) p.analog[1] = 0xFF;
    if (!play) {
        for (int i = 0; i < 4; i++) p.buttons |= uint16_t(Down(kFixed[i]) << i);
    }
    const int* stick = held.stick;
    float speed = held.walk ? float(game::Opt().walk_speed) / 100.0f : 1.0f;
    if (int x = stick[3] - stick[2]) p.lx = int16_t(float(x) * speed * float(kStickMax));
    if (int y = stick[0] - stick[1]) p.ly = int16_t(float(y) * speed * float(kStickMax));
    if (int x = stick[7] - stick[6]) p.rx = int16_t(x * kStickMax);
    if (int y = stick[4] - stick[5]) p.ry = int16_t(y * kStickMax);
    if (fixed_down || held.used) g_keyboard = true;
}

void SeenPad(const Pad& p) {
    constexpr int16_t kStickPushed = 16000;
    constexpr uint8_t kAnalogPushed = 0x20;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    auto pressed = [](uint8_t a) { return a > kAnalogPushed; };
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), pressed);
    bool sticks = pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry);
    if (p.buttons || analog || sticks) g_keyboard = false;
}

bool KeyboardActive() { return g_keyboard; }

void OnChar(unsigned c) {
    if (!g_text || !Active() || c > 0xFFFF) return;
    std::lock_guard lock(g_chars_lock);
    if (g_chars.size() < kMaxChars) g_chars += char16_t(c);
}

const Action* Actions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kCount> a{};
        for (size_t i = 0; i < kCount; i++) a[i] = kActions[i].action;
        return a;
    }();
    *count = kCount;
    return list.data();
}

std::string Binding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kCount ? Join(g_binds[action]) : std::string();
}

void Bind(size_t action, int vk, int slot) {
    if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) return;
    if (kActions[action].button == kHostKey && Mouse(vk)) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int>& keys = g_binds[action];
        int slots[2] = {0, 0};
        for (size_t i = 0; i < keys.size() && i < 2; i++) slots[i] = keys[i];
        if (vk && slots[1 - slot] == vk) slots[1 - slot] = 0;
        slots[slot] = vk;
        if (!slots[0] && !slots[1] && action == kPanelAction) return;
        keys.clear();
        for (int k : slots) {
            if (k) keys.push_back(k);
        }
        game::Opt().keys[kActions[action].action.id] = Join(keys);
    }
    app::Save();
}

void ResetBindings() {
    game::Opt().keys.clear();
    LoadBindings();
    app::Save();
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& keys = game::Opt().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto it = keys.find(kActions[i].action.id);
        g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
    }
}

int PressedKey() {
    g_capture_ms = NowMs();
    for (const Name& n : kNames)
        if (Down(n.vk)) return n.vk;
    return 0;
}

bool PanelKey(int vk) { return HostKeyBound(kPanelAction, vk); }
bool HudKey(int) { return false; }  // the game has no HUD toggle

}  // namespace input


// the touch controls
namespace touch {

namespace {

using input::NowMs;

// what a finger on the screen does
enum class Role : uint8_t { None, Stick, Look, Button, Cursor, Drag, Pinch, MenuTap, PromptTap };

// a control of the touch layout: what it shows and presses, on the controller (analog 0-7, button bits from 8) or on
// the host. Where it sits is the layout's, which the player edits and picks presets for
enum : int { kGyro = -2, kPanel = -1, kAnalogA = 0, kAnalogB = 1, kAnalogX = 2, kAnalogY = 3, kAnalogLT = 6, kAnalogRT = 7 };
constexpr int kBit = 8;  // kBit + n: buttons bit n (dpad up down left right, start, back, left thumb, right thumb)
enum class Icon : uint8_t { Text, Pause, Gear, Back, Gyro, Up, Down, Left, Right, Camera };
// Play while the player has the controls, Game outside the menus (cutscenes too), Menus in the menus and options
enum class When : uint8_t { Play, Menus, Game };
// how a control is drawn and touched: a circle, a shoulder's tab, a small pill, the dpad's rounded square. Its place's
// r is a circle's radius, a tab's or a pill's half height
enum class Shape : uint8_t { Round, Tab, Pill, Square };
struct Button {
    const char* id;     // its key in layout.toml
    const char* label;  // in the layout editor
    const char* ps2;    // the PS2's name of it, shown with the PS2's buttons; nullptr for the same
    Icon icon;
    int press;
    When when;
    Shape shape = Shape::Round;
    const char* caption = nullptr;  // what it does in the game, under a face button
    bool look = false;  // a drag from it turns the view as well, as a fire button does
    bool keep = false;  // never hidden: the way back out of the menus
    bool rt = false;    // RT held with it: a special, one touch for the trigger and a face button
};
// the layout is set in the settings panel (TOUCH CONTROLS): its preset, opacity, size, and the editor that moves the
// controls, see the functions at the end. A Text control shows its label.
constexpr Button kButtons[] = {
    {"a", "A", "CROSS", Icon::Text, kAnalogA, When::Game, Shape::Round, "JUMP"},
    {"b", "B", "CIRCLE", Icon::Text, kAnalogB, When::Game, Shape::Round, "POWER"},
    {"x", "X", "SQUARE", Icon::Text, kAnalogX, When::Game, Shape::Round, "QUICK"},
    {"y", "Y", "TRIANGLE", Icon::Text, kAnalogY, When::Game, Shape::Round, "LAUNCH"},
    {"black", "BLACK", "R2", Icon::Text, 4, When::Game, Shape::Tab},
    {"white", "WHITE", "L2", Icon::Text, 5, When::Game, Shape::Tab},
    {"lt", "LT", "L1", Icon::Text, kAnalogLT, When::Game, Shape::Tab},
    {"rt", "RT", "R1", Icon::Text, kAnalogRT, When::Game, Shape::Tab},
    {"start", "START", nullptr, Icon::Text, kBit + 4, When::Game, Shape::Pill},
    {"back", "BACK", "SELECT", Icon::Text, kBit + 5, When::Game, Shape::Pill},
    {"panel", "Settings", nullptr, Icon::Gear, kPanel, When::Game, Shape::Round, nullptr, false, true},
    {"dpad_up", "Up", nullptr, Icon::Up, kBit + 0, When::Game, Shape::Square},
    {"dpad_down", "Down", nullptr, Icon::Down, kBit + 1, When::Game, Shape::Square},
    {"dpad_left", "Left", nullptr, Icon::Left, kBit + 2, When::Game, Shape::Square},
    {"dpad_right", "Right", nullptr, Icon::Right, kBit + 3, When::Game, Shape::Square},
    // the right stick's click, which turns the camera to behind the player
    {"camera", "Camera", nullptr, Icon::Camera, kBit + 7, When::Game, Shape::Round},
    // the specials, RT with X, Y, A or B; hidden in the presets but the RPG one, shown from the layout editor
    {"special1", "1", nullptr, Icon::Text, kAnalogX, When::Game, Shape::Round, "SPECIAL", false, false, true},
    {"special2", "2", nullptr, Icon::Text, kAnalogY, When::Game, Shape::Round, "SPECIAL", false, false, true},
    {"special3", "3", nullptr, Icon::Text, kAnalogA, When::Game, Shape::Round, "SPECIAL", false, false, true},
    {"special4", "4", nullptr, Icon::Text, kAnalogB, When::Game, Shape::Round, "SPECIAL", false, false, true},
};
constexpr size_t kButtonCount = std::size(kButtons);
constexpr size_t kStick = kButtonCount;  // the sticks' places, after the buttons'
constexpr size_t kRStick = kButtonCount + 1;
constexpr size_t kPlaces = kButtonCount + 2;
bool IsStick(size_t i) { return i >= kStick; }
const char* PlaceId(size_t i) { return i == kStick ? "stick" : i == kRStick ? "rstick" : kButtons[i].id; }

// a control the layout may hide: a button but the settings gear, the way back out of the menus, and the right stick
// (its drags then turn nothing); the left stick stays, moved where the player likes
bool Hideable(int i) {
    if (i < 0) return false;
    return size_t(i) == kRStick || (!IsStick(size_t(i)) && !kButtons[size_t(i)].keep);
}

// where a control sits: its centre as shares of the screen's width and height, its radius in screen heights
struct Place {
    float x, y, r;
    bool hidden = false;
};
using Layout = std::array<Place, kPlaces>;
// The presets. Each lists the places of a b x y, black white, lt rt, start back, the settings, the dpad's up down left
// right, the camera, the specials 1 to 4, the left stick and the right stick.
// L2 over L1 and R2 over R1 in the top corners (White and Black are the L2 and R2), the dpad and the face buttons
// halfway down the sides, a stick under each, Select and Start at the bottom and the settings at the top in the middle
constexpr Layout kStandard = {{
    {0.885f, 0.62f, 0.062f}, {0.945f, 0.47f, 0.062f}, {0.825f, 0.47f, 0.062f}, {0.885f, 0.32f, 0.062f},
    {0.925f, 0.075f, 0.045f}, {0.075f, 0.075f, 0.045f},
    {0.075f, 0.19f, 0.045f}, {0.925f, 0.19f, 0.045f},
    {0.565f, 0.92f, 0.035f}, {0.435f, 0.92f, 0.035f}, {0.50f, 0.075f, 0.045f},
    {0.115f, 0.36f, 0.05f}, {0.115f, 0.58f, 0.05f}, {0.065f, 0.47f, 0.05f}, {0.165f, 0.47f, 0.05f},
    {0.74f, 0.32f, 0.045f},
    {0.66f, 0.30f, 0.05f, true}, {0.66f, 0.47f, 0.05f, true}, {0.66f, 0.64f, 0.05f, true}, {0.73f, 0.20f, 0.05f, true},
    {0.26f, 0.78f, 0.12f}, {0.74f, 0.78f, 0.12f},
}};
// the shoulders as a row of tabs along the top, Back, the settings and Start between them, a small dpad under the
// left tabs, the face buttons low under the right thumb
constexpr Layout kTopBar = {{
    {0.865f, 0.80f, 0.068f}, {0.945f, 0.62f, 0.068f}, {0.785f, 0.62f, 0.068f}, {0.865f, 0.44f, 0.068f},
    {0.19f, 0.085f, 0.048f}, {0.81f, 0.085f, 0.048f},
    {0.068f, 0.085f, 0.048f}, {0.932f, 0.085f, 0.048f},
    {0.585f, 0.075f, 0.036f}, {0.415f, 0.075f, 0.036f}, {0.50f, 0.075f, 0.045f},
    {0.10f, 0.25f, 0.042f}, {0.10f, 0.43f, 0.042f}, {0.06f, 0.34f, 0.042f}, {0.14f, 0.34f, 0.042f},
    {0.70f, 0.44f, 0.045f},
    {0.66f, 0.30f, 0.05f, true}, {0.66f, 0.47f, 0.05f, true}, {0.66f, 0.64f, 0.05f, true}, {0.73f, 0.20f, 0.05f, true},
    {0.15f, 0.74f, 0.13f}, {0.62f, 0.80f, 0.11f},
}};
// the standard's smaller and lower, the middle of the screen left clear
constexpr Layout kCompact = {{
    {0.90f, 0.78f, 0.052f}, {0.954f, 0.66f, 0.052f}, {0.846f, 0.66f, 0.052f}, {0.90f, 0.54f, 0.052f},
    {0.93f, 0.07f, 0.038f}, {0.07f, 0.07f, 0.038f},
    {0.07f, 0.17f, 0.038f}, {0.93f, 0.17f, 0.038f},
    {0.555f, 0.93f, 0.03f}, {0.445f, 0.93f, 0.03f}, {0.50f, 0.07f, 0.04f},
    {0.10f, 0.525f, 0.043f}, {0.10f, 0.715f, 0.043f}, {0.057f, 0.62f, 0.043f}, {0.143f, 0.62f, 0.043f},
    {0.78f, 0.54f, 0.038f},
    {0.66f, 0.30f, 0.05f, true}, {0.66f, 0.47f, 0.05f, true}, {0.66f, 0.64f, 0.05f, true}, {0.73f, 0.20f, 0.05f, true},
    {0.25f, 0.80f, 0.11f}, {0.72f, 0.80f, 0.11f},
}};
// the shoulders beside the dpad and the face buttons, for thumbs alone with no fingers on the top
constexpr Layout kLowShoulders = {{
    {0.885f, 0.62f, 0.062f}, {0.945f, 0.47f, 0.062f}, {0.825f, 0.47f, 0.062f}, {0.885f, 0.32f, 0.062f},
    {0.73f, 0.32f, 0.045f}, {0.27f, 0.36f, 0.045f},
    {0.27f, 0.50f, 0.045f}, {0.73f, 0.46f, 0.045f},
    {0.565f, 0.92f, 0.035f}, {0.435f, 0.92f, 0.035f}, {0.50f, 0.075f, 0.045f},
    {0.115f, 0.36f, 0.05f}, {0.115f, 0.58f, 0.05f}, {0.065f, 0.47f, 0.05f}, {0.165f, 0.47f, 0.05f},
    {0.80f, 0.17f, 0.045f},
    {0.66f, 0.30f, 0.05f, true}, {0.66f, 0.47f, 0.05f, true}, {0.66f, 0.64f, 0.05f, true}, {0.73f, 0.20f, 0.05f, true},
    {0.26f, 0.78f, 0.12f}, {0.74f, 0.78f, 0.12f},
}};
// the standard's bigger, for a tablet or big thumbs
constexpr Layout kLarge = {{
    {0.875f, 0.70f, 0.075f}, {0.952f, 0.53f, 0.075f}, {0.798f, 0.53f, 0.075f}, {0.875f, 0.36f, 0.075f},
    {0.92f, 0.08f, 0.052f}, {0.08f, 0.08f, 0.052f},
    {0.08f, 0.215f, 0.052f}, {0.92f, 0.215f, 0.052f},
    {0.57f, 0.92f, 0.04f}, {0.43f, 0.92f, 0.04f}, {0.50f, 0.08f, 0.05f},
    {0.12f, 0.375f, 0.058f}, {0.12f, 0.625f, 0.058f}, {0.064f, 0.50f, 0.058f}, {0.176f, 0.50f, 0.058f},
    {0.72f, 0.36f, 0.05f},
    {0.66f, 0.30f, 0.05f, true}, {0.66f, 0.47f, 0.05f, true}, {0.66f, 0.64f, 0.05f, true}, {0.73f, 0.20f, 0.05f, true},
    {0.28f, 0.80f, 0.13f}, {0.70f, 0.80f, 0.13f},
}};
// as a mobile action RPG: a big attack button in the bottom right corner, Power, Launch and Jump close around it and
// the four specials in a wider arc, the shoulders along the top, a small dpad under the left ones
constexpr Layout kRpg = {{
    {0.927f, 0.506f, 0.065f}, {0.763f, 0.76f, 0.065f}, {0.885f, 0.76f, 0.105f}, {0.815f, 0.539f, 0.065f},
    {0.81f, 0.085f, 0.048f}, {0.19f, 0.085f, 0.048f},
    {0.068f, 0.085f, 0.048f}, {0.932f, 0.085f, 0.048f},
    {0.585f, 0.075f, 0.036f}, {0.415f, 0.075f, 0.036f}, {0.50f, 0.075f, 0.045f},
    {0.10f, 0.25f, 0.042f}, {0.10f, 0.43f, 0.042f}, {0.06f, 0.34f, 0.042f}, {0.14f, 0.34f, 0.042f},
    {0.955f, 0.27f, 0.04f},
    {0.673f, 0.76f, 0.055f}, {0.702f, 0.525f, 0.055f}, {0.779f, 0.353f, 0.055f}, {0.878f, 0.29f, 0.055f},
    {0.15f, 0.74f, 0.13f}, {0.55f, 0.75f, 0.10f},
}};
struct Preset {
    const char* id;
    const char* label;
    const Layout* places;
};
constexpr Preset kPresets[] = {{"default", "STANDARD", &kStandard},
                               {"top_bar", "TOP BAR", &kTopBar},
                               {"compact", "COMPACT", &kCompact},
                               {"low_shoulders", "LOW SHOULDERS", &kLowShoulders},
                               {"large", "LARGE", &kLarge},
                               {"rpg", "RPG", &kRpg}};

constexpr float kStickZone = 0.42f;    // share of the width from the left a touch takes the stick in
constexpr float kFixedReach = 1.5f;    // FIXED sticks: their radii from the centre a touch takes them in
constexpr float kLookCounts = 1800.0f;  // mouse counts a swipe across the screen's width turns: 90 degrees
constexpr uint64_t kShownMs = 8000;     // the controls hide this long after the last touch while a pad is plugged in

// a touch in the menus is a tap, a swipe up or down that scrolls, or the cursor held down (a slider dragged): which,
// once it moved or stayed long enough to tell
enum class MenuTouch : uint8_t { Undecided, Cursor, Scroll };
constexpr float kMenuMove = 0.03f;     // screen heights a finger moves before it is a swipe or a drag
constexpr float kScrollNotch = 0.05f;  // screen heights of a swipe a row scrolls, about a row of the menus
constexpr int kMaxNotches = 8;         // notches a swipe runs ahead of the menu

struct Finger {
    SDL_FingerID id = 0;
    Role role = Role::None;
    size_t button = 0;     // or kStick, dragged in the editor
    float x0 = 0, y0 = 0;  // where it went down, pixels; in the editor, its offset from the control's centre
    float x = 0, y = 0;    // where it is
    MenuTouch menu = MenuTouch::Undecided;
    int menu_row = -1;  // MenuTap: the game menu's row it came down on; PromptTap: the pad press of the prompt
    uint64_t down_ms = 0;
    float notch_y = 0;  // a scroll's place at its last notch
};

std::mutex g_lock;
std::vector<Finger> g_fingers;
bool g_pressed[kButtonCount] = {};
float g_stick_x = 0, g_stick_y = 0;  // -1 to 1, y up
float g_rstick_x = 0, g_rstick_y = 0;  // the right stick, a drag on the right of the screen
float g_cursor_x = 0, g_cursor_y = 0;  // the menu cursor, pixels
bool g_cursor_down = false;
bool g_cursor_touch = false;  // the cursor follows a finger, not the mouse
uint64_t g_cursor_tap_until = 0;  // a tap holds the cursor down this long, so the menu sees a click
std::atomic<uint64_t> g_touch_ms{0};
std::atomic<uint64_t> g_back_until{0};  // the back key's tap of Start or B lasts until then
constexpr uint64_t kBackTapMs = 120;

// the layout in use, under g_lock
Layout g_layout = kStandard;
size_t g_preset = 0;
float g_opacity = 0.85f;
float g_scale = 1.0f;  // every control's size, on top of its own
// STICK MODE: floating, the left stick's side of the screen the left stick and a drag on the other the right stick
// from wherever the thumb lands; fixed, each stick where it is drawn alone, so a stray touch elsewhere moves nothing
bool g_fixed_sticks = false;
bool g_loaded = false;
// the editor: the layout before it opened (for Cancel), the control picked
std::atomic<bool> g_editing{false};
Layout g_before = kStandard;
float g_before_opacity = 0.85f;
size_t g_before_preset = 0;
int g_selected = -1;

float Width() {
    int w, h;
    host::WindowPixels(w, h);
    return float(std::max(w, 1));
}
float Height() {
    int w, h;
    host::WindowPixels(w, h);
    return float(std::max(h, 1));
}

std::filesystem::path LayoutFile() { return app::ExeDir() / "layout.toml"; }
// the presets' places changed: a layout saved before keeps its opacity and size but not its places
constexpr int64_t kLayoutVersion = 3;

void LoadLayout() {
    g_loaded = true;
    try {
        toml::table t = toml::parse_file(LayoutFile().string());
        std::string preset = t["preset"].value_or(std::string("default"));
        for (size_t i = 0; i < std::size(kPresets); i++)
            if (preset == kPresets[i].id) g_preset = i;
        g_layout = *kPresets[g_preset].places;
        g_opacity = std::clamp(float(t["opacity"].value_or(double(g_opacity))), 0.2f, 1.0f);
        g_scale = std::clamp(float(t["scale"].value_or(double(g_scale))), 0.5f, 1.5f);
        g_fixed_sticks = t["stick_mode"].value_or(std::string("floating")) == "fixed";
        if (t["version"].value_or(int64_t(1)) < kLayoutVersion) return;
        for (size_t i = 0; i < kPlaces; i++) {
            // the menus' controls are not in the editor: always the preset's
            if (!IsStick(i) && kButtons[i].when == When::Menus) continue;
            const toml::table* p = t["controls"][PlaceId(i)].as_table();
            if (!p) continue;
            Place& pl = g_layout[i];
            pl.x = std::clamp(float((*p)["x"].value_or(double(pl.x))), 0.0f, 1.0f);
            pl.y = std::clamp(float((*p)["y"].value_or(double(pl.y))), 0.0f, 1.0f);
            pl.r = std::clamp(float((*p)["r"].value_or(double(pl.r))), 0.02f, 0.25f);
            pl.hidden = (*p)["hidden"].value_or(false) && Hideable(int(i));
        }
    } catch (...) {
    }
}

void SaveLayout() {
    toml::table controls;
    for (size_t i = 0; i < kPlaces; i++) {
        const Place& p = g_layout[i];
        controls.insert(PlaceId(i),
                        toml::table{{"x", double(p.x)}, {"y", double(p.y)}, {"r", double(p.r)}, {"hidden", p.hidden}});
    }
    toml::table t{{"version", kLayoutVersion}, {"preset", kPresets[g_preset].id}, {"opacity", double(g_opacity)},
                  {"scale", double(g_scale)}, {"stick_mode", g_fixed_sticks ? "fixed" : "floating"},
                  {"controls", controls}};
    std::ofstream(LayoutFile()) << t << "\n";
}

void EnsureLayout() {
    if (!g_loaded) LoadLayout();
}

void OpenEditor() {
    g_before = g_layout;
    g_before_opacity = g_opacity;
    g_before_preset = g_preset;
    g_selected = -1;
    g_editing = true;
}

bool InMenus() { return input::g_menu; }

// The game's own menus (src/common/menu_tap.h) can be tapped, the pad's controls giving way over them; off: the
// controls stay up in the menus as on a pad, the stick and the dpad move through them
constexpr bool kMenuTaps = false;

// one of the game's own menus is up
bool MenuShown() {
    game::MenuList m;
    return game::CurrentMenu(m);
}
// one is up and taps take its rows
bool GameMenu() { return kMenuTaps && MenuShown(); }

bool ButtonShown(size_t i) {
    const Button& b = kButtons[i];
    if (g_editing) return b.when != When::Menus;
    if (g_layout[i].hidden) return false;
    if (GameMenu()) return b.keep;  // the settings gear alone
    if (b.when == When::Game) return !InMenus();
    if (b.when == When::Menus) return InMenus();
    // in play only while the player has the controls: none over a loading screen or a cutscene. A hint's pause stops
    // the look but waits for a button (reload, health pack), so the HUD still drawing keeps them up
    return !InMenus() && (input::Play() || game::HudShown());
}

void Centre(size_t i, float& cx, float& cy) {
    cx = g_layout[i].x * Width();
    cy = g_layout[i].y * Height();
}

float Radius(size_t i) { return g_layout[i].r * g_scale * Height(); }

// a control's half width and half height, pixels
constexpr float kTabAspect = 2.3f, kPillAspect = 2.1f;
void Extent(size_t i, float& hw, float& hh) {
    hh = Radius(i);
    Shape s = i < kButtonCount ? kButtons[i].shape : Shape::Round;
    hw = hh * (s == Shape::Tab ? kTabAspect : s == Shape::Pill ? kPillAspect : 1.0f);
}

// the editor's bar at the top: Reset, smaller and bigger for the picked control, hide or show it, Cancel, Done, in
// screen shares
constexpr const char* kBarLabels[] = {"RESET", "-", "+", "HIDE", "CANCEL", "DONE"};
enum { kBarReset, kBarSmaller, kBarBigger, kBarHide, kBarCancel, kBarDone, kBarCount };
constexpr float kBarY = 0.17f, kBarH = 0.09f, kBarW = 0.13f, kBarGap = 0.02f;
constexpr float kBarSizeW = 0.07f;                       // the - and + buttons
constexpr float kMinRadius = 0.02f, kMaxRadius = 0.25f;  // a control's own radius, in screen heights
constexpr float kSizeStep = 1.1f;                        // a tap on - or +

float BarWidth(int i) { return i == kBarSmaller || i == kBarBigger ? kBarSizeW : kBarW; }

// the bar's button under a point, -1 for none
int BarAt(float x, float y, float rect[4] = nullptr, int which = -1) {
    float w = Width(), h = Height();
    float total = (kBarCount - 1) * kBarGap;
    for (int i = 0; i < kBarCount; i++) total += BarWidth(i);
    float left = 0.5f - total / 2;
    for (int i = 0; i < kBarCount; i++) {
        float x0 = left * w, y0 = kBarY * h;
        float x1 = x0 + BarWidth(i) * w, y1 = y0 + kBarH * h;
        left += BarWidth(i) + kBarGap;
        if (i == which && rect) {
            rect[0] = x0, rect[1] = y0, rect[2] = x1, rect[3] = y1;
            return i;
        }
        if (which < 0 && x >= x0 && x <= x1 && y >= y0 && y <= y1) return i;
    }
    return -1;
}

void ApplyPreset(size_t i) {
    g_preset = i;
    g_layout = *kPresets[i].places;
    g_selected = -1;
}

void CloseEditor(bool keep) {
    if (keep) {
        SaveLayout();
    } else {
        g_layout = g_before;
        g_opacity = g_before_opacity;
        g_preset = g_before_preset;
    }
    g_selected = -1;
    g_editing = false;
}

// the button under a point, kButtonCount for none
size_t HitButton(float x, float y) {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (!ButtonShown(i)) continue;
        float cx, cy, hw, hh;
        Centre(i, cx, cy);
        Extent(i, hw, hh);
        if (kButtons[i].shape == Shape::Round) {
            float r = hh * 1.25f;  // a little slack around the drawn circle
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return i;
        } else if (std::fabs(x - cx) <= hw + hh * 0.2f && std::fabs(y - cy) <= hh * 1.2f) {
            return i;
        }
    }
    return kButtonCount;
}

// a stick's deflection from where the finger came down, its radius the stick's
void StickOf(const Finger& f, float& x, float& y, size_t stick = kStick) {
    float r = Radius(stick);
    float dx = (f.x - f.x0) / r, dy = (f.y - f.y0) / r;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1) {
        dx /= len;
        dy /= len;
    }
    x = dx;
    y = -dy;
}

void UpdateStick(const Finger& f) { StickOf(f, g_stick_x, g_stick_y); }

// FLOATING: the left stick's zone is the side of the screen it is placed on, kStickZone of the width at least and out
// to its place's reach, so a stick moved in the editor still takes the touches where it is drawn
bool InStickZone(float x) {
    float w = Width(), cx = g_layout[kStick].x * w, reach = Radius(kStick) * kFixedReach;
    if (g_layout[kStick].x <= 0.5f) return x < std::max(w * kStickZone, cx + reach);
    return x > std::min(w * (1 - kStickZone), cx - reach);
}

// FIXED sticks: the one a touch came down near, its deflection from the centre; kPlaces for none
size_t FixedStickAt(float x, float y) {
    for (size_t s : {kStick, kRStick}) {
        if (g_layout[s].hidden) continue;
        float cx = g_layout[s].x * Width(), cy = g_layout[s].y * Height(), r = Radius(s) * kFixedReach;
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return s;
    }
    return kPlaces;
}

// a dpad arrow held in the menus: a step as it comes down, then steps at a calm pace once held a while; the game
// steps on every poll the arrow is down, too quick to land on a row
constexpr uint64_t kStepMs = 120, kRepeatAfterMs = 450, kRepeatMs = 180;

bool DpadStep(uint64_t held) {
    if (held < kStepMs) return true;
    return held >= kRepeatAfterMs && (held - kRepeatAfterMs) % kRepeatMs < kStepMs / 2;
}

bool Dpad(size_t i) { return kButtons[i].press >= kBit && kButtons[i].press < kBit + 4 && kButtons[i].when == When::Menus; }

void Recount() {
    std::fill(std::begin(g_pressed), std::end(g_pressed), false);
    bool stick = false, rstick = false;
    for (const Finger& f : g_fingers) {
        if (f.role == Role::Button) g_pressed[f.button] = !Dpad(f.button) || DpadStep(NowMs() - f.down_ms);
        if (f.role == Role::Stick) stick = true;
        if (f.role == Role::Look) rstick = true;
    }
    if (!stick) g_stick_x = g_stick_y = 0;
    if (!rstick) g_rstick_x = g_rstick_y = 0;
}

// ---- the game's own menus (src/common/menu_tap.h): a tap on a row moves the highlight there and accepts it, as
// the pad's dpad and A would

constexpr uint64_t kMenuPressMs = 50;   // a press held this long, three frames of the menu
constexpr uint64_t kMenuGapMs = 40;      // let go this long at least, so the next press is a new one
constexpr uint64_t kMenuSettleMs = 450;  // then at most this long for the highlight to move before the next
constexpr uint64_t kMenuAcceptMs = 250;  // the highlight's move done before A: the menu takes no A while it slides
constexpr uint64_t kPortraitAcceptMs = 120;  // the character select swaps its portraits at once
constexpr int kMenuMaxPresses = 16;
constexpr float kMenuTapMove = 0.04f;    // screen heights a finger may move and still tap
// a tap's work: the row the highlight goes to, then A
int g_menu_target = -1;
bool g_menu_accept = false;
int g_menu_press = -1;       // the press held: kBit + dpad bit or kAnalogA
int g_menu_from = -1;        // the highlight when it began
int g_menu_presses = 0;
uint64_t g_menu_press_until = 0, g_menu_wait_until = 0;
uint64_t g_menu_reached = 0;  // when the highlight got to the target after a step, 0 before
bool g_menu_written = false;  // the index was set in the game's memory for this tap
// The game's menu pages: a stack at 0x6C9308, its depth there and the pages after it ([0x6C9308 + depth * 4]); a
// page's highlighted row at +0x38 (the main menu, 0x598430, keeps it at 0x598468)
constexpr uint32_t kMenuStack = 0x6C9308;
constexpr uint32_t kPageIndex = 0x38;

// the top page's index field, 0 when there is no page
uint32_t MenuIndexField() {
    uint32_t depth = kernel::R32(kMenuStack);
    if (depth < 1 || depth > 32) return 0;
    uint32_t page = kernel::R32(kMenuStack + depth * 4);
    return game::GuestMapped(page) ? page + kPageIndex : 0;
}

// a point on the screen in the 640x480 frame, as the presenter letterboxes it
bool GuestPoint(float px, float py, float& gx, float& gy) {
    float w = Width(), h = Height();
    float aspect = game::Aspect();
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    gx = (px - (w - vw) / 2) / vw * 640.0f;
    gy = (py - (h - vh) / 2) / vh * 480.0f;
    return gx >= 0 && gx < 640 && gy >= 0 && gy < 480;
}

int MenuRowAt(const game::MenuList& m, float px, float py) {
    float gx, gy;
    if (!GuestPoint(px, py, gx, gy)) return -1;
    for (size_t i = 0; i < m.rows.size(); i++) {
        const game::MenuRow& r = m.rows[i];
        float slack = m.horizontal ? 0.0f : 16.0f;
        if (gx >= r.x0 - slack && gx <= r.x1 + slack && gy >= r.top && gy < r.bottom) return int(i);
    }
    return -1;
}

// the prompt (src/common/menu_tap.h) under a point: its pad press (analog A B X Y LT RT), -1 for none
int PromptAt(float px, float py) {
    std::vector<game::Prompt> prompts;
    float gx, gy;
    if (!kMenuTaps || !game::CurrentPrompts(prompts) || !GuestPoint(px, py, gx, gy)) return -1;
    static constexpr int kPress[] = {kAnalogA, kAnalogB, kAnalogX, kAnalogY, kAnalogLT, kAnalogRT};
    for (const game::Prompt& p : prompts)
        if (p.button >= 0 && p.button < int(std::size(kPress)) && gx >= p.x0 && gx <= p.x1 && gy >= p.y0 && gy <= p.y1)
            return kPress[p.button];
    return -1;
}

// a tap on a prompt presses its button as the pad would, once
void PromptTapUp(const Finger& f) {
    if (std::hypot(f.x - f.x0, f.y - f.y0) > kMenuTapMove * Height() || f.menu_row < 0) return;
    LOGI("prompt tap: press %d", f.menu_row);
    g_menu_target = -1;
    g_menu_press = f.menu_row;
    g_menu_press_until = NowMs() + kMenuPressMs * 2;
}

void MenuTapUp(const Finger& f) {
    if (std::hypot(f.x - f.x0, f.y - f.y0) > kMenuTapMove * Height()) return;
    game::MenuList m;
    if (!game::CurrentMenu(m) || m.selected < 0 || f.menu_row < 0 || f.menu_row >= int(m.rows.size())) return;
    int steps = f.menu_row - m.selected;
    if (logx::kVerbose) {
        std::string rows;
        for (const auto& r : m.rows) rows += " " + std::to_string(int(r.top)) + ".." + std::to_string(int(r.bottom));
        float gx, gy;
        GuestPoint(f.x, f.y, gx, gy);
        LOGI("menu tap at %.0f %.0f: row %d of%s, selected %d, %d steps", gx, gy, f.menu_row, rows.c_str(), m.selected,
             steps);
        uint32_t depth = kernel::R32(kMenuStack), field = MenuIndexField();
        LOGI("menu tap: page stack depth %u, top page %08X, index field %08X = %u", depth,
             depth && depth < 33 ? kernel::R32(kMenuStack + depth * 4) : 0, field, field ? kernel::R32(field) : 0);
    }
    g_menu_target = f.menu_row;
    g_menu_accept = true;
    g_menu_presses = 0;
    g_menu_reached = 0;
    g_menu_written = false;
}

// the press due now, -1 for none: a step toward the target, held, then a wait for the highlight to move, so a press
// is never lost to the menu's own pace; A once the highlight is there
int MenuPress() {
    uint64_t now = NowMs();
    if (g_menu_press >= 0 && now < g_menu_press_until) return g_menu_press;
    g_menu_press = -1;
    if (g_menu_target < 0) return -1;
    game::MenuList m;
    if (!game::CurrentMenu(m) || m.selected < 0 || g_menu_presses >= kMenuMaxPresses) {
        g_menu_target = -1;
        return -1;
    }
    if (now < g_menu_press_until + kMenuGapMs) return -1;
    if (now < g_menu_wait_until && m.selected == g_menu_from) return -1;  // the last step not shown yet
    int press = -1;
    // the index written straight away when it is the menu's, then A at once: the menu opens the row without the
    // highlight's walk there (the game walks it a row at a time otherwise)
    if (uint32_t field = m.horizontal ? 0 : MenuIndexField();
        !g_menu_written && field && kernel::R32(field) == uint32_t(m.selected)) {
        kernel::W32(field, uint32_t(g_menu_target));
        g_menu_written = true;
        g_menu_target = -1;
        g_menu_press = kAnalogA;
        g_menu_press_until = now + kMenuPressMs;
        return kAnalogA;
    }
    if (m.selected != g_menu_target) {
        // dpad bits: up 0, down 1, left 2, right 3
        press = kBit + (m.horizontal ? (g_menu_target > m.selected ? 3 : 2) : (g_menu_target > m.selected ? 1 : 0));
    } else if (g_menu_accept) {
        // after steps, A once the highlight has settled on the row
        if (g_menu_presses && !g_menu_reached) g_menu_reached = now;
        if (g_menu_presses && now - g_menu_reached < (m.horizontal ? kPortraitAcceptMs : kMenuAcceptMs)) return -1;
        press = kAnalogA;
        g_menu_target = -1;
    } else {
        g_menu_target = -1;
        return -1;
    }
    g_menu_press = press;
    g_menu_from = m.selected;
    g_menu_presses++;
    g_menu_press_until = now + kMenuPressMs;
    g_menu_wait_until = now + kMenuPressMs + kMenuSettleMs;
    return press;
}

void Resize(size_t i, float r) { g_layout[i].r = std::clamp(r, kMinRadius, kMaxRadius); }

// the editor: a touch on the bar resets, resizes the picked control, cancels or saves; a second finger while one
// drags a control pinches it bigger or smaller; on a control picks it up to move it, anywhere else lets go of the pick
void EditDown(Finger& f, float x, float y) {
    int bar = BarAt(x, y);
    switch (bar) {
        case kBarReset:
            ApplyPreset(g_preset);
            return;
        case kBarSmaller:
        case kBarBigger:
            if (g_selected >= 0) {
                float r = g_layout[size_t(g_selected)].r;
                Resize(size_t(g_selected), bar == kBarBigger ? r * kSizeStep : r / kSizeStep);
            }
            return;
        case kBarHide:
            if (Hideable(g_selected)) g_layout[size_t(g_selected)].hidden = !g_layout[size_t(g_selected)].hidden;
            return;
        case kBarCancel:
            CloseEditor(false);
            return;
        case kBarDone:
            CloseEditor(true);
            return;
        default:
            break;
    }
    for (const Finger& d : g_fingers) {
        if (d.role != Role::Drag) continue;
        // the span between the fingers and the control's size as the pinch began
        f.role = Role::Pinch;
        f.button = d.button;
        f.x0 = std::max(std::hypot(x - d.x, y - d.y), 1.0f);
        f.y0 = g_layout[d.button].r;
        return;
    }
    // kPlaces for none: kButtonCount, HitButton's none, is the left stick's place
    size_t b = HitButton(x, y);
    if (b == kButtonCount) {
        b = kPlaces;
        for (size_t s : {kStick, kRStick}) {
            float sx = g_layout[s].x * Width(), sy = g_layout[s].y * Height(), sr = Radius(s);
            if (b == kPlaces && (x - sx) * (x - sx) + (y - sy) * (y - sy) <= sr * sr) b = s;
        }
    }
    if (b == kPlaces) {
        g_selected = -1;
        return;
    }
    g_selected = int(b);
    f.role = Role::Drag;
    f.button = b;
    f.x0 = x - g_layout[b].x * Width();
    f.y0 = y - g_layout[b].y * Height();
}

void FingerDown(SDL_FingerID id, float x, float y) {
    // Android gives a new finger the id of one lifted: one still here lost its lift, and kept, it would move with the
    // new finger (a stick pushed by a touch on the other side)
    std::erase_if(g_fingers, [id](const Finger& o) { return o.id == id; });
    Finger f;
    f.id = id;
    f.x0 = f.x = x;
    f.y0 = f.y = y;
    if (g_editing) {
        EditDown(f, x, y);
        g_fingers.push_back(f);
        return;
    }
    input::g_touch = true;
    size_t b = HitButton(x, y);
    if (b < kButtonCount) {
        f.role = Role::Button;
        f.button = b;
        f.down_ms = NowMs();
        if (kButtons[b].press == kPanel) app::ToggleMenu();
        if (kButtons[b].press == kGyro) gyro::NextMode();
    } else if (game::MenuList m; kMenuTaps && game::CurrentMenu(m) && (f.menu_row = MenuRowAt(m, x, y)) >= 0) {
        f.role = Role::MenuTap;
    } else if ((f.menu_row = PromptAt(x, y)) >= 0) {
        f.role = Role::PromptTap;
    } else if (InMenus()) {
        // not the cursor yet: a tap clicks where it lifts, a swipe scrolls without moving the cursor
        f.role = Role::Cursor;
        f.menu = MenuTouch::Undecided;
        f.down_ms = NowMs();
        g_cursor_touch = true;
    } else if (GameMenu()) {
        f.role = Role::None;  // no stick or look over a menu
    } else if (g_fixed_sticks) {
        size_t s = FixedStickAt(x, y);
        f.role = s == kStick ? Role::Stick : s == kRStick ? Role::Look : Role::None;
        if (s != kPlaces) {
            f.x0 = g_layout[s].x * Width();
            f.y0 = g_layout[s].y * Height();
            if (s == kStick) UpdateStick(f);
            else StickOf(f, g_rstick_x, g_rstick_y, kRStick);
        }
    } else if (InStickZone(x)) {
        f.role = Role::Stick;
    } else {
        f.role = g_layout[kRStick].hidden ? Role::None : Role::Look;
    }
    g_fingers.push_back(f);
    Recount();
}

// the finger becomes the cursor held down where it is
void MenuHold(Finger& f) {
    f.menu = MenuTouch::Cursor;
    g_cursor_x = f.x;
    g_cursor_y = f.y;
    g_cursor_down = true;
}

// a finger in the menus moved: up or down past kMenuMove scrolls, sideways is the cursor dragging; a scroll gives a
// wheel notch a kScrollNotch, a finger going up shows the rows below as on a phone's lists
void MenuMove(Finger& f) {
    float h = Height();
    float dx = f.x - f.x0, dy = f.y - f.y0;
    if (f.menu == MenuTouch::Undecided) {
        if (std::fabs(dy) > kMenuMove * h && std::fabs(dy) >= std::fabs(dx)) {
            f.menu = MenuTouch::Scroll;
            f.notch_y = f.y0;
        } else if (std::fabs(dx) > kMenuMove * h) {
            MenuHold(f);
        }
    }
    if (f.menu == MenuTouch::Cursor) {
        g_cursor_x = f.x;
        g_cursor_y = f.y;
    }
    if (f.menu != MenuTouch::Scroll) return;
    float notch = kScrollNotch * h;
    while (f.y <= f.notch_y - notch) {
        f.notch_y -= notch;
        if (input::g_wheel > -kMaxNotches) input::g_wheel--;
    }
    while (f.y >= f.notch_y + notch) {
        f.notch_y += notch;
        if (input::g_wheel < kMaxNotches) input::g_wheel++;
    }
}

void FingerMove(SDL_FingerID id, float x, float y) {
    for (Finger& f : g_fingers) {
        if (f.id != id) continue;
        float dx = x - f.x, dy = y - f.y;
        f.x = x;
        f.y = y;
        if (f.role == Role::Drag) {
            Place& p = g_layout[f.button];
            p.x = std::clamp((x - f.x0) / Width(), 0.0f, 1.0f);
            p.y = std::clamp((y - f.y0) / Height(), 0.0f, 1.0f);
        }
        // a pinch sizes the control by the span between its two fingers, whichever moved
        if (f.role == Role::Drag || f.role == Role::Pinch) {
            for (const Finger& o : g_fingers) {
                const Finger& pinch = f.role == Role::Pinch ? f : o;
                const Finger& drag = f.role == Role::Pinch ? o : f;
                if (&o == &f || pinch.role != Role::Pinch || drag.role != Role::Drag || pinch.button != drag.button) continue;
                Resize(f.button, pinch.y0 * std::hypot(pinch.x - drag.x, pinch.y - drag.y) / pinch.x0);
            }
        }
        if (f.role == Role::Stick) UpdateStick(f);
        if (f.role == Role::Look) StickOf(f, g_rstick_x, g_rstick_y, kRStick);
        if (f.role == Role::Button && kButtons[f.button].look) {
            float k = kLookCounts / Width();
            input::g_touch_dx += int(std::lround(dx * k));
            input::g_touch_dy += int(std::lround(dy * k));
        }
        if (f.role == Role::Cursor) MenuMove(f);
        break;
    }
}

void FingerUp(SDL_FingerID id) {
    for (const Finger& f : g_fingers)
        if (f.id == id && f.role == Role::MenuTap) MenuTapUp(f);
        else if (f.id == id && f.role == Role::PromptTap) PromptTapUp(f);
    for (const Finger& f : g_fingers) {
        if (f.id != id || f.role != Role::Cursor) continue;
        if (f.menu == MenuTouch::Undecided) {
            // a tap: the cursor goes there and clicks
            g_cursor_x = f.x;
            g_cursor_y = f.y;
            g_cursor_tap_until = NowMs() + input::kTapMs;
        }
        if (f.menu != MenuTouch::Scroll) g_cursor_down = false;
    }
    std::erase_if(g_fingers, [id](const Finger& f) { return f.id == id; });
    Recount();
}

bool PadPlugged() {
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) SDL_free(ids);
    return n > 0;
}

// verbose builds: F9 writes the image and the physical memory to dump_N.bin beside the settings, for diffing
void DumpGuest() {
    static int n = 0;
    std::filesystem::path p = app::ExeDir() / ("dump_" + std::to_string(n++) + ".bin");
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(kernel::HostPtr(0x10000)), 0x8C2000 - 0x10000);
    f.write(static_cast<const char*>(kernel::HostPtr(0x80000000u)), 64u << 20);
    LOGI("dump: %s", p.c_str());
}

void KeyEvent(const SDL_KeyboardEvent& k) {
    // Android's back key: out of the layout editor or the settings panel, else the game's pause menu (its options
    // are the settings), in the game's menus back out of them
    if (k.scancode == SDL_SCANCODE_AC_BACK) {
        if (!k.down || k.repeat) return;
        std::lock_guard lock(g_lock);
        if (g_editing) {
            CloseEditor(false);
        } else if (app::MenuOpen()) {
            app::ToggleMenu();
        } else {
            g_back_until = NowMs() + kBackTapMs;
        }
        return;
    }
    int vk = input::VkOf(k.scancode);
    if (!vk) return;
    input::g_down[size_t(vk)] = k.down;
    if (!k.down) return;
    input::g_keyboard = true;
    input::g_touch = false;
    if (logx::kVerbose && !k.repeat && vk == VK_F9) DumpGuest();
    if (!k.repeat && input::PanelKey(vk)) app::ToggleMenu();
    else if (!k.repeat && !app::MenuOpen() && input::HudKey(vk)) game::g_hide_hud = !game::g_hide_hud;
    else if (vk == VK_ESCAPE && app::MenuOpen() && !app::Capturing()) app::ToggleMenu();
    // a name being typed takes Backspace and Enter as WM_CHAR has them
    if (vk == VK_BACK) input::OnChar('\b');
    if (vk == VK_RETURN) input::OnChar('\r');
}

}  // namespace

bool OnEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            KeyEvent(e.key);
            return app::MenuOpen() ? false : true;
        case SDL_EVENT_TEXT_INPUT:
            for (const char* c = e.text.text; c && *c; c++)
                if (uint8_t(*c) < 0x80 && *c >= ' ') input::OnChar(unsigned(*c));
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) return true;
            input::g_keyboard = true;
            input::g_touch = false;
            if (input::Active() && input::Play()) {
                // the fractions carried over: a held mouse sends small moves, rounded each they were lost
                AddLook(e.motion.xrel, e.motion.yrel);
            } else {
                std::lock_guard lock(g_lock);
                g_cursor_x = e.motion.x;
                g_cursor_y = e.motion.y;
                g_cursor_touch = false;
            }
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) return true;
            if (int vk = input::VkOfButton(e.button.button)) input::g_down[size_t(vk)] = e.button.down;
            input::g_keyboard = true;
            input::g_touch = false;
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.y != 0) {
                int notch = e.wheel.y > 0 ? 1 : -1;
                input::g_wheel_ms[notch < 0] = NowMs();
                if (input::Active() && !input::Play() && !input::KeyCaptureUp()) input::g_wheel += notch;
            }
            return !app::MenuOpen();
        case SDL_EVENT_WILL_ENTER_BACKGROUND: {
            // the fingers' lifts may not come while the app is away: none is held when it is back
            std::lock_guard lock(g_lock);
            g_fingers.clear();
            g_cursor_down = false;
            Recount();
            return false;
        }
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            g_touch_ms = NowMs();
            float x = e.tfinger.x * Width(), y = e.tfinger.y * Height();
            std::lock_guard lock(g_lock);
            EnsureLayout();
            if (e.type == SDL_EVENT_FINGER_DOWN) {
                // the settings panel takes the touches, but for the button that closes it
                if (app::MenuOpen() && !g_editing) {
                    size_t b = HitButton(x, y);
                    if (b == kButtonCount || kButtons[b].press != kPanel) return false;
                }
                FingerDown(e.tfinger.fingerID, x, y);
                return true;
            }
            bool ours = std::any_of(g_fingers.begin(), g_fingers.end(),
                                    [&](const Finger& f) { return f.id == e.tfinger.fingerID; });
            if (!ours) return false;
            if (e.type == SDL_EVENT_FINGER_MOTION) FingerMove(e.tfinger.fingerID, x, y);
            else FingerUp(e.tfinger.fingerID);
            return true;
        }
        default:
            return false;
    }
}

void Apply(int port, input::Pad& pad) {
    if (port != 0 || g_editing) return;
    std::lock_guard lock(g_lock);
    Recount();  // a held dpad arrow's steps go by the clock
    bool used = false;
    for (size_t i = 0; i < kButtonCount; i++) {
        if (!g_pressed[i]) continue;
        int p = kButtons[i].press;
        if (p < 0) continue;
        used = true;
        if (p < kBit) pad.analog[p] = 0xFF;
        else pad.buttons |= uint16_t(1u << (p - kBit));
        if (kButtons[i].rt) pad.analog[kAnalogRT] = 0xFF;
    }
    if (int p = MenuPress(); p >= 0) {
        used = true;
        if (p < kBit) pad.analog[p] = 0xFF;
        else pad.buttons |= uint16_t(1u << (p - kBit));
    }
    if (NowMs() < g_back_until) {
        used = true;
        if (InMenus() || MenuShown()) pad.analog[kAnalogB] = 0xFF;
        else pad.buttons |= uint16_t(1u << 4);  // Start
    }
    if (g_stick_x || g_stick_y) {
        used = true;
        pad.lx = int16_t(std::clamp(g_stick_x, -1.0f, 1.0f) * 32767.0f);
        pad.ly = int16_t(std::clamp(g_stick_y, -1.0f, 1.0f) * 32767.0f);
    }
    if (g_rstick_x || g_rstick_y) {
        used = true;
        pad.rx = int16_t(std::clamp(g_rstick_x, -1.0f, 1.0f) * 32767.0f);
        pad.ry = int16_t(std::clamp(g_rstick_y, -1.0f, 1.0f) * 32767.0f);
    }
    if (used) pad.connected = true;
}

bool Shown() { return !PadPlugged() || NowMs() - g_touch_ms < kShownMs; }

namespace {

constexpr float kPi = 3.14159265f;

// an icon in white line art inside a button of radius r: points in units of r, from its centre, y down
struct Pen {
    ImDrawList* dl;
    ImVec2 c;
    float r;
    ImU32 col;
    float th;  // line thickness
    ImVec2 P(float x, float y) const { return ImVec2(c.x + x * r, c.y + y * r); }
    // a point turned by angle a about the centre
    ImVec2 R(float x, float y, float a) const {
        float s = std::sin(a), k = std::cos(a);
        return P(x * k - y * s, x * s + y * k);
    }
    void Line(float x0, float y0, float x1, float y1) const { dl->AddLine(P(x0, y0), P(x1, y1), col, th); }
    void Ring(float x, float y, float rad) const { dl->AddCircle(P(x, y), rad * r, col, 32, th); }
    void Dot(float x, float y, float rad) const { dl->AddCircleFilled(P(x, y), rad * r, col, 24); }
    void Box(float x0, float y0, float x1, float y1, float round = 0) const {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), col, round * r);
    }
};

void DrawIcon(const Pen& p, Icon icon) {
    switch (icon) {
        case Icon::Text:  // its label, Draw writes it
            break;
        case Icon::Pause:
            p.Box(-0.28f, -0.36f, -0.08f, 0.36f, 0.03f);
            p.Box(0.08f, -0.36f, 0.28f, 0.36f, 0.03f);
            break;
        case Icon::Gear:
            for (int i = 0; i < 8; i++) {
                float a = i * kPi / 4;
                p.dl->AddLine(p.R(0.3f, 0, a), p.R(0.52f, 0, a), p.col, p.th * 2.4f);
            }
            p.Dot(0, 0, 0.36f);
            p.dl->AddCircleFilled(p.c, 0.15f * p.r, IM_COL32(0, 0, 0, 200), 20);
            break;
        case Icon::Gyro: {  // a phone on its side between two turning arrows
            p.dl->AddRect(p.P(-0.32f, -0.2f), p.P(0.32f, 0.2f), p.col, 0.06f * p.r, 0, p.th);
            p.Dot(0.24f, 0, 0.035f);
            for (float side : {-1.0f, 1.0f}) {
                float a0 = side < 0 ? 0.62f * kPi : -0.38f * kPi, a1 = a0 + 0.76f * kPi * 0.5f;
                p.dl->PathArcTo(p.c, 0.5f * p.r, a0, a1, 12);
                p.dl->PathStroke(p.col, 0, p.th);
                ImVec2 tip = p.P(std::cos(a1) * 0.5f, std::sin(a1) * 0.5f);
                float t = a1 + kPi / 2;
                p.dl->AddTriangleFilled(tip, ImVec2(tip.x - std::cos(t - 0.5f) * 0.16f * p.r, tip.y - std::sin(t - 0.5f) * 0.16f * p.r),
                                        ImVec2(tip.x - std::cos(t + 0.5f) * 0.16f * p.r, tip.y - std::sin(t + 0.5f) * 0.16f * p.r), p.col);
            }
            break;
        }
        case Icon::Back:
        case Icon::Left:
            p.Line(0.14f, -0.36f, -0.22f, 0);
            p.Line(-0.22f, 0, 0.14f, 0.36f);
            break;
        case Icon::Right:
            p.Line(-0.14f, -0.36f, 0.22f, 0);
            p.Line(0.22f, 0, -0.14f, 0.36f);
            break;
        case Icon::Up:
            p.Line(-0.36f, 0.14f, 0, -0.22f);
            p.Line(0, -0.22f, 0.36f, 0.14f);
            break;
        case Icon::Down:
            p.Line(-0.36f, -0.14f, 0, 0.22f);
            p.Line(0, 0.22f, 0.36f, -0.14f);
            break;
        case Icon::Camera:  // a camera's body, its lens and the bump of its viewfinder
            p.dl->AddRect(p.P(-0.44f, -0.24f), p.P(0.44f, 0.32f), p.col, 0.08f * p.r, 0, p.th);
            p.Box(-0.18f, -0.36f, 0.12f, -0.24f, 0.03f);
            p.Ring(0, 0.04f, 0.17f);
            p.Dot(0.3f, -0.13f, 0.04f);
            break;
    }
}

// a colour at the layout's opacity
ImU32 Fade(int r, int g, int b, int a) { return IM_COL32(r, g, b, int(float(a) * g_opacity)); }

// the controls' look: dark glass edged in warm parchment, saffron when pressed, the face buttons rimmed in their colour
struct Rgb {
    int r, g, b;
};
constexpr Rgb kGlass = {28, 22, 16}, kEdge = {235, 205, 150}, kSaffron = {224, 138, 44}, kJade = {62, 158, 124},
              kCream = {240, 228, 205};
ImU32 Fade(Rgb c, int a) { return Fade(c.r, c.g, c.b, a); }

bool Ps2() { return game::Opt().ps2_buttons; }

// the face buttons' symbols on the PS2: cross, circle, square, triangle for A, B, X, Y, in their colours
constexpr Rgb kPs2Colours[] = {{124, 170, 232}, {228, 88, 78}, {216, 123, 176}, {79, 190, 160}};
void DrawPs2Symbol(ImDrawList* dl, ImVec2 c, float r, int face, ImU32 col, float th) {
    Pen p{dl, c, r, col, th};
    switch (face) {
        case 0:
            p.Line(-0.36f, -0.36f, 0.36f, 0.36f);
            p.Line(-0.36f, 0.36f, 0.36f, -0.36f);
            break;
        case 1: p.Ring(0, 0, 0.4f); break;
        case 2: dl->AddRect(p.P(-0.34f, -0.34f), p.P(0.34f, 0.34f), col, 0, 0, th); break;
        default:
            dl->AddTriangle(p.P(0, -0.42f), p.P(0.42f, 0.3f), p.P(-0.42f, 0.3f), col, th);
            break;
    }
}

// a control's name in the style shown
const char* Label(const Button& b) { return Ps2() && b.ps2 ? b.ps2 : b.label; }

// the Xbox controller's colour of a button: A green, B red, X blue, Y yellow, the triggers saffron, Black and White
// jade, the rest the edge's; the PS2's face buttons in theirs
Rgb ButtonColour(int press) {
    if (Ps2() && press >= kAnalogA && press <= kAnalogY) return kPs2Colours[press];
    switch (press) {
        case kAnalogA: return {95, 196, 110};
        case kAnalogB: return {224, 86, 76};
        case kAnalogX: return {111, 159, 224};
        case kAnalogY: return {232, 190, 72};
        case kAnalogLT:
        case kAnalogRT: return kSaffron;
        case 4:
        case 5: return kJade;
        default: return kEdge;
    }
}

void DrawStick(ImDrawList* dl, float th, size_t stick) {
    // a hidden right stick: faint and crossed out in the editor, else not drawn
    bool hidden = g_layout[stick].hidden;
    if (hidden && !g_editing) return;
    float opacity = g_opacity;
    if (hidden) g_opacity *= 0.4f;
    // at rest in its place, else where the thumb came down
    float r = Radius(stick);
    ImVec2 at(g_layout[stick].x * Width(), g_layout[stick].y * Height());
    Role role = stick == kStick ? Role::Stick : Role::Look;
    float sx = stick == kStick ? g_stick_x : g_rstick_x, sy = stick == kStick ? g_stick_y : g_rstick_y;
    bool held = false;
    for (const Finger& f : g_fingers)
        if (f.role == role && !g_editing) {
            at = ImVec2(f.x0, f.y0);
            held = true;
        }
    dl->AddCircleFilled(at, r, Fade(kGlass, held ? 165 : 120), 64);
    dl->AddCircle(at, r, Fade(kEdge, held ? 110 : 70), 64, th * 0.7f);
    dl->AddCircle(at, r * 0.7f, Fade(kEdge, 30), 64, th * 0.5f);
    for (int i = 0; i < 4; i++) {  // the four directions
        float a = i * kPi / 2;
        ImVec2 tip(at.x + std::cos(a) * r * 0.88f, at.y + std::sin(a) * r * 0.88f);
        ImVec2 l(at.x + std::cos(a + 0.12f) * r * 0.76f, at.y + std::sin(a + 0.12f) * r * 0.76f);
        ImVec2 rr(at.x + std::cos(a - 0.12f) * r * 0.76f, at.y + std::sin(a - 0.12f) * r * 0.76f);
        dl->AddTriangleFilled(tip, l, rr, Fade(kEdge, held ? 150 : 95));
    }
    ImVec2 knob(at.x + sx * r, at.y - sy * r);
    dl->AddCircleFilled(knob, r * 0.42f, held ? Fade(kSaffron, 150) : Fade(60, 48, 36, 170), 48);
    dl->AddCircle(knob, r * 0.42f, Fade(kEdge, held ? 200 : 120), 48, th * 0.8f);
    g_opacity = opacity;
    if (hidden) {
        float k = r * 0.35f;
        dl->AddLine(ImVec2(at.x - k, at.y - k), ImVec2(at.x + k, at.y + k), IM_COL32(255, 80, 60, 220), th);
        dl->AddLine(ImVec2(at.x - k, at.y + k), ImVec2(at.x + k, at.y - k), IM_COL32(255, 80, 60, 220), th);
    }
}

// a text centred on a point, its height a share of the screen's
void CentredText(ImDrawList* dl, ImVec2 at, float size, ImU32 colour, const char* text) {
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x - ts.x / 2, at.y - ts.y / 2), colour, text);
}

// a control in its shape: glass, its edge, its icon or label, a face button's caption under it. fade is the share
// (of 100) a hidden control shows at in the editor
void DrawControl(ImDrawList* dl, size_t i, bool on, int fade, float th) {
    const Button& b = kButtons[i];
    float cx, cy, hw, hh;
    Centre(i, cx, cy);
    Extent(i, hw, hh);
    if (on) hw *= 0.9f, hh *= 0.9f;  // pressed in
    ImVec2 c(cx, cy);
    Rgb colour = ButtonColour(b.press);
    ImU32 fill = on ? Fade(kSaffron, 120) : Fade(kGlass, 155 * fade / 100);
    switch (b.shape) {
        case Shape::Round: {
            bool face = b.caption != nullptr;
            dl->AddCircleFilled(c, hh, on && face ? Fade(colour, 110) : fill, 48);
            dl->AddCircle(c, hh, face ? Fade(colour, 200 * fade / 100) : Fade(kEdge, 70 * fade / 100), 48,
                          th * (face ? 1.1f : 0.7f));
            if (face) dl->AddCircle(c, hh * 0.82f, Fade(kEdge, 28 * fade / 100), 48, th * 0.5f);
            break;
        }
        case Shape::Tab:
        case Shape::Pill: {
            float round = b.shape == Shape::Pill ? hh : hh * 0.45f;
            ImVec2 p0(cx - hw, cy - hh), p1(cx + hw, cy + hh);
            dl->AddRectFilled(p0, p1, fill, round);
            dl->AddRect(p0, p1, Fade(kEdge, 70 * fade / 100), round, 0, th * 0.7f);
            if (b.shape == Shape::Tab)  // the trigger's or the button's colour along its foot
                dl->AddRectFilled(ImVec2(cx - hw * 0.55f, cy + hh * 0.62f), ImVec2(cx + hw * 0.55f, cy + hh * 0.62f + th * 1.3f),
                                  Fade(colour, 210 * fade / 100), th);
            break;
        }
        case Shape::Square: {
            float round = hh * 0.3f;
            dl->AddRectFilled(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), fill, round);
            dl->AddRect(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), Fade(kEdge, 60 * fade / 100), round, 0,
                        th * 0.6f);
            break;
        }
    }
    float icon = b.shape == Shape::Square ? hh * 0.85f : hh;
    DrawIcon({dl, c, icon, on ? Fade(255, 255, 255, 240) : Fade(kCream, 200 * fade / 100), th}, b.icon);
    if (Ps2() && !b.rt && b.press >= kAnalogA && b.press <= kAnalogY) {
        DrawPs2Symbol(dl, c, hh, b.press, on ? Fade(255, 255, 255, 255) : Fade(colour, 235 * fade / 100), th * 1.6f);
    } else if (b.icon == Icon::Text) {
        ImU32 text = on ? Fade(255, 255, 255, 255) : b.shape == Shape::Round ? Fade(colour, 235 * fade / 100)
                                                                             : Fade(kCream, 225 * fade / 100);
        float size = b.shape == Shape::Round ? hh * 1.05f : b.shape == Shape::Pill ? hh * 0.78f : hh * 0.8f;
        CentredText(dl, ImVec2(cx, cy - (b.shape == Shape::Tab ? hh * 0.08f : 0)), size, text, Label(b));
    }
    if (b.caption) {  // its move under it, in a dark band to read over the game
        float size = Radius(i) * 0.42f;
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, b.caption);
        ImVec2 at(cx, cy + Radius(i) + size * 0.75f);
        dl->AddRectFilled(ImVec2(at.x - ts.x / 2 - size * 0.35f, at.y - ts.y / 2 - size * 0.05f),
                          ImVec2(at.x + ts.x / 2 + size * 0.35f, at.y + ts.y / 2 + size * 0.05f), Fade(kGlass, 130 * fade / 100),
                          size * 0.3f);
        CentredText(dl, at, size, Fade(kCream, 215 * fade / 100), b.caption);
    }
}

// the editor's mark around a control
void DrawOutline(ImDrawList* dl, size_t i, ImU32 colour, float th) {
    float cx, cy, hw, hh;
    Centre(i, cx, cy);
    Extent(i, hw, hh);
    float k = th * 2;
    if (kButtons[i].shape == Shape::Round) dl->AddCircle(ImVec2(cx, cy), hh + k, colour, 48, th * 1.5f);
    else dl->AddRect(ImVec2(cx - hw - k, cy - hh - k), ImVec2(cx + hw + k, cy + hh + k), colour, hh * 0.4f, 0, th * 1.5f);
}

// the editor's bar, drawn as the controls are: Reset, Cancel, Done, and what to do under it
void DrawEditorBar(ImDrawList* dl, float th) {
    float h = Height();
    for (int i = 0; i < kBarCount; i++) {
        float r[4];
        BarAt(0, 0, r, i);
        bool done = i == kBarDone;
        bool size = i == kBarSmaller || i == kBarBigger;
        // - and + wait for a control to be picked, Hide for one that can be hidden
        int alpha = (size && g_selected < 0) || (i == kBarHide && !Hideable(g_selected)) ? 90 : 240;
        const char* label = i == kBarHide && Hideable(g_selected) && g_layout[size_t(g_selected)].hidden ? "SHOW"
                                                                                                         : kBarLabels[i];
        dl->AddRectFilled(ImVec2(r[0], r[1]), ImVec2(r[2], r[3]),
                          done ? IM_COL32(150, 115, 45, 235) : IM_COL32(20, 18, 14, 225), h * 0.015f);
        dl->AddRect(ImVec2(r[0], r[1]), ImVec2(r[2], r[3]), IM_COL32(220, 180, 100, alpha), h * 0.015f, 0, th * 0.7f);
        CentredText(dl, ImVec2((r[0] + r[2]) / 2, (r[1] + r[3]) / 2), h * (size ? 0.06f : 0.042f),
                    IM_COL32(255, 255, 255, alpha), app::Ui(label).c_str());
    }
    CentredText(dl, ImVec2(Width() / 2, (kBarY + kBarH + 0.05f) * h), h * 0.036f, IM_COL32(255, 255, 255, 220),
                app::Ui("DRAG TO MOVE, PINCH OR - + TO RESIZE").c_str());
}

// With the PS2's buttons, the game's prompts too: each Xbox icon it drew last frame (src/game/prompts.cpp) covered by
// the PS2's button, A by the cross, the grey L and R of a page's tabs by L1 and R1, Black by R2
void DrawGameIcons(float th) {
    if (!Ps2() || app::MenuOpen()) return;
    std::vector<game::Prompt> icons;
    game::CurrentIcons(icons);
    if (icons.empty()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float w = Width(), h = Height();
    float aspect = game::Aspect();
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    float ox = (w - vw) / 2, oy = (h - vh) / 2;
    // ULTRAWIDE draws the game's 2D 4:3 in the middle of the frame (native/vulkan.cpp's Flatten), its prompts too
    if (game::Ultrawide()) {
        float narrow = vh * 4.0f / 3.0f;
        ox += (vw - narrow) / 2;
        vw = narrow;
    }
    float k = vw / 640.0f;
    for (const game::Prompt& p : icons) {
        ImVec2 c(ox + (p.x0 + p.x1) / 2 * k, oy + (p.y0 + p.y1) / 2 * vh / 480.0f);
        float r = std::min(p.x1 - p.x0, p.y1 - p.y0) / 2 * k;
        float line = std::max(1.5f, r / 7);
        if (p.button <= game::kPromptY) {
            dl->AddCircleFilled(c, r * 1.04f, IM_COL32(18, 18, 22, 255), 32);
            dl->AddCircle(c, r * 0.92f, IM_COL32(200, 200, 205, 255), 32, line * 0.7f);
            Rgb col = kPs2Colours[p.button];
            DrawPs2Symbol(dl, c, r * 1.05f, p.button, IM_COL32(col.r, col.g, col.b, 255), line * 1.3f);
            continue;
        }
        const char* text = p.button == game::kPromptL       ? "L1"
                           : p.button == game::kPromptR     ? "R1"
                           : p.button == game::kPromptWhite ? "L2"
                                                            : "R2";
        float hw = r * 1.15f, hh = r * 0.8f;
        dl->AddRectFilled(ImVec2(c.x - hw, c.y - r * 1.04f), ImVec2(c.x + hw, c.y + r * 1.04f), IM_COL32(18, 18, 22, 255),
                          r * 0.35f);
        dl->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), IM_COL32(200, 200, 205, 255), r * 0.3f, 0,
                    line * 0.7f);
        CentredText(dl, c, r * 1.05f, IM_COL32(235, 235, 240, 255), text);
    }
    (void)th;
}

}  // namespace

void Draw() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    bool editing = g_editing;
    float h = Height();
    float th = std::max(2.0f, h / 360.0f);
    DrawGameIcons(th);
    if (!editing && !Shown()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (editing) dl->AddRectFilled(ImVec2(0, 0), ImVec2(Width(), h), IM_COL32(0, 0, 0, 90));
    for (size_t i = 0; i < kButtonCount; i++) {
        const Button& b = kButtons[i];
        if (!ButtonShown(i) || (app::MenuOpen() && b.press != kPanel && !editing)) continue;
        float cx, cy;
        Centre(i, cx, cy);
        float r = Radius(i);
        bool hidden = g_layout[i].hidden;
        bool on = !editing && (g_pressed[i] || (b.press == kGyro && gyro::CurrentMode() != gyro::kOff));
        int fade = hidden ? 40 : 100;  // a hidden control shows faint in the editor
        DrawControl(dl, i, on, fade, th);
        if (b.press == kGyro) {
            const char* mode = gyro::ModeName();
            float fs = r * 0.55f;
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, mode);
            dl->AddText(ImGui::GetFont(), fs, ImVec2(cx - ts.x / 2, cy + r * 1.08f), Fade(255, 255, 255, 180), mode);
        }
        if (editing && int(i) == g_selected) DrawOutline(dl, i, IM_COL32(255, 200, 60, 255), th);
        if (editing && hidden) {
            float k = r * 0.6f;
            dl->AddLine(ImVec2(cx - k, cy - k), ImVec2(cx + k, cy + k), IM_COL32(255, 80, 60, 220), th);
            dl->AddLine(ImVec2(cx - k, cy + k), ImVec2(cx + k, cy - k), IM_COL32(255, 80, 60, 220), th);
        }
    }
    if (editing || (!InMenus() && !GameMenu() && !app::MenuOpen() && input::Play()))
        for (size_t s : {kStick, kRStick}) DrawStick(dl, th, s);
    if (editing) {
        if (g_selected >= 0 && IsStick(size_t(g_selected))) {
            size_t s = size_t(g_selected);
            ImVec2 at(g_layout[s].x * Width(), g_layout[s].y * h);
            dl->AddCircle(at, Radius(s) + th * 2, IM_COL32(255, 200, 60, 255), 64, th * 1.5f);
        }
        DrawEditorBar(dl, th);
    }
}

// ---- the layout's settings, for the game's options (Recomp Settings, TOUCH); game thread, each saved at once

namespace {

// the controls moved, resized or hidden away from the preset's places
bool Custom() {
    const Layout& preset = *kPresets[g_preset].places;
    for (size_t i = 0; i < kPlaces; i++) {
        const Place& a = g_layout[i];
        const Place& b = preset[i];
        if (std::fabs(a.x - b.x) > 1e-4f || std::fabs(a.y - b.y) > 1e-4f || std::fabs(a.r - b.r) > 1e-4f ||
            a.hidden != b.hidden)
            return true;
    }
    return false;
}

uint32_t Percent(float f) { return uint32_t(std::lround(f * 100.0f)); }

}  // namespace

const char* LayoutName() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return Custom() ? "CUSTOM" : kPresets[g_preset].label;
}

// the next or previous preset; from a custom layout first back to its own preset's places, or the one before
void StepLayout(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    size_t n = std::size(kPresets);
    size_t i = Custom() ? (step > 0 ? g_preset : (g_preset + n - 1) % n) : (g_preset + n + (step > 0 ? 1 : n - 1)) % n;
    ApplyPreset(i);
    SaveLayout();
}

uint32_t ControlsOpacity() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return Percent(g_opacity);
}

// 20% to 100% in steps of 5
void StepOpacity(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_opacity = float(std::clamp(int(Percent(g_opacity)) + step * 5, 20, 100)) / 100.0f;
    SaveLayout();
}

uint32_t ControlsSize() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return Percent(g_scale);
}

// 50% to 150% in steps of 5
void StepSize(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_scale = float(std::clamp(int(Percent(g_scale)) + step * 5, 50, 150)) / 100.0f;
    SaveLayout();
}

const char* StickModeName() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return g_fixed_sticks ? "FIXED" : "FLOATING";
}

void StepStickMode(int) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_fixed_sticks = !g_fixed_sticks;
    SaveLayout();
}

// the controls over the screen to drag where they go, Done or Cancel in the bar at the top, Android's back cancels
void EditLayout() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    // the touch that picked the row is the menu's, not the editor's
    g_fingers.clear();
    g_cursor_down = false;
    Recount();
    OpenEditor();
}

void AddLook(float dx, float dy) {
    // the fraction of a count carried to the next reading: slow turns add up
    static float rx = 0, ry = 0;
    rx += dx;
    ry += dy;
    int ix = int(rx), iy = int(ry);
    rx -= float(ix);
    ry -= float(iy);
    input::g_dx += ix;
    input::g_dy += iy;
}

void PadUsed() { input::g_touch = false; }

bool LookActive() { return false; }  // no mouse look: nothing turns the view as a mouse would

bool Firing() {
    std::lock_guard lock(g_lock);
    for (size_t i = 0; i < kButtonCount; i++)
        if (g_pressed[i] && kButtons[i].press == kAnalogRT) return true;
    return input::Down(VK_LBUTTON);
}

// main thread, each turn of the event loop: the on-screen keyboard while a name is typed
void Pump() {
    gyro::Pump();
    // a mouse in play is captured as on a PC: no pointer over the picture and no screen edge to stop a turn. Let go
    // in the menus, the panel, the background and once the touch screen is used; asked again whenever Android took
    // it away
    SDL_Window* window = host::Window();
    bool capture = input::g_keyboard && !input::g_touch && LookActive();
    if (window && capture != SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, capture);
    bool want = input::g_text && input::Active();
    if (want == input::g_text_shown) return;
    input::g_text_shown = want;
    if (want) SDL_StartTextInput(host::Window());
    else SDL_StopTextInput(host::Window());
}

}  // namespace touch

namespace input {

bool TouchActive() { return g_touch; }

}  // namespace input
