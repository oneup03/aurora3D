#include <aurora/aurora.h>

#ifdef AURORA_ENABLE_GX
#include "gfx/common.hpp"
#include "gfx/render_worker.hpp"
#include "gx/fifo.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/gpu_prof.hpp"
#ifdef AURORA_ENABLE_LEIASR
#  include "webgpu/leiasr.hpp"
#endif
#include <webgpu/webgpu_cpp.h>
#endif

#ifdef AURORA_ENABLE_RMLUI
#include "rmlui.hpp"
#endif

#include "input.hpp"
#include "internal.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <magic_enum.hpp>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
Module Log("aurora");

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_queue;
using webgpu::g_surface;

uint32_t clamp_scissor_coord(double value, uint32_t maximum) noexcept {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(maximum)));
}

void set_present_viewport(const wgpu::RenderPassEncoder& pass, const gfx::Viewport& viewport, uint32_t surfaceWidth,
                          uint32_t surfaceHeight) noexcept {
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const auto scissorX = clamp_scissor_coord(std::floor(viewport.left), surfaceWidth);
  const auto scissorY = clamp_scissor_coord(std::floor(viewport.top), surfaceHeight);
  const auto scissorRight = clamp_scissor_coord(std::ceil(viewport.left + viewport.width), surfaceWidth);
  const auto scissorBottom = clamp_scissor_coord(std::ceil(viewport.top + viewport.height), surfaceHeight);
  pass.SetScissorRect(scissorX, scissorY, scissorRight - scissorX, scissorBottom - scissorY);
}
#endif

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D11
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
#ifdef DAWN_ENABLE_BACKEND_OPENGLES
    BACKEND_OPENGLES,
#endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  AuroraBackend selectedBackend = config.desiredBackend;
  bool windowCreated = false;
  if (selectedBackend != BACKEND_AUTO && window::create_window(selectedBackend)) {
    if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
      windowCreated = true;
    } else {
      window::destroy_window();
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        continue;
      }
      if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
        windowCreated = true;
        break;
      } else {
        window::destroy_window();
      }
    }
  }

  ASSERT(windowCreated, "Error creating window: {}", SDL_GetError());

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();

#ifdef AURORA_ENABLE_GX
  gfx::initialize();

  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize(size);
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

void shutdown() noexcept {
#ifdef AURORA_ENABLE_GX
  gfx::render_worker::synchronize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown();
#endif
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  input::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
  }
  return window::poll_events();
}

bool begin_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  {
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }
    if (window::is_paused()) {
      return false;
    }
    if (!g_surface) {
      webgpu::refresh_surface(true);
      if (!g_surface) {
        return false;
      }
    }
  }

  // Always start each frame on the left eye so set_efb_targets picks the left
  // EFB for pass 0. Stereo painters call aurora_set_active_eye later to swap.
  webgpu::g_activeEye = AURORA_EYE_LEFT;

  imgui::new_frame(window::get_window_size());
  if (!gfx::begin_frame()) {
    return false;
  }
#endif
  return true;
}

void end_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  gx::fifo::drain();
  gfx::finish();
  auto imguiDrawData = imgui::freeze();

  const auto& presentSource = webgpu::present_source();
  const auto viewport = webgpu::calculate_present_viewport(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                                           webgpu::g_graphicsConfig.surfaceConfiguration.height,
                                                           presentSource.size.width, presentSource.size.height);

  wgpu::BindGroup rmlBindGroup;
  wgpu::BindGroup rmlOverlayBindGroup;
  bool rmlOverlay = false;
#if AURORA_ENABLE_RMLUI
  if (rmlui::is_initialized()) {
    auto rmlFrame = rmlui::record_frame(viewport);
    rmlBindGroup = std::move(rmlFrame.bindGroup);
    rmlOverlayBindGroup = std::move(rmlFrame.overlayBindGroup);
    rmlOverlay = rmlFrame.overlay;
  }
#endif

  gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlayBindGroup = std::move(rmlOverlayBindGroup),
                  rmlOverlay, viewport,
                  imguiDrawData = std::move(imguiDrawData)](wgpu::CommandEncoder& encoder) {
    wgpu::Texture currentTexture;
    wgpu::TextureView currentView;
    auto surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::Error;
    {
      window::SurfaceLock surfaceLock;
      if (window::is_presentable() && g_surface) {
        ZoneScopedN("Acquire texture");
        wgpu::SurfaceTexture surfaceTexture;
        g_surface.GetCurrentTexture(&surfaceTexture);
        surfaceStatus = surfaceTexture.status;
        if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal) {
          currentTexture = std::move(surfaceTexture.texture);
          currentView = currentTexture.CreateView();
        }
      }
    }

    const bool canPresent = currentTexture && currentView;
    if (canPresent) {
      const bool stereo_active = webgpu::g_stereoCfg.mode != AURORA_STEREO_OFF;
      {
        // Compose parameters for the stereo-aware XFB copy shader. Mode 0
        // (OFF) degenerates to a plain left-eye blit.
        struct StereoUboData {
          uint32_t mode;
          float w;
          float h;
          float hudDepth;
        } stereoUboData{
            .mode = static_cast<uint32_t>(webgpu::g_stereoCfg.mode),
            .w = viewport.width,
            .h = viewport.height,
            .hudDepth = webgpu::g_stereoCfg.hudDepth,
        };
        g_queue.WriteBuffer(webgpu::g_StereoUbo, 0, &stereoUboData, sizeof(stereoUboData));
      }
      wgpu::BindGroup presentBindGroup;
      if (rmlBindGroup && !rmlOverlay) {
        // Mono backdrop-filter path: RmlUi composited the scene into its own
        // target. record_frame forces the overlay path whenever stereo is on.
        presentBindGroup = rmlBindGroup;
      } else if (!stereo_active) {
        const auto& resampledSource = webgpu::resample_present_source(encoder, viewport);
        presentBindGroup = webgpu::create_copy_bind_group(resampledSource);
      } else {
        // Resample each eye's EFB to the present viewport size. The stereo
        // compose shader then samples both resampled textures and produces
        // SbS / TaB / interlaced / checkerboard / anaglyph.
        const auto& leftResampled = webgpu::resample_present_source_for(encoder, viewport, AURORA_EYE_LEFT);
        const auto& rightResampled = webgpu::resample_present_source_for(encoder, viewport, AURORA_EYE_RIGHT);
        presentBindGroup = webgpu::create_copy_bind_group_stereo(leftResampled, rightResampled);

#ifdef AURORA_ENABLE_LEIASR
        // LeiaSR autostereoscopic path: render SBS into a shared D3D12 texture,
        // run the native SR weaver, then sample the woven result in the EFB-copy
        // pass below. The flow needs an early Dawn submit so the weaver's native
        // command list (executed on the same queue) sees the SBS write.
        const bool leiasr_active = webgpu::g_stereoCfg.mode == AURORA_STEREO_LEIASR &&
                                   webgpu::leiasr::is_supported() &&
                                   webgpu::leiasr::ensure_ready(static_cast<uint32_t>(viewport.width),
                                                                static_cast<uint32_t>(viewport.height),
                                                                webgpu::g_graphicsConfig.surfaceConfiguration.format);
        if (leiasr_active) {
          // Force SBS mode in the StereoUbo just for the input-render pass so the
          // existing compose shader produces a side-by-side image.
          struct StereoUboData {
            uint32_t mode;
            float w;
            float h;
            float hudDepth;
          } sbsUbo{static_cast<uint32_t>(AURORA_STEREO_SBS), viewport.width, viewport.height, 0.0f};
          g_queue.WriteBuffer(webgpu::g_StereoUbo, 0, &sbsUbo, sizeof(sbsUbo));

          wgpu::TextureView sbsView = webgpu::leiasr::input_view();
          if (sbsView) {
            const auto sbsExtent = webgpu::leiasr::input_extent();
            const std::array sbsAttachments{
                wgpu::RenderPassColorAttachment{
                    .view = sbsView,
                    .loadOp = wgpu::LoadOp::Clear,
                    .storeOp = wgpu::StoreOp::Store,
                },
            };
            const wgpu::RenderPassDescriptor sbsPassDesc{
                .label = "LeiaSR SBS compose pass",
                .colorAttachmentCount = sbsAttachments.size(),
                .colorAttachments = sbsAttachments.data(),
            };
            const auto sbsPass = encoder.BeginRenderPass(&sbsPassDesc);
            sbsPass.SetPipeline(webgpu::g_CopyPipeline);
            sbsPass.SetBindGroup(0, presentBindGroup, 0, nullptr);
            // Viewport spans the full SBS texture (2x viewport.width) so the
            // existing SBS shader writes each eye at its full resolution into
            // its half of the input.
            sbsPass.SetViewport(0.f, 0.f, static_cast<float>(sbsExtent.width),
                                static_cast<float>(sbsExtent.height), 0.f, 1.f);
            sbsPass.Draw(3);
            sbsPass.End();

            // Flush this encoder so the SBS write is visible to the native weaver.
            const wgpu::CommandBufferDescriptor preDesc{.label = "LeiaSR pre-weave buffer"};
            const auto preBuffer = encoder.Finish(&preDesc);
            g_queue.Submit(1, &preBuffer);

            webgpu::leiasr::weave();

            // Restart a fresh encoder for the woven-output -> swapchain copy, UI
            // overlay, and ImGui passes.
            const wgpu::CommandEncoderDescriptor postEncDesc{.label = "LeiaSR post-weave encoder"};
            encoder = g_device.CreateCommandEncoder(&postEncDesc);

            // Reset the StereoUbo to mode 0 (OFF) so the EFB-copy below samples
            // the woven texture cleanly through the case 0 branch (and the UI
            // overlay lands as a plain screen-depth blit).
            struct StereoUboData monoUbo{0u, viewport.width, viewport.height, 0.0f};
            g_queue.WriteBuffer(webgpu::g_StereoUbo, 0, &monoUbo, sizeof(monoUbo));

            // Replace presentBindGroup with one bound to the woven output.
            presentBindGroup = webgpu::create_copy_bind_group(webgpu::leiasr::output_texture());
          } else {
            // Couldn't acquire the SBS input texture this frame; fall back to a
            // plain mono compose so the user still sees something.
            struct StereoUboData monoUbo{0u, viewport.width, viewport.height, 0.0f};
            g_queue.WriteBuffer(webgpu::g_StereoUbo, 0, &monoUbo, sizeof(monoUbo));
          }
        }
#endif
      }
      {
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = currentView,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "EFB copy render pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
            .timestampWrites = webgpu::gpu_prof::pass_writes("Present blit"),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        // Copy EFB -> XFB (swapchain)
        pass.SetPipeline(webgpu::g_CopyPipeline);
        pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
        set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                             webgpu::g_graphicsConfig.surfaceConfiguration.height);

        pass.Draw(3);
        if (rmlOverlayBindGroup && rmlOverlay) {
          // Alpha-blend the UI-only RmlUi target over the just-composited
          // image. g_UIOverlayPipeline remaps UVs for SbS / TaB so the menu
          // lands in each eye's half; in mono (and over the LeiaSR woven
          // output, where the UBO mode was reset to 0) it is a plain
          // premultiplied screen-depth blit.
          pass.SetPipeline(webgpu::g_UIOverlayPipeline);
          pass.SetBindGroup(0, rmlOverlayBindGroup, 0, nullptr);
          pass.Draw(3);
        }
        pass.End();
      }
      {
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = currentView,
                .loadOp = wgpu::LoadOp::Load,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "ImGui render pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
            .timestampWrites = webgpu::gpu_prof::pass_writes("ImGui"),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
        imgui::render(pass, imguiDrawData);
        pass.End();
      }
    } else {
      Log.info("Skipping present; window not presentable");
    }
    webgpu::gpu_prof::frame_end(encoder);
    const wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "Redraw command buffer"};
    const auto buffer = encoder.Finish(&cmdBufDescriptor);
    {
      ZoneScopedN("Queue Submit");
      g_queue.Submit(1, &buffer);
    }
    webgpu::gpu_prof::after_submit();
    if (canPresent && g_surface) {
      ZoneScopedN("Present");
      wgpu::ConvertibleStatus status = wgpu::Status::Error;
      {
        window::SurfaceLock surfaceLock;
        if (window::is_presentable()) {
          status = g_surface.Present();
        }
      }
      if (status) {
        gfx::after_present();
      } else {
        Log.warn("Surface present failed");
        webgpu::release_surface();
      }
    } else if (g_surface) {
      switch (surfaceStatus) {
      case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
        Log.warn("Surface texture acquisition timed out");
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
      case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
        Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceStatus));
        window::push_custom_event(window::CustomEvent::RefreshSurface);
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Lost:
        Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
        webgpu::release_surface();
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Error:
        Log.warn("Surface texture is {}, dropping surface", magic_enum::enum_name(surfaceStatus));
        g_surface = {};
        break;
      default:
        if (!window::is_presentable()) {
          webgpu::release_surface();
        } else {
          Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceStatus));
        }
        break;
      }
    }
    gfx::after_submit();

    TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

    TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(gfx::g_stats.queuedPipelines));
    TracyPlot("aurora: createdPipelines", static_cast<int64_t>(gfx::g_stats.createdPipelines));
    TracyPlot("aurora: drawCallCount", static_cast<int64_t>(gfx::g_stats.drawCallCount));
    TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(gfx::g_stats.mergedDrawCallCount));
    TracyPlot("aurora: lastVertSize", static_cast<int64_t>(gfx::g_stats.lastVertSize));
    TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(gfx::g_stats.lastUniformSize));
    TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(gfx::g_stats.lastIndexSize));
    TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(gfx::g_stats.lastStorageSize));
    TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(gfx::g_stats.lastTextureUploadSize));
  });

#endif
}
} // namespace
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_end_frame() { aurora::end_frame(); }
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_resampler(AuroraSampler sampler) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_resampler(sampler);
#else
  (void)sampler;
#endif
}

void aurora_set_stereo_config(const AuroraStereoConfig* cfg) {
#ifdef AURORA_ENABLE_GX
  if (cfg == nullptr) {
    return;
  }
  aurora::webgpu::g_stereoCfg = *cfg;
  if (!aurora_stereo_mode_supported(aurora::webgpu::g_stereoCfg.mode)) {
    aurora::webgpu::g_stereoCfg.mode = AURORA_STEREO_OFF;
  }
#endif
}

void aurora_set_active_eye(AuroraEye eye) {
#ifdef AURORA_ENABLE_GX
  if (aurora::webgpu::g_activeEye == eye) {
    return;
  }
  // Flush whatever the current eye has accumulated into its render pass,
  // then start a fresh EFB pass tagged to the new eye.
  aurora::gx::fifo::drain();
  aurora::webgpu::g_activeEye = eye;
  aurora::gfx::begin_new_efb_pass_for_active_eye();
#else
  (void)eye;
#endif
}

AuroraEye aurora_get_active_eye() {
#ifdef AURORA_ENABLE_GX
  return aurora::webgpu::g_activeEye;
#else
  return AURORA_EYE_LEFT;
#endif
}

bool aurora_stereo_mode_supported(AuroraStereoMode mode) {
  switch (mode) {
  case AURORA_STEREO_OFF:
  case AURORA_STEREO_SBS:
  case AURORA_STEREO_TAB:
  case AURORA_STEREO_ROW_INTERLACED:
  case AURORA_STEREO_COL_INTERLACED:
  case AURORA_STEREO_CHECKERBOARD:
  case AURORA_STEREO_ANAGLYPH:
    return true;
  case AURORA_STEREO_LEIASR:
#if defined(AURORA_ENABLE_LEIASR) && defined(AURORA_ENABLE_GX)
    return aurora::webgpu::leiasr::is_supported();
#else
    return false;
#endif
  default:
    return false;
  }
}
