// controllers through SDL on Android: as the SDK's runtime/core/input.cpp reads them, but the plug events come from
// the main thread's event loop (Android's input reaches SDL there), and port 0 also takes the touch controls
#include "core/input.h"

#include <jni.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"
#include "host.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

void Read(SDL_Gamepad* pad, Pad& p) {
    p.connected = true;
    constexpr SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
    // A B X Y black white
    constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
                                             SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
                                             SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
    auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
    // 0 to 32767 into 0 to 255
    p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
    p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
    // SDL's sticks have y down; not plain negation: -32768 has no positive
    p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
    p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
    p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
}

// A controller Android hands over without its device: some phones' game services (MIUI's on a Redmi Note 8 Pro)
// take a DualShock 4 and pass its buttons and sticks on as a virtual device, which SDL takes for no controller.
// ShaolinMonksActivity sends those here as Android's key codes and axes; they play port 0.
struct Loose {
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, hx = 0, hy = 0;
    bool seen = false;
};
std::mutex g_loose_lock;
Loose g_loose;

// Android's key code as the pad's: a button bit (dpad, start, back, thumbs) or an analog button, -1 for neither
void LooseKey(int code, int& bit, int& analog) {
    bit = analog = -1;
    switch (code) {
        case 19: bit = 0; break;    // DPAD_UP
        case 20: bit = 1; break;    // DPAD_DOWN
        case 21: bit = 2; break;    // DPAD_LEFT
        case 22: bit = 3; break;    // DPAD_RIGHT
        case 108: bit = 4; break;   // BUTTON_START (Options)
        case 109: bit = 5; break;   // BUTTON_SELECT (Share)
        case 106: bit = 6; break;   // BUTTON_THUMBL
        case 107: bit = 7; break;   // BUTTON_THUMBR
        case 96: analog = 0; break;   // BUTTON_A (cross)
        case 97: analog = 1; break;   // BUTTON_B (circle)
        case 99: analog = 2; break;   // BUTTON_X (square)
        case 100: analog = 3; break;  // BUTTON_Y (triangle)
        case 103: analog = 4; break;  // BUTTON_R1: black
        case 102: analog = 5; break;  // BUTTON_L1: white
        case 104: analog = 6; break;  // BUTTON_L2
        case 105: analog = 7; break;  // BUTTON_R2
        default: break;
    }
}

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }
uint8_t Trigger(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

// the loose controller's state on top of what SDL read: buttons added, the stick pushed further taken
void MergeLoose(Pad& p) {
    std::lock_guard lock(g_loose_lock);
    const Loose& l = g_loose;
    if (!l.seen) return;
    p.connected = true;
    p.buttons |= l.buttons;
    // the hat as the dpad: x left -1 right 1, y up -1 down 1
    if (l.hy < -0.5f) p.buttons |= 1 << 0;
    if (l.hy > 0.5f) p.buttons |= 1 << 1;
    if (l.hx < -0.5f) p.buttons |= 1 << 2;
    if (l.hx > 0.5f) p.buttons |= 1 << 3;
    for (int i = 0; i < 8; i++) p.analog[i] = std::max(p.analog[i], l.analog[i]);
    p.analog[6] = std::max(p.analog[6], Trigger(l.lt));
    p.analog[7] = std::max(p.analog[7], Trigger(l.rt));
    // Android's y is down, the pad's up
    auto further = [](int16_t& to, int16_t v) { if (std::abs(int(v)) > std::abs(int(to))) to = v; };
    further(p.lx, Axis(l.lx));
    further(p.ly, Axis(-l.ly));
    further(p.rx, Axis(l.rx));
    further(p.ry, Axis(-l.ry));
}

}  // namespace

// ShaolinMonksActivity: the loose controller's buttons, sticks and triggers, and its letting go when the app loses the focus
extern "C" JNIEXPORT void JNICALL Java_com_racas_shaolinmonks_ShaolinMonksActivity_nativeLooseKey(JNIEnv*, jclass, jint code,
                                                                                     jboolean down) {
    int bit, analog;
    LooseKey(code, bit, analog);
    if (bit < 0 && analog < 0) return;
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    if (bit >= 0) g_loose.buttons = uint16_t(down ? g_loose.buttons | (1u << bit) : g_loose.buttons & ~(1u << bit));
    if (analog >= 0) g_loose.analog[analog] = down ? 0xFF : 0;
}

extern "C" JNIEXPORT void JNICALL Java_com_racas_shaolinmonks_ShaolinMonksActivity_nativeLooseAxes(JNIEnv*, jclass, jfloat lx,
                                                                                      jfloat ly, jfloat rx, jfloat ry,
                                                                                      jfloat lt, jfloat rt, jfloat hx,
                                                                                      jfloat hy) {
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    g_loose.lx = lx, g_loose.ly = ly, g_loose.rx = rx, g_loose.ry = ry;
    g_loose.lt = lt, g_loose.rt = rt, g_loose.hx = hx, g_loose.hy = hy;
}

extern "C" JNIEXPORT void JNICALL Java_com_racas_shaolinmonks_ShaolinMonksActivity_nativeLooseRelease(JNIEnv*, jclass) {
    std::lock_guard lock(g_loose_lock);
    bool seen = g_loose.seen;
    g_loose = {};
    g_loose.seen = seen;
}

void Init() {
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

void OnPadEvent(const SDL_Event& e) {
    std::lock_guard lock(g_lock);
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) Read(pad, p);
    }
    // a controller pushed past a rest, not just plugged in
    constexpr int16_t kStickPushed = 16000;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    if (port == 0) MergeLoose(p);
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), [](uint8_t a) { return a > 0x20; });
    if (p.buttons || analog || pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry)) touch::PadUsed();
    touch::Apply(port, p);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
