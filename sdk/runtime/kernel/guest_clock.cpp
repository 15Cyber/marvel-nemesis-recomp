// A monotonic clock shared by the guest's counters, delays and timers.
#include <atomic>
#include <chrono>
#include <mutex>

#include "kernel/kernel.h"

namespace kernel {
namespace {

// Sequentially consistent atomics keep the read-side snapshot race-free without a hot-path mutex.
std::atomic<uint64_t> g_version{0};
std::atomic<int64_t> g_total_ns{0}, g_held_ns{-1};
std::mutex g_writer;

int64_t HostNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int64_t GuestNowNs() {
    for (;;) {
        const uint64_t version = g_version.load();
        if (version & 1) continue;
        const int64_t held = g_held_ns.load(), total = g_total_ns.load();
        const int64_t now = held >= 0 ? held : HostNs() - total;
        if (version == g_version.load()) return now;
    }
}

std::chrono::steady_clock::time_point GuestNow() {
    using namespace std::chrono;
    return steady_clock::time_point(duration_cast<steady_clock::duration>(nanoseconds(GuestNowNs())));
}

bool GuestTimeHeld() { return g_held_ns.load() >= 0; }

void HoldGuestTime(bool held) {
    std::lock_guard lock(g_writer);
    if (held == GuestTimeHeld()) return;
    g_version.fetch_add(1);
    const int64_t now = HostNs();
    if (held) g_held_ns.store(now - g_total_ns.load());
    else {
        g_total_ns.store(now - g_held_ns.load());
        g_held_ns.store(-1);
    }
    g_version.fetch_add(1);
}

}  // namespace kernel
