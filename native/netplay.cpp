// Co-op over the network by lockstep. Both phones run the whole game and trade only the pads: the host is player 1
// (port 0), the guest player 2 (port 1). Every gameplay frame (the hook in src/game/game.cpp) each side takes its pad,
// files it kInputDelay frames ahead, sends it and waits for the friend's pad of the same frame, so both games read the
// same two pads on the same frame. That only keeps them together if the game is deterministic from there, so a hash of
// the game's memory is traded as well and a difference is logged (the first sign that an RNG or a clock is not synced).
//
// UDP carries the frames: every packet repeats the pads the friend has not acknowledged, so a lost packet costs
// nothing. A "gap" (no gameplay frame for a second: a menu, a load) drops back to the phone's own pad, and the next
// gameplay frame starts a new epoch: a barrier both sides pass together, frame 0 again, the host's chosen guest memory
// copied over (Options::netplay_sync, for an RNG state). TCP on the same port copies the host's saves to the guest when
// it joins.
#include "netplay.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace netplay {

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr uint32_t kMagic = 0x504E4D53;      // "SMNP"
constexpr uint32_t kSaveMagic = 0x56534D53;  // "SMSV"
constexpr uint8_t kVersion = 4;
enum Type : uint8_t { kHello = 1, kWelcome, kInput, kSync, kPing, kPong, kBye, kPad };

constexpr uint32_t kRing = 1024;      // frames kept, far more than the delay and the retransmit window
constexpr uint32_t kMaxSend = 32;     // frames in a packet
constexpr uint32_t kHashEvery = 60;   // frames between two state hashes
constexpr uint32_t kHashSlots = 64;
constexpr int kGapMs = 1000;          // no gameplay frame for this long: back to the phone's own pad
constexpr int kDeadMs = 6000;         // no packet for this long: the link is gone
constexpr size_t kPayloadMax = 1100;  // the barrier's guest memory, in one packet
constexpr size_t kStateMax = 600;     // the same riding on an input packet
constexpr uint32_t kStateEvery = 60;  // frames between two copies of the host's state words to the guest
// the host looks for the game's random number generator in all of the console's memory (the C runtime keeps its state
// per thread, on the heap): three snapshots kScanGap frames apart, from frame kScanFirst of an epoch, again kScanRepeat
// frames on while none was found
constexpr uint32_t kScanGap = 20, kScanFirst = 240, kScanRepeat = 600;
constexpr uint32_t kImageLo = 0x3F0000, kImageHi = 0x5CB000;  // the image's data and bss, for the scan's report
constexpr uint32_t kScanSteps = 5000;   // steps of the generators that are walked one by one
constexpr uint32_t kScanRounds = 4;     // per epoch
constexpr uint32_t kLcgBits = 17;       // an LCG's low bits cycle on their own: the steps between two looks, up to 2^17
constexpr uint32_t kVerifySteps = 200000;  // between two copies of a state word
constexpr uint8_t kNoGen = 0xFF;
// guest memory the hash covers without any setting: the image's data and bss, up to the page of 0x5CAB98
constexpr uint32_t kHashLo = 0x4A0000, kHashHi = 0x5CB000;
constexpr uint32_t kGuestLo = 0x10000, kGuestEnd = 0x04000000;  // what a setting may name: the console's 64 MB
constexpr uint64_t kSaveCap = 512ull << 20;

#pragma pack(push, 1)
struct WirePad {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t stick[4];
    uint8_t flags;  // bit 0: this phone's frame skip wants this frame's drawing skipped
};
#pragma pack(pop)
static_assert(sizeof(WirePad) == 19);

struct Region {
    uint32_t addr, len;
    uint8_t gen = kNoGen;  // an index into kGens for a word the scan found, else a setting's region
    uint32_t last = 0;     // host: the word as last sent
    bool seen = false;
};

struct Writer {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) {
        const uint8_t* s = static_cast<const uint8_t*>(p);
        b.insert(b.end(), s, s + n);
    }
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    bool ok = true;
    bool take(void* out, size_t k) {
        if (!ok || n < k) return ok = false;
        std::memcpy(out, p, k);
        p += k;
        n -= k;
        return true;
    }
    uint8_t u8() {
        uint8_t v = 0;
        take(&v, 1);
        return v;
    }
    uint16_t u16() {
        uint16_t v = 0;
        take(&v, 2);
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        take(&v, 4);
        return v;
    }
};

struct Header {
    uint8_t type = 0;
    uint32_t session = 0, epoch = 0;
};

// ---- state. g_mtx guards everything below it that is not atomic; g_latch only the pads Filter reads.
std::mutex g_mtx;
std::condition_variable g_cv;
std::atomic<State> g_state{State::Off};
std::atomic<bool> g_quit{false};
std::atomic<uint32_t> g_gen{0};  // one more at each Host or Join: a late thread of an old session does nothing
std::thread g_thread;
int g_udp = -1, g_tcp = -1;
sockaddr_in g_peer{};
std::atomic<bool> g_host{false};
uint32_t g_session = 0;
uint32_t g_delay = 3;
uint16_t g_port = 7777;
Clock::time_point g_last_rx;
std::string g_error;      // why the last session ended
std::string g_addrs;      // the host's own addresses, for the panel
std::string g_target;     // the guest's host address
std::string g_note;       // the save copy's progress
std::atomic<bool> g_save_ready{true};  // the guest's save copy is done (or none was asked)
std::atomic<uint32_t> g_rtt{0};
std::atomic<bool> g_desync{false};
std::atomic<int64_t> g_last_step_ms{0};
std::atomic<bool> g_barrier_flag{false};
std::atomic<int64_t> g_wait_since{0};
uint64_t g_stat_frames = 0, g_stat_wait_us = 0, g_stat_max_us = 0, g_stat_late = 0;  // under g_mtx  // ms, the friend's frame has not come since

// lockstep
bool g_lock_on = false;
bool g_barrier = false;
uint32_t g_epoch = 0, g_peer_epoch = 0, g_frame = 0;
bool g_peer_here = false;   // the friend's packet of this epoch was seen
bool g_sync_got = false;    // and, on the guest, the host's payload
std::vector<uint8_t> g_payload;
// state words the host copies to the guest every kStateEvery frames: Options::netplay_sync and what the scan found
uint32_t g_state_frame = UINT32_MAX;     // host: the frame the held copy is of
uint32_t g_state_rx_frame = UINT32_MAX;  // guest: the frame of the copy it has; the host's ack of it
uint32_t g_peer_state_ack = UINT32_MAX;  // host: the newest copy the guest has
std::vector<uint8_t> g_state_pay;        // host: the copy to send; guest: the copy received
uint64_t g_stat_corr = 0, g_stat_words = 0;  // guest: copies that changed something, state words in them
WirePad g_local[kRing], g_remote[kRing];
uint32_t g_local_next = 0;   // my frames [0, g_local_next) are filed
uint32_t g_remote_next = 0;  // the friend's frames [0, g_remote_next) are here
uint32_t g_remote_ack = 0;   // the friend holds my frames [0, g_remote_ack)
uint32_t g_lhash_frame[kHashSlots], g_lhash[kHashSlots], g_rhash_frame[kHashSlots], g_rhash[kHashSlots];
uint32_t g_send_hash_frame = UINT32_MAX, g_send_hash = 0;
std::vector<uint32_t> g_pages;  // page hashes of the latest hashed frame
uint32_t g_pages_frame = 0;

std::vector<Region> g_auto;  // the host's: state words the scan found
std::vector<uint8_t> g_snap[3];
int g_snap_n = 0;
uint32_t g_scan_round = 0;
bool g_scan_busy = false;
// the guest's: a state word is only written once it was seen to move along its generator on this phone too, so a word
// of another meaning at that address (the two heaps differ) is left alone
struct GuestWord {
    uint32_t last = 0;
    bool verified = false;
};
std::map<uint32_t, GuestWord> g_gw;
// state copy: the host leaves its barrier only after the guest has taken its pages
uint32_t g_state_copied_epoch = UINT32_MAX;  // host: the epoch whose copy was served
bool g_peer_copy = false;                    // host: the guest says a copy is on its way
uint8_t g_copy_flag = 0;                     // guest, in its SYNC: 1 a copy is pending, 2 done or given up, 0 none asked
uint64_t g_stat_unverified = 0;

// frame skip, the same on both phones: each says in its pad whether it is late, the frame is skipped if either is
std::atomic<bool> g_skip_want{false};
std::atomic<int> g_skip_now{-1};  // -1 outside lockstep

std::mutex g_latch;
input::Pad g_raw;               // the phone's own port 0, as Filter last saw it
input::Pad g_slot[2];           // what the two ports read while playing
WirePad g_live;                 // the friend's pad as it is now, outside lockstep (menus, loads)
int64_t g_live_ms = 0;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

WirePad ToWire(const input::Pad& p) {
    WirePad w{};
    w.buttons = p.buttons;
    std::memcpy(w.analog, p.analog, 8);
    w.stick[0] = p.lx;
    w.stick[1] = p.ly;
    w.stick[2] = p.rx;
    w.stick[3] = p.ry;
    return w;
}

input::Pad FromWire(const WirePad& w) {
    input::Pad p;
    p.connected = true;
    p.buttons = w.buttons;
    std::memcpy(p.analog, w.analog, 8);
    p.lx = w.stick[0];
    p.ly = w.stick[1];
    p.rx = w.stick[2];
    p.ry = w.stick[3];
    return p;
}

// "0x5C0000:256" or "5C0000:256" (hex or 0x or decimal by strtoul's base 0, plain digits then decimal)
bool ParseRegion(const std::string& s, Region& r) {
    size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    char* end = nullptr;
    unsigned long addr = std::strtoul(s.c_str(), &end, 0);
    if (end == s.c_str()) return false;
    unsigned long len = std::strtoul(s.c_str() + colon + 1, &end, 0);
    if (len == 0 || addr < kGuestLo || addr + len > kGuestEnd) return false;
    r = {uint32_t(addr), uint32_t(len)};
    return true;
}

std::vector<Region> Regions(const std::vector<std::string>& list) {
    std::vector<Region> out;
    for (const std::string& s : list) {
        Region r;
        if (ParseRegion(s, r)) out.push_back(r);
        else LOGR("netplay: region \"%s\" ignored: wants ADDRESS:LENGTH inside the console's memory", s.c_str());
    }
    return out;
}

// the guest's memory is only mapped where the game committed it: anything else faults. Every read or write of an
// address that did not come from this phone's own checks goes through this
bool Mapped(uint32_t addr, uint32_t len) {
    if (len == 0 || addr < kGuestLo || uint64_t(addr) + len > kGuestEnd) return false;
    for (uint32_t p = addr & ~(mem::kPage - 1); p < addr + len; p += mem::kPage)
        if (!mem::IsCommitted(p)) return false;
    return true;
}

// ---- state hash
uint32_t HashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return uint32_t(h ^ (h >> 32));
}

// the state hash of this frame: the named regions, else every page of the default range (kept for the diagnosis)
// netplay_hash names the regions; "auto" is the image's data and bss, which holds clocks and buffers that differ between
// two phones whatever the game does, so it is for finding the page where the games part, not for a normal session
bool HashAuto() {
    const auto& l = game::Opt().netplay_hash;
    return std::find(l.begin(), l.end(), "auto") != l.end();
}

bool HashOn() { return !game::Opt().netplay_hash.empty(); }

uint32_t HashGuest() {
    std::vector<Region> list = HashAuto() ? std::vector<Region>{} : Regions(game::Opt().netplay_hash);
    uint32_t h = 0x811C9DC5;
    if (!list.empty()) {
        for (const Region& r : list)
            if (Mapped(r.addr, r.len)) h = (h ^ HashBytes(static_cast<const uint8_t*>(kernel::HostPtr(r.addr)), r.len)) * 16777619u;
        g_pages.clear();
        return h;
    }
    g_pages.clear();
    for (uint32_t a = kHashLo; a < kHashHi; a += 0x1000) {
        uint32_t p = !Mapped(a, 0x1000) ? 0 : HashBytes(static_cast<const uint8_t*>(kernel::HostPtr(a)), 0x1000);
        g_pages.push_back(p);
        h = (h ^ p) * 16777619u;
    }
    return h;
}

// under g_mtx: the first difference is logged with this phone's page hashes; the friend's log has its own, and a
// page that differs between the two is where the games parted
void CheckHash(uint32_t frame) {
    uint32_t s = (frame / kHashEvery) % kHashSlots;
    if (g_lhash_frame[s] != frame || g_rhash_frame[s] != frame || g_lhash[s] == g_rhash[s]) return;
    if (g_desync.exchange(true)) return;
    LOGR("netplay: DESYNC at frame %u (epoch %u): mine %08x, friend's %08x", frame, g_epoch, g_lhash[s], g_rhash[s]);
    if (g_pages_frame == frame && !g_pages.empty()) {
        for (size_t i = 0; i < g_pages.size(); i += 8) {
            char line[160];
            int n = std::snprintf(line, sizeof line, "netplay: pages %06x:", unsigned(kHashLo + i * 0x1000));
            for (size_t j = i; j < std::min(i + 8, g_pages.size()); j++)
                n += std::snprintf(line + n, sizeof line - size_t(n), " %08x", g_pages[j]);
            LOGR("%s", line);
        }
    }
}

// ---- sockets
bool SameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void Send(const Writer& w) {
    if (g_udp >= 0) sendto(g_udp, w.b.data(), w.b.size(), 0, reinterpret_cast<const sockaddr*>(&g_peer), sizeof g_peer);
}

Writer Begin(Type t, uint32_t epoch) {
    Writer w;
    w.u32(kMagic);
    w.u8(t);
    w.u32(g_session);
    w.u32(epoch);
    return w;
}

void SendHello() {
    Writer w = Begin(kHello, 0);
    w.u8(kVersion);
    Send(w);
}

void SendWelcome() {
    Writer w = Begin(kWelcome, 0);
    w.u8(kVersion);
    w.u8(uint8_t(g_delay));
    Send(w);
}

void SendPing() {
    Writer w = Begin(kPing, g_epoch);
    w.u32(uint32_t(NowMs()));
    Send(w);
}

void SendBye() {
    Writer w = Begin(kBye, g_epoch);
    Send(w);
}

// under g_mtx. The guest's SYNC asks; the host's answers and carries the memory to copy.
void SendSync() {
    Writer w = Begin(kSync, g_epoch);
    uint16_t len = g_host ? uint16_t(g_payload.size()) : 0;
    w.u16(len);
    if (len) w.raw(g_payload.data(), len);
    if (!g_host) w.u8(g_copy_flag);
    Send(w);
}

// under g_mtx: every frame of mine the friend has not acknowledged, and the latest hash
void SendInput() {
    Writer w = Begin(kInput, g_epoch);
    w.u32(g_remote_next);
    uint32_t first = std::max(g_remote_ack, g_local_next > kMaxSend ? g_local_next - kMaxSend : 0u);
    uint32_t count = g_local_next > first ? g_local_next - first : 0;
    w.u32(first);
    w.u8(uint8_t(count));
    for (uint32_t i = 0; i < count; i++) w.raw(&g_local[(first + i) % kRing], sizeof(WirePad));
    w.u32(g_send_hash_frame);
    w.u32(g_send_hash);
    w.u32(g_state_rx_frame);  // the guest's ack of the host's state words
    bool attach = g_host && g_state_frame != UINT32_MAX && (g_peer_state_ack == UINT32_MAX || g_peer_state_ack < g_state_frame);
    w.u32(attach ? g_state_frame : UINT32_MAX);
    if (attach) {
        w.u16(uint16_t(g_state_pay.size()));
        w.raw(g_state_pay.data(), g_state_pay.size());
    }
    Send(w);
}

// under g_mtx: the session is over; a thread of the session's ends the loop, the panel's Stop joins it
void Fail(const std::string& why) {
    if (g_state.load() == State::Off) return;
    LOGR("netplay: %s", why.c_str());
    g_error = why;
    g_state.store(State::Off);
    g_lock_on = false;
    g_barrier = false;
    g_barrier_flag = false;
    g_quit = true;
    g_cv.notify_all();
}

// ---- saves: the host's hdd folder over TCP
bool SafeRelative(const std::string& s) {
    if (s.empty() || s.size() > 400 || s[0] == '/' || s.find('\\') != std::string::npos || s.find('\0') != std::string::npos)
        return false;
    fs::path p(s);
    for (const auto& part : p)
        if (part == ".." || part == "." || part.empty()) return false;
    return true;
}

bool SendAll(int fd, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        p += k;
        n -= size_t(k);
    }
    return true;
}

bool RecvAll(int fd, void* data, size_t n) {
    char* p = static_cast<char*>(data);
    while (n) {
        ssize_t k = recv(fd, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= size_t(k);
    }
    return true;
}

void SetTimeouts(int fd, int seconds) {
    timeval tv{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void ServeSave(int fd, bool share) {
    SetTimeouts(fd, 15);
    uint32_t magic = kSaveMagic;
    std::vector<std::pair<std::string, uint64_t>> files;
    if (share) {
        std::error_code ec;
        fs::path root = app::ExeDir() / "hdd";
        uint64_t total = 0;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            uint64_t size = it->file_size(ec);
            if (ec || total + size > kSaveCap) continue;
            total += size;
            files.emplace_back(fs::relative(it->path(), root, ec).generic_string(), size);
        }
    }
    uint32_t count = uint32_t(files.size());
    bool ok = SendAll(fd, &magic, 4) && SendAll(fd, &count, 4);
    std::vector<char> buf(1 << 16);
    for (size_t i = 0; ok && i < files.size(); i++) {
        uint16_t len = uint16_t(files[i].first.size());
        uint64_t size = files[i].second;
        std::ifstream in(app::ExeDir() / "hdd" / files[i].first, std::ios::binary);
        ok = SendAll(fd, &len, 2) && SendAll(fd, files[i].first.data(), len) && SendAll(fd, &size, 8);
        while (ok && size) {
            in.read(buf.data(), std::streamsize(std::min<uint64_t>(size, buf.size())));
            size_t got = size_t(in.gcount());
            if (!got) {
                ok = false;  // the file shrank under us: the guest sees the stream end and drops the copy
                break;
            }
            ok = SendAll(fd, buf.data(), got);
            size -= got;
        }
    }
    LOGR("netplay: host saves sent to the friend: %s (%u files)", ok ? "done" : "failed", unsigned(files.size()));
    close(fd);
}

void FetchSave(uint32_t gen, sockaddr_in host) {
    auto done = [&](const char* note) {
        if (g_gen.load() != gen) return;
        std::lock_guard lock(g_mtx);
        g_note = note;
        g_save_ready = true;
        g_cv.notify_all();
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return done("save copy failed: no socket");
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    connect(fd, reinterpret_cast<sockaddr*>(&host), sizeof host);
    pollfd pfd{fd, POLLOUT, 0};
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&pfd, 1, 5000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
        close(fd);
        return done("save copy failed: the host did not answer");
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    SetTimeouts(fd, 15);
    fs::path root = app::ExeDir();
    fs::path incoming = root / "hdd.netplay-incoming";
    std::error_code ec;
    fs::remove_all(incoming, ec);
    uint32_t magic = 0, count = 0;
    bool ok = RecvAll(fd, &magic, 4) && magic == kSaveMagic && RecvAll(fd, &count, 4) && count < 100000;
    uint64_t total = 0;
    std::vector<char> buf(1 << 16);
    for (uint32_t i = 0; ok && i < count; i++) {
        uint16_t len = 0;
        uint64_t size = 0;
        std::string rel;
        ok = RecvAll(fd, &len, 2) && len < 400;
        if (ok) {
            rel.resize(len);
            ok = RecvAll(fd, rel.data(), len) && RecvAll(fd, &size, 8) && SafeRelative(rel) && total + size <= kSaveCap;
        }
        if (!ok) break;
        total += size;
        fs::path out = incoming / rel;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream file(out, std::ios::binary);
        ok = bool(file);
        while (ok && size) {
            size_t chunk = size_t(std::min<uint64_t>(size, buf.size()));
            ok = RecvAll(fd, buf.data(), chunk);
            if (ok) file.write(buf.data(), std::streamsize(chunk));
            size -= chunk;
        }
    }
    close(fd);
    if (g_gen.load() != gen) {
        fs::remove_all(incoming, ec);
        return;
    }
    if (!ok) {
        fs::remove_all(incoming, ec);
        LOGR("netplay: the host's saves did not arrive whole, the phone's own are kept");
        return done("save copy failed: the transfer broke, your saves are untouched");
    }
    if (count == 0) {
        fs::remove_all(incoming, ec);
        return done("the host shares no saves");
    }
    // the phone's own folder is kept beside, for the last copy only
    fs::path hdd = root / "hdd", backup = root / "hdd.netplay-backup";
    fs::remove_all(backup, ec);
    fs::rename(hdd, backup, ec);
    if (ec) {
        fs::remove_all(incoming, ec);
        return done("save copy failed: could not move your saves aside, nothing changed");
    }
    fs::rename(incoming, hdd, ec);
    if (ec) {
        std::error_code undo;
        fs::rename(backup, hdd, undo);
        return done("save copy failed: nothing changed");
    }
    LOGR("netplay: host saves copied (%u files); the phone's own are in hdd.netplay-backup", count);
    done("host saves copied; yours are in hdd.netplay-backup");
}

// ---- memory comparison, for finding why two games differ. At a few frames of each epoch both phones keep a copy of the
// guest's memory (the committed pages that are not all zero), and the guest asks the host for the host's of the same
// frame over TCP and logs which pages and words differ. Other threads of the game run meanwhile, so a word that
// differs may also be one of theirs; the words that differ at every look are the ones to look at.
constexpr uint32_t kDiagMagic = 0x47444B4D;  // "MKDG"
constexpr uint32_t kDiagFrames[] = {2, 62, 302};
constexpr uint32_t kDiagMaxPages = 1500;     // fetched with their words, at most
constexpr uint32_t kDiagMaxBytes = 400u << 20;  // the host's copies held, at most
struct DiagRange {
    uint32_t lo, hi;
};
constexpr DiagRange kDiagRanges[] = {{0x10000, 0x20000000},
                                     {mem::kContiguousBase, mem::kContiguousBase + mem::kPhysicalSize},
                                     {mem::kSystemBase, mem::kSystemBase + mem::kSystemSize},
                                     {mem::kStackBase, mem::kStackBase + mem::kStackSize}};

struct Snap {
    uint32_t epoch = 0, frame = 0;
    std::vector<uint32_t> no;    // page numbers (address >> 12), ascending
    std::vector<uint32_t> hash;
    std::vector<uint8_t> data;   // 4 KB for each
};
std::mutex g_snaps_mtx;
std::vector<std::shared_ptr<Snap>> g_snaps;  // the host's

const uint8_t* ZeroPage() {
    static const std::vector<uint8_t> z(mem::kPage, 0);
    return z.data();
}

std::shared_ptr<Snap> TakeSnap(uint32_t epoch, uint32_t frame) {
    auto s = std::make_shared<Snap>();
    s->epoch = epoch;
    s->frame = frame;
    for (const DiagRange& r : kDiagRanges)
        for (uint64_t a = r.lo; a < r.hi; a += mem::kPage) {
            if (!mem::IsCommitted(uint32_t(a))) continue;
            const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(uint32_t(a)));
            if (std::memcmp(p, ZeroPage(), mem::kPage) == 0) continue;
            s->no.push_back(uint32_t(a >> 12));
            s->hash.push_back(HashBytes(p, mem::kPage));
            s->data.insert(s->data.end(), p, p + mem::kPage);
        }
    return s;
}

// the host's side of a comparison: the table of pages, then the pages the guest wants
void ServeDiag(int fd) {
    SetTimeouts(fd, 15);
    uint32_t req[2];
    if (!RecvAll(fd, req, 8)) {
        close(fd);
        return;
    }
    std::shared_ptr<Snap> s;
    {
        std::lock_guard lock(g_snaps_mtx);
        for (const auto& x : g_snaps)
            if (x->epoch == req[0] && x->frame == req[1]) s = x;
    }
    uint32_t head[3] = {kDiagMagic, s ? 0u : 1u, s ? uint32_t(s->no.size()) : 0u};
    bool ok = SendAll(fd, head, 12);
    if (ok && s) {
        std::vector<uint32_t> table;
        table.reserve(s->no.size() * 2);
        for (size_t i = 0; i < s->no.size(); i++) {
            table.push_back(s->no[i]);
            table.push_back(s->hash[i]);
        }
        uint32_t m = 0;
        ok = SendAll(fd, table.data(), table.size() * 4) && RecvAll(fd, &m, 4) && m <= kDiagMaxPages;
        std::vector<uint32_t> want(ok ? m : 0);
        ok = ok && RecvAll(fd, want.data(), size_t(m) * 4);
        for (uint32_t i = 0; ok && i < m; i++) {
            auto it = std::lower_bound(s->no.begin(), s->no.end(), want[i]);
            const uint8_t* p = it != s->no.end() && *it == want[i] ? &s->data[size_t(it - s->no.begin()) * mem::kPage] : ZeroPage();
            ok = SendAll(fd, p, mem::kPage);
        }
    }
    close(fd);
}

const char* DiagClass(uint32_t addr) {
    if (addr >= mem::kStackBase) return "stacks";
    if (addr >= mem::kSystemBase) return "system";
    if (addr >= mem::kContiguousBase) return "contiguous";
    if (addr >= kImageHi) return "heap";
    if (addr >= kImageLo) return "data";
    return "code";
}

// the guest's side, on its own thread
void CompareThread(uint32_t gen, std::shared_ptr<Snap> mine, sockaddr_in host) {
    static const uint32_t kZeroHash = HashBytes(ZeroPage(), mem::kPage);
    int fd = -1;
    std::vector<uint32_t> table;
    uint32_t n = 0;
    for (int attempt = 0; attempt < 10 && fd < 0; attempt++) {
        if (g_gen.load() != gen) return;
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return;
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
        connect(s, reinterpret_cast<sockaddr*>(&host), sizeof host);
        pollfd pfd{s, POLLOUT, 0};
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&pfd, 1, 5000) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
            close(s);
            LOGR("netplay: diag epoch %u frame %u: the host did not answer", mine->epoch, mine->frame);
            return;
        }
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
        SetTimeouts(s, 15);
        uint32_t req[3] = {kDiagMagic, mine->epoch, mine->frame}, head[3] = {};
        if (!SendAll(s, req, 12) || !RecvAll(s, head, 12) || head[0] != kDiagMagic) {
            close(s);
            return;
        }
        if (head[1] != 0 || head[2] > (1u << 20)) {  // the host is not at that frame yet
            close(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }
        n = head[2];
        table.resize(size_t(n) * 2);
        if (!RecvAll(s, table.data(), table.size() * 4)) {
            close(s);
            return;
        }
        fd = s;
    }
    if (fd < 0) return;
    // the pages that differ: the host's table against this phone's, a page one of them lacks being all zero
    std::vector<uint32_t> diff;
    size_t i = 0, j = 0;
    while (i < n || j < mine->no.size()) {
        uint32_t hp = i < n ? table[i * 2] : UINT32_MAX, mp = j < mine->no.size() ? mine->no[j] : UINT32_MAX;
        if (hp == mp) {
            if (table[i * 2 + 1] != mine->hash[j]) diff.push_back(hp);
            i++;
            j++;
        } else if (hp < mp) {
            if (table[i * 2 + 1] != kZeroHash) diff.push_back(hp);
            i++;
        } else {
            diff.push_back(mp);
            j++;
        }
    }
    std::map<std::string, uint32_t> classes;
    std::vector<uint32_t> fetch;  // not the stacks: they differ at every look
    for (uint32_t p : diff) {
        classes[DiagClass(p << 12)]++;
        if ((p << 12) < mem::kStackBase && fetch.size() < kDiagMaxPages) fetch.push_back(p);
    }
    char line[400];
    int len = std::snprintf(line, sizeof line, "netplay: diag epoch %u frame %u: %zu pages here, %u at the host, %zu differ:",
                            mine->epoch, mine->frame, mine->no.size(), n, diff.size());
    for (const auto& [name, count] : classes) len += std::snprintf(line + len, sizeof line - size_t(len), " %s %u,", name.c_str(), count);
    LOGR("%s", line);
    uint32_t m = uint32_t(fetch.size());
    std::vector<uint8_t> pages(size_t(m) * mem::kPage);
    bool ok = SendAll(fd, &m, 4) && SendAll(fd, fetch.data(), size_t(m) * 4) && RecvAll(fd, pages.data(), pages.size());
    close(fd);
    if (!ok || g_gen.load() != gen) return;
    uint64_t words = 0;
    uint32_t logged = 0, shown_pages = 0;
    for (uint32_t k = 0; k < m; k++) {
        const uint32_t addr = fetch[k] << 12;
        const uint8_t* theirs = &pages[size_t(k) * mem::kPage];
        auto it = std::lower_bound(mine->no.begin(), mine->no.end(), fetch[k]);
        const uint8_t* here = it != mine->no.end() && *it == fetch[k] ? &mine->data[size_t(it - mine->no.begin()) * mem::kPage] : ZeroPage();
        uint32_t count = 0;
        char text[500];
        int n2 = 0;
        for (uint32_t w = 0; w < mem::kPage; w += 4) {
            uint32_t a, b;
            std::memcpy(&a, theirs + w, 4);
            std::memcpy(&b, here + w, 4);
            if (a == b) continue;
            count++;
            if (count <= 6) n2 += std::snprintf(text + n2, sizeof text - size_t(n2), " +%03x %08x/%08x", w, a, b);
        }
        words += count;
        if (count && shown_pages < 90) {
            shown_pages++;
            LOGR("netplay: diag %08x (%s): %u words differ (host/here):%s", addr, DiagClass(addr), count, text);
        }
        logged += count ? 1 : 0;
    }
    LOGR("netplay: diag epoch %u frame %u: %llu words differ in %u of the %u pages fetched", mine->epoch, mine->frame,
         static_cast<unsigned long long>(words), logged, m);
}

// under g_mtx, game thread, at the start of gameplay frame f
void MaybeDiag(uint32_t f) {
    if (!game::Opt().netplay_diag || std::find(std::begin(kDiagFrames), std::end(kDiagFrames), f) == std::end(kDiagFrames)) return;
    std::shared_ptr<Snap> s = TakeSnap(g_epoch, f);
    if (g_host) {
        std::lock_guard lock(g_snaps_mtx);
        g_snaps.push_back(s);
        size_t bytes = 0;
        for (const auto& x : g_snaps) bytes += x->data.size();
        while (g_snaps.size() > 1 && (g_snaps.size() > 4 || bytes > kDiagMaxBytes)) {
            bytes -= g_snaps.front()->data.size();
            g_snaps.erase(g_snaps.begin());
        }
    } else {
        std::thread(CompareThread, g_gen.load(), s, g_peer).detach();
    }
}

// ---- state copy. The two games differ already when a level starts (counters, where the heap put things, the camera), and
// lockstep only keeps two games alike that began alike. So at each barrier the guest takes the host's pages of the
// image's data, the heap and the console's physical memory that differ from its own. The host's game thread is parked
// at its barrier meanwhile, the host's other threads are not, and the guest's own handles and kernel objects stay.
constexpr uint32_t kStateMagic = 0x54534B4D;  // "MKST"
constexpr uint32_t kStateMaxPages = 8192;
constexpr uint32_t kStampWord = 0x4A424F4B;   // kernel/objects.cpp's mark in the header of a dispatcher object the host tracks
constexpr int kStateWaitMs = 40000;
constexpr DiagRange kStateRanges[] = {{kImageLo, 0x20000000}, {mem::kContiguousBase, mem::kContiguousBase + mem::kPhysicalSize}};

bool StateIncluded(uint32_t addr) {
    for (const DiagRange& r : kStateRanges)
        if (addr >= r.lo && addr < r.hi) return true;
    return false;
}

// page number and hash of each committed page that is not all zero
std::vector<uint32_t> StateTable() {
    std::vector<uint32_t> t;
    for (const DiagRange& r : kStateRanges)
        for (uint64_t a = r.lo; a < r.hi; a += mem::kPage) {
            if (!mem::IsCommitted(uint32_t(a))) continue;
            const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(uint32_t(a)));
            if (std::memcmp(p, ZeroPage(), mem::kPage) == 0) continue;
            t.push_back(uint32_t(a >> 12));
            t.push_back(HashBytes(p, mem::kPage));
        }
    return t;
}

// the host's side: the table of its pages, then the pages the guest asks for
void ServeState(int fd) {
    SetTimeouts(fd, 20);
    uint32_t epoch = 0;
    bool ok = RecvAll(fd, &epoch, 4);
    std::vector<uint32_t> table;
    if (ok) table = StateTable();
    uint32_t head[3] = {kStateMagic, 0, uint32_t(table.size() / 2)};
    ok = ok && SendAll(fd, head, 12) && SendAll(fd, table.data(), table.size() * 4);
    uint32_t m = 0;
    ok = ok && RecvAll(fd, &m, 4) && m <= kStateMaxPages;
    std::vector<uint32_t> want(ok ? m : 0);
    ok = ok && RecvAll(fd, want.data(), size_t(m) * 4);
    for (uint32_t i = 0; ok && i < m; i++) {
        uint32_t a = want[i] << 12;
        const uint8_t* p = StateIncluded(a) && mem::IsCommitted(a) ? static_cast<const uint8_t*>(kernel::HostPtr(a)) : ZeroPage();
        ok = SendAll(fd, p, mem::kPage);
    }
    close(fd);
    LOGR("netplay: state copy served: %s (%zu pages known, %u sent)", ok ? "done" : "failed", table.size() / 2, m);
    std::lock_guard lock(g_mtx);
    g_state_copied_epoch = epoch;
    g_cv.notify_all();
}

struct StateStats {
    uint64_t written = 0, handles = 0, stamped = 0;
};

// the host's page over the guest's, word by word, but not a word that is a live handle of this phone or part of the
// header of one of its dispatcher objects: those mean something only to this phone
void ApplyStatePage(uint32_t addr, const uint8_t* host, StateStats& st) {
    uint8_t* g = static_cast<uint8_t*>(kernel::HostPtr(addr));
    uint32_t gw[1024], hw[1024];
    bool keep[1024] = {};
    std::memcpy(gw, g, mem::kPage);
    std::memcpy(hw, host, mem::kPage);
    for (uint32_t i = 3; i < 1024; i++)
        if (gw[i] == kStampWord)
            for (uint32_t k = i - 3; k <= i; k++) keep[k] = true;
    for (uint32_t i = 0; i < 1024; i++) {
        if (gw[i] == hw[i]) continue;
        if (keep[i]) {
            st.stamped++;
            continue;
        }
        uint32_t v = gw[i];
        if ((v & 3) == 0 && v >= 0x1000 && v < 0x100000 && kernel::FromHandle(v)) {
            st.handles++;
            continue;
        }
        std::memcpy(g + i * 4, &hw[i], 4);
        st.written++;
    }
}

int ConnectTcp(const sockaddr_in& host) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    sockaddr_in a = host;
    connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    pollfd pfd{s, POLLOUT, 0};
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&pfd, 1, 5000) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
    SetTimeouts(s, 20);
    return s;
}

// the guest's side, on the game thread at its barrier. false if the copy did not happen (nothing was changed then,
// or only some pages were)
bool FetchState(uint32_t epoch, const sockaddr_in& host) {
    auto t0 = Clock::now();
    static const uint32_t kZeroHash = HashBytes(ZeroPage(), mem::kPage);
    int fd = ConnectTcp(host);
    if (fd < 0) {
        LOGR("netplay: state copy failed: the host did not answer");
        return false;
    }
    uint32_t req[2] = {kStateMagic, epoch}, head[3] = {};
    if (!SendAll(fd, req, 8) || !RecvAll(fd, head, 12) || head[0] != kStateMagic || head[2] > (1u << 20)) {
        close(fd);
        LOGR("netplay: state copy failed: no table from the host");
        return false;
    }
    std::vector<uint32_t> table(size_t(head[2]) * 2);
    if (!RecvAll(fd, table.data(), table.size() * 4)) {
        close(fd);
        LOGR("netplay: state copy failed: the host's table broke");
        return false;
    }
    const std::vector<uint32_t> mine = StateTable();
    const size_t n = table.size() / 2, mn = mine.size() / 2;
    std::vector<uint32_t> want;
    uint32_t uncommitted = 0, differ = 0;
    size_t i = 0, j = 0;
    while (i < n || j < mn) {
        uint32_t hp = i < n ? table[i * 2] : UINT32_MAX, mp = j < mn ? mine[j * 2] : UINT32_MAX, page = 0;
        bool differs = false;
        if (hp == mp) {
            differs = table[i * 2 + 1] != mine[j * 2 + 1];
            page = hp;
            i++;
            j++;
        } else if (hp < mp) {
            differs = table[i * 2 + 1] != kZeroHash;  // here it is zero, or not committed
            page = hp;
            i++;
        } else {
            differs = true;  // the host's is zero
            page = mp;
            j++;
        }
        if (!differs) continue;
        differ++;
        if (!mem::IsCommitted(page << 12)) {
            uncommitted++;
            continue;
        }
        if (want.size() < kStateMaxPages) want.push_back(page);
    }
    uint32_t m = uint32_t(want.size());
    std::vector<uint8_t> pages(size_t(m) * mem::kPage);
    bool ok = SendAll(fd, &m, 4) && SendAll(fd, want.data(), size_t(m) * 4) && RecvAll(fd, pages.data(), pages.size());
    close(fd);
    if (!ok) {
        LOGR("netplay: state copy failed: the pages did not arrive, nothing changed");
        return false;
    }
    StateStats st;
    for (uint32_t k = 0; k < m; k++) ApplyStatePage(want[k] << 12, &pages[size_t(k) * mem::kPage], st);
    LOGR("netplay: state copy done in %lld ms: %u of the host's %zu pages differed (%u not committed here), %u taken, "
         "%llu words written, %llu kept as handles, %llu kept in kernel objects",
         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()), differ, n,
         uncommitted, m, static_cast<unsigned long long>(st.written), static_cast<unsigned long long>(st.handles),
         static_cast<unsigned long long>(st.stamped));
    return true;
}

// the first bytes of a connection say what it wants: a comparison or a state copy, else (nothing, the guest waits) the saves
void ServeTcp(int fd, bool share_saves) {
    pollfd pfd{fd, POLLIN, 0};
    uint32_t magic = 0;
    if (poll(&pfd, 1, 150) > 0 && recv(fd, &magic, 4, MSG_PEEK) == 4 && (magic == kDiagMagic || magic == kStateMagic)) {
        recv(fd, &magic, 4, 0);
        if (magic == kDiagMagic) ServeDiag(fd);
        else ServeState(fd);
    } else {
        ServeSave(fd, share_saves);
    }
}

// ---- packets, g_mtx held
void OnInput(Reader& r, const Header& h) {
    if (h.epoch > g_peer_epoch) g_peer_epoch = h.epoch;
    if (h.epoch != g_epoch) {
        g_cv.notify_all();
        return;
    }
    g_peer_here = true;
    uint32_t ack = r.u32(), first = r.u32();
    uint32_t count = r.u8();
    if (!r.ok) return;
    g_remote_ack = std::max(g_remote_ack, std::min(ack, g_local_next));
    for (uint32_t i = 0; i < count; i++) {
        WirePad w;
        if (!r.take(&w, sizeof w)) return;
        uint32_t frame = first + i;
        if (frame == g_remote_next) {
            g_remote[frame % kRing] = w;
            g_remote_next++;
        }
    }
    uint32_t hf = r.u32(), hv = r.u32();
    if (r.ok && hf != UINT32_MAX && hf % kHashEvery == 0) {
        uint32_t s = (hf / kHashEvery) % kHashSlots;
        g_rhash_frame[s] = hf;
        g_rhash[s] = hv;
        CheckHash(hf);
    }
    uint32_t sack = r.u32(), sf = r.u32();
    if (r.ok) {
        if (g_host) {
            if (sack != UINT32_MAX && (g_peer_state_ack == UINT32_MAX || sack > g_peer_state_ack)) g_peer_state_ack = sack;
        } else if (sf != UINT32_MAX && (g_state_rx_frame == UINT32_MAX || sf > g_state_rx_frame)) {
            uint16_t len = r.u16();
            if (r.ok && len <= kStateMax && r.n >= len) {
                g_state_pay.assign(r.p, r.p + len);
                g_state_rx_frame = sf;
            }
        }
    }
    g_cv.notify_all();
}

void OnSync(Reader& r, const Header& h) {
    if (h.epoch > g_peer_epoch) g_peer_epoch = h.epoch;
    if (h.epoch == g_epoch) {
        g_peer_here = true;
        if (!g_host) {
            uint16_t len = r.u16();
            if (r.ok && len <= kPayloadMax && r.n >= len) {
                g_payload.assign(r.p, r.p + len);
                g_sync_got = true;
            }
        } else {
            r.u16();
            uint8_t flag = r.u8();
            if (r.ok) g_peer_copy = flag == 1;
        }
        // the guest keeps asking until it has the payload: the host answers each time, even past its own barrier
        if (g_host) SendSync();
    }
    g_cv.notify_all();
}

void OnPacket(const uint8_t* data, size_t len, const sockaddr_in& from) {
    Reader r{data, len};
    if (r.u32() != kMagic) return;
    Header h;
    h.type = r.u8();
    h.session = r.u32();
    h.epoch = r.u32();
    if (!r.ok) return;
    std::lock_guard lock(g_mtx);
    State st = g_state.load();
    auto now = Clock::now();
    if (h.type == kHello && g_host) {
        if (r.u8() != kVersion) return;
        if (st == State::Hosting) {
            g_peer = from;
            g_session = std::random_device{}() | 1u;
            g_state = State::Connected;
            g_last_rx = now;
            LOGR("netplay: friend joined from %s:%u", inet_ntoa(from.sin_addr), unsigned(ntohs(from.sin_port)));
            SendWelcome();
            g_cv.notify_all();
        } else if ((st == State::Connected || st == State::Playing) && SameAddr(from, g_peer)) {
            SendWelcome();  // the first one was lost
        }
        return;
    }
    if (h.type == kWelcome && !g_host && st == State::Joining && SameAddr(from, g_peer)) {
        if (r.u8() != kVersion) return Fail("the host runs another version of the netplay");
        g_session = h.session;
        g_delay = std::clamp<uint32_t>(r.u8(), 1, 12);
        g_state = State::Connected;
        g_last_rx = now;
        LOGR("netplay: joined, input delay %u frames", g_delay);
        if (game::Opt().netplay_copy_save) {
            g_save_ready = false;
            g_note = "copying the host's saves";
            std::thread(FetchSave, g_gen.load(), g_peer).detach();
        }
        g_cv.notify_all();
        return;
    }
    if ((st != State::Connected && st != State::Playing) || !SameAddr(from, g_peer) || h.session != g_session) return;
    g_last_rx = now;
    switch (h.type) {
        case kInput: OnInput(r, h); break;
        case kSync: OnSync(r, h); break;
        case kPing: {
            uint32_t t = r.u32();
            Writer w = Begin(kPong, g_epoch);
            w.u32(t);
            Send(w);
            break;
        }
        case kPong: {
            uint32_t t = r.u32();
            if (r.ok) g_rtt = uint32_t(NowMs()) - t;
            break;
        }
        case kBye: Fail("your friend left"); break;
        case kPad: {
            WirePad w;
            if (r.take(&w, sizeof w)) {
                std::lock_guard latch(g_latch);
                g_live = w;
                g_live_ms = NowMs();
            }
            break;
        }
        default: break;
    }
}

void NetLoop() {
    auto last_hello = Clock::now() - std::chrono::seconds(1), last_ping = last_hello, last_sync = last_hello,
         last_input = last_hello, last_pad = last_hello;
    uint8_t buf[2048];
    while (!g_quit) {
        pollfd fds[2] = {{g_udp, POLLIN, 0}, {g_tcp, POLLIN, 0}};
        poll(fds, g_tcp >= 0 ? 2 : 1, 4);
        for (;;) {
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(g_udp, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < 0) break;
            OnPacket(buf, size_t(n), from);
        }
        if (g_tcp >= 0 && (fds[1].revents & POLLIN)) {
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            int fd = accept(g_tcp, reinterpret_cast<sockaddr*>(&from), &fl);
            if (fd >= 0) {
                bool mine;
                {
                    std::lock_guard lock(g_mtx);
                    mine = g_state.load() != State::Hosting && from.sin_addr.s_addr == g_peer.sin_addr.s_addr;
                }
                if (mine) std::thread(ServeTcp, fd, game::Opt().netplay_copy_save).detach();
                else close(fd);
            }
        }
        auto now = Clock::now();
        auto since = [&](Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count(); };
        std::lock_guard lock(g_mtx);
        State st = g_state.load();
        if (st == State::Joining && since(last_hello) >= 300) {
            last_hello = now;
            SendHello();
        } else if (st == State::Connected || st == State::Playing) {
            if (since(g_last_rx) > kDeadMs) {
                Fail("the connection to your friend was lost");
                break;
            }
            // outside lockstep both games read the same two pads, the host's as player 1 and the guest's as player 2, as
            // each phone's own pad now reaches the other: menus follow whoever presses, and the guest can join with Start
            if (since(last_pad) >= 8) {
                last_pad = now;
                WirePad mine;
                {
                    std::lock_guard latch(g_latch);
                    mine = ToWire(g_raw);
                }
                Writer w = Begin(kPad, g_epoch);
                w.raw(&mine, sizeof mine);
                Send(w);
            }
            if (since(last_ping) >= 500) {
                last_ping = now;
                SendPing();
            }
            if (g_barrier && since(last_sync) >= 50) {
                last_sync = now;
                SendSync();
            } else if (g_lock_on && since(last_input) >= 8) {
                last_input = now;
                SendInput();
            }
        }
    }
    std::lock_guard lock(g_mtx);
    if (g_udp >= 0) close(g_udp);
    if (g_tcp >= 0) close(g_tcp);
    g_udp = g_tcp = -1;
}

std::string LocalAddresses() {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK))
            continue;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, text, sizeof text);
        if (!out.empty()) out += ", ";
        out += text;
    }
    freeifaddrs(list);
    return out;
}

// ---- the random number generator scan (host). A generator's state is a word that, between two looks, has moved
// some steps along a fixed recurrence. Three looks that fit one recurrence leave no doubt, so the scan is not a guess
// at what the game does with the word, only at which words are the generator's.
template <uint32_t A, uint32_t C>
uint32_t Lcg(uint32_t x) {
    return x * A + C;
}
uint32_t XorShift32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}
template <uint32_t M>
uint32_t ParkMiller(uint32_t x) {
    return uint32_t(uint64_t(x) * M % 2147483647u);
}
struct Gen {
    const char* name;
    uint32_t (*step)(uint32_t);
    uint32_t a, c;  // an LCG's (checked in the scan the quick way), else 0
};
constexpr Gen kGens[] = {{"lcg 214013 2531011 (the C runtime's rand)", Lcg<214013, 2531011>, 214013, 2531011},
                         {"lcg 1664525 1013904223", Lcg<1664525, 1013904223>, 1664525, 1013904223},
                         {"lcg 69069 1", Lcg<69069, 1>, 69069, 1},
                         {"lcg 1103515245 12345", Lcg<1103515245, 12345>, 1103515245, 12345},
                         {"lcg 134775813 1", Lcg<134775813, 1>, 134775813, 1},
                         {"lcg 22695477 1", Lcg<22695477, 1>, 22695477, 1},
                         {"xorshift32", XorShift32, 0, 0},
                         {"park-miller 16807", ParkMiller<16807>, 0, 0},
                         {"park-miller 48271", ParkMiller<48271>, 0, 0}};
constexpr size_t kGenCount = sizeof kGens / sizeof kGens[0];

// walks the generator from one word to the next, up to max steps
bool Reaches(uint32_t (*step)(uint32_t), uint32_t from, uint32_t to, uint32_t max) {
    for (uint32_t k = 0; k < max; k++) {
        from = step(from);
        if (from == to) return true;
    }
    return false;
}

// a word the generator `gen` could have moved to `to` from `from` (or left alone)
bool Plausible(uint8_t gen, uint32_t from, uint32_t to) {
    return from == to || (gen < kGenCount && Reaches(kGens[gen].step, from, to, kVerifySteps));
}

// An LCG mod 2^32 with an odd increment and a multiplier of 1 mod 4 has the full period, and its low kLcgBits bits
// are the same LCG mod 2^kLcgBits, one cycle through every value: so the position of a word in that cycle, taken
// from a table, gives the steps between two words mod 2^kLcgBits without walking, and one run of the real recurrence
// (by squaring) checks it.
struct LcgTable {
    std::vector<uint32_t> pos;
    bool ok = false;
};

void BuildTable(const Gen& g, LcgTable& t) {
    const uint32_t size = 1u << kLcgBits, mask = size - 1;
    t.pos.assign(size, 0);
    uint32_t s = 0;
    for (uint32_t i = 0; i < size; i++) {
        t.pos[s] = i;
        s = (s * g.a + g.c) & mask;
    }
    t.ok = s == 0;  // back at the start after exactly one cycle
}

// x -> y in at most 2^kLcgBits steps
bool LcgReaches(const Gen& g, const LcgTable& t, uint32_t x, uint32_t y) {
    const uint32_t mask = (1u << kLcgBits) - 1;
    uint32_t n = (t.pos[y & mask] - t.pos[x & mask]) & mask;
    if (n == 0) n = mask + 1;
    uint32_t ra = 1, rc = 0, ba = g.a, bc = g.c;  // the map x -> ra * x + rc, and the one stepped by squaring
    for (uint32_t k = n; k; k >>= 1) {
        if (k & 1) {
            rc = ba * rc + bc;
            ra = ba * ra;
        }
        bc = ba * bc + bc;
        ba = ba * ba;
    }
    return ra * x + rc == y;
}

struct ScanResult {
    std::vector<Region> found;
    uint64_t moved = 0, moved_image = 0;
    std::vector<std::array<uint32_t, 4>> samples;  // address and three looks, of the image's data
};

// the words of [first, last) of three looks, for the workers
void ScanRange(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::vector<uint8_t>& c, size_t first,
               size_t last, const LcgTable* tables, bool walk, std::chrono::steady_clock::time_point deadline,
               std::mutex& out_mtx, ScanResult& out) {
    ScanResult mine;
    uint32_t tick = 0;
    bool expired = false;
    for (size_t i = first; i + 4 <= last; i += 4) {
        uint32_t x, y, z;
        std::memcpy(&x, &a[i], 4);
        std::memcpy(&y, &b[i], 4);
        std::memcpy(&z, &c[i], 4);
        if (x == y || y == z) continue;
        if (!expired && (++tick & 255) == 0 && std::chrono::steady_clock::now() >= deadline) expired = true;
        const uint32_t addr = kGuestLo + uint32_t(i);
        const bool image = addr >= kImageLo && addr < kImageHi;
        int hit = -1;
        for (size_t g = 0; g < kGenCount && hit < 0; g++) {
            const Gen& gen = kGens[g];
            if (gen.a) {
                if (!walk && tables[g].ok && LcgReaches(gen, tables[g], x, y) && LcgReaches(gen, tables[g], y, z)) hit = int(g);
            } else if (walk && !expired) {
                // walked one by one; the park-miller states are nonzero 31 bit words
                if (gen.step != XorShift32 && ((x | y | z) >> 31 || !x || !y || !z)) continue;
                if (Reaches(gen.step, x, y, kScanSteps) && Reaches(gen.step, y, z, kScanSteps)) hit = int(g);
            }
        }
        if (!walk) {
            mine.moved++;
            if (image) {
                mine.moved_image++;
                if (mine.moved_image % 16 == 1 && mine.samples.size() < 14) mine.samples.push_back({addr, x, y, z});
            }
        }
        if (hit >= 0 && mine.found.size() < 8) {
            LOGR("netplay: random number generator found at %06x (%s): %08x, %08x, %08x", addr, kGens[hit].name, x, y, z);
            Region r{addr, 4};
            r.gen = uint8_t(hit);
            mine.found.push_back(r);
        }
    }
    std::lock_guard lock(out_mtx);
    out.moved += mine.moved;
    out.moved_image += mine.moved_image;
    for (const auto& s : mine.samples) out.samples.push_back(s);
    for (const Region& r : mine.found)
        if (out.found.size() < 8) out.found.push_back(r);
}

void ScanThread(uint32_t gen, std::vector<uint8_t> a, std::vector<uint8_t> b, std::vector<uint8_t> c) {
    static std::vector<LcgTable> tables;
    static std::once_flag built;
    std::call_once(built, [] {
        tables.resize(kGenCount);
        for (size_t g = 0; g < kGenCount; g++)
            if (kGens[g].a) BuildTable(kGens[g], tables[g]);
    });
    ScanResult out;
    std::mutex out_mtx;
    const unsigned workers = 3;
    const size_t span = (a.size() / 4 / workers + 1) * 4;
    // the LCGs first, which is quick; the others are walked step by step, only if those found nothing, and for a while
    for (bool walk : {false, true}) {
        if (walk && !out.found.empty()) break;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        std::vector<std::thread> pool;
        for (unsigned w = 0; w < workers; w++)
            pool.emplace_back([&, w] {
                ScanRange(a, b, c, w * span, std::min(a.size(), (w + 1) * span), tables.data(), walk, deadline, out_mtx, out);
            });
        for (std::thread& t : pool) t.join();
    }
    LOGR("netplay: scan of the console's memory: %llu words moved in both intervals (%llu in the image's data), %u generator(s) found",
         static_cast<unsigned long long>(out.moved), static_cast<unsigned long long>(out.moved_image), unsigned(out.found.size()));
    if (out.found.empty()) {
        // for finding the game's generator by hand: some of the image's words that moved, with their three looks
        for (const auto& s : out.samples) LOGR("netplay: moved word %06x: %08x %08x %08x", s[0], s[1], s[2], s[3]);
    }
    std::lock_guard lock(g_mtx);
    g_scan_busy = false;
    if (g_gen.load() != gen || g_state.load() == State::Off) return;
    for (const Region& r : out.found) g_auto.push_back(r);
}

// under g_mtx, host, at the start of gameplay frame f of the epoch
void MaybeScan(uint32_t f) {
    if (!game::Opt().netplay_auto_rng || g_scan_busy || !g_auto.empty() || g_scan_round >= kScanRounds) return;
    uint32_t base = kScanFirst + g_scan_round * kScanRepeat;
    int which = f == base ? 0 : f == base + kScanGap ? 1 : f == base + 2 * kScanGap ? 2 : -1;
    if (which < 0 || which != g_snap_n) return;
    // the committed pages only, the others stay zero in every look
    g_snap[which].assign(kGuestEnd - kGuestLo, 0);
    for (uint32_t a = kGuestLo; a < kGuestEnd; a += mem::kPage)
        if (mem::IsCommitted(a)) std::memcpy(&g_snap[which][a - kGuestLo], kernel::HostPtr(a), mem::kPage);
    g_snap_n = which + 1;
    if (which < 2) return;
    g_snap_n = 0;
    g_scan_busy = true;
    g_scan_round++;
    std::thread(ScanThread, g_gen.load(), std::move(g_snap[0]), std::move(g_snap[1]), std::move(g_snap[2])).detach();
}

// under g_mtx, game thread: the guest memory the host chose, in the payload: a count, then address, length,
// generator and contents of each region
std::vector<uint8_t> MakePayload(size_t max) {
    Writer w;
    // a found word that no longer moves along its generator is not one any more (a level was unloaded): dropped
    for (size_t i = 0; i < g_auto.size();) {
        Region& r = g_auto[i];
        uint32_t v = 0;
        const bool mapped = Mapped(r.addr, 4);
        if (mapped) std::memcpy(&v, kernel::HostPtr(r.addr), 4);
        if (!mapped || (r.seen && !Plausible(r.gen, r.last, v))) {
            LOGR("netplay: the word at %06x no longer moves like a generator, dropped", r.addr);
            g_auto.erase(g_auto.begin() + long(i));
            continue;
        }
        r.last = v;
        r.seen = true;
        i++;
    }
    std::vector<Region> list = Regions(game::Opt().netplay_sync);
    list.insert(list.end(), g_auto.begin(), g_auto.end());
    w.u8(0);
    uint8_t n = 0;
    for (const Region& r : list) {
        if (!Mapped(r.addr, r.len)) continue;
        if (w.b.size() + 9 + r.len > max) {
            LOGR("netplay: sync region %06x+%u does not fit one packet, skipped", r.addr, r.len);
            continue;
        }
        w.u32(r.addr);
        w.u32(r.len);
        w.u8(r.gen);
        w.raw(kernel::HostPtr(r.addr), r.len);
        n++;
    }
    w.b[0] = n;
    return std::move(w.b);
}

// writes them into the guest's memory; how many words of it changed
uint32_t ApplyBytes(const std::vector<uint8_t>& bytes) {
    Reader r{bytes.data(), bytes.size()};
    uint8_t n = r.u8();
    uint32_t changed = 0;
    for (uint8_t i = 0; r.ok && i < n; i++) {
        uint32_t addr = r.u32(), len = r.u32();
        uint8_t gen = r.u8();
        if (!r.ok || len > r.n || addr < kGuestLo || uint64_t(addr) + len > kGuestEnd) break;
        if (!Mapped(addr, len)) {  // not committed on this phone (the heaps differ): not ours to write
            r.p += len;
            r.n -= len;
            continue;
        }
        bool write = true;
        if (gen != kNoGen && len == 4) {
            uint32_t cur;
            std::memcpy(&cur, kernel::HostPtr(addr), 4);
            auto it = g_gw.find(addr);
            if (it == g_gw.end()) {
                it = g_gw.emplace(addr, GuestWord{cur, false}).first;
                write = false;
                LOGR("netplay: watching the word at %06x (%08x here) before copying the host's", addr, cur);
            } else {
                GuestWord& gw = it->second;
                bool moved_like = cur != gw.last && Plausible(gen, gw.last, cur);
                if (!gw.verified) {
                    if (moved_like) {
                        gw.verified = true;
                        LOGR("netplay: the word at %06x moves like the generator here too, copying it", addr);
                    } else {
                        gw.last = cur;
                        write = false;
                        g_stat_unverified++;
                    }
                } else if (cur != gw.last && !moved_like) {
                    gw.verified = false;
                    gw.last = cur;
                    write = false;
                    LOGR("netplay: the word at %06x stopped moving like the generator, left alone", addr);
                }
            }
            if (write) {
                uint32_t host_value;
                std::memcpy(&host_value, r.p, 4);
                g_gw[addr].last = host_value;
            }
        }
        if (write) {
            if (std::memcmp(kernel::HostPtr(addr), r.p, len) != 0) changed++;
            std::memcpy(kernel::HostPtr(addr), r.p, len);
        }
        r.p += len;
        r.n -= len;
    }
    return changed;
}

void BuildPayload() { g_payload = MakePayload(kPayloadMax); }

void ApplyPayload() { ApplyBytes(g_payload); }

// under g_mtx: a new epoch: frame 0 on both sides after both have arrived. false when the session ended.
bool Barrier(std::unique_lock<std::mutex>& lk) {
    auto begin = [] {
        g_epoch = std::max(g_epoch + 1, g_peer_epoch);
        g_peer_here = g_sync_got = false;
        g_peer_copy = false;
        g_copy_flag = !g_host && game::Opt().netplay_state_copy ? 1 : 0;
        g_frame = 0;
        g_local_next = g_remote_next = g_remote_ack = g_delay;
        for (uint32_t i = 0; i < g_delay; i++) g_local[i] = g_remote[i] = WirePad{};
        g_send_hash_frame = UINT32_MAX;
        g_state_frame = g_state_rx_frame = g_peer_state_ack = UINT32_MAX;
        g_state_pay.clear();
        g_scan_round = 0;  // each epoch (a level) looks again, unless a generator is known
        g_snap_n = 0;
        std::fill(std::begin(g_lhash_frame), std::end(g_lhash_frame), UINT32_MAX);
        std::fill(std::begin(g_rhash_frame), std::end(g_rhash_frame), UINT32_MAX);
        if (g_host) BuildPayload();
        else g_payload.clear();
        g_barrier = true;
        g_barrier_flag = true;
    };
    g_state = State::Connected;
    g_lock_on = false;
    begin();
    LOGR("netplay: epoch %u: waiting for the friend to reach gameplay", g_epoch);
    SendSync();
    Clock::time_point copy_since{};
    for (;;) {
        if (g_state.load() == State::Off) return false;
        if (g_peer_epoch > g_epoch) begin();
        if (g_peer_here && (g_host || g_sync_got)) {
            if (g_host) {
                // the guest takes the host's pages first: the host's game stays where it is until then
                if (g_peer_copy && g_state_copied_epoch != g_epoch) {
                    if (copy_since == Clock::time_point{}) copy_since = Clock::now();
                    if (Clock::now() - copy_since < std::chrono::milliseconds(kStateWaitMs)) {
                        g_cv.wait_for(lk, std::chrono::milliseconds(20));
                        continue;
                    }
                    LOGR("netplay: the friend's state copy did not finish, going on");
                }
            } else if (g_copy_flag == 1) {
                const uint32_t epoch = g_epoch;
                const sockaddr_in host = g_peer;
                lk.unlock();
                FetchState(epoch, host);
                lk.lock();
                g_copy_flag = 2;
                SendSync();
                if (g_state.load() == State::Off) return false;
                if (g_peer_epoch > g_epoch) continue;
            }
            break;
        }
        g_cv.wait_for(lk, std::chrono::milliseconds(20));
    }
    if (!g_host) ApplyPayload();
    g_barrier = false;
    g_barrier_flag = false;
    g_lock_on = true;
    g_state = State::Playing;
    g_last_step_ms = NowMs();
    LOGR("netplay: epoch %u: both at gameplay, lockstep on (%s, %u frames of delay)", g_epoch,
         g_host ? "player 1" : "player 2", g_delay);
    return true;
}

bool Start(bool host, const std::string& target) {
    Stop();
    std::lock_guard lock(g_mtx);
    g_gen++;
    g_auto.clear();
    g_gw.clear();
    g_scan_round = 0;
    g_snap_n = 0;
    g_error.clear();
    g_note.clear();
    g_quit = false;
    g_host = host;
    g_epoch = g_peer_epoch = g_frame = 0;
    g_lock_on = g_barrier = false;
    g_barrier_flag = false;
    g_desync = false;
    g_save_ready = true;
    g_rtt = 0;
    g_last_step_ms = 0;
    g_session = 0;
    g_delay = std::clamp<uint32_t>(game::Opt().netplay_delay, 1, 12);
    g_port = uint16_t(std::clamp<uint32_t>(game::Opt().netplay_port, 1024, 65535));
    g_peer = {};
    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons(g_port);
    g_target = target;
    if (!host) {
        std::string ip = target;
        // "address:port"
        size_t colon = ip.rfind(':');
        if (colon != std::string::npos && ip.find(':') == colon) {
            g_peer.sin_port = htons(uint16_t(std::strtoul(ip.c_str() + colon + 1, nullptr, 10)));
            ip.resize(colon);
        }
        if (inet_pton(AF_INET, ip.c_str(), &g_peer.sin_addr) != 1) {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            if (getaddrinfo(ip.c_str(), nullptr, &hints, &res) != 0 || !res) {
                g_error = "that address is not valid";
                return false;
            }
            g_peer.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        if (g_peer.sin_port == 0) g_peer.sin_port = htons(g_port);
    }
    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp < 0) {
        g_error = "no socket";
        return false;
    }
    fcntl(g_udp, F_SETFL, fcntl(g_udp, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(g_udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = host ? htons(g_port) : 0;
    if (bind(g_udp, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0) {
        g_error = host ? "the port is taken" : "no socket";
        close(g_udp);
        g_udp = -1;
        return false;
    }
    if (host) {
        g_addrs = LocalAddresses();
        g_tcp = socket(AF_INET, SOCK_STREAM, 0);
        if (g_tcp >= 0) {
            fcntl(g_tcp, F_SETFL, fcntl(g_tcp, F_GETFL) | O_NONBLOCK);
            setsockopt(g_tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (bind(g_tcp, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0 || listen(g_tcp, 2) != 0) {
                LOGR("netplay: no TCP on port %u, the save copy is off", unsigned(g_port));
                close(g_tcp);
                g_tcp = -1;
            }
        }
    }
    g_last_rx = Clock::now();
    g_state = host ? State::Hosting : State::Joining;
    g_thread = std::thread(NetLoop);
    LOGR("netplay: %s on port %u", host ? "hosting" : "joining", unsigned(g_port));
    return true;
}

}  // namespace

void Host() { Start(true, ""); }

void Join(const std::string& address) { Start(false, address); }

void Stop() {
    {
        std::lock_guard lock(g_mtx);
        if (g_state.load() == State::Connected || g_state.load() == State::Playing) SendBye();
        if (g_state.load() != State::Off) g_error.clear();
        g_state = State::Off;
        g_lock_on = g_barrier = false;
        g_barrier_flag = false;
        g_quit = true;
        g_cv.notify_all();
    }
    if (g_thread.joinable() && g_thread.get_id() != std::this_thread::get_id()) g_thread.join();
}

State GetState() { return g_state.load(std::memory_order_relaxed); }

bool InSync() { return !g_desync.load(std::memory_order_relaxed); }

std::string Status() {
    std::lock_guard lock(g_mtx);
    char text[256];
    switch (g_state.load()) {
        case State::Off: return g_error;
        case State::Hosting:
            std::snprintf(text, sizeof text, "HOSTING ON %s PORT %u, WAITING FOR YOUR FRIEND", g_addrs.empty() ? "?" : g_addrs.c_str(),
                          unsigned(g_port));
            return text;
        case State::Joining: return "JOINING " + g_target;
        case State::Connected:
            if (!g_save_ready) return "COPYING THE HOST'S SAVES";
            if (g_barrier) return "WAITING FOR YOUR FRIEND TO REACH GAMEPLAY";
            return "CONNECTED, START THE SAME CO-OP GAME ON BOTH PHONES";
        case State::Playing:
            if (g_wait_since.load() && NowMs() - g_wait_since.load() > 750) return "WAITING FOR YOUR FRIEND";
            std::snprintf(text, sizeof text, "PLAYER %d, PING %u MS, DELAY %u, %s", g_host ? 1 : 2, unsigned(g_rtt.load()),
                          unsigned(g_delay), g_desync ? "OUT OF SYNC" : "IN SYNC");
            return text;
    }
    return "";
}

void Filter(int port, input::Pad& pad) {
    if (port == 0) {
        std::lock_guard lock(g_latch);
        g_raw = pad;
    }
    State st = g_state.load(std::memory_order_relaxed);
    if (st != State::Connected && st != State::Playing) return;
    if (port != 0 && port != 1) return;
    bool playing = st == State::Playing && NowMs() - g_last_step_ms.load(std::memory_order_relaxed) < kGapMs;
    std::lock_guard lock(g_latch);
    if (playing) {
        pad = g_slot[port];
        return;
    }
    // not in lockstep: the host's live pad is player 1 and the guest's live pad player 2, on both phones
    bool host = g_host.load();
    input::Pad remote;
    remote.connected = true;
    if (NowMs() - g_live_ms < 300) remote = FromWire(g_live);
    if (host) {
        if (port == 1) pad = remote;
    } else {
        pad = port == 0 ? remote : g_raw;
        pad.connected = true;
    }
}

void Rumble(int port, uint16_t& left, uint16_t& right) {
    if (g_state.load(std::memory_order_relaxed) != State::Playing) return;
    // the friend's motors are not on this phone. The guest's own are on port 1, which the phone has no pad for.
    if (g_host.load() && port == 0) return;
    left = 0;
    right = 0;
}

void Step() {
    g_skip_now = -1;
    State st = g_state.load(std::memory_order_acquire);
    if ((st != State::Connected && st != State::Playing) || !g_save_ready.load()) return;
    input::Poll(0);  // the phone's own pad now, kept by Filter
    std::unique_lock lock(g_mtx);
    // no gameplay frame for a while, though the game was running (not paused in the background): it left gameplay,
    // for a menu or a load, and the epoch ends. Both phones leave at the same frame, so they meet at a new barrier.
    static uint32_t seen_pauses = 0;
    uint32_t pauses = game::g_focus_pauses.load();
    bool was_paused = pauses != seen_pauses;
    seen_pauses = pauses;
    if (g_lock_on && !was_paused && NowMs() - g_last_step_ms.load() > kGapMs) {
        LOGR("netplay: no gameplay frame for %lld ms, leaving epoch %u", static_cast<long long>(NowMs() - g_last_step_ms.load()),
             g_epoch);
        g_lock_on = false;
    }
    for (;;) {
        State now_state = g_state.load();
        if (now_state != State::Connected && now_state != State::Playing) return;
        if (!g_lock_on && !Barrier(lock)) return;
        const uint32_t f = g_frame;
        {
            std::lock_guard latch(g_latch);
            WirePad mine = ToWire(g_raw);
            mine.flags = g_skip_want.load() ? 1 : 0;
            g_local[(f + g_delay) % kRing] = mine;
        }
        g_local_next = f + g_delay + 1;
        if (f % kHashEvery == 0 && HashOn()) {
            uint32_t h = HashGuest();
            uint32_t s = (f / kHashEvery) % kHashSlots;
            g_lhash_frame[s] = f;
            g_lhash[s] = h;
            g_pages_frame = f;
            g_send_hash_frame = f;
            g_send_hash = h;
            CheckHash(f);
        }
        // the host's state words (the game's random numbers) are copied to the guest at the start of every kStateEvery-th
        // frame, so the two games' generators stay one however many times either was drawn from in between
        if (g_host && f > 0 && f % kStateEvery == 0) {
            g_state_pay = MakePayload(kStateMax);
            g_state_frame = f;
        }
        if (g_host) MaybeScan(f);
        MaybeDiag(f);
        const bool needs_state = !g_host && f > 0 && f % kStateEvery == 0;
        SendInput();
        auto wait_start = Clock::now();
        // A frame that is slow to come is waited for, however long the link lives (a phone in the background, a bad
        // patch of Wi-Fi): leaving the epoch here would leave one phone up to the delay's frames ahead of the other,
        // and the games would differ for good. Only a friend who began a new epoch (it left gameplay) ends the wait.
        bool resync = false;
        while (g_remote_next <= f || (needs_state && (g_state_rx_frame == UINT32_MAX || g_state_rx_frame < f))) {
            if (g_state.load() == State::Off) return;
            if (g_peer_epoch > g_epoch) {
                resync = true;
                break;
            }
            if (!g_wait_since.load()) g_wait_since = NowMs();
            g_cv.wait_for(lock, std::chrono::milliseconds(4));
        }
        g_wait_since = 0;
        {
            uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - wait_start).count());
            g_stat_frames++;
            g_stat_wait_us += us;
            g_stat_max_us = std::max(g_stat_max_us, us);
            if (us > 4000) g_stat_late++;
            if (g_stat_frames >= 600) {
                LOGR("netplay: 600 frames, waited %llu us each on average, %llu over 4 ms, longest %llu ms, ping %u ms, delay %u; "
                     "state words: %llu copies had to change the guest's (of 10), %zu found by the scan, %llu not trusted yet",
                     static_cast<unsigned long long>(g_stat_wait_us / g_stat_frames), static_cast<unsigned long long>(g_stat_late),
                     static_cast<unsigned long long>(g_stat_max_us / 1000), unsigned(g_rtt.load()), g_delay,
                     static_cast<unsigned long long>(g_stat_corr), g_auto.size(),
                     static_cast<unsigned long long>(g_stat_unverified));
                g_stat_unverified = 0;
                g_stat_frames = g_stat_wait_us = g_stat_max_us = g_stat_late = 0;
                g_stat_corr = g_stat_words = 0;
            }
        }
        if (resync) {
            LOGR("netplay: the friend left gameplay at frame %u, leaving epoch %u", f, g_epoch);
            g_lock_on = false;
            continue;
        }
        if (needs_state) {
            uint32_t changed = ApplyBytes(g_state_pay);
            g_stat_words += g_state_pay.size() > 1 ? 1 : 0;
            if (changed) g_stat_corr++;
        }
        WirePad mine = g_local[f % kRing], theirs = g_remote[f % kRing];
        {
            std::lock_guard latch(g_latch);
            g_slot[0] = FromWire(g_host ? mine : theirs);
            g_slot[1] = FromWire(g_host ? theirs : mine);
        }
        g_skip_now = ((mine.flags | theirs.flags) & 1) ? 1 : 0;
        g_frame = f + 1;
        g_last_step_ms = NowMs();
        return;
    }
}

void SetSkipWant(bool want) { g_skip_want = want; }

int SkipThisFrame() { return g_skip_now.load(); }

}  // namespace netplay
