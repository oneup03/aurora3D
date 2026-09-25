#include "leiasr.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <dawn/native/D3D12Backend.h>

#include "../logging.hpp"
#include "../window.hpp"

// SR-lib's wrapper (extern/SR-lib, bo3b/SR-lib api_expansion) rather than the
// raw SDK headers. It owns the SRContext + IDX12Weaver1 lifetime, performs
// create -> CreateDX12Weaver -> initialize() in the one order that makes eye
// tracking engage, pairs SRContext::create() with deleteSRContext() (the object
// lives in the SR DLL, so `delete` puts it on the wrong heap), reads the input
// texture's dimensions and format off its resource desc so the weaver can never
// be told a size it doesn't have, probes the delay-loaded SR DLLs before
// touching any SDK entry point, and converts the SDK's exceptions -- notably
// ServerNotAvailableException when the SR service isn't running -- into an
// HRESULT.
#include "SR.hpp"

#include <SDL3/SDL_system.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <exception>

namespace aurora::webgpu::leiasr {

namespace {

Module Log("aurora::leiasr");

using Microsoft::WRL::ComPtr;

// Lifecycle state machine. Disabled is sticky: once we fail to init we never
// try again this session (avoids per-frame retries on machines with no SDK or
// no Leia display).
enum class State { Uninit, Ready, Disabled };
State g_state = State::Uninit;

// SR-lib's DX12 interface. It wraps IDX12Weaver1 (the non-deprecated weaver):
// output goes to whatever render target is bound on the command list at weave()
// time, so we bind the woven RTV ourselves and don't pass an output buffer to
// the SDK. Released with Delete(), never `delete`.
SimulatedReality::SRInterfaceDX12* g_sr = nullptr;

// Native D3D12 handles (shared with Dawn).
ComPtr<ID3D12Device> g_d3dDevice;
ComPtr<ID3D12CommandQueue> g_d3dQueue;
ComPtr<ID3D12CommandAllocator> g_d3dAllocator;
ComPtr<ID3D12GraphicsCommandList> g_d3dCommandList;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
D3D12_CPU_DESCRIPTOR_HANDLE g_wovenRtv = {};

// SBS input + woven output. Created on g_d3dDevice and imported into Dawn.
ComPtr<ID3D12Resource> g_sbsResource;
ComPtr<ID3D12Resource> g_wovenResource;
wgpu::SharedTextureMemory g_sbsMemory;
wgpu::SharedTextureMemory g_wovenMemory;
wgpu::Texture g_sbsTexture;
wgpu::Texture g_wovenTexture;
TextureWithSampler g_wovenWithSampler;

uint32_t g_width = 0;
uint32_t g_height = 0;
wgpu::TextureFormat g_wgpuFormat = wgpu::TextureFormat::Undefined;
DXGI_FORMAT g_dxgiFormat = DXGI_FORMAT_UNKNOWN;
HWND g_hwnd = nullptr;

// Track whether Dawn currently holds BeginAccess on each shared texture so
// we can pair Begin/End correctly across frames. SharedTextureMemory rejects
// a second BeginAccess on a texture that's still in the previous Begin.
bool g_sbsInDawnAccess = false;
bool g_wovenInDawnAccess = false;

// Last resource pushed to the weaver via SetInputTexture, and last format
// pushed via SetOutputFormat. Neither call is free -- the first tells the
// weaver to rebind its sampling source -- so skip them when nothing changed.
// The resource pointer alone is enough to key the input now that SR-lib reads
// width/height/format off the resource desc: a size or format change here
// always goes through release_d3d_resources() + a fresh CreateCommittedResource,
// so a different geometry implies a different pointer. Both are cleared by
// release_d3d_resources() so a fresh allocation re-pushes.
ID3D12Resource* g_lastWeaverInputResource = nullptr;
DXGI_FORMAT g_lastWeaverOutputFormat = DXGI_FORMAT_UNKNOWN;

DXGI_FORMAT to_dxgi_format(wgpu::TextureFormat fmt) noexcept {
  switch (fmt) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case wgpu::TextureFormat::BGRA8Unorm:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

HWND get_native_hwnd() noexcept {
  SDL_Window* window = window::get_sdl_window();
  if (window == nullptr) {
    return nullptr;
  }
  SDL_PropertiesID props = SDL_GetWindowProperties(window);
  return static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

// LeiaSR DLLs are delay-loaded so the exe runs without the SR Platform
// installed. Probing with LoadLibraryW is what makes that safe: touching SDK
// entry points without the DLLs on disk raises the delay-load helper's SEH
// exception, which a C++ catch can't intercept without compiling this whole TU
// /EHa.
//
// SR-lib runs this same probe inside CreateSRInterfaceDX12, so try_init()
// doesn't strictly need it. We keep our own copy because is_runtime_installed()
// exposes it to the UI thread, which must never reach the real init (see the
// threading contract in the header) but still needs a cheap "is LeiaSR worth
// offering" answer.
//
// Probe the DirectX weaver DLL, not just the core runtime: a machine can have
// the core present while the backend weaver DLL is missing, which would clear a
// core-only guard and then raise SEH inside CreateDX12Weaver -- exactly what the
// preflight exists to prevent. SimulatedRealityDirectX.dll also statically
// imports DimencoWeaving, SimulatedRealityCore, SimulatedRealityDisplays,
// SimulatedRealityFaceTrackers and opencv_world343, so loading it resolves the
// whole chain including the OpenCV dependency the SR import libs pull in.
// DimencoWeaving is probed as well rather than leaning on that static
// dependency set, which is an implementation detail of a given SDK build.
//
// Deliberately no FreeLibrary: we're about to use these ourselves, and another
// module in the process may already depend on what we just loaded.
bool sr_runtime_dlls_available() noexcept {
  static int s_cached = -1;
  if (s_cached >= 0) {
    return s_cached != 0;
  }
  const bool ok = LoadLibraryW(L"SimulatedRealityDirectX.dll") != nullptr && //
                  LoadLibraryW(L"DimencoWeaving.dll") != nullptr;
  s_cached = ok ? 1 : 0;
  return ok;
}

void release_d3d_resources() noexcept {
  g_sbsTexture = {};
  g_wovenTexture = {};
  g_sbsMemory = {};
  g_wovenMemory = {};
  g_wovenWithSampler = {};
  g_sbsResource.Reset();
  g_wovenResource.Reset();
  g_width = 0;
  g_height = 0;
  g_wgpuFormat = wgpu::TextureFormat::Undefined;
  g_dxgiFormat = DXGI_FORMAT_UNKNOWN;
  // Textures are gone, so any prior Dawn access is implicitly invalid.
  g_sbsInDawnAccess = false;
  g_wovenInDawnAccess = false;
  // Force a fresh SetInputTexture / SetOutputFormat on the next weave -- the
  // underlying ID3D12Resource pointer is gone and reusing the cache would lie.
  g_lastWeaverInputResource = nullptr;
  g_lastWeaverOutputFormat = DXGI_FORMAT_UNKNOWN;
}

// Transient predicates: if any of these fail, init() returns false but stays
// in Uninit so the next call retries (common case: UI queried support before
// Dawn finished picking a backend or creating a surface).
bool dawn_is_ready() noexcept {
  return g_device != nullptr && g_backendType == wgpu::BackendType::D3D12;
}

// RENDER-WORKER-THREAD ONLY. Runs SRContext::create / CreateDX12Weaver /
// SRContext::initialize (inside SR-lib's CreateSRInterfaceDX12), which take
// Dawn's shared D3D12 device+queue and let
// the SR runtime take over + resize the host window (it re-enters the window
// proc via SendMessage during initialize()). Calling this from the UI/main
// thread deadlocks: that thread owns the message pump, and if it's blocked
// here inside a settings-apply the SR window messages can never be serviced --
// an unbounded SR window-proc recursion (observed live in the debugger). It is
// reached only from is_supported()/ensure_ready() on the present path; the
// UI-thread callers were rerouted to is_runtime_installed(). Do not add a
// UI-thread caller.
bool try_init() {
  if (g_state != State::Uninit) {
    return g_state == State::Ready;
  }

  // ---- transient: don't latch Disabled if these fail (might recover) ----
  if (!dawn_is_ready()) {
    // Don't log every poll; the UI hits this on every frame before launch.
    return false;
  }
  HWND hwnd = get_native_hwnd();
  if (hwnd == nullptr) {
    return false;
  }
  if (!sr_runtime_dlls_available()) {
    // No SR Platform installed -- expected case on most machines. Latch
    // Disabled (we won't retry) since LoadLibrary failure is sticky.
    Log.warn("LeiaSR disabled: SR Platform DLLs not on PATH (install LeiaSR/Platform to enable)");
    g_state = State::Disabled;
    return false;
  }

  // ---- from here, failures are sticky: the SR runtime / display state is
  //      not expected to change mid-session in a way we can detect. ----
  Log.warn("LeiaSR: probing SR runtime + display (first-time init)");

  ComPtr<ID3D12Device> d3dDevice;
  ComPtr<ID3D12CommandQueue> d3dQueue;
  try {
    d3dDevice = dawn::native::d3d12::GetD3D12Device(g_device.Get());
    d3dQueue = dawn::native::d3d12::GetD3D12CommandQueue(g_device.Get());
  } catch (const std::exception& e) {
    Log.warn("LeiaSR disabled: Dawn D3D12 interop threw: {}", e.what());
    g_state = State::Disabled;
    return false;
  }
  if (!d3dDevice || !d3dQueue) {
    Log.warn("LeiaSR disabled: Dawn returned null D3D12 device/queue");
    g_state = State::Disabled;
    return false;
  }

  // One call replaces context creation, weaver creation and context
  // initialization: SR-lib performs them in the required order (initialize()
  // strictly after the weaver exists, or eye tracking silently never engages
  // and the panel shows an un-woven still). This is also the canonical "do you
  // have a Leia display?" check -- CreateDX12Weaver succeeds only when an SR
  // display is connected and addressable. Failures arrive as an HRESULT rather
  // than an exception, including ServerNotAvailableException when the SR
  // service isn't running; the try/catch is belt-and-braces.
  SimulatedReality::SRInterfaceDX12* sr = nullptr;
  try {
    const HRESULT hr = SimulatedReality::CreateSRInterfaceDX12(d3dDevice.Get(), hwnd, &sr);
    if (FAILED(hr) || sr == nullptr) {
      Log.warn("LeiaSR disabled: CreateSRInterfaceDX12 failed (hr 0x{:08x}; SR service running, display connected?)",
               static_cast<uint32_t>(hr));
      g_state = State::Disabled;
      return false;
    }
  } catch (const std::exception& e) {
    Log.warn("LeiaSR disabled: CreateSRInterfaceDX12 threw: {}", e.what());
    g_state = State::Disabled;
    return false;
  } catch (...) {
    Log.warn("LeiaSR disabled: CreateSRInterfaceDX12 threw unknown exception");
    g_state = State::Disabled;
    return false;
  }

  // Avoid double-gamma: our SBS intermediate and the swap chain hold linear
  // UNORM values, so don't ask the weaver shader to convert either way. This is
  // weaver state rather than context state, so it doesn't matter that it now
  // lands after SRContext::initialize() (which happened inside the create).
  try {
    sr->SetShaderSRGBConversion(false, false);
  } catch (...) {
    // Non-fatal -- fall back to whatever default the SDK picks.
  }

  // Command allocator + list owned by the weaver path. Reset each frame.
  HRESULT hr = d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d3dAllocator));
  if (FAILED(hr)) {
    Log.warn("LeiaSR disabled: CreateCommandAllocator failed: 0x{:08x}", static_cast<uint32_t>(hr));
    sr->Delete();
    g_state = State::Disabled;
    return false;
  }
  hr = d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d3dAllocator.Get(), nullptr,
                                    IID_PPV_ARGS(&g_d3dCommandList));
  if (FAILED(hr)) {
    Log.warn("LeiaSR disabled: CreateCommandList failed: 0x{:08x}", static_cast<uint32_t>(hr));
    g_d3dAllocator.Reset();
    sr->Delete();
    g_state = State::Disabled;
    return false;
  }
  g_d3dCommandList->Close();

  // RTV heap (single slot) used to bind the woven texture as the weaver's
  // output. Allocated once; the actual RTV is (re)created in create_shared_pair.
  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDesc.NumDescriptors = 1;
  rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  hr = d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
  if (FAILED(hr)) {
    Log.warn("LeiaSR disabled: CreateDescriptorHeap(RTV) failed: 0x{:08x}", static_cast<uint32_t>(hr));
    g_d3dCommandList.Reset();
    g_d3dAllocator.Reset();
    sr->Delete();
    g_state = State::Disabled;
    return false;
  }
  g_wovenRtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

  // Commit all the successfully-created state.
  g_sr = sr;
  g_d3dDevice = std::move(d3dDevice);
  g_d3dQueue = std::move(d3dQueue);
  g_hwnd = hwnd;

  Log.warn("LeiaSR initialized");
  g_state = State::Ready;
  return true;
}

bool create_shared_pair(uint32_t width, uint32_t height, DXGI_FORMAT format, wgpu::TextureFormat wgpuFormat) {
  release_d3d_resources();
  // release_d3d_resources clears g_wgpuFormat / g_dxgiFormat -- restore them so
  // the import lambda below + the new TextureWithSampler bundle pick them up.
  g_wgpuFormat = wgpuFormat;
  g_dxgiFormat = format;

  // Full-SbS: input is 2x wider than the swapchain so each eye gets its own
  // full-resolution half. The DX12 example does the same; halving the input
  // would force the weaver to upscale and blur the per-eye image.
  const uint32_t sbsWidth = width * 2;
  const uint32_t sbsHeight = height;
  const uint32_t wovenWidth = width;
  const uint32_t wovenHeight = height;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  const auto create_one = [&](uint32_t w, uint32_t h, ComPtr<ID3D12Resource>& out, const char* label) -> bool {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // SIMULTANEOUS_ACCESS is required by Dawn's SharedTextureMemoryD3D12Resource
    // import path. It allows the resource to remain in COMMON state across the
    // Dawn <-> native SR weaver hand-offs without explicit transitions.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    const HRESULT hr = g_d3dDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
      Log.warn("LeiaSR: {} CreateCommittedResource failed 0x{:08x}", label, static_cast<uint32_t>(hr));
      return false;
    }
    return true;
  };

  if (!create_one(sbsWidth, sbsHeight, g_sbsResource, "SBS input")) {
    return false;
  }
  // Woven output is read by Dawn (sample in the swapchain copy) and also written
  // by the weaver as a render target. UNORDERED_ACCESS isn't needed for the
  // non-deprecated weaver path; RENDER_TARGET + SRV is enough.
  if (!create_one(wovenWidth, wovenHeight, g_wovenResource, "Woven output")) {
    g_sbsResource.Reset();
    return false;
  }

  // Import both into Dawn via SharedTextureMemoryD3D12Resource. Because the
  // ID3D12Resource was created on Dawn's own device, no cross-device sharing
  // is needed -- the same physical resource is just exposed as a wgpu::Texture.
  const auto import_one = [&](const ComPtr<ID3D12Resource>& res, wgpu::TextureUsage usage, uint32_t w, uint32_t h,
                              wgpu::SharedTextureMemory& memOut, wgpu::Texture& texOut) -> bool {
    dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor d3dDesc;
    d3dDesc.resource = res;
    wgpu::SharedTextureMemoryDescriptor memDesc{};
    memDesc.nextInChain = &d3dDesc;
    memOut = g_device.ImportSharedTextureMemory(&memDesc);
    if (memOut == nullptr) {
      Log.warn("LeiaSR: ImportSharedTextureMemory returned null");
      return false;
    }
    wgpu::TextureDescriptor texDesc{};
    texDesc.usage = usage;
    texDesc.dimension = wgpu::TextureDimension::e2D;
    texDesc.size = {w, h, 1};
    texDesc.format = g_wgpuFormat;
    texDesc.mipLevelCount = 1;
    texDesc.sampleCount = 1;
    texOut = memOut.CreateTexture(&texDesc);
    return texOut != nullptr;
  };

  if (!import_one(g_sbsResource, wgpu::TextureUsage::RenderAttachment, sbsWidth, sbsHeight, g_sbsMemory,
                  g_sbsTexture)) {
    release_d3d_resources();
    return false;
  }
  if (!import_one(g_wovenResource, wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc, wovenWidth,
                  wovenHeight, g_wovenMemory, g_wovenTexture)) {
    release_d3d_resources();
    return false;
  }

  // Build a TextureWithSampler bundle for the woven output so the EFB-copy
  // pass can sample it like any other Dawn texture.
  wgpu::SamplerDescriptor samplerDesc{};
  samplerDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
  samplerDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
  samplerDesc.magFilter = wgpu::FilterMode::Linear;
  samplerDesc.minFilter = wgpu::FilterMode::Linear;
  g_wovenWithSampler.texture = g_wovenTexture;
  g_wovenWithSampler.view = g_wovenTexture.CreateView();
  g_wovenWithSampler.size = {wovenWidth, wovenHeight, 1};
  g_wovenWithSampler.format = g_wgpuFormat;
  g_wovenWithSampler.sampler = g_device.CreateSampler(&samplerDesc);

  // Create the RTV for the woven texture so we can bind it as the weaver's
  // output target in weave(). The format is the same as the swapchain.
  D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
  rtvDesc.Format = format;
  rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  g_d3dDevice->CreateRenderTargetView(g_wovenResource.Get(), &rtvDesc, g_wovenRtv);

  g_width = width;
  g_height = height;
  return true;
}

bool begin_access(const wgpu::Texture& tex, const wgpu::SharedTextureMemory& mem) {
  wgpu::SharedTextureMemoryBeginAccessDescriptor desc{};
  desc.initialized = true;
  desc.concurrentRead = false;
  desc.fenceCount = 0;
  return mem.BeginAccess(tex, &desc) == wgpu::Status::Success;
}

void end_access(const wgpu::Texture& tex, const wgpu::SharedTextureMemory& mem) {
  wgpu::SharedTextureMemoryEndAccessState endState{};
  mem.EndAccess(tex, &endState);
}

} // namespace

bool is_supported() {
  if (g_state == State::Uninit) {
    try_init();
  }
  return g_state == State::Ready;
}

bool is_runtime_installed() {
  // DLL-presence probe only -- no SRContext, no weaver, no window takeover, so
  // this is safe on the UI/main thread (unlike is_supported()/try_init()).
  return sr_runtime_dlls_available();
}

bool ensure_ready(uint32_t width, uint32_t height, wgpu::TextureFormat format) {
  if (!is_supported()) {
    return false;
  }
  if (width == 0 || height == 0) {
    return false;
  }
  const DXGI_FORMAT dxgi = to_dxgi_format(format);
  if (dxgi == DXGI_FORMAT_UNKNOWN) {
    Log.warn("LeiaSR: unsupported surface format {}", static_cast<int>(format));
    return false;
  }
  if (g_sbsResource && width == g_width && height == g_height && format == g_wgpuFormat) {
    return true;
  }
  return create_shared_pair(width, height, dxgi, format);
}

wgpu::TextureView input_view() {
  if (!g_sbsTexture) {
    return {};
  }
  // Hand the texture to Dawn for writing. End_access happens at the start of
  // weave() so the native command list (executed on Dawn's queue) sees the
  // SBS write.
  if (g_sbsInDawnAccess) {
    // Shouldn't happen in normal flow; previous frame's input_view was not
    // followed by weave(). Release before re-Begin so we don't trip Dawn.
    end_access(g_sbsTexture, g_sbsMemory);
    g_sbsInDawnAccess = false;
  }
  if (!begin_access(g_sbsTexture, g_sbsMemory)) {
    return {};
  }
  g_sbsInDawnAccess = true;
  return g_sbsTexture.CreateView();
}

const TextureWithSampler& output_texture() {
  return g_wovenWithSampler;
}

Extent input_extent() {
  return {g_width * 2u, g_height};
}

void weave() {
  if (g_state != State::Ready || !g_sr || !g_sbsResource || !g_wovenResource) {
    return;
  }

  // Release Dawn's access on the SBS texture; its contents are now flushed.
  if (g_sbsInDawnAccess) {
    end_access(g_sbsTexture, g_sbsMemory);
    g_sbsInDawnAccess = false;
  }
  // Previous frame's Dawn sample on the woven output is also complete by now
  // (Dawn submit + present finished); release that BeginAccess so we can hand
  // the texture back to the native weaver as a render target.
  if (g_wovenInDawnAccess) {
    end_access(g_wovenTexture, g_wovenMemory);
    g_wovenInDawnAccess = false;
  }

  // Build a fresh native command list that runs the weaver.
  if (FAILED(g_d3dAllocator->Reset())) {
    return;
  }
  if (FAILED(g_d3dCommandList->Reset(g_d3dAllocator.Get(), nullptr))) {
    return;
  }

  // Output buffer needs to be RENDER_TARGET; input needs to be COMMON (the
  // weaver's internal SRV expects that, per the SDK docs).
  D3D12_RESOURCE_BARRIER toRT{};
  toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toRT.Transition.pResource = g_wovenResource.Get();
  toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  g_d3dCommandList->ResourceBarrier(1, &toRT);

  // The new IDX12Weaver1 API writes to whatever RT is currently bound, so
  // bind the woven RTV before calling weave().
  g_d3dCommandList->OMSetRenderTargets(1, &g_wovenRtv, FALSE, nullptr);

  // D3D12 rasterizes against whatever RSSetViewports last set ON THE COMMAND
  // LIST, not against what the weaver is told below. Our command list is freshly
  // reset each frame so nothing has dirtied it, but the weave still needs the
  // destination extent set explicitly -- the woven output is g_width wide, half
  // the SBS input, and a stale wider viewport would rasterize the weave at 2x
  // and show only its left portion.
  const D3D12_VIEWPORT viewport{0.f, 0.f, static_cast<float>(g_width), static_cast<float>(g_height), 0.f, 1.f};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
  g_d3dCommandList->RSSetViewports(1, &viewport);
  g_d3dCommandList->RSSetScissorRects(1, &scissor);

  // Track whether weaver hand-off saw a fatal exception this frame (e.g. the
  // SR display was unplugged). If so, tear down to State::Disabled so we
  // don't spam exceptions every frame for the rest of the session.
  bool weave_fatal = false;

  try {
    // The SBS input is 2x width and SR-lib reads that extent (and the format)
    // straight off the resource desc, so the weaver can't be told a size the
    // texture doesn't have. Rebinding isn't free -- it makes the weaver
    // re-create its sampling view -- so skip it while the resource is unchanged,
    // which is every frame except the first after a (re)allocation.
    if (g_sbsResource.Get() != g_lastWeaverInputResource) {
      g_sr->SetInputTexture(g_sbsResource.Get());
      g_lastWeaverInputResource = g_sbsResource.Get();
    }
    // Must match the bound render target's actual format; a mismatch weaves with
    // visibly wrong colors rather than failing.
    if (g_dxgiFormat != g_lastWeaverOutputFormat) {
      g_sr->SetOutputFormat(g_dxgiFormat);
      g_lastWeaverOutputFormat = g_dxgiFormat;
    }
    // Sets the command list + viewport + scissor and records the weave into it.
    g_sr->Weave(g_d3dCommandList.Get(), viewport, scissor);
  } catch (const std::exception& e) {
    Log.warn("LeiaSR: weave threw: {}", e.what());
    weave_fatal = true;
  } catch (...) {
    Log.warn("LeiaSR: weave threw unknown exception");
    weave_fatal = true;
  }

  if (weave_fatal) {
    // Most likely the SR display vanished mid-session (unplug, sleep, service
    // crash). Latch State::Disabled so is_supported() returns false next
    // frame and aurora.cpp's leiasr_active gate falls back to plain SBS for
    // the rest of the session.
    //
    // Don't tear down the woven/SBS textures here: aurora.cpp will still call
    // output_texture() in the post-weave compose this frame, and a null
    // texture there would just crash differently. We just deliver one frame
    // of stale woven content while the fallback engages.
    //
    // Close the command list without executing -- the barriers-only command
    // list has no useful work, and skipping the COMMON→RENDER_TARGET barrier
    // execution leaves the woven resource in COMMON, which is exactly what
    // begin_access expects below. Then re-issue begin_access so Dawn can
    // sample the texture this frame (without it Dawn validation rejects the
    // sample).
    try { g_d3dCommandList->Close(); } catch (...) {}
    if (begin_access(g_wovenTexture, g_wovenMemory)) {
      g_wovenInDawnAccess = true;
    }
    g_state = State::Disabled;
    Log.warn("LeiaSR disabled for remainder of session due to weave failure; full teardown deferred to shutdown");
    return;
  }

  // Back to COMMON so SharedTextureMemory's next BeginAccess sees the expected
  // initial state when Dawn samples the output texture.
  D3D12_RESOURCE_BARRIER toCommon{};
  toCommon.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toCommon.Transition.pResource = g_wovenResource.Get();
  toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  toCommon.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  g_d3dCommandList->ResourceBarrier(1, &toCommon);

  g_d3dCommandList->Close();
  ID3D12CommandList* lists[] = {g_d3dCommandList.Get()};
  g_d3dQueue->ExecuteCommandLists(1, lists);

  // The EFB-copy pass that samples output_texture() runs through Dawn next; the
  // same-queue FIFO ordering ensures the weave is visible before the sample.
  if (begin_access(g_wovenTexture, g_wovenMemory)) {
    g_wovenInDawnAccess = true;
  }
}

void shutdown() {
  release_d3d_resources();
  // Delete() destroys the weaver (via IDestroyable::destroy(), never `delete` --
  // that's the deprecated PredictingDX12Weaver convention and asserts in debug)
  // and then releases the SRContext with SRContext::deleteSRContext(), the
  // matching pair for SRContext::create(). Both live in the SR DLL, so freeing
  // either with our own `delete` would put them on the wrong heap.
  if (g_sr != nullptr) {
    g_sr->Delete();
    g_sr = nullptr;
  }
  g_wovenRtv = {};
  g_rtvHeap.Reset();
  g_d3dCommandList.Reset();
  g_d3dAllocator.Reset();
  g_d3dQueue.Reset();
  g_d3dDevice.Reset();
  g_state = State::Uninit;
}

} // namespace aurora::webgpu::leiasr
