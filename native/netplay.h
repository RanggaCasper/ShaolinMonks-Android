#pragma once
// co-op over the network (netplay.cpp): both phones run the game and trade the pads' input, one frame of gameplay
// at a time. The host is player 1 (port 0), the guest player 2 (port 1).
#include <cstdint>
#include <string>

namespace input {
struct Pad;
}

namespace netplay {

enum class State : uint8_t {
    Off,
    Hosting,    // listening, no friend yet
    Joining,    // asking the host
    Connected,  // linked; the pads are the phone's own until both reach gameplay
    Playing,    // lockstep: both ports come from the trade
};

// main thread (the panel). Both stop a running session first.
void Host();
void Join(const std::string& address);
void Stop();

State GetState();
// one line for the panel and the overlay, empty when off
std::string Status();
// false once the two phones' game state was seen to differ
bool InSync();

// any thread, the game's pad hook: port 0 is read as the phone's own pad, then both ports are replaced
void Filter(int port, input::Pad& pad);
void Rumble(int port, uint16_t& left, uint16_t& right);

// game thread, once per gameplay frame, before the game reads its pads: waits for the friend's input
void Step();

// frame skip is the same on both phones: SetSkipWant (game thread, before Step) says this phone is behind its clock,
// and SkipThisFrame (after Step) is 1 when either phone is, 0 when neither, -1 when not in lockstep
void SetSkipWant(bool want);
int SkipThisFrame();

}  // namespace netplay
