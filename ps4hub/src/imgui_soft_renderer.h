#pragma once
// Software renderer backend for Dear ImGui (>= 1.92 texture API).
//
// The OpenOrbis SDL2 port is 2.0.9 (no SDL_RenderGeometry) and presents via a
// CPU framebuffer anyway, so ImGui triangles are rasterised straight into the
// 32-bit window surface. Axis-aligned quads (rect fills, glyphs, images) take
// a fast path; everything else goes through an exact fixed-point
// edge-function rasteriser with the top-left fill rule so translucent panels
// have no seams.
//
// Besides ImGui's own atlas it samples app-owned RGBA textures (icons):
// 1:1 draws use nearest sampling, scaled draws are filtered bilinearly.
#include <cstdint>

#include "imgui.h"

namespace SoftRender {

struct Target {
  uint32_t *pixels;
  int width;
  int height;
  int pitchPixels;
  int rShift, gShift, bShift;
  uint32_t alphaBits; // value OR-ed into every written pixel (opaque alpha)
};

// App-owned straight-alpha RGBA32 texture.
struct Texture {
  uint8_t *pixels = nullptr;
  int width = 0;
  int height = 0;
};
// Tagged so the sampler can tell app textures from ImGui's ImTextureData.
inline ImTextureID TextureId(const Texture *t) {
  return (ImTextureID)((uintptr_t)t | 1u);
}

bool Init(); // sets ImGuiBackendFlags_RendererHasTextures / HasVtxOffset
void Shutdown();

// Animated ambient backdrop: navy gradient plus two slowly drifting light
// pools. Rendered into a cache a few rows per frame (cheap), then copied.
class Backdrop {
public:
  bool init(int width, int height, uint32_t topRgb, uint32_t bottomRgb);
  void release();
  // Re-renders 1/kBands of the cache for time `t` (seconds), then blits all.
  void drawTo(const Target &t, float timeSec);

private:
  static constexpr int kBands = 8;
  void renderRows(int y0, int y1, float timeSec);
  uint32_t *cache_ = nullptr; // finished pixels in the target format
  int fmtR_ = 16, fmtG_ = 8, fmtB_ = 0;
  uint32_t fmtA_ = 0xFF000000u;
  int w_ = 0, h_ = 0, band_ = 0;
  uint32_t top_ = 0, bottom_ = 0;
  bool primed_ = false;
};

void RenderDrawData(ImDrawData *drawData, const Target &t);

} // namespace SoftRender
