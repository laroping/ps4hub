#include "imgui_soft_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace SoftRender {
namespace {

struct Rgba {
  int r, g, b, a;
};

inline int div255(int x) { return (x + 1 + (x >> 8)) >> 8; }

inline Rgba unpackVtx(ImU32 c) {
  return {(int)((c >> IM_COL32_R_SHIFT) & 0xFF),
          (int)((c >> IM_COL32_G_SHIFT) & 0xFF),
          (int)((c >> IM_COL32_B_SHIFT) & 0xFF),
          (int)((c >> IM_COL32_A_SHIFT) & 0xFF)};
}

// True for 8:8:8 layouts with green in the middle byte (ARGB / ABGR), where
// red and blue can be blended together in one 32-bit multiply.
inline bool packedLayout(const Target &t) {
  return t.gShift == 8 && ((t.rShift == 16 && t.bShift == 0) ||
                           (t.rShift == 0 && t.bShift == 16));
}

// Two-channels-at-once blend: a in 0..255 is widened to 0..256 so 255 is exact.
inline uint32_t blendPacked(uint32_t src, uint32_t dst, int a,
                            uint32_t alphaBits) {
  uint32_t a256 = (uint32_t)a + ((uint32_t)a >> 7), ia = 256 - a256;
  uint32_t rb =
      (((src & 0xFF00FFu) * a256 + (dst & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu;
  uint32_t g =
      (((src & 0x00FF00u) * a256 + (dst & 0x00FF00u) * ia) >> 8) & 0x00FF00u;
  return rb | g | alphaBits;
}

inline void blendPixel(const Target &t, uint32_t *d, int r, int g, int b,
                       int a) {
  if (a <= 0)
    return;
  if (a >= 255) {
    *d = ((uint32_t)r << t.rShift) | ((uint32_t)g << t.gShift) |
         ((uint32_t)b << t.bShift) | t.alphaBits;
    return;
  }
  if (packedLayout(t)) {
    uint32_t src = ((uint32_t)r << t.rShift) | ((uint32_t)g << 8) |
                   ((uint32_t)b << t.bShift);
    *d = blendPacked(src, *d, a, t.alphaBits);
    return;
  }
  uint32_t v = *d;
  int ia = 255 - a;
  int dr = (v >> t.rShift) & 0xFF, dg = (v >> t.gShift) & 0xFF,
      db = (v >> t.bShift) & 0xFF;
  dr = div255(r * a + dr * ia);
  dg = div255(g * a + dg * ia);
  db = div255(b * a + db * ia);
  *d = ((uint32_t)dr << t.rShift) | ((uint32_t)dg << t.gShift) |
       ((uint32_t)db << t.bShift) | t.alphaBits;
}

// Texture sampling. ImTextureID is either an ImTextureData* (ImGui atlas,
// sampled in place) or a tagged SoftRender::Texture* (app images); 0 = white.
struct Sampler {
  const unsigned char *px = nullptr;
  int w = 0, h = 0, bpp = 0;
  bool appTexture = false;

  explicit Sampler(ImTextureID id) {
    uintptr_t raw = (uintptr_t)id;
    if (raw & 1u) {
      auto *tex = reinterpret_cast<const Texture *>(raw & ~(uintptr_t)1u);
      if (tex && tex->pixels) {
        px = tex->pixels;
        w = tex->width;
        h = tex->height;
        bpp = 4;
        appTexture = true;
      }
    } else if (raw) {
      auto *tex = reinterpret_cast<ImTextureData *>(raw);
      if (tex->Pixels) {
        px = tex->Pixels;
        w = tex->Width;
        h = tex->Height;
        bpp = tex->BytesPerPixel;
      }
    }
  }

  inline Rgba nearest(float u, float v) const {
    if (!px)
      return {255, 255, 255, 255};
    int x = (int)(u * w), y = (int)(v * h);
    x = x < 0 ? 0 : (x >= w ? w - 1 : x);
    y = y < 0 ? 0 : (y >= h ? h - 1 : y);
    const unsigned char *p = px + (size_t)(y * w + x) * bpp;
    if (bpp == 1)
      return {255, 255, 255, p[0]};
    return {p[0], p[1], p[2], p[3]};
  }

  inline Rgba bilinear(float u, float v) const {
    if (!px || bpp != 4)
      return nearest(u, v);
    float fx = u * w - 0.5f, fy = v * h - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    int ax = (int)((fx - x0) * 256.0f), ay = (int)((fy - y0) * 256.0f);
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = std::clamp(x0, 0, w - 1);
    x1 = std::clamp(x1, 0, w - 1);
    y0 = std::clamp(y0, 0, h - 1);
    y1 = std::clamp(y1, 0, h - 1);
    const unsigned char *p00 = px + (size_t)(y0 * w + x0) * 4;
    const unsigned char *p10 = px + (size_t)(y0 * w + x1) * 4;
    const unsigned char *p01 = px + (size_t)(y1 * w + x0) * 4;
    const unsigned char *p11 = px + (size_t)(y1 * w + x1) * 4;
    int c[4];
    for (int i = 0; i < 4; ++i) {
      int top = p00[i] * (256 - ax) + p10[i] * ax;
      int bot = p01[i] * (256 - ax) + p11[i] * ax;
      c[i] = (top * (256 - ay) + bot * ay) >> 16;
    }
    return {c[0], c[1], c[2], c[3]};
  }
};

struct Clip {
  int x0, y0, x1, y1;
}; // half-open pixel rect

inline void shadeAndBlend(const Target &t, uint32_t *d, const Rgba &c,
                          const Rgba &tx) {
  blendPixel(t, d, div255(c.r * tx.r), div255(c.g * tx.g), div255(c.b * tx.b),
             div255(c.a * tx.a));
}

void drawQuad(const Target &t, const Clip &clip, const ImDrawVert &tl,
              const ImDrawVert &br, ImU32 col, const Sampler &s) {
  // Pixel (x,y) is covered when its centre lies in [x0,x1) x [y0,y1).
  float fx0 = std::min(tl.pos.x, br.pos.x), fx1 = std::max(tl.pos.x, br.pos.x);
  float fy0 = std::min(tl.pos.y, br.pos.y), fy1 = std::max(tl.pos.y, br.pos.y);
  int x0 = std::max(clip.x0, (int)std::ceil(fx0 - 0.5f));
  int x1 = std::min(clip.x1, (int)std::ceil(fx1 - 0.5f));
  int y0 = std::max(clip.y0, (int)std::ceil(fy0 - 0.5f));
  int y1 = std::min(clip.y1, (int)std::ceil(fy1 - 0.5f));
  if (x0 >= x1 || y0 >= y1)
    return;

  Rgba c = unpackVtx(col);
  if (c.a <= 0)
    return;
  bool solidUv = tl.uv.x == br.uv.x && tl.uv.y == br.uv.y;
  if (solidUv) {
    Rgba tx = s.nearest(tl.uv.x, tl.uv.y);
    Rgba f = {div255(c.r * tx.r), div255(c.g * tx.g), div255(c.b * tx.b),
              div255(c.a * tx.a)};
    if (f.a <= 0)
      return;
    if (packedLayout(t)) {
      // Constant colour: precompute the source half of the blend once.
      uint32_t src = ((uint32_t)f.r << t.rShift) | ((uint32_t)f.g << 8) |
                     ((uint32_t)f.b << t.bShift);
      if (f.a >= 255) {
        for (int y = y0; y < y1; ++y) {
          uint32_t *row = t.pixels + (size_t)y * t.pitchPixels;
          std::fill(row + x0, row + x1, src | t.alphaBits);
        }
        return;
      }
      uint32_t a256 = (uint32_t)f.a + ((uint32_t)f.a >> 7), ia = 256 - a256;
      uint32_t srcRB = (src & 0xFF00FFu) * a256,
               srcG = (src & 0x00FF00u) * a256;
      for (int y = y0; y < y1; ++y) {
        uint32_t *row = t.pixels + (size_t)y * t.pitchPixels;
        for (int x = x0; x < x1; ++x) {
          uint32_t d = row[x];
          row[x] = (((srcRB + (d & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu) |
                   (((srcG + (d & 0x00FF00u) * ia) >> 8) & 0x00FF00u) |
                   t.alphaBits;
        }
      }
      return;
    }
    for (int y = y0; y < y1; ++y) {
      uint32_t *row = t.pixels + (size_t)y * t.pitchPixels;
      for (int x = x0; x < x1; ++x)
        blendPixel(t, row + x, f.r, f.g, f.b, f.a);
    }
    return;
  }

  // Map pixel centres onto the UV span (handles mirrored quads too).
  float du = (br.uv.x - tl.uv.x) / (br.pos.x - tl.pos.x);
  float dv = (br.uv.y - tl.uv.y) / (br.pos.y - tl.pos.y);

  // Glyphs: the dynamic font atlas is rasterised at the drawn size, so text
  // is almost always 1:1. Walk the alpha texels with integers.
  if (!s.appTexture && s.px && s.bpp == 1 && du > 0.0f && dv > 0.0f &&
      std::fabs(du * s.w - 1.0f) < 0.02f &&
      std::fabs(dv * s.h - 1.0f) < 0.02f) {
    int tx0 = (int)((tl.uv.x + ((float)x0 + 0.5f - tl.pos.x) * du) * s.w);
    int ty = (int)((tl.uv.y + ((float)y0 + 0.5f - tl.pos.y) * dv) * s.h);
    for (int y = y0; y < y1; ++y, ++ty) {
      if (ty < 0 || ty >= s.h)
        continue;
      uint32_t *row = t.pixels + (size_t)y * t.pitchPixels;
      const unsigned char *src = s.px + (size_t)ty * s.w;
      for (int x = x0, tx = tx0; x < x1; ++x, ++tx) {
        if (tx < 0 || tx >= s.w)
          continue;
        int ta = src[tx];
        if (ta)
          blendPixel(t, row + x, c.r, c.g, c.b, div255(c.a * ta));
      }
    }
    return;
  }
  // App images drawn at their native size stay pixel-exact; scaled ones are
  // filtered so focus zoom animations do not shimmer.
  bool filter =
      s.appTexture && (std::fabs(std::fabs(du) * s.w - 1.0f) > 0.02f ||
                       std::fabs(std::fabs(dv) * s.h - 1.0f) > 0.02f);

  // App image at native size and unmirrored: walk texels with integers and,
  // for an opaque white tint, skip the per-channel shading entirely.
  if (s.appTexture && !filter && du > 0.0f && dv > 0.0f) {
    const bool plain = c.r == 255 && c.g == 255 && c.b == 255 && c.a == 255;
    int tx0 = (int)((tl.uv.x + ((float)x0 + 0.5f - tl.pos.x) * du) * s.w);
    int ty = (int)((tl.uv.y + ((float)y0 + 0.5f - tl.pos.y) * dv) * s.h);
    for (int y = y0; y < y1; ++y, ++ty) {
      if (ty < 0 || ty >= s.h)
        continue;
      uint32_t *row = t.pixels + (size_t)y * t.pitchPixels;
      const unsigned char *src = s.px + ((size_t)ty * s.w) * 4;
      for (int x = x0, tx = tx0; x < x1; ++x, ++tx) {
        if (tx < 0 || tx >= s.w)
          continue;
        const unsigned char *p = src + (size_t)tx * 4;
        if (plain)
          blendPixel(t, row + x, p[0], p[1], p[2], p[3]);
        else
          shadeAndBlend(t, row + x, c, Rgba{p[0], p[1], p[2], p[3]});
      }
    }
    return;
  }

  for (int y = y0; y < y1; ++y) {
    float v = tl.uv.y + ((float)y + 0.5f - tl.pos.y) * dv;
    uint32_t *row = t.pixels + (size_t)y * t.pitchPixels;
    float u = tl.uv.x + ((float)x0 + 0.5f - tl.pos.x) * du;
    if (filter)
      for (int x = x0; x < x1; ++x, u += du)
        shadeAndBlend(t, row + x, c, s.bilinear(u, v));
    else
      for (int x = x0; x < x1; ++x, u += du)
        shadeAndBlend(t, row + x, c, s.nearest(u, v));
  }
}

constexpr int kSub = 8; // 8 bits of sub-pixel precision
constexpr int64_t kOne = 1 << kSub;

void drawTriangle(const Target &t, const Clip &clip, const ImDrawVert *a,
                  const ImDrawVert *b, const ImDrawVert *c, const Sampler &s) {
  int64_t X0 = std::llround(a->pos.x * kOne),
          Y0 = std::llround(a->pos.y * kOne);
  int64_t X1 = std::llround(b->pos.x * kOne),
          Y1 = std::llround(b->pos.y * kOne);
  int64_t X2 = std::llround(c->pos.x * kOne),
          Y2 = std::llround(c->pos.y * kOne);

  int64_t area = (X1 - X0) * (Y2 - Y0) - (Y1 - Y0) * (X2 - X0);
  if (area == 0)
    return;
  if (area < 0) {
    std::swap(X1, X2);
    std::swap(Y1, Y2);
    std::swap(b, c);
    area = -area;
  }

  int minX = std::max<int>(clip.x0, (int)(std::min({X0, X1, X2}) >> kSub));
  int maxX = std::min<int>(clip.x1 - 1,
                           (int)((std::max({X0, X1, X2}) + kOne - 1) >> kSub));
  int minY = std::max<int>(clip.y0, (int)(std::min({Y0, Y1, Y2}) >> kSub));
  int maxY = std::min<int>(clip.y1 - 1,
                           (int)((std::max({Y0, Y1, Y2}) + kOne - 1) >> kSub));
  if (minX > maxX || minY > maxY)
    return;

  // Edge E_ij(p) = (Xj-Xi)*(Py-Yi) - (Yj-Yi)*(Px-Xi); positive inside.
  // Top-left rule: an edge owns its boundary when dy<0, or dy==0 && dx>0.
  struct Edge {
    int64_t dx, dy, bias;
  };
  auto mk = [](int64_t xi, int64_t yi, int64_t xj, int64_t yj) {
    Edge e{xj - xi, yj - yi, 0};
    bool topLeft = e.dy < 0 || (e.dy == 0 && e.dx > 0);
    e.bias = topLeft ? 0 : -1;
    return e;
  };
  Edge e12 = mk(X1, Y1, X2, Y2), e20 = mk(X2, Y2, X0, Y0),
       e01 = mk(X0, Y0, X1, Y1);

  int64_t px = (int64_t)minX * kOne + kOne / 2,
          py = (int64_t)minY * kOne + kOne / 2;
  int64_t w0Row = e12.dx * (py - Y1) - e12.dy * (px - X1);
  int64_t w1Row = e20.dx * (py - Y2) - e20.dy * (px - X2);
  int64_t w2Row = e01.dx * (py - Y0) - e01.dy * (px - X0);
  const int64_t w0dx = -e12.dy * kOne, w1dx = -e20.dy * kOne,
                w2dx = -e01.dy * kOne;
  const int64_t w0dy = e12.dx * kOne, w1dy = e20.dx * kOne,
                w2dy = e01.dx * kOne;

  Rgba ca = unpackVtx(a->col), cb = unpackVtx(b->col), cc = unpackVtx(c->col);
  if (ca.a == 0 && cb.a == 0 && cc.a == 0)
    return;
  bool flatCol = a->col == b->col && b->col == c->col;
  bool flatUv = a->uv.x == b->uv.x && a->uv.x == c->uv.x &&
                a->uv.y == b->uv.y && a->uv.y == c->uv.y;
  Rgba flatTex = s.nearest(a->uv.x, a->uv.y);
  const float inv = 1.0f / (float)area;

  for (int y = minY; y <= maxY;
       ++y, w0Row += w0dy, w1Row += w1dy, w2Row += w2dy) {
    uint32_t *row = t.pixels + (size_t)y * t.pitchPixels;
    int64_t w0 = w0Row, w1 = w1Row, w2 = w2Row;
    bool inside = false; // triangles are convex: once a row's span ends, stop
    for (int x = minX; x <= maxX; ++x, w0 += w0dx, w1 += w1dx, w2 += w2dx) {
      if (w0 + e12.bias < 0 || w1 + e20.bias < 0 || w2 + e01.bias < 0) {
        if (inside)
          break;
        continue;
      }
      inside = true;
      if (flatCol && flatUv) {
        shadeAndBlend(t, row + x, ca, flatTex);
        continue;
      }
      float l0 = (float)w0 * inv, l1 = (float)w1 * inv, l2 = 1.0f - l0 - l1;
      Rgba col = flatCol
                     ? ca
                     : Rgba{(int)(ca.r * l0 + cb.r * l1 + cc.r * l2 + 0.5f),
                            (int)(ca.g * l0 + cb.g * l1 + cc.g * l2 + 0.5f),
                            (int)(ca.b * l0 + cb.b * l1 + cc.b * l2 + 0.5f),
                            (int)(ca.a * l0 + cb.a * l1 + cc.a * l2 + 0.5f)};
      Rgba tx =
          flatUv
              ? flatTex
              : (s.appTexture
                     ? s.bilinear(a->uv.x * l0 + b->uv.x * l1 + c->uv.x * l2,
                                  a->uv.y * l0 + b->uv.y * l1 + c->uv.y * l2)
                     : s.nearest(a->uv.x * l0 + b->uv.x * l1 + c->uv.x * l2,
                                 a->uv.y * l0 + b->uv.y * l1 + c->uv.y * l2));
      shadeAndBlend(t, row + x, col, tx);
    }
  }
}

// ImGui's PrimRect/PrimRectUV emit 4 verts (TL,TR,BR,BL) + indices 0,1,2,0,2,3.
inline bool isAxisQuad(const ImDrawIdx *idx, const ImDrawVert *vtx) {
  if (idx[3] != idx[0] || idx[4] != idx[2])
    return false;
  const ImDrawVert &a = vtx[idx[0]], &b = vtx[idx[1]], &c = vtx[idx[2]],
                   &d = vtx[idx[5]];
  if (a.col != b.col || a.col != c.col || a.col != d.col)
    return false;
  return a.pos.y == b.pos.y && b.pos.x == c.pos.x && c.pos.y == d.pos.y &&
         d.pos.x == a.pos.x && a.uv.y == b.uv.y && b.uv.x == c.uv.x &&
         c.uv.y == d.uv.y && d.uv.x == a.uv.x;
}

void updateTexture(ImTextureData *tex) {
  switch (tex->Status) {
  case ImTextureStatus_WantCreate:
  case ImTextureStatus_WantUpdates:
    // Pixels are sampled in place from tex->Pixels; nothing to upload.
    tex->SetTexID((ImTextureID)(uintptr_t)tex);
    tex->SetStatus(ImTextureStatus_OK);
    break;
  case ImTextureStatus_WantDestroy:
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
    break;
  default:
    break;
  }
}

// 4x4 ordered dither in 1/256 steps (removes banding in the 8-bit gradient).
constexpr int kBayer[16] = {0,  128, 32, 160, 192, 64,  224, 96,
                            48, 176, 16, 144, 240, 112, 208, 80};

} // namespace

bool Init() {
  ImGuiIO &io = ImGui::GetIO();
  io.BackendRendererName = "imgui_soft_renderer";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset |
                     ImGuiBackendFlags_RendererHasTextures;
  io.Fonts->TexDesiredFormat =
      ImTextureFormat_Alpha8; // 1 byte/texel, white glyphs
  return true;
}

void Shutdown() {
  for (ImTextureData *tex : ImGui::GetPlatformIO().Textures) {
    if (tex->RefCount == 1) {
      tex->SetTexID(ImTextureID_Invalid);
      tex->SetStatus(ImTextureStatus_Destroyed);
    }
  }
  ImGuiIO &io = ImGui::GetIO();
  io.BackendRendererName = nullptr;
  io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset |
                       ImGuiBackendFlags_RendererHasTextures);
}

bool Backdrop::init(int width, int height, uint32_t topRgb,
                    uint32_t bottomRgb) {
  release();
  if (width <= 0 || height <= 0)
    return false;
  cache_ = static_cast<uint32_t *>(
      std::malloc((size_t)width * height * sizeof(uint32_t)));
  if (!cache_)
    return false;
  w_ = width;
  h_ = height;
  top_ = topRgb;
  bottom_ = bottomRgb;
  primed_ = false;
  band_ = 0;
  return true;
}

void Backdrop::release() {
  std::free(cache_);
  cache_ = nullptr;
  w_ = h_ = 0;
}

void Backdrop::renderRows(int y0, int y1, float t) {
  struct Glow {
    float cx, cy, rx, ry, strength;
    int r, g, b;
  };
  const float W = (float)w_, H = (float)h_;
  // Two soft light pools in the accent colours, drifting over ~a minute.
  Glow glows[2] = {
      {W * (0.72f + 0.08f * sinf(t * 0.11f)),
       H * (0.22f + 0.07f * sinf(t * 0.07f + 1.0f)), W * 0.40f, H * 0.50f,
       0.42f, 0x00, 0x70, 0xD1},
      {W * (0.16f + 0.07f * sinf(t * 0.09f + 2.0f)),
       H * (0.92f + 0.05f * cosf(t * 0.05f)), W * 0.34f, H * 0.38f, 0.25f, 0x00,
       0xA2, 0xFF},
  };

  static std::vector<int> gx[2];
  for (int k = 0; k < 2; ++k) {
    gx[k].resize(w_);
    for (int x = 0; x < w_; ++x) {
      float d = ((float)x - glows[k].cx) / glows[k].rx;
      gx[k][x] = (int)(255.0f * std::exp(-d * d * 2.2f));
    }
  }

  const int tr = (top_ >> 16) & 0xFF, tg = (top_ >> 8) & 0xFF, tb = top_ & 0xFF;
  const int br = (bottom_ >> 16) & 0xFF, bg = (bottom_ >> 8) & 0xFF,
            bb = bottom_ & 0xFF;
  const int den = h_ > 1 ? h_ - 1 : 1;

  for (int y = y0; y < y1; ++y) {
    // Base colour in 8.8 fixed point.
    int r8 = tr * 256 + (br - tr) * 256 * y / den;
    int g8 = tg * 256 + (bg - tg) * 256 * y / den;
    int b8 = tb * 256 + (bb - tb) * 256 * y / den;
    int rowK[2];
    for (int k = 0; k < 2; ++k) {
      float d = ((float)y - glows[k].cy) / glows[k].ry;
      rowK[k] = (int)(255.0f * glows[k].strength * std::exp(-d * d * 2.2f));
    }
    uint32_t *out = cache_ + (size_t)y * w_;
    const int *dither = kBayer + (y & 3) * 4;
    for (int x = 0; x < w_; ++x) {
      int wa = gx[0][x] * rowK[0], wb = gx[1][x] * rowK[1]; // 0..65025
      int r = r8 + ((wa * glows[0].r + wb * glows[1].r) >> 8);
      int g = g8 + ((wa * glows[0].g + wb * glows[1].g) >> 8);
      int b = b8 + ((wa * glows[0].b + wb * glows[1].b) >> 8);
      int d = dither[x & 3];
      r = std::min(255, (r + d) >> 8);
      g = std::min(255, (g + d) >> 8);
      b = std::min(255, (b + d) >> 8);
      out[x] = ((uint32_t)r << fmtR_) | ((uint32_t)g << fmtG_) |
               ((uint32_t)b << fmtB_) | fmtA_;
    }
  }
}

void Backdrop::drawTo(const Target &t, float timeSec) {
  if (!t.pixels)
    return;
  if (!cache_ || w_ != t.width || h_ != t.height) {
    if (!init(t.width, t.height, top_ ? top_ : 0x00112C,
              bottom_ ? bottom_ : 0x002F6C))
      return;
  }
  // The cache holds finished pixels in the target's own format, so the
  // per-frame cost is one memcpy plus re-rendering a thin band of rows.
  if (!primed_ || fmtR_ != t.rShift || fmtG_ != t.gShift || fmtB_ != t.bShift ||
      fmtA_ != t.alphaBits) {
    fmtR_ = t.rShift;
    fmtG_ = t.gShift;
    fmtB_ = t.bShift;
    fmtA_ = t.alphaBits;
    renderRows(0, h_, timeSec);
    primed_ = true;
  } else {
    int y0 = h_ * band_ / kBands, y1 = h_ * (band_ + 1) / kBands;
    renderRows(y0, y1, timeSec);
    band_ = (band_ + 1) % kBands;
  }

  if (t.pitchPixels == w_) {
    std::memcpy(t.pixels, cache_, (size_t)w_ * h_ * sizeof(uint32_t));
  } else {
    for (int y = 0; y < h_; ++y)
      std::memcpy(t.pixels + (size_t)y * t.pitchPixels, cache_ + (size_t)y * w_,
                  (size_t)w_ * sizeof(uint32_t));
  }
}

void RenderDrawData(ImDrawData *dd, const Target &t) {
  if (!dd || !dd->Valid || !t.pixels || t.width <= 0 || t.height <= 0)
    return;
  if (dd->Textures)
    for (ImTextureData *tex : *dd->Textures)
      if (tex->Status != ImTextureStatus_OK)
        updateTexture(tex);

  const ImVec2 off = dd->DisplayPos;
  for (const ImDrawList *list : dd->CmdLists) {
    const ImDrawVert *vtxBase = list->VtxBuffer.Data;
    const ImDrawIdx *idxBase = list->IdxBuffer.Data;
    for (const ImDrawCmd &cmd : list->CmdBuffer) {
      if (cmd.UserCallback) {
        if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
          cmd.UserCallback(list, &cmd);
        continue;
      }
      Clip clip{std::max(0, (int)std::floor(cmd.ClipRect.x - off.x)),
                std::max(0, (int)std::floor(cmd.ClipRect.y - off.y)),
                std::min(t.width, (int)std::ceil(cmd.ClipRect.z - off.x)),
                std::min(t.height, (int)std::ceil(cmd.ClipRect.w - off.y))};
      if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1)
        continue;

      Sampler s(cmd.GetTexID());
      const ImDrawVert *vtx = vtxBase + cmd.VtxOffset;
      const ImDrawIdx *idx = idxBase + cmd.IdxOffset;
      const unsigned n = cmd.ElemCount;

      // Vertex positions are relative to DisplayPos; it is (0,0) for the
      // single fullscreen viewport this app uses.
      for (unsigned i = 0; i + 3 <= n;) {
        if (i + 6 <= n && isAxisQuad(idx + i, vtx)) {
          drawQuad(t, clip, vtx[idx[i]], vtx[idx[i + 2]], vtx[idx[i]].col, s);
          i += 6;
        } else {
          drawTriangle(t, clip, &vtx[idx[i]], &vtx[idx[i + 1]],
                       &vtx[idx[i + 2]], s);
          i += 3;
        }
      }
    }
  }
}

} // namespace SoftRender
