#pragma once

#include <cstddef>
#include <cstdint>

#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {

inline constexpr size_t InlineDrawPayloadSize = 128;

/// Generational handle: 0 is never valid, and IDs are not reused after unregister_draw_type.
using DrawTypeId = uint64_t;
inline constexpr DrawTypeId InvalidDrawType = 0;

struct Range {
  uint32_t offset = 0;
  uint32_t size = 0;

  bool operator==(const Range& rhs) const { return offset == rhs.offset && size == rhs.size; }
  bool operator!=(const Range& rhs) const { return !(*this == rhs); }
};

struct DrawContext {
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::TextureFormat colorFormat;
  wgpu::TextureFormat depthFormat;
  uint32_t sampleCount = 1;
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;
};

/// Invoked on the render worker thread while replaying the pass the draw was
/// recorded into. The encoder's pipeline/bind-group/viewport/scissor state is
/// restored after the callback returns. Handles in the context are borrowed and
/// valid only for the duration of the call. sampleCount/target dimensions are
/// those of the containing pass (offscreen passes are always single-sample).
using DrawCallback = void (*)(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass,
                              const void* payload, size_t payloadSize, void* userdata);

struct DrawTypeDescriptor {
  const char* label = nullptr;
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
};

wgpu::Device device() noexcept;
wgpu::Queue queue() noexcept;
wgpu::TextureFormat color_format() noexcept;
wgpu::TextureFormat depth_format() noexcept;
uint32_t sample_count() noexcept;
bool uses_reversed_z() noexcept;

/// Depth snapshot queries. The renderer keeps a rate-limited (30Hz),
/// triple-buffered CPU-side copy of the frame's final depth attachment,
/// downscaled to the logical GC framebuffer grid, with values normalized to GX
/// Z24: 0 at the near plane, 0x00FFFFFF at the far plane, INCREASING WITH
/// DISTANCE in both the forward and reversed-Z configurations. Because the
/// capture is taken on the last render pass of the frame, and 2D/J2D drawing
/// runs with depth writes disabled, the grid holds the 3D scene only.
///
/// Arms a capture. Cheap and idempotent; the rate limiter decides when one is
/// actually taken, so callers that want a steady stream just call it per frame.
void request_depth_snapshot() noexcept;
/// Counter bumped once per landed snapshot; 0 until the first one arrives.
/// Compare against the last value you processed to skip redundant reductions.
uint64_t depth_snapshot_generation() noexcept;
/// Reduces a normalized [0,1] region of the latest snapshot to cols*rows block
/// minima (the nearest Z24 in each block) in one pass. `out` needs cols*rows
/// entries. Returns the generation sampled, or 0 if no snapshot exists yet.
uint64_t sample_depth_snapshot_min(float x0, float y0, float x1, float y1, uint32_t cols, uint32_t rows,
                                   uint32_t* out) noexcept;

DrawTypeId register_draw_type(const DrawTypeDescriptor& desc);
void unregister_draw_type(DrawTypeId type) noexcept;
/// Records an inline custom draw into the currently open render pass at the
/// current position in the command stream. Payload (<= InlineDrawPayloadSize)
/// is copied. Returns false (with a warning) outside an active render pass.
bool push_custom_draw(DrawTypeId type, const void* payload, size_t payloadSize);

/// Generational handle: 0 is never valid, and IDs are not reused after unregister_encoder_task_type.
using EncoderTaskId = uint64_t;
inline constexpr EncoderTaskId InvalidEncoderTask = 0;

struct EncoderTaskContext {
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer storageBuffer;
};

/// Invoked on the render worker thread with the frame's command encoder,
/// positioned between two render passes. The callback may begin/end compute
/// passes and record copies on the encoder; it must leave no pass open when it
/// returns and must not Finish the encoder. Handles in the context are borrowed
/// and valid only for the duration of the call. Data appended to the streaming
/// buffers before the task was pushed is GPU-visible inside it.
using EncoderTaskCallback = void (*)(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
                                     const void* payload, size_t payloadSize, void* userdata);

struct EncoderTaskDescriptor {
  const char* label = nullptr;
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
};

EncoderTaskId register_encoder_task_type(const EncoderTaskDescriptor& desc);
void unregister_encoder_task_type(EncoderTaskId type) noexcept;
/// Seals the current EFB pass, records an encoder task to execute on the frame
/// encoder at this point, and resumes rendering on a pass that loads the
/// existing contents. Payload semantics match push_custom_draw. Returns false
/// (with a warning) outside an active render pass or while an offscreen pass
/// is open.
bool push_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize);

/// Append transient data to the shared per-frame streaming buffers. Returned
/// ranges are valid for the current frame only. Returns an empty Range (with a
/// warning) outside an active recording frame.
Range push_verts(const uint8_t* data, size_t length, size_t alignment);
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
Range push_uniform(const uint8_t* data, size_t length);
Range push_storage(const uint8_t* data, size_t length);

struct ResolveDesc {
  bool color = true;
  bool depth = false;
};

struct ResolvedTargets {
  wgpu::TextureView color;  // single-sample snapshot; null if not requested
  wgpu::TextureView depth;  // single-sample R32Float depth snapshot; null if not requested
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// Snapshots the current pass targets into pooled textures (valid for the
/// current frame), then: on the EFB, continues rendering on a fresh EFB pass
/// (GXCopyTex semantics); in an offscreen pass created by create_pass, ends it
/// and restores the suspended EFB pass (GXRestoreFrameBuffer semantics).
/// Requesting neither color nor depth is a plain pass break (or offscreen
/// close, discarding its output). Depth is left null when unsupported by the
/// device. Returns false (with a warning) outside an active render pass.
bool resolve_pass(const ResolveDesc& desc, ResolvedTargets& out);

/// Opens an offscreen render pass (GXCreateFrameBuffer semantics): cleared
/// single-sample color+depth at (width, height) with full-target
/// viewport/scissor. Subsequent draws target it until resolve_pass restores the
/// EFB. Nesting is unsupported: returns false (with a warning) outside an
/// active render pass or while any offscreen pass is already open.
bool create_pass(uint32_t width, uint32_t height);

/// True while an offscreen pass (create_pass or GXCreateFrameBuffer) is open.
bool is_offscreen() noexcept;

/// Blocks until the render worker has drained its queue. After this returns,
/// no draw callback is executing or queued to execute; used before unloading
/// code that registered draw types. Callable from the game thread only.
void synchronize();

} // namespace aurora::gfx
