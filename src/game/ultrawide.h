#pragma once
#include "xdk/d3d8.h"

namespace game::ultrawide {

class SceneScope {
    d3d::FullWidthScope canvas_;
    uint32_t previous_camera_;
public:
    explicit SceneScope(uint32_t camera);
    ~SceneScope();
    SceneScope(const SceneScope&) = delete;
    SceneScope& operator=(const SceneScope&) = delete;
};

}  // namespace game::ultrawide
