#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imgui.h>
#include <toml++/toml.hpp>
#include "app/app.h"
#include "core/log.h"
#include "common/pc_video.h"
namespace game::pc_video {
namespace { Settings g_settings; }
OutputSize Output() {
    std::lock_guard lock(app::UiLock());
    RECT client{};
    if (GetClientRect(static_cast<HWND>(app::Window()), &client) && client.right > 0 && client.bottom > 0)
        return {uint32_t(client.right), uint32_t(client.bottom)};
    const auto size = app::Output();
    return {size.width, size.height};
}
Settings Read() { std::lock_guard lock(app::UiLock()); return g_settings; }
void Set(const Settings& s) {
    std::lock_guard lock(app::UiLock());
    g_settings = s;
    if (g_settings.scale > 3) g_settings.scale = 0;
    LOGR("movies: scale=%u skip_logos=%u", g_settings.scale, unsigned(g_settings.skip_logos));
}
void Load(const void* table) {
    const auto& c = (*static_cast<const toml::table*>(table))["pc_video"];
    g_settings.scale = c["scale"].value_or(0u);
    if (g_settings.scale > 3) g_settings.scale = 0;
    g_settings.skip_logos = c["skip_logos"].value_or(false);
}
void Save(void* table) {
    static_cast<toml::table*>(table)->insert_or_assign("pc_video",
        toml::table{{"scale", int64_t(g_settings.scale)}, {"skip_logos", g_settings.skip_logos}});
}
void Draw(Settings& draft) {
    const char* names[] = {"Game original", "Preserve video aspect ratio", "Within a 4:3 frame", "Fill screen (crop)"};
    int scale = int(draft.scale);
    if (ImGui::Combo("Video framing", &scale, names, 4)) draft.scale = uint32_t(scale);
    ImGui::TextWrapped("Original preserves the player. Preserve aspect ratio shows the whole video; 4:3 also limits the frame. Fill crops the edges without stretching.");
    ImGui::TextWrapped("Applies only to VP6 videos. Camera, menus, HUD and 3D cutscenes are unchanged.");
    ImGui::Checkbox("Skip EA and Marvel logos", &draft.skip_logos);
    ImGui::TextWrapped("Uses normal player completion. Campaign videos and the Attract demo are not skipped.");
    if (ImGui::Button("Restore Default Videos")) draft = Settings{};
}
}  // namespace game::pc_video
