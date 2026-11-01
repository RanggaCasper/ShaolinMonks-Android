// two processes on loopback, each a fake game thread that calls netplay::Step() once a frame
#include <sys/mman.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

static double g_loss = 0;
static ssize_t lossy_sendto(int fd, const void* b, size_t n, int fl, const sockaddr* a, socklen_t l) {
    static thread_local std::mt19937 rng(std::random_device{}());
    if (std::uniform_real_distribution<>(0, 1)(rng) < g_loss) return ssize_t(n);
    return ::sendto(fd, b, n, fl, a, l);
}
#define sendto lossy_sendto
#include "netplay.cpp"
#undef sendto

namespace kernel {
uint8_t* g_base = nullptr;
}
namespace kernel {
ObjectRef FromHandle(uint32_t h) {
    static int dummy;
    return h == 0x1004 ? ObjectRef(ObjectRef{}, reinterpret_cast<Object*>(&dummy)) : nullptr;
}  // one live handle, for the copy
}
namespace mem {
// the harness maps everything but 0x3000000 up, to see the scan skip unmapped pages
bool IsCommitted(uint32_t va) { return va < 0x4000000 && (va < 0x3000000 || va >= 0x3800000); }
}
namespace game {
Options& Opt() {
    static Options o;
    return o;
}
}  // namespace game
namespace app {
std::filesystem::path ExeDir() { return std::getenv("NP_DIR"); }
}  // namespace app
namespace logx {
void Write(Level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[%s] ", std::getenv("NP_ROLE"));
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}
}  // namespace logx

static std::atomic<uint32_t> g_tick{0};
static bool g_host_side = false;

namespace input {
Pad Poll(int port) {
    Pad p;
    p.connected = true;
    if (port == 0) {
        uint32_t t = g_tick;
        // after frame 300 each side holds one constant pad, to see the end state of a resync
        if (t >= 300) {
            p.buttons = g_host_side ? 0x11 : 0x22;
        } else {
            p.buttons = uint16_t((t * 7 + (g_host_side ? 0 : 100)) & 0xFFF);
            p.lx = int16_t(t * (g_host_side ? 13 : 29));
            p.analog[0] = uint8_t(t);
        }
    }
    netplay::Filter(port, p);
    return p;
}
}  // namespace input

int main(int argc, char** argv) {
    if (argc < 6) return 2;
    g_host_side = argv[1][0] == 'h';
    setenv("NP_ROLE", g_host_side ? "HOST " : "GUEST", 1);
    setenv("NP_DIR", argv[2], 1);
    game::Opt().netplay_port = uint32_t(std::atoi(argv[3]));
    g_loss = std::atof(argv[4]);
    int frames = std::atoi(argv[5]);
    int gap = argc > 6 ? std::atoi(argv[6]) : 0;
    game::Opt().netplay_delay = 3;
    game::Opt().netplay_sync = {"0x100000:16"};
    game::Opt().netplay_hash = {"0x100000:16"};
    kernel::g_base = static_cast<uint8_t*>(mmap(nullptr, 0x04000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    mprotect(kernel::HostPtr(0x3000000), 0x800000, PROT_NONE);  // like the console's memory the game did not commit
    if (g_host_side) std::memcpy(kernel::HostPtr(0x100000), "HOSTRNGSTATE0123", 16);
    *static_cast<uint32_t*>(kernel::HostPtr(0x2000010)) = g_host_side ? 12345u : 99999u;  // a C runtime rand state, seeded apart
    {   // what the state copy has to deal with: a word that differs, a live handle of the guest, a dispatcher object's header
        uint32_t* w = static_cast<uint32_t*>(kernel::HostPtr(0x2100000));
        w[0] = g_host_side ? 0xAAAA0001u : 0xBBBB0002u;
        w[4] = g_host_side ? 0x1111u : 0x1004u;
        w[8] = g_host_side ? 0x7777u : 0x8888u;   // +0 of a header
        w[11] = 0x4A424F4Bu;                       // its stamp, +12
        w[12] = g_host_side ? 0x5u : 0x6u;         // beyond the header: taken
    }
    if (g_host_side) netplay::Host();
    else {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        netplay::Join("127.0.0.1");
    }
    for (int i = 0; i < 200 && netplay::GetState() != netplay::State::Connected; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // the save copy
    std::fprintf(stderr, "[%s] status: %s\n", std::getenv("NP_ROLE"), netplay::Status().c_str());
    g_tick = 0;
    { input::Pad a = input::Poll(0), b = input::Poll(1); std::this_thread::sleep_for(std::chrono::milliseconds(100)); a = input::Poll(0); b = input::Poll(1);
      std::fprintf(stderr, "[%s] menu phase: port0 %03x port1 %03x\n", std::getenv("NP_ROLE"), a.buttons, b.buttons); }
    for (int i = 0; i < frames; i++) {
        g_tick = uint32_t(i);
        if (gap == 2 && !g_host_side && i == 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));  // the app in the background
            game::g_focus_pauses++;
        }
        if (gap == 1 && !g_host_side && i >= 200 && i < 290) {
            // the guest is in a menu for a while: no gameplay frames
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (i == 450 && !g_host_side) static_cast<uint8_t*>(kernel::HostPtr(0x100000))[3] ^= 0xFF;  // the games part here
        netplay::Step();
        if (i == 0) {
            uint32_t* w = static_cast<uint32_t*>(kernel::HostPtr(0x2100000));
            std::fprintf(stderr, "[%s] after the barrier: %08x %08x %08x %08x %08x | rng %08x\n", std::getenv("NP_ROLE"), w[0], w[4], w[8], w[11], w[12],
                         *static_cast<uint32_t*>(kernel::HostPtr(0x2000010)));
        }
        {   // the fake game draws from its generator a few times a frame, and keeps a counter and a clock-like word
            uint32_t* rng = static_cast<uint32_t*>(kernel::HostPtr(0x2000010));
            for (int k = 0; k < int(i % 4) + 1; k++) *rng = *rng * 214013u + 2531011u;
            (*static_cast<uint32_t*>(kernel::HostPtr(0x500100)))++;
            *static_cast<uint32_t*>(kernel::HostPtr(0x500200)) = uint32_t(i) * 2654435761u + (g_host_side ? 0u : 7u);
        }
        if (std::getenv("NP_NOISE")) {  // a million words that change every frame, like buffers of a real game
            uint32_t* n = static_cast<uint32_t*>(kernel::HostPtr(0x1000000));
            uint32_t v = uint32_t(i) * 2246822519u + (g_host_side ? 1u : 2u);
            for (int k = 0; k < 1000000; k++) { v ^= v << 13; v ^= v >> 17; v ^= v << 5; n[k] = v; }
        }
        input::Pad p0 = input::Poll(0), p1 = input::Poll(1);
        std::printf("%d %03x %d %02x | %03x %d %02x\n", i, p0.buttons, p0.lx, p0.analog[0], p1.buttons, p1.lx, p1.analog[0]);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::fprintf(stderr, "[%s] rng word at the end: %08x\n", std::getenv("NP_ROLE"), *static_cast<uint32_t*>(kernel::HostPtr(0x2000010)));
    std::fprintf(stderr, "[%s] end: %s, rng bytes \"%.16s\", in sync %d\n", std::getenv("NP_ROLE"), netplay::Status().c_str(),
                 reinterpret_cast<char*>(kernel::HostPtr(0x100000)), int(netplay::InSync()));
    netplay::Stop();
    return 0;
}
