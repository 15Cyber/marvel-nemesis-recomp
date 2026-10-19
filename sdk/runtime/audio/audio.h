#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);
// Output thread stops its voice and mixer pulls; queued samples and per-voice pause states survive.
void SetPaused(bool paused);

}  // namespace audio
