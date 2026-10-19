#pragma once
#include <array>
#include <cstdint>

struct X86Context;

namespace game::pc_cheats {
struct Settings {
    std::array<bool, 2> health{}, energy{};
    bool diagnostics = false;
    bool operator==(const Settings&) const = default;
};
enum class Unlock : uint32_t { None, All, Characters, Arenas, Cards, Comics };
enum class Result : uint32_t { None, Queued, Unlocked, Refilled, NoPlayer };
Settings Read();
void Set(const Settings& settings);  // Session only; never written to settings.toml.
bool Enabled(int player, bool energy);
bool Active();
const char* UnlockLabel(Unlock unlock);
bool RequestUnlock(Unlock unlock);
void RequestRefill(int player);
const char* Status();
Result LastResult();
bool PlayerPresent(int player);
void CancelConfirmation();
void Draw(Settings& draft);

// Only the guest-thread integration consumes requests or touches guest actors.
Unlock TakeUnlock();
uint32_t TakeRefills();
void Complete(Result result);
void PublishPlayer(int player, uint32_t actor);
void Tick(X86Context& ctx);
}  // namespace game::pc_cheats
