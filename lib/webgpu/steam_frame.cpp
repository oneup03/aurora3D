#include "steam_frame.hpp"

#include "../logging.hpp"

#include <frame3d/frame3d.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::webgpu::steam_frame {
namespace {
Module Log("aurora::steam_frame");

using Clock = std::chrono::steady_clock;

constexpr int32_t kVkImageLayoutUndefined = 0; // VK_IMAGE_LAYOUT_UNDEFINED
constexpr auto kRetryDelay = std::chrono::seconds(5);

struct Image {
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureView view;
};

frame3d_context* g_ctx = nullptr;
std::vector<Image> g_images;
uint32_t g_eyeWidth = 0;
uint32_t g_eyeHeight = 0;
wgpu::TextureFormat g_format = wgpu::TextureFormat::Undefined;
int g_current = -1;
Clock::time_point g_retryAt{};
// Set once Dawn couldn't import the driver-chosen layout (e.g. UBWC); LINEAR from then on.
bool g_linearOnly = false;
bool g_unsupported = false; // latched: this device can't import dma-bufs at all
bool g_warnedNoFence = false;

void log_from_library(frame3d_log_level level, const char* message, void*) {
  switch (level) {
  case FRAME3D_LOG_DEBUG:
    Log.debug("{}", message);
    break;
  case FRAME3D_LOG_INFO:
    Log.info("{}", message);
    break;
  case FRAME3D_LOG_WARN:
    Log.warn("{}", message);
    break;
  default:
    Log.error("{}", message);
    break;
  }
}

uint32_t drm_format_for(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return FRAME3D_FORMAT_RGBA8;
  case wgpu::TextureFormat::BGRA8Unorm:
    return FRAME3D_FORMAT_BGRA8;
  default:
    return 0;
  }
}

// Runs fn inside validation / out-of-memory / internal error scopes and returns the
// first error, or empty. The uncaptured-error handler is fatal once the GPU is
// initialized, and a rejected dma-buf must cost us the mode, not the game.
template <typename Fn>
std::string capture_errors(Fn&& fn) {
  g_device.PushErrorScope(wgpu::ErrorFilter::Internal);
  g_device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
  g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
  fn();
  std::string error;
  for (int i = 0; i < 3; ++i) {
    const auto future = g_device.PopErrorScope(
        wgpu::CallbackMode::WaitAnyOnly,
        [&error](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView message) {
          if (status == wgpu::PopErrorScopeStatus::Success && type != wgpu::ErrorType::NoError && error.empty()) {
            error = std::string(std::string_view(message));
          }
        });
    g_instance.WaitAny(future, 5000000000);
  }
  return error;
}

void destroy() {
  g_images.clear();
  if (g_ctx != nullptr) {
    frame3d_destroy(g_ctx);
    g_ctx = nullptr;
    Log.info("Steam Frame output stopped");
  }
  g_current = -1;
  g_eyeWidth = g_eyeHeight = 0;
  g_format = wgpu::TextureFormat::Undefined;
}

enum class CreateResult { Ok, NoContext, ImportFailed };

CreateResult create(uint32_t eyeWidth, uint32_t eyeHeight, wgpu::TextureFormat format, uint32_t drmFormat,
                    bool linear) {
  frame3d_config cfg;
  frame3d_config_init(&cfg);
  cfg.eye_width = eyeWidth;
  cfg.eye_height = eyeHeight;
  cfg.drm_format = drmFormat;
  cfg.prefer_linear = linear;
  cfg.log = log_from_library;
  g_ctx = frame3d_create(&cfg);
  if (g_ctx == nullptr) {
    return CreateResult::NoContext;
  }

  const uint32_t count = frame3d_buffer_count(g_ctx);
  g_images.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    frame3d_buffer_info info{};
    frame3d_get_buffer_info(g_ctx, i, &info);
    // Dawn hands the fd to Vulkan on a successful import, and takes one fd for all
    // planes; the library keeps its own, so give Dawn a duplicate.
    const int fd = dup(info.planes[0].fd);
    if (fd < 0) {
      Log.warn("Steam Frame: dup of an image fd failed");
      destroy();
      return CreateResult::ImportFailed;
    }
    std::array<wgpu::SharedTextureMemoryDmaBufPlane, FRAME3D_MAX_PLANES> planes{};
    for (uint32_t p = 0; p < info.plane_count; ++p) {
      planes[p] = {.fd = fd, .offset = info.planes[p].offset, .stride = info.planes[p].stride};
    }
    wgpu::SharedTextureMemoryDmaBufDescriptor dmaBufDesc{};
    dmaBufDesc.size = {info.width, info.height, 1};
    dmaBufDesc.drmFormat = info.drm_format;
    dmaBufDesc.drmModifier = info.drm_modifier;
    dmaBufDesc.planeCount = info.plane_count;
    dmaBufDesc.planes = planes.data();
    wgpu::SharedTextureMemoryDescriptor memDesc{};
    memDesc.nextInChain = &dmaBufDesc;
    memDesc.label = "Steam Frame image";

    Image& img = g_images[i];
    std::string error = capture_errors([&] { img.memory = g_device.ImportSharedTextureMemory(&memDesc); });
    if (!error.empty() || !img.memory) {
      close(fd); // not imported, so still ours
      Log.warn("Steam Frame: Dawn couldn't import a {}x{} image with layout {:#x}: {}", info.width, info.height,
               info.drm_modifier, error);
      destroy();
      return CreateResult::ImportFailed;
    }
    const wgpu::TextureDescriptor texDesc{
        .label = "Steam Frame image",
        .usage = wgpu::TextureUsage::RenderAttachment,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {info.width, info.height, 1},
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    error = capture_errors([&] { img.texture = img.memory.CreateTexture(&texDesc); });
    if (!error.empty() || !img.texture) {
      Log.warn("Steam Frame: creating a texture on an imported image failed: {}", error);
      destroy();
      return CreateResult::ImportFailed;
    }
    img.view = img.texture.CreateView();
  }
  g_eyeWidth = eyeWidth;
  g_eyeHeight = eyeHeight;
  g_format = format;
  Log.info("Steam Frame output ready: {}x{} per eye", eyeWidth, eyeHeight);
  return CreateResult::Ok;
}

} // namespace

bool is_runtime_available() { return frame3d_runtime_available(); }

bool ensure_ready(uint32_t eyeWidth, uint32_t eyeHeight, wgpu::TextureFormat format) {
  if (g_ctx != nullptr && eyeWidth == g_eyeWidth && eyeHeight == g_eyeHeight && format == g_format) {
    return true;
  }
  destroy();
  if (g_unsupported || eyeWidth == 0 || eyeHeight == 0 || Clock::now() < g_retryAt) {
    return false;
  }
  if (!g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryDmaBuf) ||
      !g_device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD)) {
    Log.warn("Steam Frame output unavailable: the GPU device can't import dma-bufs (needs "
             "SharedTextureMemoryDmaBuf and SharedFenceSyncFD)");
    g_unsupported = true;
    return false;
  }
  const uint32_t drmFormat = drm_format_for(format);
  if (drmFormat == 0) {
    Log.warn("Steam Frame output unavailable: unsupported surface format {}", static_cast<uint32_t>(format));
    g_unsupported = true;
    return false;
  }

  CreateResult result = create(eyeWidth, eyeHeight, format, drmFormat, g_linearOnly);
  if (result == CreateResult::ImportFailed && !g_linearOnly) {
    Log.info("Steam Frame: retrying with a LINEAR layout");
    g_linearOnly = true;
    result = create(eyeWidth, eyeHeight, format, drmFormat, true);
  }
  if (result != CreateResult::Ok) {
    g_retryAt = Clock::now() + kRetryDelay;
    return false;
  }
  return true;
}

wgpu::TextureView begin_frame() {
  if (g_ctx == nullptr || g_current >= 0) {
    return {};
  }
  const int index = frame3d_acquire(g_ctx);
  if (index < 0) {
    return {};
  }
  Image& img = g_images[static_cast<size_t>(index)];
  // Each frame overwrites the whole image, so its previous contents and layout don't
  // matter: begin from UNDEFINED, uninitialized.
  wgpu::SharedTextureMemoryVkImageLayoutBeginState layout{};
  layout.oldLayout = kVkImageLayoutUndefined;
  layout.newLayout = kVkImageLayoutUndefined;
  wgpu::SharedTextureMemoryBeginAccessDescriptor desc{};
  desc.nextInChain = &layout;
  desc.concurrentRead = false;
  desc.initialized = false;
  desc.fenceCount = 0;
  if (img.memory.BeginAccess(img.texture, &desc) != wgpu::Status::Success) {
    frame3d_release(g_ctx, index);
    return {};
  }
  g_current = index;
  return img.view;
}

void end_frame() {
  if (g_ctx == nullptr || g_current < 0) {
    return;
  }
  Image& img = g_images[static_cast<size_t>(g_current)];
  wgpu::SharedTextureMemoryVkImageLayoutEndState layout{};
  wgpu::SharedTextureMemoryEndAccessState state{};
  state.nextInChain = &layout;
  img.memory.EndAccess(img.texture, &state);

  // The sync fd belongs to the SharedFence; the library takes ownership of what it
  // gets, so hand it a duplicate.
  int fence = -1;
  for (size_t i = 0; i < state.fenceCount; ++i) {
    wgpu::SharedFenceSyncFDExportInfo syncFd{};
    wgpu::SharedFenceExportInfo info{};
    info.nextInChain = &syncFd;
    state.fences[i].ExportInfo(&info);
    if (info.type == wgpu::SharedFenceType::SyncFD && syncFd.handle >= 0) {
      if (fence >= 0) {
        close(fence);
      }
      fence = dup(syncFd.handle);
    }
  }
  if (fence < 0 && !g_warnedNoFence) {
    Log.warn("Steam Frame: no sync fd from EndAccess; showing frames without waiting for the GPU");
    g_warnedNoFence = true;
  }
  frame3d_submit(g_ctx, g_current, fence);
  g_current = -1;
}

void shutdown() { destroy(); }

} // namespace aurora::webgpu::steam_frame
