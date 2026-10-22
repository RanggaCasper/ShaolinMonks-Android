#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish, Indonesian. Indonesian is the
// recomp's own text only: the game files have none, so the game itself stays in English
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};
inline bool KnownLanguage(const std::string& code) {
    return std::find(std::begin(kLanguages), std::end(kLanguages), code) != std::end(kLanguages);
}

// SSAA renders at twice the height, the presenter averages it down
inline constexpr uint32_t kAaOff = 0, kAaFxaa = 1, kAaSsaa = 2;

struct Options {
    bool skip_intros = false;  // logos at boot and the film after pressing start
    bool fps_counter = false;
    uint32_t fps_position = 0;  // the counter's place, src/common/menu.cpp's kFpsPositionNames
    bool fps_usage = true;      // the counter's CPU and GPU load
    bool fps_temps = true;      // the counter's CPU, GPU and battery temperatures
    bool frame_skip = true;  // under 60 fps gameplay keeps its speed and draws fewer frames, src/game/game.cpp
    uint32_t fmv_scale = 0;  // 4:3 films on a wider screen: 0 fill it, top and bottom cut; 1 kept 4:3
    uint32_t screen_filter = 0;  // the present's look: 0 none, 1 sharpen, 2 vivid, 3 scanlines, 4 CRT, 5 HDR
    uint32_t anti_aliasing = 0;  // kAaOff, kAaFxaa or kAaSsaa
    uint32_t aspect_ratio = 0;  // the frame's shape in hundredths (133 is 4:3), 0 the whole screen; common/aspect.h
    bool ultrawide = false;     // a frame wider than 4:3 shows more of the sides instead of stretching, common/aspect.h
    bool reload_blur = true;
    bool bloom = true;  // the glow added over the frame, native/vulkan.cpp skips it when off
    bool fog = true;    // the distance fog, native/vulkan.cpp draws without it when off
    uint32_t draw_distance = 1;  // times the game's LOD and draw distances
    float mouse_sensitivity = 1.0f;
    float touch_sensitivity = 1.0f;  // the touch screen's look, apart from the mouse's
    bool invert_mouse = false;
    bool hold_aim = true;  // scopes too, keyboard and mouse
    uint32_t fov = 70;  // horizontal degrees on a 4:3 frame, 70 is the game's
    uint32_t viewmodel_fov = 70;  // the gun and arms, the same measure
    uint32_t walk_speed = 50;  // percent of running, while the walk key is held
    uint32_t audio_output = 0;  // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    std::string language = "int";  // one of kLanguages
    bool infinite_ammo = false;  // cheat: the player's magazine and reserve never go down
    uint32_t rapid_fire = 1;  // cheat: times the player's rate of fire, 1 the game's
    bool no_recoil = false;  // cheat: the player's shots do not kick the view
    uint32_t game_speed = 100;  // cheat: percent of the game's speed, menus and films keep theirs
    bool god_mode = false;  // cheat: the players' health stays full
    bool one_hit_kill = false;  // cheat: any hit of the players' kills an enemy
    bool infinite_grenades = false;  // cheat: throwing a grenade keeps the count
    bool unlock_characters = false;  // cheat: every character on the character select, src/game/cheats.cpp
    bool infinite_fatality = false;  // cheat: the players' mana (the fatality meter) stays full
    bool infinite_energy = false;    // cheat: the players' energy, the blue bar under the health, stays full
    bool max_mana = false;  // cheat: the mana bar at its longest, every level of it
    bool max_health = false;  // cheat: every health upgrade, the health bar at its longest
    bool max_experience = false;  // cheat: the players' experience at its most, every move to buy
    bool skip_cutscenes = false;  // cheat: any cinematic and film can be skipped, seen or not, src/game/cheats.cpp
    bool unlock_extras = false;  // cheat: every unlockable of the Kontent menu (art, films, costumes, arenas)
    bool ps2_buttons = false;  // the touch controls and the game's button prompts show the PS2's buttons
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
    std::map<std::string, std::string> pad;   // action id to a controller's input, absent is SDL's mapping
};
Options& Opt();
// the language setting as this run started, the language the game boots in
const std::string& BootLanguage();
bool LanguageAvailable(const std::string& code);  // src/game/game.cpp: the LANGUAGE row offers it
inline std::atomic<float> g_fps{0};  // from the host, for the game's own counter
inline std::atomic<bool> g_hide_hud{false};  // the toggle HUD key: the gameplay HUD is not drawn
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits while the app is in the background
inline std::atomic<uint32_t> g_focus_pauses{0};  // focus pauses ended so far
void WaitWhileUnfocused();  // native/game_host.cpp: game thread, before the present
void ApplyCheats();         // src/game/cheats.cpp: game thread, at each present
bool HudShown();  // src/game/game.cpp: the player's HUD drew lately, a hint's pause too

}  // namespace game
