#include "app/app.h"
#include "common/settings_panel.h"
#include "common/pc_controls.h"
#include "common/pc_video.h"
#include "common/pc_render.h"
#include "common/desktop_menu.h"

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Marvel Nemesis: Rise of the Imperfects", // window and splash
    .short_title = L"Marvel Nemesis",               // message boxes
    .name = L"MarvelNemesis",  // window classes, crash dumps, handover event
    .log = "marvelnemesis",  // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "MarvelNemesis"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"fps_counter", &game::settings_panel::FpsCounter(), nullptr}},  // game settings kept in the settings file
    .load = [](const void* table) {
        game::pc_controls::Load(table);
        game::pc_video::Load(table);
        game::pc_render::Load(table);
    },
    .save = [](void* table) {
        game::pc_controls::Save(table);
        game::pc_video::Save(table);
        game::pc_render::Save(table);
    },
    .init = game::settings_panel::Initialize,  // after the settings load, before the window and boot
    .loaded = nullptr,  // image in memory, before its entry point runs
    .window = game::settings_panel::WindowReady,  // the game window is up
    .message = [](void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {  // window messages first; true takes one
        return game::desktop_menu::Message(hwnd, msg, wp, lp, result) ||
               game::settings_panel::Message(hwnd, msg, wp, lp, result);
    },
    .ui = game::settings_panel::Draw,  // ImGui drawing each frame
    .pad = game::settings_panel::FilterPad,  // changes a polled pad
    .rumble = game::settings_panel::FilterRumble,  // changes a rumble before it is set
    .controller_port = game::pc_controls::ControllerPort,
    .ui_input = game::settings_panel::PrepareInput,
    .supported_xbe_sha1 = "66481feb81d9a00a63f0d2f0a203a297b712cfb5",
    .data_directory = "saves",
    .mods_directory = "mods",
    .expand_widescreen = true,
};
