#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"

namespace game {

namespace {

// port 0 has the touch controls, the keyboard and mouse; the game sees no input while the panel is open
void Pad(int port, input::Pad& pad) {
    input::SeenPad(pad);
    if (port == 0) pad.connected = true;
    if (app::MenuOpen()) pad = input::Pad{.connected = pad.connected};
    else if (port == 0) input::Keyboard(pad);
}

// no vibration while the keyboard and mouse are in use
void Rumble(int port, uint16_t& left, uint16_t& right) {
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
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
}

// after the settings load, before boot
void Init() {
    BootLanguage();  // latched before the options can change the setting
    input::LoadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert("touch_sensitivity", double(o.touch_sensitivity));
    t.insert("language", o.language);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
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
                {"fps_usage", &game::Opt().fps_usage, nullptr},
                {"fps_temps", &game::Opt().fps_temps, nullptr},
                {"fps_position", nullptr, &game::Opt().fps_position},
                {"unlock_characters", &game::Opt().unlock_characters, nullptr},
                {"unlock_extras", &game::Opt().unlock_extras, nullptr},
                {"god_mode", &game::Opt().god_mode, nullptr},
                {"infinite_fatality", &game::Opt().infinite_fatality, nullptr},
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
