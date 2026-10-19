#include <atomic>
#include <imgui.h>
#include <toml++/toml.hpp>

#include "common/pc_render.h"
#include "core/log.h"

namespace game::pc_render {
namespace {

std::atomic<uint32_t> g_object_distance{100};
constexpr uint32_t kDefaultBloom = (25u << 1) | (75u << 8) | (100u << 15);
std::atomic<uint32_t> g_bloom{kDefaultBloom};

uint32_t InRange(uint32_t value, uint32_t low, uint32_t high, uint32_t fallback) {
    return value >= low && value <= high ? value : fallback;
}

uint32_t PackBloom(const Settings& s) {
    return uint32_t(s.bloom) | (InRange(s.bloom_strength, 0, 100, 25) << 1) |
           (InRange(s.bloom_threshold, 0, 95, 75) << 8) |
           (InRange(s.bloom_radius, 25, 300, 100) << 15);
}

uint32_t Normalize(uint32_t value) {
    switch (value) {
        case 100: case 150: case 200: case 300: return value;
        default: return 100;
    }
}

}  // namespace

uint32_t ObjectDistancePercent() { return g_object_distance.load(); }
gpu::BloomParameters BloomSettings() {
    const uint32_t value = g_bloom.load();
    return {value & 1, (value >> 1) & 0x7F, (value >> 8) & 0x7F, (value >> 15) & 0x1FF};
}

Settings Read() {
    const auto bloom = BloomSettings();
    return Settings{ObjectDistancePercent(), bloom.enabled != 0, bloom.strength, bloom.threshold, bloom.radius};
}

void Set(const Settings& settings) {
    const uint32_t value = Normalize(settings.object_distance);
    if (g_object_distance.exchange(value) != value)
        LOGR("pc-render: object_distance=%u%% (object range only)", value);
    const uint32_t bloom = PackBloom(settings);
    if (g_bloom.exchange(bloom) != bloom) {
        const auto p = BloomSettings();
        LOGR("pc-render: bloom=%u strength=%u threshold=%u radius=%u (SDR scene only)",
             p.enabled, p.strength, p.threshold, p.radius);
    }
}

void Load(const void* table) {
    const auto& c = (*static_cast<const toml::table*>(table))["pc_render"];
    g_object_distance = Normalize(c["object_distance"].value_or(100u));
    Settings s;
    s.bloom = c["bloom"].value_or(false);
    s.bloom_strength = c["bloom_strength"].value_or(25u);
    s.bloom_threshold = c["bloom_threshold"].value_or(75u);
    s.bloom_radius = c["bloom_radius"].value_or(100u);
    g_bloom = PackBloom(s);
}

void Save(void* table) {
    const auto s = Read();
    static_cast<toml::table*>(table)->insert_or_assign("pc_render", toml::table{
        {"object_distance", int64_t(s.object_distance)}, {"bloom", s.bloom},
        {"bloom_strength", int64_t(s.bloom_strength)}, {"bloom_threshold", int64_t(s.bloom_threshold)},
        {"bloom_radius", int64_t(s.bloom_radius)}});
}

void Draw(Settings& draft) {
    ImGui::Checkbox("Bloom (experimental)", &draft.bloom);
    ImGui::BeginDisabled(!draft.bloom);
    int strength = int(draft.bloom_strength), threshold = int(draft.bloom_threshold), radius = int(draft.bloom_radius);
    if (ImGui::SliderInt("Bloom strength", &strength, 0, 100, "%d%%")) draft.bloom_strength = uint32_t(strength);
    if (ImGui::SliderInt("Bloom threshold", &threshold, 0, 95, "%d%%")) draft.bloom_threshold = uint32_t(threshold);
    if (ImGui::SliderInt("Bloom radius", &radius, 25, 300, "%d%%")) draft.bloom_radius = uint32_t(radius);
    ImGui::EndDisabled();
    ImGui::TextWrapped("Spreads bright areas of the scene. Higher thresholds select fewer areas. No HDR monitor required.");
    ImGui::TextWrapped("Bright clothing and reflections may also glow. HUD, menus and videos are excluded.");
    if (Read().bloom) {
        const auto status = gpu::g_bloom_status.load();
        if (status == gpu::BloomStatus::Active) ImGui::TextUnformatted("Bloom: applied to the recent 3D scene.");
        else if (status == gpu::BloomStatus::Unavailable) ImGui::TextWrapped("Bloom unavailable this session; see the log. Disable and apply during gameplay to retry.");
        else ImGui::TextUnformatted("Bloom: waiting for a compatible 3D scene.");
    }
    ImGui::Separator();
    const uint32_t values[] = {100, 150, 200, 300};
    const char* names[] = {"Original (1x)", "1.5x", "2x", "3x"};
    int selected = 0;
    for (int i = 0; i < 4; ++i) if (values[i] == draft.object_distance) selected = i;
    if (ImGui::Combo("Object distance (experimental)", &selected, names, 4))
        draft.object_distance = values[selected];
    ImGui::TextWrapped("Extends the range of distance-culled objects. Results depend on the level and may increase CPU and GPU load.");
    ImGui::TextWrapped("World sectors retain their original limits. Model detail and shadow distance are unchanged.");
}

}  // namespace game::pc_render
