#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  SAMPLER_BILINEAR,
  SAMPLER_AREA,
} AuroraSampler;

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

#define MEM1_DEFAULT_SIZE (24 * 1024 * 1024)
#define ARAM_DEFAULT_SIZE (16 * 1024 * 1024)

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  bool vsync;
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureDumps;
  bool allowCpuAdapter;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;
} AuroraConfig;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_resampler(AuroraSampler sampler);

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);

typedef enum {
  AURORA_STEREO_OFF = 0,
  AURORA_STEREO_SBS,
  AURORA_STEREO_TAB,
  AURORA_STEREO_ROW_INTERLACED,
  AURORA_STEREO_COL_INTERLACED,
  AURORA_STEREO_CHECKERBOARD,
  AURORA_STEREO_ANAGLYPH,
  AURORA_STEREO_LEIASR,
} AuroraStereoMode;

typedef struct {
  AuroraStereoMode mode;
  float eyeSeparation;
  float convergence;
  float hudDepth;
  // Multiplier applied to all GX indirect-texture matrices (heat haze,
  // refraction overlays, water distortion). Screen-space refraction creates
  // vergence-accommodation conflict in stereo because the distorting surface
  // sits at one depth while the sampled pixels come from varying depths
  // behind it. Lower values soften the effect; 0 disables it. Only applied
  // when mode != AURORA_STEREO_OFF. Set to 1.0 for unchanged behavior --
  // callers building this struct with aggregate init must include this
  // field, otherwise it zero-initializes to a flat refraction.
  float refractionAmplitudeScale;

  // Ghost / crosstalk reduction. Every stereo display leaks some of each
  // eye's image into the other; how visible that leak is depends on the
  // BRIGHTNESS DIFFERENCE between the eyes, so compressing the signal range
  // in the compose pass (the last thing that touches the pixels before they
  // reach the panel) reduces what you see. Two independent levers, both
  // global, both exact no-ops at their defaults:
  //
  //  ghostContrast   1.0 = off. Squeezes toward mid-grey, shrinking |L - R|
  //                  directly and leaving (1-contrast)/2 of headroom at EACH
  //                  end of the range. Costs contrast across the whole image.
  //                  Useful range ~0.90..1.0.
  //  ghostBlackFloor 0.0 = off. Raises the black floor and leaves white
  //                  alone. Displays that CANCEL crosstalk (autostereo
  //                  panels, incl. the LeiaSR weaver) pre-subtract a fraction
  //                  of the opposite eye, which drives dark pixels below zero
  //                  where the render target clamps them -- and the clamped
  //                  part is exactly what survives as a visible ghost. This
  //                  lever buys foot-room for that. Costs black level rather
  //                  than contrast. Useful range ~0.02..0.05; blacks go grey
  //                  fast above that. Only helps on displays that actually
  //                  cancel; where nothing subtracts, only ghostContrast
  //                  reduces visible ghosting.
  //
  // Both are applied LAST in the compose shader, after mode selection, and
  // are expected to be forced to (1.0, 0.0) by the caller when mode ==
  // AURORA_STEREO_OFF. Callers using aggregate init MUST set these -- a
  // zero-initialized ghostContrast would flatten the image to mid-grey.
  float ghostContrast;
  float ghostBlackFloor;
} AuroraStereoConfig;

typedef enum {
  AURORA_EYE_LEFT = 0,
  AURORA_EYE_RIGHT = 1,
} AuroraEye;

void aurora_set_stereo_config(const AuroraStereoConfig* cfg);
void aurora_set_active_eye(AuroraEye eye);
AuroraEye aurora_get_active_eye();
bool aurora_stereo_mode_supported(AuroraStereoMode mode);

#ifdef __cplusplus
}
#endif

#endif
