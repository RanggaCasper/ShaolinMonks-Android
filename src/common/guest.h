#pragma once
#include <chrono>
#include <cstdint>

// what the game hooks share about the guest: where its memory is, and a clock for their timeouts
namespace game {

// guest memory the game's data may point into: the image, then the physical memory's 64 MB
inline constexpr uint32_t kImageBegin = 0x10000, kImageEnd = 0x8C2000;
inline constexpr uint32_t kPhysicalBegin = 0x80000000u, kPhysicalEnd = 0x84000000u;

inline bool GuestMapped(uint32_t va) {
    return (va >= kImageBegin && va < kImageEnd) || (va >= kPhysicalBegin && va < kPhysicalEnd);
}

inline int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace game
