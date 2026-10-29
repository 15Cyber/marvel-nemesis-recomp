// APT integration for the supported default.xbe. No disc assets are modified.
// CMenu owns a flat array of clip names, m_nSize, n (columns) and m (rows).
// Append an entry, then reuse the native text clips for paged PC settings.
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "xdk/hle.h"
#include "app/app.h"
#include "core/log.h"
#include "core/input.h"
#include "common/pc_controls.h"
#include "common/settings_panel.h"
#include "common/pc_cheats.h"
#include "common/pc_development.h"
#include "common/desktop_menu.h"

namespace {
constexpr uint32_t kPlayer = 0x518DA0, kVm = 0x518F58;
constexpr const char* kButton = "m_pc_settings";
thread_local bool g_busy = false;
constexpr std::array<const char*, 7> kKeys = {"keyUp", "keyDown", "keyLeft", "keyRight", "keySelect", "keyBack", "keyStart"};
struct Entry {
    uint32_t root, menu, select;
    int index;
    std::string path;
    std::array<uint32_t, kKeys.size()> keys{};
    bool development = false; // Options only; never launch scenes from Pause.
};
std::vector<Entry> g_entries;
std::mutex g_entries_lock;
uint32_t g_last_music_event = 0;
uint32_t g_last_music_flags = 0;
uint32_t g_last_music_controller = 0;
uint32_t g_requested_music_volume = 0x3F800000u;
bool g_suppressed_music_event = false;
struct SavedRow {
    std::string name, text, auto_size;
    uint32_t clip = 0, field = 0, color = 0, data_flags = 0;
    std::array<float, 6> matrix{};
};
struct NativePage {
    Entry entry;
    std::vector<SavedRow> rows;
    size_t top = 0;
};
std::optional<NativePage> g_page;
std::optional<Entry> g_pending;
std::chrono::steady_clock::time_point g_ignore_until{};

bool Address(uint32_t p, uint32_t size = 4) {
    if (p < 0x10000 || !size || uint64_t(p) + size > 0x100000000ull) return false;
    for (uint64_t page = p & ~uint64_t(0xFFF); page < uint64_t(p) + size; page += 0x1000)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
uint32_t Type(uint32_t value) {
    return Address(value, 12) && (kernel::R32(value + 4) & 0x8000) ? kernel::R32(value + 4) & 0x3F : 0;
}
bool Clip(uint32_t value) { const auto t = Type(value); return t >= 0x0C && t <= 0x12 && Address(value, 0x60); }

// Keep names for the lifetime of the VM. A name handle points to a ref-counted
// token: u16 uses/length/capacity/hash, followed by its zero-terminated text.
uint32_t Name(const std::string& text) {
    static std::map<std::string, uint32_t> names;
    auto& at = names[text];
    if (!at) {
        at = mem::AllocSystem(uint32_t(text.size()) + 13);
        kernel::W32(at, at + 4);
        kernel::G<uint16_t>(at + 4) = 0x4000;
        kernel::G<uint16_t>(at + 6) = uint16_t(text.size());
        kernel::G<uint16_t>(at + 8) = uint16_t(text.size());
        kernel::G<uint16_t>(at + 10) = 0;
        std::memcpy(kernel::HostPtr(at + 12), text.c_str(), text.size() + 1);
    }
    return at;
}
uint32_t Text(const std::string& text) { return Name(text) + 12; }
uint32_t Get(X86Context& ctx, uint32_t object, const char* key) {
    if (!Type(object)) return 0;
    if (Clip(object) && !Address(kernel::R32(object + 0x48), 0x14)) return 0;
    return kernel::CallGuest(ctx, 0x11EBF0, {object, Name(key)});
}
uint32_t Own(X86Context& ctx, uint32_t object, const char* key) {
    if (!Type(object)) return 0;
    if (Clip(object) && !Address(kernel::R32(object + 0x48), 0x14)) return 0;
    const uint32_t vtable = kernel::R32(object);
    if (!Address(vtable, 16)) return 0;
    ctx.ecx = object;
    const uint32_t members = kernel::CallGuest(ctx, kernel::R32(vtable + 0x0C), {});
    if (!Address(members, 16)) return 0;
    ctx.ecx = members;
    const uint32_t value = kernel::CallGuest(ctx, 0x1246A0, {Name(key)});
    return Type(value) ? value : 0;
}
bool Set(X86Context& ctx, uint32_t object, const char* key, uint32_t value) {
    if (!Type(object) || !Type(value)) return false;
    if (Clip(object) && !Address(kernel::R32(object + 0x48), 0x14)) return false;
    ctx.ecx = kVm;
    return (kernel::CallGuest(ctx, 0x134ED0, {object, 0, Name(key), value, 1, 0, 1}) & 0xFF) != 0;
}
uint32_t String(X86Context& ctx, const std::string& text) {
    const uint32_t value = kernel::CallGuest(ctx, 0x11F800, {});
    if (Type(value) != 1) return 0;
    ctx.ecx = value;
    kernel::CallGuest(ctx, 0x11F230, {Text(text)});
    return value;
}
uint32_t Integer(X86Context& ctx, int number) {
    const uint32_t value = kernel::CallGuest(ctx, 0x11EF80, {16});
    if (!Address(value, 16)) return 0;
    std::memset(kernel::HostPtr(value), 0, 16);
    ctx.ecx = value;
    kernel::CallGuest(ctx, 0x11FA90, {7});
    kernel::W32(value + 8, uint32_t(number));
    return value;
}
int Number(uint32_t value, int fallback = -1) {
    const auto t = Type(value);
    if (t == 7) return int32_t(kernel::R32(value + 8));
    if (t == 6) {
        const float v = kernel::G<float>(value + 8);
        if (std::isfinite(v) && v >= -10000 && v <= 10000) return int(v);
    }
    return fallback;
}
uint32_t Color(uint32_t value, uint32_t fallback = 0x9EF5F7) {
    if (Type(value) == 7) return kernel::R32(value + 8) & 0xFFFFFF;
    if (Type(value) == 6) {
        const auto color = kernel::G<float>(value + 8);
        if (std::isfinite(color) && color >= 0 && color <= 0xFFFFFF) return uint32_t(color);
    }
    return fallback;
}
std::string ReadString(uint32_t value) {
    if (Type(value) != 1) return {};
    const uint32_t token = kernel::R32(value + 8);
    if (!Address(token, 9)) return {};
    const uint32_t length = kernel::R16(token + 2);
    if (!length || length > 128 || !Address(token + 8, length + 1)) return {};
    return std::string(static_cast<const char*>(kernel::HostPtr(token + 8)), length);
}
uint32_t ArrayAt(uint32_t array, int index) {
    if (Type(array) != 0x16 || !Address(array, 0x28) || index < 0 || index >= int(kernel::R32(array + 0x24))) return 0;
    const uint32_t data = kernel::R32(array + 0x1C);
    return Address(data, uint32_t(index + 1) * 4) ? kernel::R32(data + uint32_t(index) * 4) : 0;
}
void Call(X86Context& ctx, const std::string& path, const std::string& function, const std::vector<std::string>& args = {}, uint32_t result = 0) {
    std::vector<uint32_t> words{Text(function), result, Text(path), uint32_t(args.size())};
    for (const auto& arg : args) words.push_back(Text(arg));
    kernel::CallGuest(ctx, 0x1208D0, words.data(), words.size());
}
void MenuSound(X86Context& ctx, size_t key) {
    // main.apt FE_SFX ids; Global_PlayFESound retains the native mixer and mute gates.
    constexpr std::array<uint32_t, 6> sounds{0, 0, 2, 2, 1, 5};
    if (key >= sounds.size()) return;
    const X86Context saved = ctx;
    kernel::CallGuest(ctx, 0xE39C0, {sounds[key]});
    ctx = saved;
}
bool Label(X86Context& ctx, uint32_t clip) {
    uint32_t field = Get(ctx, clip, "theText");
    if (!field) return false;
    if (ReadString(Get(ctx, field, "text")) == "PC SETTINGS") return true;
    return Set(ctx, field, "text", String(ctx, "PC SETTINGS"));
}

Entry MakeEntry(X86Context& ctx, uint32_t root, uint32_t menu, uint32_t select, int index, const std::string& path, bool development) {
    Entry entry{root, menu, select, index, path};
    entry.development = development;
    for (size_t i = 0; i < kKeys.size(); ++i) entry.keys[i] = Own(ctx, root, kKeys[i]);
    return entry;
}

// Temporary probe: join the pause field to the glyph blob actually submitted for drawing.
struct PauseGlyphProbe {
    uint32_t root = 0, row = 0, left = 0, right = 0, toggle = 0, field = 0, data = 0;
    unsigned reports = 0;
    std::string state;
    std::array<unsigned, 2> arrow_reports{};
    std::array<std::string, 2> arrow_state{};
    unsigned final_reports = 0;
    std::string final_state;
};
std::array<PauseGlyphProbe, 8> g_pause_glyph_probes{};
size_t g_pause_glyph_next = 0;


struct PauseDrawTrace { uint32_t root = 0; unsigned quads = 0, transforms = 0; };
thread_local PauseDrawTrace* g_pause_draw = nullptr;
bool TracePauseDraw(uint32_t blob, PauseDrawTrace& trace) {
    if (!Address(blob, 0x6C)) return false;
    for (auto& probe : g_pause_glyph_probes) {
        if (probe.final_reports >= 4 || Type(probe.field) != 0x0F || !Clip(probe.field) ||
            !Address(probe.data, 0x78) || kernel::R32(probe.field + 0x48) != probe.data ||
            kernel::R32(probe.data + 0x20) != blob || !Clip(probe.root)) continue;
        char label[33]{};
        std::memcpy(label, kernel::HostPtr(blob + 0x4C), 32);
        if (std::strcmp(label, "ON") && std::strcmp(label, "OFF")) continue;
        if (probe.final_reports && probe.final_state == label && !(probe.final_reports & 1)) continue;
        probe.final_state = label;
        ++probe.final_reports;
        trace.root = probe.root;
        LOGR("[DEBUG-pause-final] root=%08X text=%s sample=%u versus=%u players=%u font=%08X size=%.3f spacing=%.3f wrap=%u",
             trace.root, label, probe.final_reports, kernel::R32(0x4F42A0), kernel::R32(0x4F40C0),
             kernel::R32(blob + 0x28), kernel::G<float>(blob + 0x14), kernel::G<float>(blob + 0x40), kernel::R32(blob + 0x3C));
        return true;
    }
    return false;
}
void ReportPauseFont() {
    const auto text = kernel::R32(0x515AD0);
    if (!g_pause_draw || !Address(text, 0x48)) return;
    LOGR("[DEBUG-pause-final] root=%08X ctext=%08X position=%.3f,%.3f font=%08X color=%08X dirty=%u wrap=%u align=%u scale=%.6f flags=%08X screen=%.3f,%.3f",
         g_pause_draw->root, text, kernel::G<float>(text), kernel::G<float>(text + 4), kernel::R32(text + 8),
         kernel::R32(text + 0x18), kernel::R32(text + 0x1C), kernel::R32(text + 0x38), kernel::R32(text + 0x3C),
         kernel::G<float>(text + 0x40), kernel::R32(0x53BCDC), kernel::G<float>(0x53BD04), kernel::G<float>(0x53BD08));
    for (unsigned row = 0; row < 4; ++row) {
        const auto at = 0x5248A0u + row * 16;
        LOGR("[DEBUG-pause-final] root=%08X final_matrix_row=%u values=%.6f,%.6f,%.6f,%.6f", g_pause_draw->root,
             row, kernel::G<float>(at), kernel::G<float>(at + 4), kernel::G<float>(at + 8), kernel::G<float>(at + 12));
    }
}

void ReportPauseGlyph(uint32_t blob) {
    for (auto& probe : g_pause_glyph_probes) {
        if (!probe.field || probe.reports >= 4 || Type(probe.field) != 0x0F || !Address(probe.field, 0x60) ||
            !Address(probe.data, 0x78) || kernel::R32(probe.field + 0x48) != probe.data ||
            kernel::R32(probe.data + 0x20) != blob || !Address(blob, 0x6C) ||
            !Clip(probe.toggle) || kernel::R32(probe.field + 0x44) != probe.toggle ||
            !Clip(probe.row) || kernel::R32(probe.toggle + 0x44) != probe.row ||
            !Clip(probe.root) || kernel::R32(probe.row + 0x44) != probe.root ||
            !Clip(probe.left) || !Clip(probe.right) ||
            kernel::R32(probe.left + 0x44) != probe.row || kernel::R32(probe.right + 0x44) != probe.row) continue;
        char text[33]{};
        std::memcpy(text, kernel::HostPtr(blob + 0x4C), 32);
        if (probe.reports && probe.state == text) continue;
        probe.state = text;
        ++probe.reports;
        LOGR("[DEBUG-pause-layout] draw root=%08X row=%08X field=%08X data=%08X blob=%08X text=%s align=%u bounds=%.2f,%.2f,%.2f,%.2f",
             probe.root, probe.row, probe.field, probe.data, blob, text, kernel::R32(blob + 0x20),
             kernel::G<float>(blob + 4), kernel::G<float>(blob + 8),
             kernel::G<float>(blob + 0x0C), kernel::G<float>(blob + 0x10));
        for (const auto matrix : {probe.row + 0x0Cu, probe.left + 0x0Cu, probe.right + 0x0Cu,
                                  probe.toggle + 0x0Cu, probe.field + 0x0Cu}) {
            LOGR("[DEBUG-pause-layout] matrix=%08X values=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                 matrix, kernel::G<float>(matrix), kernel::G<float>(matrix + 4),
                 kernel::G<float>(matrix + 8), kernel::G<float>(matrix + 0x0C),
                 kernel::G<float>(matrix + 0x10), kernel::G<float>(matrix + 0x14));
        }
        // Native 10A8B0 expands APT's 2x3 matrix into these six entries of a 4x4 matrix.
        LOGR("[DEBUG-pause-layout] composed=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
             kernel::G<float>(0x515C80), kernel::G<float>(0x515C84),
             kernel::G<float>(0x515C90), kernel::G<float>(0x515C94),
             kernel::G<float>(0x515CB0), kernel::G<float>(0x515CB4));
    }
}

// Observe the vector leaf's actual draw transform, without invoking APT or changing layout.
void ReportPauseArrow(uint32_t clip) {
    if (Type(clip) != 0x0C || !Clip(clip)) return;
    const auto shape = kernel::R32(clip + 0x48);
    if (!Address(shape, 0x10)) return;
    const auto descriptor = kernel::R32(shape + 0x0C);
    if (!Address(descriptor, 0x1C) || kernel::R32(descriptor) != 1) return;
    for (auto& probe : g_pause_glyph_probes) {
        if (!Clip(probe.field) || Type(probe.field) != 0x0F ||
            !Address(probe.data, 0x78) || kernel::R32(probe.field + 0x48) != probe.data ||
            !Clip(probe.toggle) || kernel::R32(probe.field + 0x44) != probe.toggle ||
            !Clip(probe.row) || kernel::R32(probe.toggle + 0x44) != probe.row ||
            !Clip(probe.root) || kernel::R32(probe.row + 0x44) != probe.root) continue;
        const auto token = kernel::R32(probe.data + 0x18);
        if (!Address(token, 8)) continue;
        const auto length = kernel::R16(token + 2);
        if (!length || length > 32 || !Address(token + 8, length + 1)) continue;
        const std::string state(static_cast<const char*>(kernel::HostPtr(token + 8)), length);
        for (unsigned side = 0; side < 2; ++side) {
            const auto arrow = side ? probe.right : probe.left;
            if (probe.arrow_reports[side] >= 2 || !Clip(arrow) || kernel::R32(arrow + 0x44) != probe.row) continue;
            auto parent = clip;
            bool matches = false;
            for (unsigned depth = 0; depth < 4 && Clip(parent); ++depth) {
                if (parent == arrow) { matches = true; break; }
                parent = kernel::R32(parent + 0x44);
            }
            if (!matches || (probe.arrow_reports[side] && probe.arrow_state[side] == state)) continue;
            probe.arrow_state[side] = state;
            ++probe.arrow_reports[side];
            LOGR("[DEBUG-pause-layout] arrow root=%08X side=%u clip=%08X descriptor=%08X source=%s bounds=%.3f,%.3f,%.3f,%.3f",
                 probe.root, side, clip, descriptor, state.c_str(), kernel::G<float>(descriptor + 8),
                 kernel::G<float>(descriptor + 0x0C), kernel::G<float>(descriptor + 0x10),
                 kernel::G<float>(descriptor + 0x14));
            LOGR("[DEBUG-pause-layout] arrow side=%u composed=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f screen_scale=%.3f,%.3f",
                 side, kernel::G<float>(0x515C80), kernel::G<float>(0x515C84),
                 kernel::G<float>(0x515C90), kernel::G<float>(0x515C94),
                 kernel::G<float>(0x515CB0), kernel::G<float>(0x515CB4),
                 kernel::G<float>(0x53BD04), kernel::G<float>(0x53BD08));
        }
    }
}

void AlignPauseVibration(X86Context& ctx, uint32_t root, const char* name = "m_option1") {
    const auto row = Get(ctx, root, name);
    if (!Clip(row) || kernel::R32(row + 0x44) != root) return;
    const auto left = Get(ctx, row, "m_left"), right = Get(ctx, row, "m_right");
    const auto toggle = Get(ctx, row, "m_toggleoption");
    if (!Clip(left) || !Clip(right) || !Clip(toggle) ||
        kernel::R32(left + 0x44) != row || kernel::R32(right + 0x44) != row ||
        kernel::R32(toggle + 0x44) != row) return;
    const auto field = Get(ctx, toggle, "theText");
    if (Type(field) != 0x0F || !Address(field, 0x60) || kernel::R32(field + 0x44) != toggle) return;
    const auto data = kernel::R32(field + 0x48);
    if (!Address(data, 0x78)) return;
    const auto probe = std::find_if(g_pause_glyph_probes.begin(), g_pause_glyph_probes.end(),
        [=](const PauseGlyphProbe& at) { return at.field == field && at.root == root && at.data == data; });
    if (probe == g_pause_glyph_probes.end())
        g_pause_glyph_probes[g_pause_glyph_next++ % g_pause_glyph_probes.size()] = {root, row, left, right, toggle, field, data};
    // The native width getter synchronizes the same measured bounds used by the text renderer.
    if (!Get(ctx, field, "_width") || !(kernel::R32(data + 0x6C) & 1)) return;
    const float low = kernel::G<float>(data + 0x50), high = kernel::G<float>(data + 0x58);
    const float left_x = kernel::G<float>(left + 0x1C), right_x = kernel::G<float>(right + 0x1C);
    const float parent_x = kernel::G<float>(toggle + 0x1C), parent_scale = kernel::G<float>(toggle + 0x0C);
    const float scale = kernel::G<float>(field + 0x0C), before = kernel::G<float>(field + 0x1C);
    for (const float value : {low, high, left_x, right_x, parent_x, parent_scale, scale, before})
        if (!std::isfinite(value)) return;
    if (high <= low || right_x <= left_x || std::abs(parent_scale) < 0.0001f || std::abs(scale) < 0.0001f) return;
    // These authored rows are horizontal; retain any other layout rather than assuming unrotated axes.
    for (const auto clip : {toggle, field})
        for (unsigned offset : {0x10u, 0x14u})
            if (!std::isfinite(kernel::G<float>(clip + offset)) ||
                std::abs(kernel::G<float>(clip + offset)) > 0.0001f) return;
    const float x = ((left_x + right_x) * 0.5f - parent_x) / parent_scale - scale * (low + high) * 0.5f;
    if (!std::isfinite(x) || std::abs(x - before) < 0.01f) return;
    kernel::G<float>(field + 0x1C) = x;
    kernel::W32(field + 0x58, kernel::R32(field + 0x58) | 0x10000u);
    LOGR("pc-menu: pause toggle %s centered text=%s bounds=%.2f..%.2f arrows=%.2f..%.2f x=%.2f->%.2f autoSize=%u",
         name, ReadString(Get(ctx, field, "text")).c_str(), low, high, left_x, right_x, before, x, kernel::R32(data + 0x38));
}


struct SurvivalResultPage {
    Entry entry;
    bool main_menu = false, records_tab = false, ready = false;
    int painted_selection = -1, painted_tab = -1;
};
std::optional<SurvivalResultPage> g_result;
bool ResultPhase(game::pc_survival::Phase phase) {
    return phase == game::pc_survival::Phase::Won || phase == game::pc_survival::Phase::Lost;
}
uint32_t ResultField(X86Context& ctx, uint32_t clip) {
    auto field = Get(ctx, clip, "theText");
    if (Type(field) == 0x0F && Clip(field) && kernel::R32(field + 0x44) == clip) return field;
    const auto label = Get(ctx, clip, "m_textoption");
    if (!Clip(label) || kernel::R32(label + 0x44) != clip) return 0;
    field = Get(ctx, label, "theText");
    return Type(field) == 0x0F && Clip(field) && kernel::R32(field + 0x44) == label ? field : 0;
}
bool PutText(X86Context& ctx, uint32_t field, const std::string& text);

void ResultHeading(X86Context& ctx, uint32_t clip, unsigned depth = 0) {
    if (!Clip(clip) || depth > 3) return;
    if (Type(clip) == 0x0F) {
        // PAUSED is an unnamed, localized EditText in both authored pause layouts.
        // Its original descriptor survives localization (1556C7, Apt Data:7 +34).
        const auto data = kernel::R32(clip + 0x48);
        const auto descriptor = Address(data, 0x10) ? kernel::R32(data + 0x0C) : 0;
        const auto initial = Address(descriptor, 0x3C) && kernel::R32(descriptor) == 2 &&
            kernel::R32(descriptor + 4) == 0x09876543 ? kernel::R32(descriptor + 0x34) : 0;
        const auto label = ReadString(Get(ctx, clip, "text"));
        if ((Address(initial, sizeof("~kT_PAUSED")) && !std::memcmp(kernel::HostPtr(initial), "~kT_PAUSED", sizeof("~kT_PAUSED"))) ||
            label == "~kT_PAUSED" || label == "PAUSED")
            Set(ctx, clip, "_visible", Integer(ctx, 0));
        return;
    }
    if (Type(clip) != 0x0D && Type(clip) != 0x12) return;
    const auto data = kernel::R32(clip + 0x48);
    const auto list = Address(data, 0x28) ? kernel::R32(data + 0x24) : 0;
    auto child = Address(list) ? kernel::R32(list) : 0;
    for (unsigned budget = 0; Address(child, 0x60) && budget < 128; ++budget) {
        const auto next = kernel::R32(child + 0x50);
        if (kernel::R32(child + 0x44) == clip) ResultHeading(ctx, child, depth + 1);
        if (next == child) break;
        child = next;
    }
}
struct ResultLine {
    std::string text;
    float x = 0, y = 0, scale = 0.65f, width = 294;
    int color = 0x9EF5F7;
};
std::array<ResultLine, 24> ResultLines(const game::pc_survival::View& view, const SurvivalResultPage& page) {
    constexpr int gold = 0xD5AE32, white = 0xFFFFFF, muted = 0x69B9BE;
    std::array<ResultLine, 24> lines{};
    lines[0] = {view.phase == game::pc_survival::Phase::Won ? "SURVIVAL COMPLETE" : "SURVIVAL DEFEAT", 0, -43, 0.95f, 294, white};
    const std::string arena = view.venue < game::pc_survival::Arenas.size() ? game::pc_survival::Arenas[view.venue].label : "SURVIVAL";
    lines[1] = {arena + "  /  " + (view.ai_partner ? "SOLO + AI" : view.cooperative ? "COOP" : "SOLO") +
        "  /  " + game::pc_survival::DifficultyFor(view.difficulty).label +
        (view.endless ? "  /  ENDLESS" : "  /  CLASSIC") +
        (view.start_wave > 1 ? "  /  TEST WAVE " + std::to_string(view.start_wave) : ""), 0, 7, 0.46f, 294, muted};
    lines[2] = {page.records_tab ? "MATCH" : "> MATCH", 0, -13, 0.66f, 136, page.records_tab ? muted : gold};
    lines[3] = {page.records_tab ? "> RECORDS" : "RECORDS", 148, -13, 0.66f, 146, page.records_tab ? gold : muted};
    if (!page.records_tab) {
        lines[4] = {"WAVES CLEARED", 0, 25, 0.45f, 94, muted};
        lines[5] = {std::to_string(view.waves_cleared) + (view.endless ? "" : " / " + std::to_string(view.waves_required)), 0, 42, 1.0f, 94, white};
        lines[6] = {"ENEMIES DEFEATED", 100, 25, 0.45f, 99, muted};
        lines[7] = {std::to_string(view.total_defeated), 100, 42, 1.0f, 99, white};
        lines[8] = {"TIME", 205, 25, 0.45f, 89, muted};
        lines[9] = {game::pc_survival::SurvivalTime(view.elapsed_seconds), 205, 42, 1.0f, 89, white};
        lines[10] = {"PLAYER", 0, 80, 0.48f, 150, muted};
        lines[11] = {"KILLS", 164, 80, 0.48f, 65, muted};
        lines[12] = {"DEATHS", 235, 80, 0.48f, 59, muted};
        lines[13] = {"PLAYER 1", 0, 100, 0.68f, 150};
        lines[14] = {std::to_string(view.kills), 164, 100, 0.68f, 65, white};
        lines[15] = {std::to_string(view.deaths), 235, 100, 0.68f, 59, white};
        if (view.cooperative) {
            lines[16] = {view.ai_partner ? "AI PARTNER" : "PLAYER 2", 0, 122, 0.68f, 150};
            lines[17] = {std::to_string(view.partner_kills), 164, 122, 0.68f, 65, white};
            lines[18] = {std::to_string(view.partner_deaths), 235, 122, 0.68f, 59, white};
        }
        if (view.shared_kills) lines[19] = {"OTHER KILLS: " + std::to_string(view.shared_kills), 0, 145, 0.48f, 294, muted};
        if (view.records.new_best) lines[20] = {"NEW LOCAL BEST - SEE RECORDS", 0, 162, 0.48f, 294, gold};
    } else {
        using game::pc_survival::RecordStatus;
        const auto& record = view.records;
        if (record.status == RecordStatus::Saved) {
            const auto& stats = record.stats;
            lines[4] = {"BEST WAVES", 0, 25, 0.45f, 94, muted};
            lines[5] = {std::to_string(stats.best_waves), 0, 42, 1.0f, 94, white};
            lines[6] = {"BEST KILLS", 100, 25, 0.45f, 99, muted};
            lines[7] = {std::to_string(stats.best_enemies), 100, 42, 1.0f, 99, white};
            lines[8] = {view.endless ? "FORMAT" : "FASTEST WIN", 205, 25, 0.45f, 89, muted};
            lines[9] = {view.endless ? "ENDLESS" : stats.fastest_win_seconds ? game::pc_survival::SurvivalTime(stats.fastest_win_seconds) : "NO WIN YET", 205, 42, stats.fastest_win_seconds ? 1.0f : 0.56f, 89, white};
            lines[10] = {"RUNS", 0, 88, 0.52f, 90, muted};
            lines[11] = {std::to_string(stats.runs), 90, 84, 0.75f, 54, white};
            if (!view.endless) {
                lines[12] = {"WINS", 160, 88, 0.52f, 80, muted};
                lines[13] = {std::to_string(stats.wins), 240, 84, 0.75f, 54, white};
            }
            lines[14] = {"TOTAL KILLS", 0, 112, 0.52f, 90, muted};
            lines[15] = {std::to_string(stats.enemies), 90, 108, 0.75f, 54, white};
            lines[16] = {"TOTAL DEATHS", 160, 112, 0.52f, 80, muted};
            lines[17] = {std::to_string(stats.deaths), 240, 108, 0.75f, 54, white};
            lines[18] = {record.new_best ? "NEW LOCAL BEST" : "LOCAL RECORDS", 0, 145, 0.55f, 294, record.new_best ? gold : white};
            lines[19] = {"THIS ARENA, MODE AND DIFFICULTY", 0, 162, 0.45f, 294, muted};
        } else {
            lines[4] = {record.status == RecordStatus::TestRun ? "TEST RUN" : "RECORDS UNAVAILABLE", 0, 36, 0.80f, 294, white};
            lines[5] = {record.status == RecordStatus::TestRun ? "START AT WAVE 1 TO SET RECORDS" : "THIS RESULT WAS NOT SAVED", 0, 66, 0.55f, 294, muted};
            lines[6] = {"MATCH STATS ARE ON THE MATCH TAB", 0, 100, 0.50f, 294, muted};
        }
    }
    lines[21] = {std::string(page.main_menu ? "  " : "> ") + "PLAY AGAIN", 0, 180, 0.64f, 294, page.main_menu ? 0x9EF5F7 : gold};
    lines[22] = {std::string(page.main_menu ? "> " : "  ") + "MAIN MENU", 0, 204, 0.64f, 294, page.main_menu ? gold : 0x9EF5F7};
    lines[23] = {"LEFT / RIGHT: TABS    UP / DOWN: SELECT", 0, 226, 0.32f, 294, muted};
    return lines;
}
bool RenderSurvivalLines(X86Context& ctx, const Entry& entry, const char* source_name,
                         const char* prefix, unsigned depth, std::span<const ResultLine> lines,
                         std::array<float, 2> origin, float text_height = 0) {
    const auto root = entry.root, source = Get(ctx, root, source_name);
    if (!Clip(source) || kernel::R32(source + 0x44) != root) return false;
    bool ready = true;
    for (unsigned i = 0; i < lines.size(); ++i) {
        const auto name = std::string(prefix) + std::to_string(i);
        const auto row = Get(ctx, root, name.c_str());
        if (!Clip(row) || kernel::R32(row + 0x44) != root) {
            Call(ctx, entry.path + "." + source_name, "duplicateMovieClip", {name, std::to_string(depth + i)});
            ready = false;
            continue;
        }
        const auto field = ResultField(ctx, row), data = kernel::R32(row + 0x48);
        if (!field || !Address(data, 0x20)) { ready = false; continue; }
        kernel::W32(data + 0x1C, kernel::R32(data + 0x1C) & ~0x02000000u);
        const auto& line = lines[i];
        Set(ctx, row, "_visible", Integer(ctx, line.text.empty() ? 0 : 1));
        Set(ctx, row, "_alpha", Integer(ctx, 100));
        Set(ctx, field, "autoSize", String(ctx, "left"));
        ready &= PutText(ctx, field, line.text);
        float scale = line.scale;
        // Fit measured native glyphs inside each column, including large career counters.
        const float width = float(Number(Get(ctx, field, "_width"), 0)) * std::abs(kernel::G<float>(source + 0x0C));
        if (std::isfinite(width) && width > 0) scale = std::min(scale, line.width / width);
        // The MatchType header has taller authored bounds than the pause label.
        if (text_height > 0) {
            const float height = float(Number(Get(ctx, field, "_height"), 0)) * std::abs(kernel::G<float>(source + 0x18));
            if (std::isfinite(height) && height > 0) scale = std::min(scale, line.scale * text_height / height);
        }
        for (unsigned m = 0; m < 4; ++m)
            kernel::G<float>(row + 0x0C + m * 4) = kernel::G<float>(source + 0x0C + m * 4) * scale;
        kernel::G<float>(row + 0x1C) = origin[0] + line.x;
        kernel::G<float>(row + 0x20) = origin[1] + line.y;
        kernel::W32(row + 0x58, kernel::R32(row + 0x58) | 0x10000u);
        Set(ctx, field, "textColor", Integer(ctx, line.color));
    }
    return ready;
}
bool RenderSurvivalResult(X86Context& ctx, uint32_t root, const std::string& path) {
    const auto view = game::pc_survival::ReadView();
    if (!ResultPhase(view.phase) || kernel::R32(0x4C3444) != 5) return false;
    if (!g_result || g_result->entry.root != root) {
        const auto menu = Get(ctx, root, "g_oMenu");
        g_result = SurvivalResultPage{MakeEntry(ctx, root, menu, Own(ctx, root, "keySelect"), 0, path, false)};
        LOGR("survival-results: native result page at %s waves=%u/%u kills=%u/%u tabKeys=%08X,%08X", path.c_str(),
             view.waves_cleared, view.waves_required, view.total_defeated, view.enemies_required,
             g_result->entry.keys[2], g_result->entry.keys[3]);
    }
    auto& page = *g_result;
    ResultHeading(ctx, root);
    if (page.ready && page.painted_selection == int(page.main_menu) && page.painted_tab == int(page.records_tab)) return true;
    const auto source = Get(ctx, root, "m_option0");
    if (!Clip(source) || kernel::R32(source + 0x44) != root) return true;
    const float x = kernel::G<float>(source + 0x1C), y = kernel::G<float>(source + 0x20);
    if (!std::isfinite(x) || !std::isfinite(y)) return true;
    const auto lines = ResultLines(view, page);
    const bool ready = RenderSurvivalLines(ctx, page.entry, "m_option0", "m_survival_result", 30100, lines, {x, y});
    // Only this terminal pause instance is changed. Native callbacks and grids stay untouched.
    for (const auto* name : {"m_option0", "m_option1", "m_option2", "m_pc_settings", "m_quit_popup", "objectiveText"}) {
        const auto row = Get(ctx, root, name);
        if (Clip(row) && kernel::R32(row + 0x44) == root) Set(ctx, row, "_visible", Integer(ctx, 0));
    }
    if (ready && (!page.ready || page.painted_tab != int(page.records_tab)))
        LOGR("survival-results: %s tab ready; left/right tabs, up/down select, A confirm, B main menu", page.records_tab ? "records" : "match");
    page.ready = ready;
    if (ready) { page.painted_selection = int(page.main_menu); page.painted_tab = int(page.records_tab); }
    return true;
}
bool SurvivalResultAction(uint32_t fn) {
    if (!g_result || !Clip(g_result->entry.root) || kernel::R32(0x4C3444) != 5 ||
        !ResultPhase(game::pc_survival::ReadView().phase)) return false;
    const auto& keys = g_result->entry.keys;
    const auto found = std::find(keys.begin(), keys.end(), fn);
    if (!fn || found == keys.end()) return false;
    const auto key = size_t(found - keys.begin());
    if (key == 0 || key == 1) g_result->main_menu = !g_result->main_menu;
    if ((key == 2 || key == 3) && g_result->records_tab != (key == 3)) {
        g_result->records_tab = key == 3;
        g_result->ready = false;
    }
    if (key == 5 || (key == 4 && g_result->ready && g_result->main_menu)) game::pc_survival::RequestExit();
    else if (key == 4 && g_result->ready) game::pc_survival::RequestReplay();
    return true;
}

void Inspect(X86Context& ctx, uint32_t root, const std::string& path, std::vector<Entry>& entries) {
    // Classification must use OWN members, never the VM's global-scope fallback.
    const bool options = Own(ctx, root, "kCredits") && Own(ctx, root, "kSettings") &&
        Own(ctx, root, "g_aMenuClips");
    // OptionsSettings also owns g_aCBFunc/g_nNumOptions and m_option0..3.
    // Only the actual pause page owns the resume selection and quit popup.
    // Classify before cloning anything: cloning a toggle also copies its value
    // fields and corrupts the stock Settings layout even if appending then fails.
    const bool pause = Own(ctx, root, "kSELECTION_RESUMEGAME") && Clip(Get(ctx, root, "m_quit_popup")) &&
        Own(ctx, root, "g_aCBFunc") && Own(ctx, root, "g_nNumOptions");
    if (!options && !pause) return;
    if (pause && RenderSurvivalResult(ctx, root, path)) return;
    if (pause) AlignPauseVibration(ctx, root);
    static std::set<std::string> reported;
    if (reported.size() < 64 && reported.emplace(path).second)
        LOGR("pc-menu: recognized %s page at %s", options ? "Options" : "Pause", path.c_str());
    const uint32_t menu = Get(ctx, root, "g_oMenu"), grid = Get(ctx, menu, "m_aGrid");
    const uint32_t select = Get(ctx, root, "keySelect");
    const int columns = Number(Get(ctx, menu, "n"));
    if (!menu || !select || Type(grid) != 0x16 || columns != 1) return;
    const int count = int(kernel::R32(grid + 0x24));
    if (count < 2 || count > 12) return;
    uint32_t button = Get(ctx, root, kButton);
    for (int index = 0; index < count; ++index)
        if (ReadString(ArrayAt(grid, index)) == kButton && Clip(button)) {
            if (!g_page || g_page->entry.root != root) Label(ctx, button);
            entries.push_back(MakeEntry(ctx, root, menu, select, index, path, options));
            return;
        }
    // Pause destroys this APT screen on resume; keep its native button grid untouched.
    if (pause) return;
    const uint32_t old_size = Get(ctx, menu, "m_nSize"), old_rows = Get(ctx, menu, "m");
    if (Number(old_size) != count || Number(old_rows) != count) return;
    std::vector<uint32_t> clips;
    std::vector<float> ys;
    std::string last_name;
    for (int i = 0; i < count; ++i) {
        const auto name = ReadString(ArrayAt(grid, i));
        if (name.empty()) return;
        const auto clip = Get(ctx, root, name.c_str());
        if (!Clip(clip)) return;
        const float y = kernel::G<float>(clip + 0x20);
        if (!std::isfinite(y)) return;
        clips.push_back(clip); ys.push_back(y); last_name = name;
    }
    if (!Clip(button)) {
        // The new clip's initialization actions run through the normal APT queue.
        Call(ctx, path + "." + last_name, "duplicateMovieClip", {kButton, "30000"});
        return;
    }
    if (!Get(ctx, button, "Select") || !Label(ctx, button)) return;
    const uint32_t name_value = String(ctx, kButton);
    const uint32_t size_value = Integer(ctx, count + 1);
    if (!name_value || !size_value) return;
    // First prepare navigation. Publish the extra array element last, so scripts
    // cannot encounter it until its button and selection handler are ready.
    // Retain the old values across Set, which releases the previous member.
    ctx.ecx = old_size; kernel::CallGuest(ctx, kernel::R32(kernel::R32(old_size)), {});
    ctx.ecx = old_rows; kernel::CallGuest(ctx, kernel::R32(kernel::R32(old_rows)), {});
    const bool ready = Set(ctx, menu, "m_nSize", size_value) && Set(ctx, menu, "m", size_value);
    if (ready) {
        ctx.ecx = grid;
        kernel::CallGuest(ctx, 0x151C30, {uint32_t(count), name_value});
    }
    const bool appended = ready && ReadString(ArrayAt(grid, count)) == kButton;
    if (!appended) { Set(ctx, menu, "m_nSize", old_size); Set(ctx, menu, "m", old_rows); }
    ctx.ecx = old_size; kernel::CallGuest(ctx, kernel::R32(kernel::R32(old_size) + 4), {});
    ctx.ecx = old_rows; kernel::CallGuest(ctx, kernel::R32(kernel::R32(old_rows) + 4), {});
    if (!appended) return;
    // Fit the extra row into the original vertical span instead of going offscreen.
    const float span = ys.back() - ys.front();
    if (std::abs(span) > 1) {
        for (int i = 0; i < count; ++i)
            kernel::G<float>(clips[size_t(i)] + 0x20) = ys.front() + span * float(i) / float(count);
        kernel::G<float>(button + 0x20) = ys.back();
    }
    kernel::G<float>(button + 0x1C) = kernel::G<float>(clips.back() + 0x1C);
    entries.push_back(MakeEntry(ctx, root, menu, select, count, path, options));
    LOGR("pc-menu: PC Settings added to %s at %s, index %d", options ? "Options" : "Pause", path.c_str(), count);
}

bool PutText(X86Context& ctx, uint32_t field, const std::string& text) {
    if (ReadString(Get(ctx, field, "text")) == text) return true;
    return Set(ctx, field, "text", String(ctx, text));
}

void ApplyMusicVolume(X86Context& ctx) {
    const auto music = kernel::R32(0x5148C4);
    if (!kernel::R32(0x47F364) || !Address(music, 0x2F8)) return;
    const X86Context saved = ctx;
    ctx.ecx = music;
    kernel::CallGuest(ctx, 0xF7AD0, {g_requested_music_volume});
    ctx = saved;
}

void ToggleMusic(X86Context& ctx) {
    const X86Context saved = ctx;
    bool enabled;
    {
        std::lock_guard lock(app::UiLock());
        enabled = app::Get().music_enabled = !app::Get().music_enabled;
    }
    ApplyMusicVolume(ctx);
    if (enabled && g_suppressed_music_event) {
        g_suppressed_music_event = false;
        if (g_last_music_event && kernel::R32(0x47F364) &&
            kernel::R32(0x5148C4) == g_last_music_controller && Address(g_last_music_controller, 0x2F4)) {
            ctx.ecx = g_last_music_controller;
            kernel::CallGuest(ctx, 0xF7A80, {g_last_music_event, g_last_music_flags});
        }
    }
    ctx = saved;
    if (!app::Save()) LOGE("pc-menu: music preference could not be saved");
    LOGR("pc-menu: music %s", enabled ? "ON" : "OFF");
}

// Survival keeps the original APT screens and their character/unlock logic.
enum class SurvivalScreen { Main, Match, Characters, Venue };
struct SurvivalPage {
    SurvivalScreen screen;
    uint32_t root, menu;
    std::string path;
    std::array<uint32_t, kKeys.size()> keys{};
};
std::vector<SurvivalPage> g_survival_pages;
struct SurvivalFlow {
    bool active = false, cooperative = false, ai_partner = false, mode_selected = false, selected = false;
    std::array<unsigned, 2> characters{}, costumes{}, lives{3, 3}, controllers{0, 1};
    unsigned venue = 2;
    unsigned difficulty = 0;
    bool endless = false;
    uint32_t solo_completed_root = 0;
    std::array<uint32_t, 12> cache{};
    std::array<uint32_t, 2> bindings{};
};
SurvivalFlow g_survival;
struct SurvivalSetup {
    Entry entry;
    unsigned choice = 0, row = 0, difficulty = 0;
    bool endless = false, ready = false;
};
std::optional<SurvivalSetup> g_survival_setup;
constexpr const char* kRecordsButton = "m_pc_survival_records";
constexpr std::array<const char*, 3> kRecordModeLabels{"SOLO", "SOLO + AI", "COOP"};
static_assert(kRecordModeLabels.size() == game::pc_survival::RecordBook{}[0].size());
struct SurvivalRecordsBrowser {
    Entry entry;
    std::optional<game::pc_survival::RecordBook> book;
    unsigned venue = 0, mode = 0, difficulty = 0;
    bool endless = false;
    int painted_venue = -1, painted_mode = -1, painted_difficulty = -1;
    int painted_endless = -1;
    bool ready = false;
};
std::optional<SurvivalRecordsBrowser> g_records_browser;
bool g_survival_main_select = false;
bool g_survival_load_committed = false;
std::optional<std::pair<uint32_t, std::string>> g_solo_next;
void EndSurvivalFrontend() {
    g_survival_setup.reset();
    g_records_browser.reset();
    g_survival_load_committed = false;
    if (!g_survival.active) return;
    for (unsigned i = 0; i < g_survival.cache.size(); ++i)
        kernel::W32(0x515CFC + i * 4, g_survival.cache[i]);
    for (unsigned i = 0; i < g_survival.bindings.size(); ++i)
        kernel::W32(0x515A00 + i * 4, g_survival.bindings[i]);
    g_survival.active = false;
}
void BeginSurvivalFrontend() {
    EndSurvivalFrontend();
    g_survival = {};
    for (unsigned i = 0; i < g_survival.cache.size(); ++i)
        g_survival.cache[i] = kernel::R32(0x515CFC + i * 4);
    for (unsigned i = 0; i < g_survival.bindings.size(); ++i)
        g_survival.bindings[i] = kernel::R32(0x515A00 + i * 4);
    g_survival.active = true;
    LOGR("survival-menu: native frontend entered");
}
int SelectedIndex(X86Context& ctx, const SurvivalPage& page, const char* menu_name) {
    static uint32_t result = 0;
    if (!result) result = mem::AllocSystem(64);
    std::memset(kernel::HostPtr(result), 0, 64);
    Call(ctx, page.path + "." + menu_name, "getSelectIndex", {"0"}, result);
    const char* text = static_cast<const char*>(kernel::HostPtr(result));
    char* end = nullptr;
    const long index = std::strtol(text, &end, 10);
    return end != text && *end == '\0' && index >= 0 && index < 32 ? int(index) : -1;
}
void SyncSurvivalButtonVisibility(X86Context& ctx, uint32_t root, uint32_t grid, uint32_t button) {
    const auto reference = Get(ctx, root, ReadString(ArrayAt(grid, 0)).c_str());
    if (!Clip(reference) || reference == button) return;
    // A clone can inherit an intro fade; follow a native button rather than retaining that snapshot.
    for (const auto* property : {"_alpha", "_visible"}) {
        const auto value = Get(ctx, reference, property);
        if (Type(value)) Set(ctx, button, property, value);
    }
}
bool PutMainButtonText(X86Context& ctx, uint32_t button, const char* label) {
    const auto field = Get(ctx, button, "theText");
    if (ReadString(Get(ctx, field, "text")) == label) return true;
    if (!PutText(ctx, field, label)) return false;
    // Use clashMainMenu's authored cyan for clone labels; native Select still owns the gold highlight.
    return Set(ctx, field, "textColor", Integer(ctx, 0x92EBFF));
}
bool AddSurvivalButton(X86Context& ctx, uint32_t root, const std::string& path) {
    const auto menu = Own(ctx, root, "g_oMenu");
    const auto grid = Get(ctx, menu, "m_aGrid"), clips = Own(ctx, root, "g_aMenuClips");
    constexpr const char* button_name = "m_pc_survival";
    if (Type(grid) != 0x16 || Type(clips) != 0x16 || Number(Get(ctx, menu, "n")) != 1) return false;
    const int count = int(kernel::R32(grid + 0x24));
    const auto button = Get(ctx, root, button_name);
    if (ReadString(ArrayAt(grid, 2)) == button_name && Clip(button)) {
        SyncSurvivalButtonVisibility(ctx, root, grid, button);
        return PutMainButtonText(ctx, button, "SURVIVAL");
    }
    if (count < 4 || count > 6 || int(kernel::R32(clips + 0x24)) != count ||
        Number(Get(ctx, menu, "m_nSize")) != count || Number(Get(ctx, menu, "m")) != count) return false;
    std::vector<std::string> names;
    std::vector<uint32_t> buttons;
    std::vector<float> ys;
    for (int i = 0; i < count; ++i) {
        const auto name = ReadString(ArrayAt(grid, i));
        if (name.empty() || ReadString(ArrayAt(clips, i)) != name) return false;
        const auto clip = Get(ctx, root, name.c_str());
        if (!Clip(clip) || !std::isfinite(kernel::G<float>(clip + 0x20))) return false;
        names.push_back(name); buttons.push_back(clip); ys.push_back(kernel::G<float>(clip + 0x20));
    }
    if (!Clip(button)) {
        Call(ctx, path + "." + names.back(), "duplicateMovieClip", {button_name, "30001"});
        return false;
    }
    if (!Get(ctx, button, "Select") || !PutMainButtonText(ctx, button, "SURVIVAL")) return false;
    constexpr std::array<const char*, 4> shifted{"kOnline_Selection", "kRewards_Selection", "kOptions_Selection", "kQuit_Selection"};
    std::array<int, 4> indices{};
    for (size_t i = 0; i < shifted.size(); ++i) {
        indices[i] = Number(Own(ctx, root, shifted[i]));
        if (indices[i] < 2 || indices[i] > count) return false;
    }
    // Publish both native arrays in the same order; no original button is replaced.
    names.insert(names.begin() + 2, button_name);
    const std::vector<uint32_t> arrays = grid == clips ? std::vector<uint32_t>{grid} : std::vector<uint32_t>{grid, clips};
    for (const auto array : arrays) {
        for (int i = count; i >= 2; --i) {
            const auto value = String(ctx, names[size_t(i)]);
            ctx.ecx = array;
            kernel::CallGuest(ctx, 0x151C30, {uint32_t(i), value});
        }
    }
    Set(ctx, menu, "m_nSize", Integer(ctx, count + 1));
    Set(ctx, menu, "m", Integer(ctx, count + 1));
    for (size_t i = 0; i < shifted.size(); ++i) Set(ctx, root, shifted[i], Integer(ctx, indices[i] + 1));
    buttons.insert(buttons.begin() + 2, button);
    const float span = ys.back() - ys.front();
    for (int i = 0; i <= count; ++i)
        kernel::G<float>(buttons[size_t(i)] + 0x20) = ys.front() + span * float(i) / float(count);
    kernel::G<float>(button + 0x1C) = kernel::G<float>(buttons[1] + 0x1C);
    SyncSurvivalButtonVisibility(ctx, root, grid, button);
    LOGR("survival-menu: SURVIVAL inserted below Story at %s", path.c_str());
    return true;
}

bool AddDesktopButtons(X86Context& ctx, uint32_t root, const std::string& path) {
    constexpr std::array<const char*, 1> names{"m_pc_quit"};
    constexpr std::array<const char*, 1> labels{"QUIT GAME"};
    const auto menu = Own(ctx, root, "g_oMenu");
    const auto grid = Get(ctx, menu, "m_aGrid"), clips = Own(ctx, root, "g_aMenuClips");
    if (Type(grid) != 0x16 || Type(clips) != 0x16 || Number(Get(ctx, menu, "n")) != 1) return false;
    const int count = int(kernel::R32(grid + 0x24));
    if (count < 5 || count > 8 || int(kernel::R32(clips + 0x24)) != count ||
        Number(Get(ctx, menu, "m_nSize")) != count || Number(Get(ctx, menu, "m")) != count) return false;
    std::array<uint32_t, 1> extra{};
    for (size_t i = 0; i < names.size(); ++i) extra[i] = Get(ctx, root, names[i]);
    if (ReadString(ArrayAt(grid, count - 1)) == names[0]) {
        for (size_t i = 0; i < extra.size(); ++i) {
            if (!Clip(extra[i]) || ReadString(ArrayAt(clips, count - 1 + int(i))) != names[i]) return false;
            SyncSurvivalButtonVisibility(ctx, root, grid, extra[i]);
            if (!PutMainButtonText(ctx, extra[i], labels[i])) return false;
        }
        return true;
    }
    if (count > 6) return false;
    std::vector<uint32_t> buttons;
    for (int i = 0; i < count; ++i) {
        const auto name = ReadString(ArrayAt(grid, i));
        const auto button = Get(ctx, root, name.c_str());
        if (name.empty() || ReadString(ArrayAt(clips, i)) != name || !Clip(button) ||
            !std::isfinite(kernel::G<float>(button + 0x20)) ||
            !std::isfinite(kernel::G<float>(button + 0x1C))) return false;
        buttons.push_back(button);
    }
    for (size_t i = 0; i < extra.size(); ++i) {
        if (!Clip(extra[i])) {
            Call(ctx, path + "." + ReadString(ArrayAt(grid, count - 1)), "duplicateMovieClip",
                 {names[i], std::to_string(30002 + i)});
            return false;
        }
        if (!Get(ctx, extra[i], "Select") || !PutMainButtonText(ctx, extra[i], labels[i])) return false;
    }
    const float first = kernel::G<float>(buttons.front() + 0x20), last = kernel::G<float>(buttons.back() + 0x20);
    if (last <= first) return false;
    const float first_x = kernel::G<float>(buttons.front() + 0x1C), last_x = kernel::G<float>(buttons.back() + 0x1C);
    // Survival already occupies one added row. Restore the authored pitch, extending upward from the last row.
    const float step = (last - first) / float(count - 2);
    const float slope = (last_x - first_x) / (last - first);
    const std::vector<uint32_t> arrays = grid == clips ? std::vector<uint32_t>{grid} : std::vector<uint32_t>{grid, clips};
    for (const auto array : arrays) for (size_t i = 0; i < names.size(); ++i) {
        const auto value = String(ctx, names[i]);
        ctx.ecx = array;
        kernel::CallGuest(ctx, 0x151C30, {uint32_t(count + i), value});
    }
    Set(ctx, menu, "m_nSize", Integer(ctx, count + 1));
    Set(ctx, menu, "m", Integer(ctx, count + 1));
    Set(ctx, root, "kQuit_Selection", Integer(ctx, count));
    buttons.insert(buttons.end(), extra.begin(), extra.end());
    for (size_t i = 0; i < buttons.size(); ++i) {
        const float y = last - step * float(buttons.size() - 1 - i);
        kernel::G<float>(buttons[i] + 0x20) = y;
        kernel::G<float>(buttons[i] + 0x1C) = last_x + slope * (y - last);
    }
    for (const auto button : extra) {
        SyncSurvivalButtonVisibility(ctx, root, grid, button);
    }
    LOGR("desktop-menu: QUIT GAME appended at %s; row pitch=%.2f, bottom=(%.2f, %.2f)",
         path.c_str(), step, last_x, last);
    return true;
}

void SurvivalVisible(X86Context& ctx, uint32_t root, const char* name, bool visible) {
    const auto object = Get(ctx, root, name);
    if (!Clip(object)) return;
    const bool hidden = Number(Own(ctx, object, "g_pcSurvivalHidden"), 0) == 1;
    if (!visible) {
        if (!hidden) {
            const auto previous = Get(ctx, object, "_visible");
            if (!Type(previous)) return;
            Set(ctx, object, "g_pcSurvivalVisible", previous);
            Set(ctx, object, "g_pcSurvivalHidden", Integer(ctx, 1));
        }
        Set(ctx, object, "_visible", Integer(ctx, 0));
    } else if (hidden) {
        Set(ctx, object, "_visible", Own(ctx, object, "g_pcSurvivalVisible"));
        Set(ctx, object, "g_pcSurvivalHidden", Integer(ctx, 0));
    }
}
bool ConfigureSurvivalMatch(X86Context& ctx, uint32_t root, const std::string& path) {
    const auto menu = Own(ctx, root, "g_oMenu2");
    const auto grid = Get(ctx, menu, "m_aGrid"), clips = Own(ctx, root, "g_aMenu2Clips");
    if (Type(grid) != 0x16 || Type(clips) != 0x16 || Number(Get(ctx, menu, "n")) != 1) return false;
    const auto count = kernel::R32(grid + 0x24);
    if ((count != 2 && count != 3) || kernel::R32(clips + 0x24) != count ||
        Number(Get(ctx, menu, "m_nSize")) != int(count) || Number(Get(ctx, menu, "m")) != int(count)) return false;
    for (const auto array : {grid, clips})
        if (ReadString(ArrayAt(array, 0)) != "m_1vsCOM_head" ||
            ReadString(ArrayAt(array, 1)) != "m_1vs2_head") return false;
    const auto first = Get(ctx, root, "m_1vsCOM_head"), last = Get(ctx, root, "m_1vs2_head");
    const auto button = Get(ctx, root, kRecordsButton);
    if (!Clip(first) || !Clip(last) || kernel::R32(first + 0x44) != root || kernel::R32(last + 0x44) != root) return false;
    const float x0 = kernel::G<float>(first + 0x1C), x1 = kernel::G<float>(last + 0x1C);
    const float y0 = kernel::G<float>(first + 0x20), y1 = kernel::G<float>(last + 0x20);
    if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1) || y1 <= y0) return false;
    if (count == 3) {
        if (Number(Own(ctx, root, "g_pcSurvivalRecordsMenu"), 0) != 1 || !Clip(button) ||
            kernel::R32(button + 0x44) != root || ReadString(ArrayAt(grid, 2)) != kRecordsButton ||
            ReadString(ArrayAt(clips, 2)) != kRecordsButton) return false;
        if (!g_survival.active) {
            // Array.pop releases the added value through the VM; Versus retains its original two rows.
            Call(ctx, path + ".g_oMenu2.m_aGrid", "pop");
            if (grid != clips) Call(ctx, path + ".g_aMenu2Clips", "pop");
            if (kernel::R32(grid + 0x24) != 2 || kernel::R32(clips + 0x24) != 2) return false;
            Set(ctx, menu, "m_nSize", Integer(ctx, 2));
            Set(ctx, menu, "m", Integer(ctx, 2));
            Set(ctx, root, "g_pcSurvivalRecordsMenu", Integer(ctx, 0));
            SurvivalVisible(ctx, root, kRecordsButton, false);
            Call(ctx, path + ".g_oMenu2", "SelectFirst", {"0", "0"});
            return true;
        }
    } else {
        if (!g_survival.active) {
            if (Clip(button) && kernel::R32(button + 0x44) == root) SurvivalVisible(ctx, root, kRecordsButton, false);
            return true;
        }
        if (!Clip(button)) {
            Call(ctx, path + ".m_1vsCOM_head", "duplicateMovieClip", {kRecordsButton, "30002"});
            return false;
        }
        if (kernel::R32(button + 0x44) != root || !Get(ctx, button, "Select") ||
            !PutText(ctx, Get(ctx, button, "theText"), "RECORDS")) return false;
        for (const auto array : grid == clips ? std::vector<uint32_t>{grid} : std::vector<uint32_t>{grid, clips}) {
            const auto added = String(ctx, kRecordsButton);
            ctx.ecx = array; kernel::CallGuest(ctx, 0x151C30, {2, added});
        }
        Set(ctx, menu, "m_nSize", Integer(ctx, 3));
        Set(ctx, menu, "m", Integer(ctx, 3));
        Set(ctx, root, "g_pcSurvivalRecordsMenu", Integer(ctx, 1));
        LOGR("survival-menu: RECORDS appended below COOP at %s", path.c_str());
    }
    // Follow the authored intro movement so the cloned row cannot remain offscreen or faded.
    if (!Set(ctx, button, "_x", String(ctx, std::to_string(x1 + (x1 - x0)))) ||
        !Set(ctx, button, "_y", String(ctx, std::to_string(y1 + (y1 - y0))))) return false;
    SyncSurvivalButtonVisibility(ctx, root, grid, button);
    return PutText(ctx, Get(ctx, button, "theText"), "RECORDS");
}

std::array<ResultLine, 23> RecordsBrowserLines(const SurvivalRecordsBrowser& browser) {
    constexpr int gold = 0xD5AE32, white = 0xFFFFFF, muted = 0x69B9BE;
    std::array<ResultLine, 23> lines{};
    lines[0] = {"SURVIVAL RECORDS", 0, 0, 0.95f, 374, white};
    lines[1] = {std::string(browser.endless ? "ENDLESS  /  " : "CLASSIC  /  ") + game::pc_survival::DifficultyFor(browser.difficulty).label, 0, 28, 0.42f, 374, muted};
    constexpr std::array<float, 3> mode_x{0, 104, 244};
    for (unsigned i = 0; i < kRecordModeLabels.size(); ++i)
        lines[2 + i] = {std::string(browser.mode == i ? "> " : "") + kRecordModeLabels[i], mode_x[i], 50, 0.58f,
                        i == 0 ? 94.0f : 130.0f, browser.mode == i ? gold : muted};
    lines[5] = {"ARENA", 0, 70, 0.42f, 150, muted};
    lines[6] = {game::pc_survival::Arenas[browser.venue].label, 164, 70, 0.58f, 210, white};
    for (unsigned i = 0; i < game::pc_survival::Arenas.size(); ++i) {
        const bool played = browser.book && (*browser.book)[i][browser.mode].runs;
        lines[7 + i] = {std::string(browser.venue == i ? "> " : "") + game::pc_survival::Arenas[i].label,
                        0, 88 + 20.0f * i, 0.54f, 150, browser.venue == i ? gold : played ? white : muted};
    }
    if (!browser.book) {
        lines[14] = {"RECORDS UNAVAILABLE", 164, 112, 0.68f, 210, white};
        lines[15] = {"EXISTING FILE WAS NOT CHANGED", 164, 142, 0.44f, 210, muted};
    } else {
        const auto& stats = (*browser.book)[browser.venue][browser.mode];
        if (!stats.runs) {
            lines[14] = {"NO RUNS YET", 164, 112, 0.72f, 210, white};
            lines[15] = {"PLAY THIS ARENA TO SET RECORDS", 164, 142, 0.44f, 210, muted};
        } else {
            lines[14] = {"BEST WAVES", 164, 100, 0.42f, 90, muted};
            lines[15] = {std::to_string(stats.best_waves), 164, 113, 0.90f, 90, white};
            lines[16] = {"BEST KILLS", 264, 100, 0.42f, 110, muted};
            lines[17] = {std::to_string(stats.best_enemies), 264, 113, 0.90f, 110, white};
            lines[18] = {browser.endless ? "FORMAT" : "FASTEST 20-WAVE WIN", 164, 152, 0.42f, 210, muted};
            lines[19] = {browser.endless ? "ENDLESS SURVIVAL" : stats.fastest_win_seconds ? game::pc_survival::SurvivalTime(stats.fastest_win_seconds) : "NO WIN YET",
                         164, 165, stats.fastest_win_seconds ? 0.80f : 0.55f, 210, white};
            lines[20] = {"RUNS: " + std::to_string(stats.runs) + (browser.endless ? "" : "   WINS: " + std::to_string(stats.wins)), 164, 197, 0.48f, 210};
            lines[21] = {"KILLS: " + std::to_string(stats.enemies) + "   DEATHS: " + std::to_string(stats.deaths), 164, 218, 0.48f, 210};
        }
    }
    lines[22] = {"UP/DOWN: ARENA  L/R: MODE  A: DIFFICULTY  B: BACK", 0, 234, 0.40f, 374, muted};
    return lines;
}
void RenderSurvivalSetup(X86Context& ctx, uint32_t root, bool visible) {
    if (!visible) {
        for (unsigned i = 0; i < 9; ++i) {
            const auto clip = Get(ctx, root, ("m_survival_setup" + std::to_string(i)).c_str());
            if (Clip(clip) && kernel::R32(clip + 0x44) == root) Set(ctx, clip, "_visible", Integer(ctx, 0));
        }
        if (g_survival_setup && g_survival_setup->entry.root == root) g_survival_setup->ready = false;
        return;
    }
    auto& setup = *g_survival_setup;
    if (setup.ready) return;
    constexpr int gold = 0xD5AE32, white = 0xFFFFFF, muted = 0x69B9BE;
    const std::array<ResultLine, 9> lines{{
        {setup.choice == 2 ? "SURVIVAL RECORDS" : "SURVIVAL SETUP", 0, 0, 0.95f, 374, white},
        {setup.choice == 2 ? "CHOOSE RECORD FILTERS" : setup.choice == 0 ? "SOLO" : "COOP", 0, 32, 0.60f, 374, muted},
        {"FORMAT", 0, 68, 0.95f, 140, setup.row == 0 ? gold : muted},
        {setup.endless ? "< ENDLESS >" : "< CLASSIC - 20 WAVES >", 150, 68, 0.95f, 224, setup.row == 0 ? gold : white},
        {"DIFFICULTY", 0, 108, 0.95f, 140, setup.row == 1 ? gold : muted},
        {std::string("< ") + game::pc_survival::DifficultyFor(setup.difficulty).label + " >", 150, 108, 0.95f, 224, setup.row == 1 ? gold : white},
        {std::string(setup.row == 2 ? "> " : "  ") + (setup.choice == 2 ? "VIEW RECORDS" : "CONTINUE"), 0, 158, 0.95f, 374, setup.row == 2 ? gold : white},
        {setup.choice == 2 ? "LOCAL RECORDS - THIS FORMAT AND DIFFICULTY" : game::pc_survival::DifficultyFor(setup.difficulty).boss_lives ?
            "BOSS REWARD: +" + std::to_string(game::pc_survival::DifficultyFor(setup.difficulty).boss_lives) + " LIVES" :
            "BOSS REWARD: NO EXTRA LIVES", 0, 196, 0.60f, 374, muted},
        {"UP/DOWN: SELECT  L/R: CHANGE  A: CONFIRM  B: BACK", 0, 234, 0.46f, 374, muted}
    }};
    setup.ready = RenderSurvivalLines(ctx, setup.entry, "m_1vsCOM_head", "m_survival_setup", 30300, lines, {438, 116}, 29.4f);
}
bool RecordsBrowserBackdrop(X86Context& ctx, uint32_t root, bool active, const std::string& path) {
    constexpr const char* name = "m_survival_book_background";
    const auto panel = Get(ctx, root, name);
    const bool owned = Clip(panel) && kernel::R32(panel + 0x44) == root;
    if (!active) {
        if (owned) Set(ctx, panel, "_visible", Integer(ctx, 0));
        SurvivalVisible(ctx, root, "bar1", true);
        return true;
    }
    const auto source = Get(ctx, root, "bar1");
    if (!Clip(source) || kernel::R32(source + 0x44) != root) return false;
    if (!Clip(panel)) {
        Call(ctx, path + ".bar1", "duplicateMovieClip", {name, "30190"});
        return false;
    }
    if (!owned) return false;
    // Retail bar1: black parallelogram (62.3,2.15)..(199.15,2.15)..(140.15,288.55)..(2.1,288.55).
    // Widen its interior while retaining the native slant; only this disposable clone is transformed.
    constexpr float sx = 504.0f / (199.15f - 62.3f), sy = 258.0f / (288.55f - 2.15f);
    constexpr float shear = ((62.3f - 2.1f) * sx - 60.0f) / (288.55f - 2.15f);
    kernel::G<float>(panel + 0x0C) = sx;
    kernel::G<float>(panel + 0x10) = 0;
    kernel::G<float>(panel + 0x14) = shear;
    kernel::G<float>(panel + 0x18) = sy;
    kernel::G<float>(panel + 0x1C) = 410.0f - 62.3f * sx - 2.15f * shear;
    kernel::G<float>(panel + 0x20) = 107.0f - 2.15f * sy;
    kernel::W32(panel + 0x58, kernel::R32(panel + 0x58) | 0x10000);
    if (!Set(ctx, panel, "_alpha", Integer(ctx, 100)) || !Set(ctx, panel, "_visible", Integer(ctx, 1))) return false;
    SurvivalVisible(ctx, root, "bar1", false);
    return true;
}
void InspectRecordsBrowser(X86Context& ctx, uint32_t root, const std::string& path) {
    const bool active = g_survival.active && g_records_browser && g_records_browser->entry.root == root &&
        g_records_browser->entry.menu == Own(ctx, root, "g_oMenu2");
    const bool setup = g_survival.active && g_survival_setup && g_survival_setup->entry.root == root &&
        g_survival_setup->entry.menu == Own(ctx, root, "g_oMenu2");
    const bool overlay = active || setup;
    const bool backdrop_ready = RecordsBrowserBackdrop(ctx, root, overlay, path);
    RenderSurvivalSetup(ctx, root, setup && !active && backdrop_ready);
    for (const auto* name : {"m_1vsCOM_head", "m_1vs2_head"}) SurvivalVisible(ctx, root, name, !overlay);
    const auto button = Get(ctx, root, kRecordsButton);
    if (Clip(button) && kernel::R32(button + 0x44) == root)
        SurvivalVisible(ctx, root, kRecordsButton, g_survival.active && !overlay);
    if (!active) {
        for (unsigned i = 0; i < 23; ++i) {
            const auto name = "m_survival_book" + std::to_string(i);
            const auto clip = Get(ctx, root, name.c_str());
            if (Clip(clip) && kernel::R32(clip + 0x44) == root) Set(ctx, clip, "_visible", Integer(ctx, 0));
        }
        return;
    }
    auto& browser = *g_records_browser;
    if (!backdrop_ready) { browser.ready = false; return; }
    if (browser.ready && browser.painted_venue == int(browser.venue) && browser.painted_mode == int(browser.mode) &&
        browser.painted_difficulty == int(browser.difficulty) && browser.painted_endless == int(browser.endless)) return;
    browser.ready = RenderSurvivalLines(ctx, browser.entry, "m_1vsCOM_head", "m_survival_book", 30200,
                                       RecordsBrowserLines(browser), {438, 116}, 29.4f);
    if (browser.ready) {
        browser.painted_venue = int(browser.venue); browser.painted_mode = int(browser.mode);
        browser.painted_difficulty = int(browser.difficulty);
        browser.painted_endless = int(browser.endless);
        LOGR("survival-records: browser arena=%s mode=%s difficulty=%s format=%s available=%u", game::pc_survival::Arenas[browser.venue].key,
             kRecordModeLabels[browser.mode], game::pc_survival::DifficultyFor(browser.difficulty).key, browser.endless ? "endless" : "classic", browser.book.has_value());
    }
}
void OpenRecordsBrowser(X86Context& ctx, const SurvivalPage& page, unsigned difficulty, bool endless) {
    g_records_browser = SurvivalRecordsBrowser{MakeEntry(ctx, page.root, page.menu, page.keys[4], 2, page.path, false),
                                               game::pc_survival::ReadRecords(difficulty, endless), 0, 0, difficulty, endless};
    InspectRecordsBrowser(ctx, page.root, page.path);
}
bool RecordsBrowserAction(size_t key) {
    if (!g_records_browser) return false;
    if (key == 5) { g_records_browser.reset(); return true; }
    auto& browser = *g_records_browser;
    const unsigned arenas = unsigned(game::pc_survival::Arenas.size()), modes = unsigned(kRecordModeLabels.size());
    if (key == 0) browser.venue = (browser.venue + arenas - 1) % arenas;
    if (key == 1) browser.venue = (browser.venue + 1) % arenas;
    if (key == 2) browser.mode = (browser.mode + modes - 1) % modes;
    if (key == 3) browser.mode = (browser.mode + 1) % modes;
    if (key == 4) {
        browser.difficulty = (browser.difficulty + 1) % unsigned(game::pc_survival::Difficulties.size());
        browser.book = game::pc_survival::ReadRecords(browser.difficulty, browser.endless);
    }
    if (key < 5) browser.ready = false;
    return true;
}
bool EditSurvivalSetup(size_t key) {
    if (!g_survival_setup) return true;
    if (key == 5) { g_survival_setup.reset(); return true; }
    auto& setup = *g_survival_setup;
    if (key == 0 || key == 1) {
        setup.row = (setup.row + (key == 0 ? 2u : 1u)) % 3u;
        setup.ready = false;
        return true;
    }
    if ((key == 2 || key == 3 || key == 4) && setup.row < 2) {
        if (setup.row == 0) setup.endless = !setup.endless;
        else {
            const auto count = unsigned(game::pc_survival::Difficulties.size());
            setup.difficulty = (setup.difficulty + (key == 2 ? count - 1 : 1)) % count;
        }
        setup.ready = false;
        return true;
    }
    return !(key == 4 && setup.row == 2 && setup.ready);
}

bool SurvivalHumanPartner() {
    // Poll() includes the keyboard's virtual pad; only mapped physical controllers select human Co-op.
    const auto devices = input::Controllers();
    const auto ports = game::pc_controls::Read().controller;
    return ports[0] < devices.size() && ports[1] < devices.size() && ports[0] != ports[1] &&
        devices[ports[0]].connected && devices[ports[1]].connected;
}

void SurvivalCaption(X86Context& ctx, uint32_t root, const char* name, const char* text, bool active) {
    const auto clip = Get(ctx, root, name), field = Get(ctx, clip, "theText");
    if (!Type(field)) return;
    const auto previous = Own(ctx, clip, "g_pcSurvivalCaption");
    if (active) {
        if (!previous) Set(ctx, clip, "g_pcSurvivalCaption", String(ctx, ReadString(Get(ctx, field, "text"))));
        PutText(ctx, field, text);
    } else if (previous) PutText(ctx, field, ReadString(previous));
}
void UpdateSoloSelection(X86Context& ctx, uint32_t root, const std::string& path) {
    const bool solo = g_survival.active && g_survival.mode_selected && !g_survival.cooperative;
    constexpr std::array<const char*, 13> partner_clips{
        "playerBackThing1", "m_charMask1", "m_char1", "m_crawl1", "m_charText1", "m_pressStart1", "m_att1",
        "m_costume1", "m_lives1", "m_ready1", "locked1", "m_back1", "m_cursor1"};
    for (const auto* name : partner_clips) SurvivalVisible(ctx, root, name, !solo);
    if (!solo) return;
    const int ready = Number(Get(ctx, root, "kPLAYERSTATE_READY"));
    if (ready < 0 || Number(ArrayAt(Own(ctx, root, "g_aPlayerState"), 0)) != ready) {
        g_survival.solo_completed_root = 0; return;
    }
    if (g_survival.solo_completed_root == root) return;
    const auto data = Get(ctx, root, "gData");
    const int id = Number(ArrayAt(Get(ctx, data, "m_CS_aCharSelect"), 0));
    const int costume = Number(ArrayAt(Get(ctx, data, "m_CS_aCharCostume"), 0));
    const int lives = Number(ArrayAt(Get(ctx, data, "m_CS_aCharLives"), 0));
    if (id < 0 || id >= 18 || costume < 0 || costume > 1 || lives < 1 || lives > 9) return;
    // The stock loading screen waits for two portrait widths, even in solo.
    // Supply a UI-only copy; PrepareNative ignores every P2 field in solo.
    for (const auto* member : {"m_CS_aCharSelect", "m_CS_aCharCostume", "m_CS_aCharLives"}) {
        const auto array = Get(ctx, data, member), value = ArrayAt(array, 0);
        if (!Type(value)) return;
        ctx.ecx = array;
        kernel::CallGuest(ctx, 0x151C30, {1, value});
    }
    g_survival.solo_completed_root = root;
    g_solo_next = std::make_pair(root, path);
    LOGR("survival-menu: solo hero ready; skipped computer selection id=%d costume=%d lives=%d", id, costume, lives);
}

void InspectSurvival(X86Context& ctx, uint32_t root, const std::string& path, std::vector<SurvivalPage>& pages) {
    std::optional<SurvivalScreen> screen;
    const char* menu_name = "g_oMenu";
    if (Own(ctx, root, "kMultiPlayer_Selection") && Own(ctx, root, "kSingle_Selection") && Own(ctx, root, "kOnline_Selection")) {
        if (!AddSurvivalButton(ctx, root, path)) return;
        if (!AddDesktopButtons(ctx, root, path)) return;
        screen = SurvivalScreen::Main;
    } else if (Own(ctx, root, "kMatchType_1vsCOM") && Own(ctx, root, "g_aMenu2Clips")) {
        screen = SurvivalScreen::Match; menu_name = "g_oMenu2";
        SurvivalCaption(ctx, root, "m_1vsCOM_head", "SOLO", g_survival.active);
        SurvivalCaption(ctx, root, "m_1vs2_head", "COOP", g_survival.active);
        SurvivalVisible(ctx, root, "m_timer", !g_survival.active);
        if (ConfigureSurvivalMatch(ctx, root, path)) {
            InspectRecordsBrowser(ctx, root, path);
        }
    } else if (Own(ctx, root, "g_aPlayers") && Own(ctx, root, "kSelect_COM") && Own(ctx, root, "g_nSelState")) {
        screen = SurvivalScreen::Characters;
        if (g_survival.active) Set(ctx, Get(ctx, root, "gData"), "m_MT_eMatchType", Integer(ctx, 0));
        UpdateSoloSelection(ctx, root, path);
    } else if (Own(ctx, root, "g_oVenueList") && Own(ctx, root, "g_aVenueObject")) {
        screen = SurvivalScreen::Venue;
    }
    if (Own(ctx, root, "cId0") && Own(ctx, root, "cId1") &&
        Clip(Get(ctx, root, "m_c0")) && Clip(Get(ctx, root, "m_c1")) && Clip(Get(ctx, root, "m_background"))) {
        const bool visible = !(g_survival.active || g_survival_load_committed) || g_survival.cooperative;
        SurvivalVisible(ctx, root, "m_c1", visible);
        SurvivalVisible(ctx, root, "m_char1", visible);
    }
    if (!screen) return;
    SurvivalPage page{*screen, root, Own(ctx, root, menu_name), path};
    for (size_t i = 0; i < kKeys.size(); ++i) page.keys[i] = Own(ctx, root, kKeys[i]);
    pages.push_back(std::move(page));
}


struct SurvivalSelectionPatch {
    uint32_t root = 0, value = 0, data = 0;
    const char* key = "kMultiPlayer_Selection";
};
bool SurvivalAction(X86Context& ctx, uint32_t fn, SurvivalSelectionPatch& patch) {
    for (const auto& page : g_survival_pages) {
        if (!Clip(page.root)) continue;
        const auto key = std::find(page.keys.begin(), page.keys.end(), fn);
        if (!fn || key == page.keys.end()) continue;
        const auto key_index = size_t(key - page.keys.begin());
        if (page.screen == SurvivalScreen::Main && key_index == 4 &&
            ReadString(ArrayAt(Get(ctx, page.menu, "m_aGrid"), 2)) == "m_pc_survival") {
            const int index = SelectedIndex(ctx, page, "g_oMenu");
            const auto selected = ReadString(ArrayAt(Get(ctx, page.menu, "m_aGrid"), index));
            if (selected == "m_pc_quit") {
                EndSurvivalFrontend();
                MenuSound(ctx, key_index);
                game::desktop_menu::Request(game::desktop_menu::Action::Quit);
                return true;
            }
            if (index != 2) { EndSurvivalFrontend(); return false; }
            BeginSurvivalFrontend();
            const auto value = Own(ctx, page.root, "kMultiPlayer_Selection");
            if (!Type(value)) { EndSurvivalFrontend(); return true; }
            ctx.ecx = value; kernel::CallGuest(ctx, kernel::R32(kernel::R32(value)), {});
            ctx.ecx = page.root; kernel::CallGuest(ctx, kernel::R32(kernel::R32(page.root)), {});
            patch = {page.root, value};
            if (!Set(ctx, page.root, "kMultiPlayer_Selection", Integer(ctx, 2))) {
                EndSurvivalFrontend(); return true;
            }
            g_survival_main_select = true;
            return false;
        }
        if (!g_survival.active) return false;
        if (page.screen == SurvivalScreen::Match) {
            if (g_records_browser && g_records_browser->entry.root == page.root &&
                g_records_browser->entry.menu == page.menu) {
                const bool consumed = RecordsBrowserAction(key_index);
                if (consumed) MenuSound(ctx, key_index);
                return consumed;
            }
            if (g_survival_setup && g_survival_setup->entry.root == page.root && g_survival_setup->entry.menu == page.menu) {
                const bool edit = key_index < 2 || key_index == 5 ||
                    (key_index < 5 && g_survival_setup->row < 2);
                if (EditSurvivalSetup(key_index)) {
                    if (edit) MenuSound(ctx, key_index);
                    return true;
                }
                if (Own(ctx, page.root, "g_oMenu2") != page.menu || !ConfigureSurvivalMatch(ctx, page.root, page.path)) return true;
                const auto setup = *g_survival_setup;
                if (setup.choice == 2) {
                    OpenRecordsBrowser(ctx, page, setup.difficulty, setup.endless);
                    MenuSound(ctx, key_index);
                    return true;
                }
                if (SelectedIndex(ctx, page, "g_oMenu2") != int(setup.choice)) return true;
                g_survival.difficulty = setup.difficulty;
                g_survival.endless = setup.endless;
                g_survival_setup.reset();
                InspectRecordsBrowser(ctx, page.root, page.path);
                LOGR("survival-menu: setup confirmed format=%s difficulty=%s", g_survival.endless ? "endless" : "classic",
                     game::pc_survival::DifficultyFor(g_survival.difficulty).key);
            } else {
                if (key_index == 5) { EndSurvivalFrontend(); return false; }
                if (key_index == 2 || key_index == 3) return true;
                if (key_index == 4) {
                    if (Own(ctx, page.root, "g_oMenu2") != page.menu || !ConfigureSurvivalMatch(ctx, page.root, page.path)) return true;
                    const int index = SelectedIndex(ctx, page, "g_oMenu2");
                    if (index < 0 || index > 2) return true;
                    g_survival_setup = SurvivalSetup{MakeEntry(ctx, page.root, page.menu, page.keys[4], index, page.path, false),
                        unsigned(index), 0, g_survival.difficulty, g_survival.endless};
                    InspectRecordsBrowser(ctx, page.root, page.path);
                    MenuSound(ctx, key_index);
                    return true;
                }
            }
            if (key_index == 4) {
                if (Own(ctx, page.root, "g_oMenu2") != page.menu ||
                    !ConfigureSurvivalMatch(ctx, page.root, page.path)) return true;
                const int index = SelectedIndex(ctx, page, "g_oMenu2");
                if (index < 0 || index > 1) return true;
                const bool ai_partner = index == 1 && !SurvivalHumanPartner();
                const auto value = Own(ctx, page.root, "kMatchType_1vsCOM");
                if (!Type(value)) return true;
                ctx.ecx = value; kernel::CallGuest(ctx, kernel::R32(kernel::R32(value)), {});
                ctx.ecx = page.root; kernel::CallGuest(ctx, kernel::R32(kernel::R32(page.root)), {});
                patch.root = page.root; patch.value = value; patch.key = "kMatchType_1vsCOM";
                // The original COM path lets P1 choose the ally; two physical controllers keep native Co-op selection.
                if (!Set(ctx, page.root, patch.key, Integer(ctx, ai_partner ? 1 : 0))) return true;
                g_survival.cooperative = index != 0;
                g_survival.ai_partner = ai_partner;
                g_survival.mode_selected = true;
                g_survival.selected = false;
                g_survival.solo_completed_root = 0;
                const auto data = Get(ctx, page.root, "gData");
                if (!Type(data)) return true;
                ctx.ecx = data; kernel::CallGuest(ctx, kernel::R32(kernel::R32(data)), {});
                patch.data = data;
                Set(ctx, data, "m_MT_eMatchType", Integer(ctx, 0));
                Call(ctx, page.path + ".m_timer", "Enable", {"1"});
                LOGR("survival-menu: selected %s", index == 0 ? "SOLO" : ai_partner ? "COOP + AI" : "COOP");
            }
        }
        return false;
    }
    return false;
}
void RestoreSurvivalSelection(X86Context& ctx, const SurvivalSelectionPatch& patch) {
    if (patch.data) {
        // setMenuValues may restore a remembered timed match. Survival always uses stocks.
        Set(ctx, patch.data, "m_MT_eMatchType", Integer(ctx, 0));
        ctx.ecx = patch.data; kernel::CallGuest(ctx, kernel::R32(kernel::R32(patch.data) + 4), {});
    }
    if (!patch.root) return;
    Set(ctx, patch.root, patch.key, patch.value);
    ctx.ecx = patch.value; kernel::CallGuest(ctx, kernel::R32(kernel::R32(patch.value) + 4), {});
    ctx.ecx = patch.root; kernel::CallGuest(ctx, kernel::R32(kernel::R32(patch.root) + 4), {});
    g_survival_main_select = false;
}

void SettingsToggleChildren(X86Context& ctx, uint32_t row, bool restore) {
    const auto toggle = Get(ctx, row, "m_toggleoption");
    if (!Clip(toggle) || kernel::R32(toggle + 0x44) != row) return;
    for (const char* name : {"m_toggleoption", "m_left", "m_right"}) {
        const auto child = Get(ctx, row, name);
        if (!Clip(child) || kernel::R32(child + 0x44) != row) continue;
        const bool hidden = Number(Own(ctx, child, "g_pcSettingsHidden"), 0) == 1;
        if (restore) {
            if (!hidden) continue;
            const auto previous = Own(ctx, child, "g_pcSettingsVisible");
            if (Type(previous)) Set(ctx, child, "_visible", previous);
            Set(ctx, child, "g_pcSettingsHidden", Integer(ctx, 0));
        } else {
            if (hidden) continue;
            const auto previous = Get(ctx, child, "_visible");
            if (Type(previous) && Set(ctx, child, "g_pcSettingsVisible", previous) &&
                Set(ctx, child, "g_pcSettingsHidden", Integer(ctx, 1)))
                Set(ctx, child, "_visible", Integer(ctx, 0));
        }
    }
}

void RestorePage(X86Context& ctx) {
    if (!g_page) return;
    const auto entry = g_page->entry;
    game::settings_panel::NativeEnd();
    for (const auto& row : g_page->rows) {
        // A timeline may have replaced a child while the page was open.
        if (Get(ctx, entry.root, row.name.c_str()) != row.clip || !Clip(row.clip) ||
            ResultField(ctx, row.clip) != row.field) continue;
        for (size_t i = 0; i < row.matrix.size(); ++i)
            kernel::G<float>(row.clip + 0x0C + uint32_t(i) * 4) = row.matrix[i];
        const auto data = kernel::R32(row.clip + 0x48);
        if (Address(data, 0x20)) kernel::W32(data + 0x1C, row.data_flags);
        SettingsToggleChildren(ctx, row.clip, true);
        if (!row.auto_size.empty()) Set(ctx, row.field, "autoSize", String(ctx, row.auto_size));
        PutText(ctx, row.field, row.text);
        Set(ctx, row.field, "textColor", Integer(ctx, int(row.color)));
    }
    g_page.reset();
    g_ignore_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    // Retained for the session so cached fields cannot become dangling GC values.
    if (Type(entry.root)) {
        ctx.ecx = entry.root;
        kernel::CallGuest(ctx, kernel::R32(kernel::R32(entry.root) + 4), {});
    }
    LOGR("pc-menu: native settings closed; restored %s", entry.path.c_str());
}

bool BeginPage(X86Context& ctx, const Entry& entry) {
    if (!Clip(entry.root) || Get(ctx, entry.root, "g_oMenu") != entry.menu) return false;
    const uint32_t grid = Get(ctx, entry.menu, "m_aGrid");
    if (Type(grid) != 0x16) return false;
    const auto count = kernel::R32(grid + 0x24);
    if (count < 4 || count > 12) return false;
    NativePage page{entry};
    // Prepare everything before changing the first visible row. Reuse native
    // text fields, font and background; no ImGui window is opened here.
    for (uint32_t i = 0; i < count; ++i) {
        SavedRow row;
        row.name = ReadString(ArrayAt(grid, int(i)));
        row.clip = Get(ctx, entry.root, row.name.c_str());
        if (!Clip(row.clip)) {
            LOGR("pc-menu: native row unavailable: %s", row.name.c_str()); return false;
        }
        row.field = ResultField(ctx, row.clip);
        if (Type(row.field) != 0x0F) {
            LOGR("pc-menu: native text field unavailable: %s (type %u)", row.name.c_str(), Type(row.field)); return false;
        }
        row.text = ReadString(Get(ctx, row.field, "text"));
        row.auto_size = ReadString(Get(ctx, row.field, "autoSize"));
        row.color = Color(Get(ctx, row.field, "textColor"));
        const auto data = kernel::R32(row.clip + 0x48);
        if (!Address(data, 0x20)) return false;
        row.data_flags = kernel::R32(data + 0x1C);
        for (size_t m = 0; m < row.matrix.size(); ++m) {
            row.matrix[m] = kernel::G<float>(row.clip + 0x0C + uint32_t(m) * 4);
            if (!std::isfinite(row.matrix[m])) return false;
        }
        page.rows.push_back(std::move(row));
    }
    // Options has no keyRight script. Navigation comes from mapped host input,
    // so a page need only provide the select callback used to open this entry.
    if (Type(entry.select) != 9 && Type(entry.select) != 0x0A) return false;
    if (!game::settings_panel::NativeBegin(entry.development)) return false;
    ctx.ecx = entry.root;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(entry.root)), {});
    g_page = std::move(page);
    for (const auto& row : g_page->rows) {
        const auto data = kernel::R32(row.clip + 0x48);
        kernel::W32(data + 0x1C, row.data_flags & ~0x02000000u); // stop each button's timeline
        Set(ctx, row.field, "autoSize", String(ctx, "left"));
        SettingsToggleChildren(ctx, row.clip, false);
    }
    g_ignore_until = std::chrono::steady_clock::now();
    LOGR("pc-menu: native page ready at %s (%u stock rows)", entry.path.c_str(), count);
    return true;
}

std::string Caption(std::string text) {
    // The Xbox font is an ASCII subset. Win32 key names can contain accents;
    // use the existing numeric key fallback in the model for those names.
    for (auto& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
void RenderPage(X86Context& ctx) {
    if (g_pending) {
        const Entry entry = std::move(*g_pending);
        g_pending.reset();
        if (!BeginPage(ctx, entry)) LOGR("pc-menu: could not prepare native page at %s", entry.path.c_str());
    }
    if (!g_page) return;
    if (Get(ctx, g_page->entry.root, "g_oMenu") != g_page->entry.menu) { RestorePage(ctx); return; }
    if (const auto sound = game::settings_panel::TakeNativeSound()) MenuSound(ctx, size_t(*sound));
    for (unsigned count = game::settings_panel::TakeNativeMusicToggles(); count; --count) ToggleMusic(ctx);
    // The snapshot owns its strings, and releases UiLock before any guest calls.
    const auto view = game::settings_panel::NativeView();
    if (!view.open) { RestorePage(ctx); return; }
    auto& page = *g_page;
    const auto visible = page.rows.size() - 2; // heading, scrolling rows, instructions
    if (size_t(view.selected) < page.top) page.top = size_t(view.selected);
    if (size_t(view.selected) >= page.top + visible) page.top = size_t(view.selected) - visible + 1;
    page.top = std::min(page.top, view.rows.size() > visible ? view.rows.size() - visible : size_t(0));
    // Keep the heading inside the slanted panel: 12 APT units right of the previous layout.
    const float left = page.rows.front().matrix[4];
    const float first_y = page.rows.front().matrix[5], last_y = page.rows.back().matrix[5];
    for (size_t i = 0; i < page.rows.size(); ++i) {
        const auto& row = page.rows[i];
        if (Get(ctx, page.entry.root, row.name.c_str()) != row.clip || !Clip(row.clip) ||
            ResultField(ctx, row.clip) != row.field) { RestorePage(ctx); return; }
        const bool heading = i == 0, footer = i + 1 == page.rows.size();
        const size_t index = heading ? 0 : page.top + i - 1;
        const bool selected = !heading && !footer && index < view.rows.size() && int(index) == view.selected;
        std::string text = heading ? view.title : footer ? view.hint :
            index < view.rows.size() ? (selected ? "> " : "  ") + view.rows[index] : "";
        if (footer && text.size() > 78) text = text.substr(0, 75) + "...";
        if (heading && !view.rows.empty())
            text += "  " + std::to_string(view.selected + 1) + "/" + std::to_string(view.rows.size());
        if (!PutText(ctx, row.field, Caption(std::move(text)))) {
            LOGR("pc-menu: text setter failed; restoring original page"); RestorePage(ctx); return;
        }
        float scale = footer ? 0.45f : 1.0f;
        // Keep the authored menu size unless a long setting or a dense page needs to fit.
        const float width = float(Number(Get(ctx, row.field, "_width"), 0)) * std::abs(row.matrix[0]);
        const float height = float(Number(Get(ctx, row.field, "_height"), 0)) * std::abs(row.matrix[3]);
        const float row_height = std::abs(last_y - first_y) / float(page.rows.size() - 1);
        if (std::isfinite(width) && width > 0) scale = std::min(scale, 294.0f / width);
        if (std::isfinite(height) && height > 0 && row_height > 0)
            scale = std::min(scale, row_height * 0.90f / height);
        for (size_t m = 0; m < 4; ++m)
            kernel::G<float>(row.clip + 0x0C + uint32_t(m) * 4) = row.matrix[m] * scale;
        kernel::G<float>(row.clip + 0x1C) = left;
        kernel::G<float>(row.clip + 0x20) = first_y + (last_y - first_y) * float(i) / float(page.rows.size() - 1);
        const int color = selected ? 0xD5AE32 : heading ? 0xFFFFFF : 0x9EF5F7;
        if (Color(Get(ctx, row.field, "textColor"), 0) != uint32_t(color))
            Set(ctx, row.field, "textColor", Integer(ctx, color));
    }
}

void Walk(X86Context& ctx, uint32_t clip, const std::string& path, int depth, int& budget, std::vector<Entry>& entries,
          std::vector<SurvivalPage>& survival_pages) {
    if (!Clip(clip) || --budget < 0 || depth > 8 || path.size() > 180) return;
    const uint32_t type = Type(clip);
    if (type != 0x0D && type != 0x12) return;
    InspectSurvival(ctx, clip, path, survival_pages);
    Inspect(ctx, clip, path, entries);
    const uint32_t data = kernel::R32(clip + 0x48);
    const uint32_t list = Address(data, 0x28) ? kernel::R32(data + 0x24) : 0;
    uint32_t child = Address(list) ? kernel::R32(list) : 0;
    for (int left = 128; Address(child, 0x60) && budget > 0 && left; --left) {
        const auto next_child = kernel::R32(child + 0x50);
        const uint32_t token = kernel::R32(child + 8);
        if (Clip(child) && Address(token, 9)) {
            const auto length = kernel::R16(token + 2);
            if (length && length < 64 && Address(token + 8, length + 1)) {
                const std::string name(static_cast<const char*>(kernel::HostPtr(token + 8)), length);
                if (name.find_first_of("./:") == std::string::npos)
                    Walk(ctx, child, path + "." + name, depth + 1, budget, entries, survival_pages);
            }
        }
        if (next_child == child) break;
        child = next_child;
    }
}

void Update(X86Context& ctx) {
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    const bool terminal = ResultPhase(game::pc_survival::ReadView().phase);
    if (!terminal) g_result.reset();
    const bool result_pending = terminal && (!g_result || !g_result->ready ||
        g_result->painted_selection != int(g_result->main_menu) || g_result->painted_tab != int(g_result->records_tab));
    const bool browser_pending = g_records_browser && (!g_records_browser->ready ||
        g_records_browser->painted_venue != int(g_records_browser->venue) || g_records_browser->painted_mode != int(g_records_browser->mode) ||
        g_records_browser->painted_difficulty != int(g_records_browser->difficulty) ||
        g_records_browser->painted_endless != int(g_records_browser->endless));
    if (now < next && !g_pending && !g_survival.active && !result_pending && !browser_pending) return;
    next = now + std::chrono::milliseconds(200);
    const uint32_t player = kernel::R32(kPlayer);
    if (!Address(player, 0x112C)) {
        g_survival_pages.clear();
        g_solo_next.reset();
        EndSurvivalFrontend();
        RestorePage(ctx);
        g_pending.reset();
        std::lock_guard lock(g_entries_lock); g_entries.clear(); return;
    }
    const uint32_t list = kernel::R32(player + 0x1128);
    uint32_t clip = Address(list) ? kernel::R32(list) : 0;
    std::vector<Entry> entries;
    std::vector<SurvivalPage> survival_pages;
    g_solo_next.reset();
    int budget = 512;
    for (int left = 64; Address(clip, 0x60) && left; --left) {
        const auto next_clip = kernel::R32(clip + 0x50);
        const int level = int32_t(kernel::R32(clip + 0x54) << 15) >> 15;
        if (Type(clip) == 0x12 && level >= 0)
            Walk(ctx, clip, "_level" + std::to_string(level), 0, budget, entries, survival_pages);
        if (next_clip == clip) break;
        clip = next_clip;
    }
    g_survival_pages = std::move(survival_pages);
    if (g_records_browser && std::none_of(g_survival_pages.begin(), g_survival_pages.end(), [](const SurvivalPage& page) {
        return page.screen == SurvivalScreen::Match && page.root == g_records_browser->entry.root && page.menu == g_records_browser->entry.menu;
    })) g_records_browser.reset();
    if (g_survival_setup && std::none_of(g_survival_pages.begin(), g_survival_pages.end(), [](const SurvivalPage& page) {
        return page.screen == SurvivalScreen::Match && page.root == g_survival_setup->entry.root && page.menu == g_survival_setup->entry.menu;
    })) g_survival_setup.reset();
    if (g_solo_next) {
        const auto pending = std::move(*g_solo_next);
        g_solo_next.reset();
        if (g_survival.active && Clip(pending.first)) Call(ctx, pending.second, "NextScreen");
    }
    if (g_page && std::none_of(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.root == g_page->entry.root && entry.menu == g_page->entry.menu;
    })) RestorePage(ctx);
    if (g_pending && std::none_of(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.root == g_pending->root && entry.menu == g_pending->menu;
    })) {
        LOGR("pc-menu: opening cancelled because the source page changed");
        g_pending.reset();
    }
    std::lock_guard lock(g_entries_lock);
    g_entries = std::move(entries);
}
bool DevelopmentFrontend(X86Context& ctx) {
    // Copy under the host lock, release it before invoking any APT getter.
    std::vector<Entry> entries;
    {
        std::lock_guard lock(g_entries_lock);
        entries = g_entries;
    }
    for (const auto& entry : entries) {
        if (entry.development && Clip(entry.root) &&
            Get(ctx, entry.root, "g_oMenu") == entry.menu &&
            Own(ctx, entry.root, "kCredits") && Own(ctx, entry.root, "kSettings")) return true;
    }
    return false;
}
}  // namespace

X86_FUNC(__imp__sub_000F7A80);
X86_FUNC(sub_000F7A80) {
    const auto event = kernel::R32(ctx.esp + 4);
    if (event != 0x15D2CD5u) {
        g_last_music_event = event;
        g_last_music_flags = kernel::R32(ctx.esp + 8);
        g_last_music_controller = ctx.ecx;
    }
    if (app::ReadSettings().music_enabled || event == 0x15D2CD5u)
        __imp__sub_000F7A80(ctx, base);
    else {
        g_suppressed_music_event = true;
        ctx.esp += 12; // native ret 8: suppress new music while OFF
    }
}

X86_FUNC(__imp__sub_000F7AD0);
X86_FUNC(sub_000F7AD0) {
    const auto argument = ctx.esp + 4;
    const auto volume = kernel::G<float>(argument);
    if (std::isfinite(volume) && volume > 0.0f) g_requested_music_volume = kernel::R32(argument);
    if (!app::ReadSettings().music_enabled) kernel::W32(argument, 0);
    __imp__sub_000F7AD0(ctx, base);
}

X86_FUNC(__imp__sub_0013C2D0);
X86_FUNC(sub_0013C2D0) {
    __imp__sub_0013C2D0(ctx, base);
    if (g_busy) return;
    const X86Context saved = ctx;
    g_busy = true;
    game::pc_cheats::Tick(ctx);
    Update(ctx);
    RenderPage(ctx);
    const bool frontend = DevelopmentFrontend(ctx);
    const bool replay_ready = !g_page && !g_pending && !g_survival.active &&
        std::any_of(g_survival_pages.begin(), g_survival_pages.end(), [](const SurvivalPage& page) {
            return page.screen == SurvivalScreen::Main;
        });
    game::pc_development::Tick(ctx, frontend, frontend && !g_page && !g_pending, replay_ready);
    g_busy = false;
    ctx = saved;
}

X86_FUNC(__imp__sub_0012FC90);
X86_FUNC(sub_0012FC90) {
    // Use the VM's ordinary undefined-function path for intercepted menu actions.
    // It releases arguments and supplies the normal result, preserving stack rules.
    const uint32_t fn_slot = ctx.esp + 8;
    const uint32_t fn = kernel::R32(fn_slot);
    bool consumed = !g_busy && SurvivalResultAction(fn);
    std::optional<Entry> candidate;
    if (!consumed && !g_busy && fn) {
        if (g_page) candidate = g_page->entry;
        else if (g_pending) candidate = *g_pending;
        else {
            std::lock_guard lock(g_entries_lock);
            const auto found = std::find_if(g_entries.begin(), g_entries.end(),
                [fn](const Entry& entry) { return fn == entry.select; });
            if (found != g_entries.end()) candidate = *found;
        }
    }
    if (candidate && Type(candidate->root)) {
        const auto& entry = *candidate;
        const auto key = std::find(entry.keys.begin(), entry.keys.end(), fn);
        if (fn && (key != entry.keys.end() || fn == entry.select)) {
            const auto now = std::chrono::steady_clock::now();
            if (g_page || g_pending || now < g_ignore_until) {
                consumed = true;
                // Swallow any already queued guest menu action. The host input
                // path owns navigation while the native settings session runs.
            } else if (fn == entry.select) {
                const X86Context saved = ctx;
                g_busy = true;
                static uint32_t result = 0;
                if (!result) result = mem::AllocSystem(64);
                std::memset(kernel::HostPtr(result), 0, 64);
                if (Get(ctx, entry.root, "g_oMenu") == entry.menu)
                    Call(ctx, entry.path + ".g_oMenu", "getSelectIndex", {"0"}, result);
                const char* text = static_cast<const char*>(kernel::HostPtr(result));
                char* end = nullptr;
                const long index = std::strtol(text, &end, 10);
                consumed = end != text && *end == '\0' && index == entry.index;
                g_busy = false;
                ctx = saved;
                if (consumed) {
                    g_pending = entry;
                    MenuSound(ctx, 4);
                    LOGR("pc-menu: opening native PC Settings from %s", entry.path.c_str());
                }
            }
        }
    }
    SurvivalSelectionPatch survival_patch;
    if (!consumed && !g_busy && fn) {
        const X86Context saved = ctx;
        g_busy = true;
        consumed = SurvivalAction(ctx, fn, survival_patch);
        g_busy = false;
        ctx = saved;
    }
    if (consumed) kernel::W32(fn_slot, 0);
    __imp__sub_0012FC90(ctx, base);
    if (consumed) kernel::W32(fn_slot, fn);
    if (survival_patch.root || survival_patch.data) {
        const X86Context saved = ctx;
        const bool busy = g_busy;
        g_busy = true;
        RestoreSurvivalSelection(ctx, survival_patch);
        g_busy = busy;
        ctx = saved;
    }
}

X86_FUNC(__imp__sub_000E5960);
X86_FUNC(sub_000E5960) {
    const auto argument = ctx.esp + 4;
    const auto selected = kernel::R32(argument);
    const bool survival = g_survival_main_select && g_survival.active;
    if (survival) kernel::W32(argument, 0); // Original frontend setup for the Versus-derived screens.
    __imp__sub_000E5960(ctx, base);
    if (survival) kernel::W32(argument, selected);
}
X86_FUNC(__imp__sub_000E3490);
X86_FUNC(sub_000E3490) {
    if (g_survival.active) {
        const auto controller = kernel::R32(ctx.esp + 4), slot = kernel::R32(ctx.esp + 8);
        if (slot < 2) g_survival.controllers[slot] = controller;
    }
    __imp__sub_000E3490(ctx, base);
}
X86_FUNC(__imp__sub_000E36D0);
X86_FUNC(sub_000E36D0) {
    if (g_survival_load_committed) { ctx.esp += 4; return; }
    if (!g_survival.active) { __imp__sub_000E36D0(ctx, base); return; }
    // GAME_StartVsGame passes combat, match, difficulty, time. Never apply its live Versus flags.
    ctx.esp += 4; // cdecl: caller owns all four arguments.
}
X86_FUNC(__imp__sub_000E37A0);
X86_FUNC(sub_000E37A0) {
    if (g_survival_load_committed) { ctx.esp += 4; return; }
    if (!g_survival.active) { __imp__sub_000E37A0(ctx, base); return; }
    g_survival.characters = {kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 16)};
    g_survival.lives = {kernel::R32(ctx.esp + 8), kernel::R32(ctx.esp + 20)};
    g_survival.costumes = {kernel::R32(ctx.esp + 12), kernel::R32(ctx.esp + 24)};
    g_survival.venue = kernel::R32(ctx.esp + 28);
    g_survival.selected = true;
    LOGR("survival-menu: captured original character/arena selection venue=%u", g_survival.venue);
    ctx.esp += 4; // cdecl; leave all seven arguments for the native parser to release.
}
X86_FUNC(__imp__sub_000E3360);
X86_FUNC(sub_000E3360) {
    if (g_survival_load_committed) { ctx.esp += 4; return; }
    if (!g_survival.active) { __imp__sub_000E3360(ctx, base); return; }
    const auto selection = g_survival;
    EndSurvivalFrontend(); // Restore native caches before Survival snapshots the campaign records.
    const X86Context saved = ctx;
    std::string error;
    if (!selection.mode_selected || !selection.selected ||
        !game::pc_development::LaunchSurvival(ctx, selection.characters, selection.costumes, selection.lives,
                                             selection.controllers, selection.cooperative, selection.venue, error, 1, selection.ai_partner, selection.difficulty, selection.endless)) {
        if (error.empty()) error = "SURVIVAL SELECTION INVALID; RETURN TO MAIN MENU";
        game::pc_development::Complete(error);
        LOGR("survival-menu: launch rejected: %s", error.c_str());
        kernel::W32(0x4C343C, 1); // Native frontend recovery instead of a partially configured Versus load.
    }
    // A repeated loading-frame callback must never fall through to live Versus setup.
    g_survival_load_committed = true;
    ctx = saved;
    ctx.esp += 4;
}

X86_FUNC(__imp__sub_0010C5B0);
X86_FUNC(sub_0010C5B0) {
    const auto blob = kernel::R32(ctx.esp + 4);
    ReportPauseGlyph(blob);
    PauseDrawTrace trace;
    auto* previous = g_pause_draw;
    g_pause_draw = TracePauseDraw(blob, trace) ? &trace : nullptr;
    __imp__sub_0010C5B0(ctx, base);
    g_pause_draw = previous;
}

X86_FUNC(__imp__sub_00157D80);
X86_FUNC(sub_00157D80) {
    ReportPauseArrow(ctx.ecx);
    __imp__sub_00157D80(ctx, base);
}

// Diagnostic wrappers keep the original cdecl returns and never change native draw state.
X86_FUNC(__imp__sub_000DDD40);
X86_FUNC(sub_000DDD40) {
    const bool report = g_pause_draw && g_pause_draw->quads < 8 && kernel::R32(ctx.esp) == 0xDFC18;
    std::array<uint32_t, 4> output{};
    if (report) {
        if (!g_pause_draw->quads) ReportPauseFont();
        for (unsigned i = 0; i < output.size(); ++i) output[i] = kernel::R32(ctx.esp + 0x24 + i * 4);
    }
    __imp__sub_000DDD40(ctx, base);
    if (report && std::all_of(output.begin(), output.end(), [](uint32_t at) { return Address(at, 4); })) {
        ++g_pause_draw->quads;
        LOGR("[DEBUG-pause-final] root=%08X quad=%u bounds=%.3f,%.3f,%.3f,%.3f",
             g_pause_draw->root, g_pause_draw->quads, kernel::G<float>(output[0]), kernel::G<float>(output[1]),
             kernel::G<float>(output[2]), kernel::G<float>(output[3]));
    }
}
X86_FUNC(__imp__sub_001CF510);
X86_FUNC(sub_001CF510) {
    const auto vertices = kernel::R32(ctx.esp + 4);
    const bool report = g_pause_draw && g_pause_draw->transforms < 8 && kernel::R32(ctx.esp) == 0xDFC63 &&
        kernel::R32(ctx.esp + 8) == 2 && Address(vertices, 0x24);
    __imp__sub_001CF510(ctx, base);
    if (report) {
        ++g_pause_draw->transforms;
        LOGR("[DEBUG-pause-final] root=%08X transformed=%u corners=%.3f,%.3f,%.3f,%.3f",
             g_pause_draw->root, g_pause_draw->transforms, kernel::G<float>(vertices), kernel::G<float>(vertices + 4),
             kernel::G<float>(vertices + 0x1C), kernel::G<float>(vertices + 0x20));
    }
}
