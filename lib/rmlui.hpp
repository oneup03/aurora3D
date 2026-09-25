#pragma once

#include "aurora/aurora.h"
#include "webgpu/gpu.hpp"

#include <SDL3/SDL_events.h>
#include <aurora/rmlui.hpp>
#include <dawn/webgpu_cpp.h>

namespace aurora::rmlui {

struct RecordedFrame {
  wgpu::BindGroup bindGroup;
  // Bind group for webgpu::g_UIOverlayPipeline (stereo-aware UI blit); only
  // meaningful when `overlay` is true.
  wgpu::BindGroup overlayBindGroup;
  bool overlay = false;
};

void initialize(const AuroraWindowSize& size) noexcept;
void handle_event(SDL_Event& event) noexcept;
RecordedFrame record_frame(const webgpu::Viewport& presentViewport) noexcept;
void shutdown() noexcept;

} // namespace aurora::rmlui
