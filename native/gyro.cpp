// Aiming by turning the phone, as mobile shooters have it, or the controller where it has a gyroscope of its own: the
// turn rates become mouse counts, which the game's mouse look (src/game/mouse.cpp) turns the view by. Off, always on,
// or only while firing; the mode and the sensitivity are set in the game's options menu and kept in android.toml
// beside the game's settings.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/mouse_keyboard.h"
#include "core/log.h"
#include "host.h"

namespace gyro {

namespace {

constexpr float kDegreesPerCount = 0.05f;  // src/game/mouse.cpp's kDegrees
constexpr float kDeadZone = 0.015f;        // radians a second: a hand held still
constexpr const char* kNames[kModes] = {"OFF", "ON", "FIRE"};

std::atomic<uint32_t> g_mode{kOff};
float g_sensitivity = 1.5f;  // view degrees per degree the phone turns
bool g_loaded = false;
SDL_Sensor* g_sensor = nullptr;
bool g_no_sensor = false;   // the phone has no gyroscope
bool g_pad_gyro = false;    // a controller's is on
uint64_t g_last_ns = 0;
uint64_t g_pad_last_ns = 0;

std::filesystem::path File() { return app::ExeDir() / "android.toml"; }

void Load() {
    g_loaded = true;
    try {
        toml::table t = toml::parse_file(File().string());
        g_mode = std::min<uint32_t>(t["gyro"].value_or(uint32_t(kOff)), kModes - 1);
        g_sensitivity = float(t["gyro_sensitivity"].value_or(double(g_sensitivity)));
    } catch (...) {
    }
}

void Save() {
    toml::table t{{"gyro", int64_t(g_mode.load())}, {"gyro_sensitivity", double(g_sensitivity)}};
    std::ofstream(File()) << t << "\n";
}

SDL_SensorID FindGyro() {
    int n = 0;
    SDL_SensorID found = 0;
    if (SDL_SensorID* ids = SDL_GetSensors(&n)) {
        for (int i = 0; i < n && !found; i++)
            if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_GYRO) found = ids[i];
        SDL_free(ids);
    }
    return found;
}

}  // namespace

uint32_t CurrentMode() {
    if (!g_loaded) Load();
    return g_mode;
}

// the game's options menu (src/common/menu.cpp) sets these; saved at once
void SetMode(uint32_t mode) {
    if (!g_loaded) Load();
    g_mode = mode % kModes;
    Save();
}

float Sensitivity() {
    if (!g_loaded) Load();
    return g_sensitivity;
}

void SetSensitivity(float s) {
    if (!g_loaded) Load();
    g_sensitivity = std::clamp(s, 0.1f, 5.0f);
    Save();
}

const char* ModeName() { return kNames[g_mode % kModes]; }

void NextMode() {
    g_mode = (g_mode + 1) % kModes;
    LOGI("gyro: %s", ModeName());
    Save();
}

// a controller's own gyroscope (a DualShock 4's, a DualSense's), on while a mode wants it; Android has them from 12 on
bool PumpPads(bool want) {
    bool any = false;
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; i++) {
            SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
            if (!pad || !SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) continue;
            if (SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO) != want) {
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, want);
                if (want) LOGI("gyro: %s's own", SDL_GetGamepadName(pad));
                g_pad_last_ns = 0;
            }
            any = true;
        }
        SDL_free(ids);
    }
    return any;
}

void Pump() {
    if (!g_loaded) Load();
    bool want = g_mode != kOff && host::Foreground();
    g_pad_gyro = PumpPads(want);
    if (want == (g_sensor != nullptr) || g_no_sensor) return;
    if (!want) {
        SDL_CloseSensor(g_sensor);
        g_sensor = nullptr;
        return;
    }
    if (!SDL_WasInit(SDL_INIT_SENSOR) && !SDL_InitSubSystem(SDL_INIT_SENSOR)) return;
    SDL_SensorID id = FindGyro();
    g_sensor = id ? SDL_OpenSensor(id) : nullptr;
    g_last_ns = 0;
    if (!g_sensor) {
        LOGW("gyro: no gyroscope on this device");
        // a controller's may still turn the view
        g_no_sensor = true;
        if (!g_pad_gyro) g_mode = kOff;
    }
}

// a turn in radians a second about the view's up axis (positive: left) and its right axis (positive: looking up)
void Turn(float yaw, float pitch, uint64_t now, uint64_t& last) {
    float dt = last && now > last ? float(now - last) * 1e-9f : 0;
    last = now;
    if (dt <= 0 || dt > 0.1f) return;
    bool firing = g_mode == kFiring ? touch::Firing() : true;
    if (!firing || !touch::LookActive()) return;
    if (std::fabs(yaw) < kDeadZone) yaw = 0;
    if (std::fabs(pitch) < kDeadZone) pitch = 0;
    if (!yaw && !pitch) return;
    float k = g_sensitivity * dt * (180.0f / 3.14159265f) / kDegreesPerCount;
    touch::AddLook(-yaw * k, -pitch * k);
}

// The phone's gyro reads in its natural (portrait) axes, radians a second: x right, y up, z out of the screen. Held in
// landscape, the turn about the world's up axis is the turn about the phone's x axis and the tilt about the world's
// right axis is the turn about its y axis, signed by which side is up. A controller's reads as held in front of the
// player: x right, y up, z back, so its pitch is about x and its yaw about y. The controller turns the view while it is
// what is played with, the phone while the touch screen is.
void OnEvent(const SDL_Event& e) {
    bool touch = input::TouchActive();
    if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
        if (e.gsensor.sensor != SDL_SENSOR_GYRO || touch) return;
        uint64_t now = e.gsensor.sensor_timestamp ? e.gsensor.sensor_timestamp : e.gsensor.timestamp;
        Turn(e.gsensor.data[1], e.gsensor.data[0], now, g_pad_last_ns);
        return;
    }
    if (!g_sensor || e.sensor.which != SDL_GetSensorID(g_sensor) || (g_pad_gyro && !touch)) return;
    uint64_t now = e.sensor.sensor_timestamp ? e.sensor.sensor_timestamp : e.sensor.timestamp;
    float wx = e.sensor.data[0], wy = e.sensor.data[1];
    SDL_Window* w = host::Window();
    bool flipped = w && SDL_GetCurrentDisplayOrientation(SDL_GetDisplayForWindow(w)) == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    Turn(flipped ? -wx : wx, flipped ? wy : -wy, now, g_last_ns);
}

}  // namespace gyro
