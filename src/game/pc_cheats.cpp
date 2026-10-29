// Retail default.xbe only. Keep actor access on the guest thread; UI sends requests.
#include "common/pc_cheats.h"
#include "common/pc_survival.h"
#include "core/log.h"
#include "xdk/hle.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>

namespace {
constexpr uint32_t kPlayers = 0x4F3B40, kStride = 0xB0, kCount = 0x4F40C0;
constexpr uint32_t kHealth = 0x32C, kMaxHealth = 0x330, kEnergy = 0x334, kMaxEnergy = 0x338;
std::array<std::atomic<uint64_t>, 2> g_damage{}, g_energy{}, g_refilled{};
std::array<uint32_t, 2> g_seen{};

bool Address(uint32_t p, uint32_t size) {
    if (p < 0x10000 || !size || uint64_t(p) + size > 0x100000000ull) return false;
    for (uint64_t page = p & ~uint64_t(0xFFF); page < uint64_t(p) + size; page += 0x1000)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
// The same slot table feeds HUD getters 0x10FCF0/0x10FD30. Binder 0x983D0
// sets both record+0x28 and actor+0x30C. Never retain an actor between calls.
uint32_t Player(int p) {
    const uint32_t count = kernel::R32(kCount);
    if (p < 0 || p > 1 || count > 8 || uint32_t(p) >= count) return 0;
    const auto record = kPlayers + uint32_t(p) * kStride;
    const auto actor = kernel::R32(record + 0x28);
    if (!Address(actor, 0x408) || kernel::G<uint8_t>(actor + 0x10) != 7 ||
        kernel::R32(actor + 0x30C) != record) return 0;
    return actor;
}
int Slot(uint32_t actor) {
    for (int p = 0; p < 2; ++p) if (actor && Player(p) == actor) return p;
    return -1;
}
bool Alive(uint32_t actor) {
    const float life = kernel::G<float>(actor + kHealth);
    return std::isfinite(life) && life > 0 && (kernel::R32(actor + 0x314) & 0x100) == 0;
}
bool Maximum(uint32_t actor, bool energy, float& value) {
    value = kernel::G<float>(actor + (energy ? kMaxEnergy : kMaxHealth));
    return std::isfinite(value) && value > 0;
}
void Fill(uint32_t actor, bool energy) {
    float maximum;
    if (!Alive(actor) || !Maximum(actor, energy, maximum)) return;
    kernel::G<float>(actor + (energy ? kEnergy : kHealth)) = maximum;
    if (!energy) kernel::G<float>(actor + 0x33C) = maximum;  // HUD/recoverable health
}
void Maintain(uint32_t actor, int p) {
    if (p < 0 || !Alive(actor)) return;
    if (game::pc_cheats::Enabled(p, false)) Fill(actor, false);
    if (game::pc_cheats::Enabled(p, true)) Fill(actor, true);
}
}

namespace game::pc_cheats {
void Tick(X86Context& ctx) {
    const auto unlock = TakeUnlock();
    if (unlock != Unlock::None) {
        // Run the preserved native routines, including their existing record loops.
        const X86Context saved = ctx;
        switch (unlock) {
        case Unlock::All: kernel::CallGuest(ctx, 0xE3AB0, {}); break;
        case Unlock::Characters: kernel::CallGuest(ctx, 0x2ACF0, {1}); break;
        case Unlock::Arenas: kernel::CallGuest(ctx, 0x2AD10, {1}); break;
        case Unlock::Cards: kernel::CallGuest(ctx, 0xE4740, {1}); break;
        case Unlock::Comics: kernel::CallGuest(ctx, 0xE4760, {1}); break;
        default: break;
        }
        ctx = saved;
        Complete(Result::Unlocked);
        LOGR("cheats: native unlock action=%u applied (no automatic save requested)", unsigned(unlock));
    }
    const auto refills = TakeRefills();
    const bool active = Active();
    if (!active && !refills) {
        for (int p = 0; p < 2; ++p) PublishPlayer(p, 0);
        return;
    }
    bool refill_ok = true;
    for (int p = 0; p < 2; ++p) {
        const auto actor = Player(p);
        PublishPlayer(p, actor);
        if (actor != g_seen[p]) {
            LOGR("cheats: player=%d actor=%08X record=%08X count=%u", p + 1, actor,
                 kPlayers + uint32_t(p) * kStride, kernel::R32(kCount));
            g_seen[p] = actor;
        }
        if ((refills & (1u << p)) != 0) {
            const bool ok = actor && Alive(actor);
            if (ok) { Fill(actor, false); Fill(actor, true); ++g_refilled[p]; }
            refill_ok &= ok;
            LOGR("cheats: refill player=%d actor=%08X applied=%u", p + 1, actor, unsigned(ok));
        }
        if (actor) Maintain(actor, p);
    }
    if (refills) Complete(refill_ok ? Result::Refilled : Result::NoPlayer);
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::seconds(5);
        for (int p = 0; p < 2; ++p) {
            const auto actor = Player(p);
            LOGR("cheats: summary player=%d actor=%08X health=%.2f/%.2f energy=%.2f/%.2f blocked_damage=%llu free_energy=%llu refills=%llu",
                 p + 1, actor, actor ? kernel::G<float>(actor + kHealth) : 0.0f,
                 actor ? kernel::G<float>(actor + kMaxHealth) : 0.0f,
                 actor ? kernel::G<float>(actor + kEnergy) : 0.0f,
                 actor ? kernel::G<float>(actor + kMaxEnergy) : 0.0f,
                 static_cast<unsigned long long>(g_damage[p].load()), static_cast<unsigned long long>(g_energy[p].load()),
                 static_cast<unsigned long long>(g_refilled[p].load()));
        }
    }
}
}  // namespace game::pc_cheats

X86_FUNC(__imp__sub_00030510);
X86_FUNC(sub_00030510) {
    // Entry argument 4 is the damage amount copied to a local at 0x30513/0x3051B.
    // Keep the native control flow and x87 result; substitute zero before death is decided.
    const int p = game::pc_cheats::Active() ? Slot(ctx.ecx) : -1;
    const uint32_t argument = ctx.esp + 0x10;
    const bool cheat = game::pc_cheats::Enabled(p, false);
    const bool friendly = game::pc_survival::FriendlyDamage(ctx, ctx.ecx, kernel::R32(ctx.esp + 4));
    if ((cheat || friendly || game::pc_survival::Protected(ctx.ecx)) && Alive(ctx.ecx) &&
        std::isfinite(kernel::G<float>(argument)) && kernel::G<float>(argument) > 0) {
        const uint32_t saved = kernel::R32(argument);
        kernel::W32(argument, 0);
        if (cheat) ++g_damage[p];
        __imp__sub_00030510(ctx, base);
        kernel::W32(argument, saved);
    } else __imp__sub_00030510(ctx, base);
}

X86_FUNC(__imp__sub_000902C0);
X86_FUNC(sub_000902C0) {
    const int p = game::pc_cheats::Active() ? Slot(ctx.ecx) : -1;
    if (game::pc_cheats::Enabled(p, true) && Alive(ctx.ecx)) {
        // Native freepsi returns success without consumption; scope it to this actor.
        Fill(ctx.ecx, true);
        ++g_energy[p];
        kernel::Return(ctx, 1, 1);
    } else __imp__sub_000902C0(ctx, base);
}

X86_FUNC(__imp__sub_000A35B0);
X86_FUNC(sub_000A35B0) {
    const uint32_t actor = ctx.ecx;
    const int p = game::pc_cheats::Active() ? Slot(actor) : -1;
    if (p >= 0) Maintain(actor, p);
    __imp__sub_000A35B0(ctx, base);
    // Revalidate after guest callbacks; a stage transition may have detached the actor.
    if (p >= 0 && Player(p) == actor) Maintain(actor, p);
}
