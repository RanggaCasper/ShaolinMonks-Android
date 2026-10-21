// Cheats, from the settings panel's General page
#include <algorithm>
#include <bitset>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "app/app.h"
#include "common/guest.h"
#include "common/options.h"
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {

namespace {

// ---- the unlockables

// A bitset of 13 dwords, one bit an unlockable, set by the scripts' unlockchar (0x0B6640) and its kin for the other
// kinds (0x0B6680 on, from bit 0x5C, 0x65, 0xA6 artwork, 0x157) and read by CharLocked (0x0B7030); the character
// select lists a character whose bit is set (0x04E4C7)
constexpr uint32_t kUnlocked = 0x5A375C;
constexpr size_t kUnlockBits = 13 * 32;
// the characters' unlockables, from the character select's table (0x4E5F98, 12 bytes a character): Liu Kang and
// Kung Lao (0 and 1) are there from the start
constexpr uint32_t kCharacters[] = {83, 89, 88, 82, 90, 84};
// the story's and Ko-op's character select (0x04E54F) skips those bits: its third and sixth characters (88 and 82)
// come with a byte each set to 1, the other unlockables are for Versus alone
constexpr uint32_t kStoryCharacters[] = {0x5A378D, 0x5A378E};
// the unlocks the cheats make, as one set: the bitset's bits, then the story's bytes
constexpr size_t kUnlockCount = kUnlockBits + std::size(kStoryCharacters);
using Unlocks = std::bitset<kUnlockCount>;

bool Unlocked(size_t i) {
    if (i < kUnlockBits) return R32(kUnlocked + uint32_t(i >> 5) * 4) >> (i & 31) & 1;
    return G<uint8_t>(kStoryCharacters[i - kUnlockBits]) != 0;
}

void SetUnlocked(size_t i, bool on) {
    if (i < kUnlockBits) {
        uint32_t at = kUnlocked + uint32_t(i >> 5) * 4;
        W32(at, on ? R32(at) | 1u << (i & 31) : R32(at) & ~(1u << (i & 31)));
        return;
    }
    G<uint8_t>(kStoryCharacters[i - kUnlockBits]) = on ? 1 : 0;
}

Unlocks CharacterUnlocks() {
    Unlocks u;
    for (uint32_t id : kCharacters) u.set(id);
    for (size_t i = kUnlockBits; i < kUnlockCount; i++) u.set(i);
    return u;
}

// every other unlockable: the Kontent menu's art, films, costumes and arenas
Unlocks ExtraUnlocks() {
    Unlocks u;
    for (size_t i = 0; i < kUnlockBits; i++) u.set(i);
    for (uint32_t id : kCharacters) u.reset(id);
    return u;
}

// The unlocks the cheats made, kept in a file beside the settings: the game saves them into the player's profile as
// its own, so with a cheat off they are taken back out (and the player's own unlocks kept) until the game saves the
// profile without them
std::filesystem::path RecordFile() { return app::ExeDir() / "cheat_unlocks.txt"; }

Unlocks g_record;
bool g_record_loaded = false;
int64_t g_clear_from = -1;  // the save's time as the clearing began, -1 before

void SaveRecord() { std::ofstream(RecordFile()) << g_record.to_string() << "\n"; }

void LoadRecord() {
    g_record_loaded = true;
    std::ifstream in(RecordFile());
    std::string text;
    in >> text;
    if (text.size() == kUnlockCount && text.find_first_not_of("01") == std::string::npos) {
        g_record = Unlocks(text);
        return;
    }
    // none yet, or the first version's (the characters alone): the characters' unlocks in the save are the cheat's
    g_record = CharacterUnlocks();
    SaveRecord();
}

// the newest time the game wrote a profile's save, 0 for none
int64_t SaveTime() {
    std::error_code ec;
    int64_t newest = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(app::ExeDir() / "hdd" / "E" / "UDATA", ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().filename() != "save.dat") continue;
        auto t = std::filesystem::last_write_time(it->path(), ec);
        if (!ec) newest = std::max<int64_t>(newest, t.time_since_epoch().count());
    }
    return newest;
}

// The cheats' unlocks are set again at every frame, as the game sets them: a profile loaded after keeps them, and
// saving keeps them in the save too. The ones of a cheat turned off are cleared at every frame, a profile loaded with
// them too, until the game saves the profile cleared
void ApplyUnlocks() {
    if (!g_record_loaded) LoadRecord();
    Unlocks want;
    if (Opt().unlock_characters) want |= CharacterUnlocks();
    if (Opt().unlock_extras) want |= ExtraUnlocks();
    Unlocks record = g_record;
    for (size_t i = 0; i < kUnlockCount; i++) {
        if (want[i]) {
            if (!Unlocked(i)) record.set(i);
            SetUnlocked(i, true);
        } else if (record[i]) {
            SetUnlocked(i, false);
        }
    }
    Unlocks clearing = record & ~want;
    if (clearing.none()) g_clear_from = -1;
    if (record != g_record) {
        g_record = record;
        SaveRecord();
    }
    if (clearing.none()) return;
    // a save written after the clearing began holds none of them: they are done
    static int64_t checked = 0;
    if (NowMs() - checked < 1000) return;
    checked = NowMs();
    int64_t saved = SaveTime();
    if (g_clear_from < 0) {
        g_clear_from = saved;
        return;
    }
    if (saved > g_clear_from) {
        LOGI("cheats: the profile was saved without %zu of the cheats' unlocks", clearing.count());
        g_record &= want;
        g_clear_from = -1;
        SaveRecord();
    }
}

// ---- the players

// The player's profile (0x0C4850): per player (1 and 2) a record of 0x50 bytes from +0x48F0, experience at +0
// (0x07BB00) and the fatality meter, a float, at +0x14 (0x0C9710), the meter's level at +0x38 (0x0C9770); +0x88 is
// the story's players, 1 or 2 with the second a player too (0x073ED0)
constexpr uint32_t kProfile = 0x5ACEBC;
constexpr uint32_t kPlayerRecords = 0x48F0, kPlayerRecordSize = 0x50;
constexpr uint32_t kExperience = 0x00, kFatality = 0x14, kFatalityLevel = 0x38;
constexpr uint32_t kStoryPlayers = 0x88;
// the health upgrades (0x0CAEC0): their count at +0xE0, at most 4; a body's most health is the base at +0x94, from
// the table at 0x50B544 by upgrades and the story's players (0x0C9580), times the multiplier at +0x90, halved for two
// players (0x0D4790)
constexpr uint32_t kHealthUpgrades = 0xE0, kBaseHealth = 0x94, kHealthMultiplier = 0x90;
constexpr uint32_t kBaseHealthTable = 0x50B544;
constexpr uint32_t kMostUpgrades = 4;
// the players' bodies (0x0D4790): two pointers in the object list at 0x510040, health at +0x368 and its most at +0x370
// (SetHealth, 0x153CD0)
constexpr uint32_t kPlayerObjects[] = {0x51005C, 0x510060};
constexpr uint32_t kHealth = 0x368, kMaxHealth = 0x370;
// every body of the level, the players' and their enemies' (0x16B2D0): kBodySize bytes each from [0x510044], as many
// as [0x5142A4], a word each at [0x51004C] not 0 for those in use
constexpr uint32_t kBodies = 0x510044, kBodiesUsed = 0x51004C, kBodyCount = 0x5142A4;
constexpr uint32_t kBodySize = 0x508;
// the players' characters (0x144350 on the table at 0x64D218): their profile record at +0x68, and the fatality
// meter's unit (0x082EA0), a third of [[[+0x2B6C] + 0x54] + 4]
constexpr uint32_t kCharacterTable = 0x64D218;
constexpr uint32_t kRecordIndex = 0x68, kCharacterInfo = 0x2B6C;
constexpr uint32_t kInfoMeter = 0x54, kMeterMost = 4;
constexpr int32_t kMaxExperience = 999999;

uint32_t Profile() {
    uint32_t p = R32(kProfile);
    return GuestMapped(p) ? p : 0;
}

// player n's record (1 or 2) in the profile
uint32_t PlayerRecord(uint32_t profile, uint32_t n) { return profile + kPlayerRecords + n * kPlayerRecordSize; }

bool SecondPlayer() {
    uint32_t p = Profile();
    if (!p) return false;
    uint32_t players = R32(p + kStoryPlayers);
    return players == 1 || players == 2;
}

// the players' bodies; the second only with two players in the story, it is a rival in Versus
template <typename F>
void ForEachPlayerBody(F&& f) {
    for (size_t n = 0; n < std::size(kPlayerObjects); n++) {
        if (n && !SecondPlayer()) break;
        uint32_t body = R32(kPlayerObjects[n]);
        if (GuestMapped(body)) f(body);
    }
}

void FullHealth() {
    ForEachPlayerBody([](uint32_t body) {
        int32_t most = int32_t(R32(body + kMaxHealth));
        if (most <= 0 || most > 100000) return;
        if (int32_t(R32(body + kHealth)) < most) W32(body + kHealth, uint32_t(most));
    });
}

// The mana of player n (1 or 2), the fatality meter drawn as the blue bar: a level's top is a third of the character's
// most, at [info + 4] (AddFatality, 0x084980, caps it at both), so three levels is the longest bar. Longest sets the
// level to three, full fills it to the level's top
constexpr int32_t kManaLevels = 3;
void Mana(uint32_t n, bool longest, bool full) {
    uint32_t profile = Profile();
    uint32_t character = R32(kCharacterTable + (n * 3 + 3) * 4);
    {  // DIAG
        static int64_t logged[3] = {};
        if (NowMs() - logged[n] > 3000) {
            logged[n] = NowMs();
            uint32_t rec = GuestMapped(character) ? R32(character + kRecordIndex) : 0xFFFFFFFF;
            uint32_t info = GuestMapped(character) ? R32(character + kCharacterInfo) : 0;
            uint32_t meter = GuestMapped(info) ? R32(info + kInfoMeter) : 0;
            float most = GuestMapped(meter) ? G<float>(meter + kMeterMost) : -1;
            uint32_t at = profile ? PlayerRecord(profile, rec < 3 ? rec : 0) : 0;
            LOGI("mana diag n=%u profile=%08X char=%08X rec=%u info=%08X meter=%08X most=%g level=%d amount=%g", n,
                 profile, character, rec, info, meter, most, at ? int32_t(R32(at + kFatalityLevel)) : -1,
                 at ? G<float>(at + kFatality) : -1.0f);
        }
    }
    if (!profile || !GuestMapped(character)) return;
    uint32_t record = R32(character + kRecordIndex);
    if (record < 1 || record > 2) return;
    uint32_t info = R32(character + kCharacterInfo);
    if (!GuestMapped(info)) return;
    uint32_t meter = R32(info + kInfoMeter);
    if (!GuestMapped(meter)) return;
    float most = G<float>(meter + kMeterMost);
    if (!(most > 0 && most < 1000000)) return;
    uint32_t at = PlayerRecord(profile, record);
    if (longest && int32_t(R32(at + kFatalityLevel)) < kManaLevels) W32(at + kFatalityLevel, kManaLevels);
    int32_t level = int32_t(R32(at + kFatalityLevel));
    if (!full || level <= 0 || level > 100) return;
    float top = std::min(float(level) * most / kManaLevels, most);
    if (G<float>(at + kFatality) < top) G<float>(at + kFatality) = top;
}

// every enemy's health down to 1: the next hit kills
void OneHitKill() {
    uint32_t bodies = R32(kBodies), used = R32(kBodiesUsed), count = R32(kBodyCount);
    if (!GuestMapped(bodies) || !GuestMapped(used) || count > 4096) return;
    uint32_t players[] = {R32(kPlayerObjects[0]), R32(kPlayerObjects[1])};
    for (uint32_t i = 0; i < count; i++) {
        if (!G<uint16_t>(used + i * 2)) continue;
        uint32_t body = bodies + i * kBodySize;
        if (body == players[0] || body == players[1]) continue;
        int32_t health = int32_t(R32(body + kHealth)), most = int32_t(R32(body + kMaxHealth));
        if (most > 1 && most < 1000000 && health > 1) W32(body + kHealth, 1);
    }
}

// every health upgrade, and the players' bars as long as the game makes them with all of them
void MaxHealth() {
    uint32_t profile = Profile();
    if (!profile) return;
    W32(profile + kHealthUpgrades, kMostUpgrades);
    uint32_t players = std::min<uint32_t>(R32(profile + kStoryPlayers), 2);
    int32_t base = int32_t(R32(kBaseHealthTable + (kMostUpgrades * 5 + players) * 4));
    int32_t multiplier = int32_t(R32(profile + kHealthMultiplier));
    if (base <= 0 || base > 100000 || multiplier < 1 || multiplier > 15) return;
    W32(profile + kBaseHealth, uint32_t(base));
    int32_t most = base * multiplier;
    if (SecondPlayer()) most /= 2;
    ForEachPlayerBody([most](uint32_t body) {
        int32_t now = int32_t(R32(body + kMaxHealth));
        if (now <= 0 || now >= most) return;
        // the bar grows by what was missing, as the game's upgrade fills it
        W32(body + kMaxHealth, uint32_t(most));
        W32(body + kHealth, R32(body + kHealth) + uint32_t(most - now));
    });
}

void MaxExperience() {
    uint32_t profile = Profile();
    if (!profile) return;
    for (uint32_t n = 1; n <= 2; n++) {
        uint32_t at = PlayerRecord(profile, n) + kExperience;
        if (int32_t(R32(at)) < kMaxExperience) W32(at, uint32_t(kMaxExperience));
    }
}

}  // namespace

// at every present, game thread
void ApplyCheats() {
    ApplyUnlocks();
    if (Opt().max_health) MaxHealth();
    if (Opt().god_mode) FullHealth();
    if (Opt().one_hit_kill) OneHitKill();
    if (Opt().infinite_fatality || Opt().max_mana)
        for (uint32_t n = 1; n <= 2; n++) Mana(n, Opt().max_mana, Opt().infinite_fatality);
    if (Opt().max_experience) MaxExperience();
}

}  // namespace game
