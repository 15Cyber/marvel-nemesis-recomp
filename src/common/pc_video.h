#pragma once
#include <cstdint>
namespace game::pc_video {
struct Settings {
    uint32_t scale = 0;  // 0 original, 1 fit native aspect, 2 fit inside 4:3, 3 fill/crop
    bool skip_logos = false;
    bool operator==(const Settings&) const = default;
};
struct OutputSize { uint32_t width = 0, height = 0; };
OutputSize Output();  // actual client area, including a manually resized window
Settings Read();
void Set(const Settings&);
void Load(const void*);
void Save(void*);
void Draw(Settings&);
}  // namespace game::pc_video
