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
#include <string>
#include <vector>

#include "xdk/hle.h"
#include "core/log.h"
#include "common/settings_panel.h"
#include "common/pc_cheats.h"
#include "common/pc_development.h"

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
    if (ReadString(ArrayAt(grid, count - 1)) == kButton && Clip(button)) {
        if (!g_page || g_page->entry.root != root) Label(ctx, button);
        entries.push_back(MakeEntry(ctx, root, menu, select, count - 1, path, options));
        return;
    }
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
void RestorePage(X86Context& ctx) {
    if (!g_page) return;
    const auto entry = g_page->entry;
    game::settings_panel::NativeEnd();
    for (const auto& row : g_page->rows) {
        // A timeline may have replaced a child while the page was open.
        if (Get(ctx, entry.root, row.name.c_str()) != row.clip || !Clip(row.clip) ||
            Get(ctx, row.clip, "theText") != row.field) continue;
        for (size_t i = 0; i < row.matrix.size(); ++i)
            kernel::G<float>(row.clip + 0x0C + uint32_t(i) * 4) = row.matrix[i];
        const auto data = kernel::R32(row.clip + 0x48);
        if (Address(data, 0x20)) kernel::W32(data + 0x1C, row.data_flags);
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
        row.field = Get(ctx, row.clip, "theText");
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
            Get(ctx, row.clip, "theText") != row.field) { RestorePage(ctx); return; }
        const bool heading = i == 0, footer = i + 1 == page.rows.size();
        const size_t index = heading ? 0 : page.top + i - 1;
        const bool selected = !heading && !footer && index < view.rows.size() && int(index) == view.selected;
        std::string text = heading ? view.title : footer ? view.hint :
            index < view.rows.size() ? (selected ? "> " : "  ") + view.rows[index] : "";
        if (footer && text.size() > 78) text = text.substr(0, 75) + "...";
        if (heading && !view.rows.empty())
            text += "  " + std::to_string(view.selected + 1) + "/" + std::to_string(view.rows.size());
        const float scale = footer ? 0.35f : 0.60f;
        for (size_t m = 0; m < 4; ++m)
            kernel::G<float>(row.clip + 0x0C + uint32_t(m) * 4) = row.matrix[m] * scale;
        kernel::G<float>(row.clip + 0x1C) = left;
        kernel::G<float>(row.clip + 0x20) = first_y + (last_y - first_y) * float(i) / float(page.rows.size() - 1);
        if (!PutText(ctx, row.field, Caption(std::move(text)))) {
            LOGR("pc-menu: text setter failed; restoring original page"); RestorePage(ctx); return;
        }
        const int color = selected ? 0xD5AE32 : heading ? 0xFFFFFF : 0x9EF5F7;
        if (Color(Get(ctx, row.field, "textColor"), 0) != uint32_t(color))
            Set(ctx, row.field, "textColor", Integer(ctx, color));
    }
}

void Walk(X86Context& ctx, uint32_t clip, const std::string& path, int depth, int& budget, std::vector<Entry>& entries) {
    if (!Clip(clip) || --budget < 0 || depth > 8 || path.size() > 180) return;
    const uint32_t type = Type(clip);
    if (type != 0x0D && type != 0x12) return;
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
                    Walk(ctx, child, path + "." + name, depth + 1, budget, entries);
            }
        }
        if (next_child == child) break;
        child = next_child;
    }
}

void Update(X86Context& ctx) {
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next && !g_pending) return;
    next = now + std::chrono::milliseconds(200);
    const uint32_t player = kernel::R32(kPlayer);
    if (!Address(player, 0x112C)) {
        RestorePage(ctx);
        g_pending.reset();
        std::lock_guard lock(g_entries_lock); g_entries.clear(); return;
    }
    const uint32_t list = kernel::R32(player + 0x1128);
    uint32_t clip = Address(list) ? kernel::R32(list) : 0;
    std::vector<Entry> entries;
    int budget = 512;
    for (int left = 64; Address(clip, 0x60) && left; --left) {
        const auto next_clip = kernel::R32(clip + 0x50);
        const int level = int32_t(kernel::R32(clip + 0x54) << 15) >> 15;
        if (Type(clip) == 0x12 && level >= 0)
            Walk(ctx, clip, "_level" + std::to_string(level), 0, budget, entries);
        if (next_clip == clip) break;
        clip = next_clip;
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
    game::pc_development::Tick(ctx, frontend, frontend && !g_page && !g_pending);
    g_busy = false;
    ctx = saved;
}

X86_FUNC(__imp__sub_0012FC90);
X86_FUNC(sub_0012FC90) {
    // Use the VM's ordinary undefined-function path for intercepted menu actions.
    // It releases arguments and supplies the normal result, preserving stack rules.
    const uint32_t fn_slot = ctx.esp + 8;
    const uint32_t fn = kernel::R32(fn_slot);
    bool consumed = false;
    std::optional<Entry> candidate;
    if (!g_busy && fn) {
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
                    LOGR("pc-menu: opening native PC Settings from %s", entry.path.c_str());
                }
            }
        }
    }
    if (consumed) kernel::W32(fn_slot, 0);
    __imp__sub_0012FC90(ctx, base);
    if (consumed) kernel::W32(fn_slot, fn);
}
