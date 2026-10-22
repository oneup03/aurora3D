#pragma once

#include <cstdint>

#include "gpu.hpp"

// Steam Frame stereo output (Linux, Dawn's Vulkan backend) through Frame-3D-Lib.
//
// The library shows a full side-by-side image on a SteamVR overlay of our own, kept
// over the game's Steam screen as the user moves, resizes or curves it; see
// extern/Frame-3D-Lib. Its images are dma-bufs, imported here into Dawn as
// SharedTextureMemory. The flow per frame, on the render worker:
//
//   1. ensure_ready() creates the library context for this eye size and format.
//   2. begin_frame() acquires a free image and begins Dawn access. Render LEFT into
//      the left half and RIGHT into the right half of the returned view.
//   3. Submit those commands, then end_frame() ends access and hands the image and
//      its sync-fd fence to the library, which shows it once the GPU is done.
//
// The window itself only needs a cleared frame while this runs: SteamVR draws our
// overlay over the Steam screen that shows it.

namespace aurora::webgpu::steam_frame {

// Cheap, thread-safe, side-effect free: SteamVR's runtime library is loadable. For
// the settings UI and aurora_stereo_mode_supported; the device's dma-buf support is
// checked by ensure_ready() on the render thread.
bool is_runtime_available();

// Render thread. Recreates the context when the size or format changes; false while
// the output can't run (no SteamVR, unsupported device, failed import), retrying
// every few seconds.
bool ensure_ready(uint32_t eyeWidth, uint32_t eyeHeight, wgpu::TextureFormat format);

// Render thread, after ensure_ready(). A (2 * eyeWidth) x eyeHeight render target, or
// null when every image is still in use (skip the overlay this frame).
wgpu::TextureView begin_frame();

// Render thread, after the Dawn submit that wrote begin_frame()'s view.
void end_frame();

// Drops the context, which removes the overlay. Called when the stereo mode
// changes away and on GPU shutdown; cheap when there is no context.
void shutdown();

} // namespace aurora::webgpu::steam_frame
