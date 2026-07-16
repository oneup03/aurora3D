#pragma once

#include <cstdint>

#include "gpu.hpp"

// LeiaSR (Simulated Reality) autostereoscopic weaving for Dawn's D3D12 backend.
//
// The weaver takes a side-by-side (SbS) stereo image and interleaves it for a
// Leia/Dimenco autostereoscopic display, with per-eye warping driven by face
// tracking. The flow per frame is:
//
//   1. Dawn renders LEFT into the left half of input_view() and RIGHT into the
//      right half (one render pass, viewport scissored per eye).
//   2. Dawn submits its command buffer.
//   3. weave() builds a native D3D12 command list, records the SR weave
//      commands (input -> output), and submits on Dawn's native command queue.
//   4. Dawn samples output_texture() in a copy pass to the swapchain.
//
// Same ID3D12Device + ID3D12CommandQueue as Dawn, so FIFO submission ordering
// is enough; no explicit fences needed.

namespace aurora::webgpu::leiasr {

// True if the SDK runtime is loaded, an SRContext was created, a Leia display
// is connected, and the weaver is alive. Lazily initialized on first call.
bool is_supported();

// Ensure the weaver and SbS/woven textures exist at the given size + format.
// Recreates them on size change. Returns false if anything failed.
bool ensure_ready(uint32_t width, uint32_t height, wgpu::TextureFormat format);

// Render target view for the SBS stereo input. The texture is sized
// 2*width x height so each eye renders at full resolution into its half (the
// alternative -- writing into a width x height texture -- squashes each eye
// to half resolution before the SR weaver can see it). Render LEFT into the
// left half, RIGHT into the right half. Valid only after ensure_ready returns
// true.
wgpu::TextureView input_view();

// Dimensions of the SBS input texture in pixels (2*width, height). Use these
// to set the viewport when rendering into input_view().
struct Extent {
  uint32_t width;
  uint32_t height;
};
Extent input_extent();

// Texture+sampler bundle for the woven output, suitable for sampling in the
// EFB-copy pass. Valid only after ensure_ready returns true.
const TextureWithSampler& output_texture();

// Build a native D3D12 command list, run the weaver (input -> output), and
// submit on Dawn's command queue. Must be called between the Dawn submit that
// wrote input_view() and the Dawn submit that samples output_texture().
void weave();

// Drop the weaver, SR resources, and shared textures. Called on swapchain
// resize (before recreating) and on shutdown.
void shutdown();

} // namespace aurora::webgpu::leiasr
