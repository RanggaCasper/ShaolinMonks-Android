#include <algorithm>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/pad_binds.h"
#include "netplay.h"

namespace game {

namespace {

// port 0 has the touch controls, the keyboard and mouse; the game sees no input while the panel is open
void Pad(int port, input::Pad& pad) {
    input::SeenPad(pad);
    if (port == 0) pad.connected = true;
    if (app::MenuOpen()) pad = input::Pad{.connected = pad.connected};
    else if (port == 0) input::Keyboard(pad);
    netplay::Filter(port, pad);  // last: the phone's own port 0 is what the friend gets, then both ports are the trade's
}

// no vibration while the keyboard and mouse are in use
void Rumble(int port, uint16_t& left, uint16_t& right) {
    netplay::Rumble(port, left, right);
    if (port != 0 || !input::KeyboardActive()) return;
    left = 0;
    right = 0;
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    o.mouse_sensitivity = float(t["mouse_sensitivity"].value_or(double(o.mouse_sensitivity)));
    // the touch screen followed the mouse's before it had its own: a player's choice carries over
    o.touch_sensitivity = float(t["touch_sensitivity"].value_or(double(o.mouse_sensitivity)));
    std::string language = t["language"].value_or(o.language);
    if (KnownLanguage(language)) o.language = language;
    o.netplay_address = t["netplay_address"].value_or(o.netplay_address);
    o.netplay_port = uint32_t(std::clamp<int64_t>(t["netplay_port"].value_or<int64_t>(o.netplay_port), 1024, 65535));
    o.netplay_delay = uint32_t(std::clamp<int64_t>(t["netplay_delay"].value_or<int64_t>(o.netplay_delay), 1, 12));
    o.netplay_copy_save = t["netplay_copy_save"].value_or(o.netplay_copy_save);
    o.netplay_auto_rng = t["netplay_auto_rng"].value_or(o.netplay_auto_rng);
    o.netplay_diag = t["netplay_diag"].value_or(o.netplay_diag);
    o.netplay_state_copy = t["netplay_state_copy"].value_or(o.netplay_state_copy);
    for (auto [key, list] : {std::pair{"netplay_sync", &o.netplay_sync}, std::pair{"netplay_hash", &o.netplay_hash}})
        if (const toml::array* a = t[key].as_array()) {
            list->clear();
            for (auto&& item : *a)
                if (auto v = item.value<std::string>()) list->push_back(*v);
        }
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
    if (const toml::table* pad = t["pad"].as_table())
        for (auto&& [id, value] : *pad)
            if (auto v = value.value<std::string>()) o.pad[std::string(id.str())] = *v;
}

// after the settings load, before boot
void Init() {
    BootLanguage();  // latched before the options can change the setting
    input::LoadBindings();
    input::LoadPadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert("touch_sensitivity", double(o.touch_sensitivity));
    t.insert("language", o.language);
    t.insert("netplay_address", o.netplay_address);
    t.insert("netplay_port", int64_t(o.netplay_port));
    t.insert("netplay_delay", int64_t(o.netplay_delay));
    t.insert("netplay_copy_save", o.netplay_copy_save);
    t.insert("netplay_auto_rng", o.netplay_auto_rng);
    t.insert("netplay_diag", o.netplay_diag);
    t.insert("netplay_state_copy", o.netplay_state_copy);
    toml::array sync, hash;
    for (const std::string& v : o.netplay_sync) sync.push_back(v);
    for (const std::string& v : o.netplay_hash) hash.push_back(v);
    t.insert("netplay_sync", sync);
    t.insert("netplay_hash", hash);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
    toml::table pad;
    for (const auto& [id, value] : o.pad) pad.insert(id, value);
    t.insert("pad", pad);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

const std::string& BootLanguage() {
    static const std::string language = Opt().language;
    return language;
}

}  // namespace game

const app::GameInfo app::kGame = {
    .title = L"Mortal Kombat: Shaolin Monks",
    .short_title = L"Shaolin Monks",
    .name = L"ShaolinMonks",
    .log = "shaolinmonks",
    .settings = "shaolinmonks.toml",
    .splash = nullptr,
    .missing = L"Put the game in the ShaolinMonks folder on the phone's storage: its disc image (.iso), or default.xbe "
               L"with the rest of the disc's files and folders beside it.",
    .images = {{"default.xbe", "ShaolinMonks"}},
    .options = {{"fps_counter", &game::Opt().fps_counter, nullptr},
                {"frame_skip", &game::Opt().frame_skip, nullptr},
                {"ultrawide", &game::Opt().ultrawide, nullptr},
                {"bloom", &game::Opt().bloom, nullptr},
                {"fog", &game::Opt().fog, nullptr},
                {"fps_usage", &game::Opt().fps_usage, nullptr},
                {"fps_temps", &game::Opt().fps_temps, nullptr},
                {"fps_position", nullptr, &game::Opt().fps_position},
                {"unlock_characters", &game::Opt().unlock_characters, nullptr},
                {"unlock_extras", &game::Opt().unlock_extras, nullptr},
                {"god_mode", &game::Opt().god_mode, nullptr},
                {"infinite_fatality", &game::Opt().infinite_fatality, nullptr},
                {"infinite_energy", &game::Opt().infinite_energy, nullptr},
                {"max_mana", &game::Opt().max_mana, nullptr},
                {"max_experience", &game::Opt().max_experience, nullptr},
                {"one_hit_kill", &game::Opt().one_hit_kill, nullptr},
                {"max_health", &game::Opt().max_health, nullptr},
                {"ps2_buttons", &game::Opt().ps2_buttons, nullptr},
                {"skip_cutscenes", &game::Opt().skip_cutscenes, nullptr},
                {"audio_output", nullptr, &game::Opt().audio_output},
                {"aspect_ratio", nullptr, &game::Opt().aspect_ratio},
                {"screen_filter", nullptr, &game::Opt().screen_filter},
                {"anti_aliasing", nullptr, &game::Opt().anti_aliasing}},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = app::DrawUi,
    .pad = game::Pad,
    .rumble = game::Rumble,
};
