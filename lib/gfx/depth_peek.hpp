#pragma once

#include "common.hpp"

#include <cstdint>
#include <vector>

namespace aurora::gfx::depth_peek {

void initialize();
void shutdown();

void request_snapshot() noexcept;
bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept;

// Monotonically increasing counter, bumped once each time a new snapshot
// finishes mapping. 0 means nothing has landed yet. Consumers that reduce the
// whole grid can compare against the generation they last processed and skip
// the work entirely on frames where the data is unchanged -- snapshots land at
// DepthPeekSnapshotHz (30), so at 60fps that is every other frame at best.
uint64_t latest_generation() noexcept;

// Reduces a rectangular region of the latest snapshot to `cols * rows` block
// minima -- the NEAREST Z24 within each block -- in a single pass under one
// lock. The region is given in normalized [0,1] snapshot coordinates; it is
// clamped to the grid and each block is guaranteed at least one source texel.
// `out` must hold cols*rows entries. Returns the generation actually sampled,
// or 0 if no snapshot is available (in which case `out` is untouched).
//
// Block-minimum rather than point-sampling because the intended consumer wants
// "the nearest thing that covers pixels": a min preserves near geometry that a
// sparse point grid would miss, while a low percentile taken across the blocks
// still rejects the handful of blocks a single stray texel can contaminate.
uint64_t reduce_min_blocks(float x0, float y0, float x1, float y1, uint32_t cols, uint32_t rows,
                           uint32_t* out) noexcept;

void encode_frame_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& depthView,
                           wgpu::Extent3D sourceSize, uint32_t msaaSamples) noexcept;
void after_submit() noexcept;

namespace testing {
void reset() noexcept;
bool snapshot_requested() noexcept;
void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data);
} // namespace testing

} // namespace aurora::gfx::depth_peek
