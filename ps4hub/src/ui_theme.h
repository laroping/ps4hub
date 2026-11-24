#pragma once
#include <cfloat>
#include <cmath>

#include "imgui.h"

namespace Theme {

constexpr unsigned kBgTop = 0x00112C;
constexpr unsigned kBgBottom = 0x002F6C;

inline ImU32 rgba(int r, int g, int b, int a = 255) {
  return IM_COL32(r, g, b, a);
}
inline ImU32 alpha(ImU32 c, float a) {
  int na = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * (a < 0   ? 0
                                                     : a > 1 ? 1
                                                             : a));
  return (c & ~IM_COL32_A_MASK) | ((ImU32)na << IM_COL32_A_SHIFT);
}

inline ImU32 accent() { return rgba(0x00, 0x70, 0xD1); }       // #0070d1
inline ImU32 accentBright() { return rgba(0x00, 0xA2, 0xFF); } // #00a2ff
inline ImU32 white() { return rgba(0xFF, 0xFF, 0xFF); }
inline ImU32 textPrimary() { return rgba(0xF4, 0xF8, 0xFF); }
inline ImU32 textMuted() { return rgba(0xA9, 0xBC, 0xD9); }
inline ImU32 textDim() { return rgba(0x6F, 0x85, 0xA8); }
inline ImU32 danger() { return rgba(0xFF, 0x6B, 0x7D); }
inline ImU32 success() { return rgba(0x4A, 0xE3, 0xA4); }
inline ImU32 warning() { return rgba(0xFF, 0xC2, 0x4B); }

// Glass: rgba(15, 25, 45, 0.70).
inline ImU32 glassFill() { return rgba(15, 25, 45, 179); }
inline ImU32 glassEdge() { return rgba(160, 200, 255, 38); }
inline ImU32 glassSheen() { return rgba(255, 255, 255, 12); }

constexpr float kRadius = 22.0f;
constexpr float kBandH =
    kRadius * 2.5f; // lighter title band at the top of every card

inline void Apply() {
  ImGuiStyle &s = ImGui::GetStyle();
  s.WindowPadding = ImVec2(0, 0);
  s.WindowBorderSize = 0.0f;
  s.WindowRounding = 0.0f;
  s.ItemSpacing = ImVec2(0, 0);
  s.AntiAliasedLines = true;
  s.AntiAliasedFill = true;
  s.CircleTessellationMaxError = 0.6f;
  s.FontSizeBase = 28.0f;
  s.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(textPrimary());
  s.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
}

inline ImVec2 Measure(float size, const char *s) {
  return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
}
inline void Text(ImDrawList *dl, float x, float y, float size, ImU32 col,
                 const char *s) {
  dl->AddText(ImGui::GetFont(), size, ImVec2(x, y), col, s);
}

// One anti-aliased quarter disc: solid fan to radius r-0.5, then a 1px fringe
// fading to transparent at r+0.5 (same edge quality as ImGui's AA fill).
inline void CornerFan(ImDrawList *dl, ImVec2 c, float r, float a0, float a1,
                      ImU32 col) {
  const int n = r < 8.0f ? 4 : (r < 24.0f ? 8 : 12);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const ImU32 clear = col & ~IM_COL32_A_MASK;
  dl->PrimReserve(n * 9, 1 + (n + 1) * 2);
  const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
  dl->PrimWriteVtx(c, uv, col);
  for (int k = 0; k <= n; ++k) {
    float t = a0 + (a1 - a0) * (float)k / (float)n;
    float cs = std::cos(t), sn = std::sin(t);
    dl->PrimWriteVtx(ImVec2(c.x + cs * (r - 0.5f), c.y + sn * (r - 0.5f)), uv,
                     col);
    dl->PrimWriteVtx(ImVec2(c.x + cs * (r + 0.5f), c.y + sn * (r + 0.5f)), uv,
                     clear);
  }
  for (int k = 0; k < n; ++k) {
    ImDrawIdx in0 = (ImDrawIdx)(base + 1 + 2 * k), out0 = (ImDrawIdx)(in0 + 1);
    ImDrawIdx in1 = (ImDrawIdx)(in0 + 2), out1 = (ImDrawIdx)(in0 + 3);
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx(in0);
    dl->PrimWriteIdx(in1);
    dl->PrimWriteIdx(in0);
    dl->PrimWriteIdx(in1);
    dl->PrimWriteIdx(out1);
    dl->PrimWriteIdx(in0);
    dl->PrimWriteIdx(out1);
    dl->PrimWriteIdx(out0);
  }
}

// Filled rounded rectangle built from three axis-aligned rects plus four
// corner fans. Looks the same as AddRectFilled(..., rounding) but the software
// rasteriser only visits covered pixels; ImGui's single triangle fan made it
// walk huge, mostly empty bounding boxes for every large panel.
inline void FillRoundRect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col,
                          float r = 0.0f, bool roundBottom = true) {
  if (((col >> IM_COL32_A_SHIFT) & 0xFF) == 0 || b.x <= a.x || b.y <= a.y)
    return;
  r = std::fmin(r, std::fmin((b.x - a.x) * 0.5f,
                             (b.y - a.y) * (roundBottom ? 0.5f : 1.0f)));
  if (r < 1.0f) {
    dl->AddRectFilled(a, b, col);
    return;
  }
  const float pi = 3.14159265f;
  const float yIn0 = a.y + r, yIn1 = roundBottom ? b.y - r : b.y;
  dl->AddRectFilled(ImVec2(a.x + r, a.y), ImVec2(b.x - r, b.y),
                    col); // centre column
  if (yIn1 > yIn0) {
    dl->AddRectFilled(ImVec2(a.x, yIn0), ImVec2(a.x + r, yIn1),
                      col); // left strip
    dl->AddRectFilled(ImVec2(b.x - r, yIn0), ImVec2(b.x, yIn1),
                      col); // right strip
  }
  CornerFan(dl, ImVec2(a.x + r, a.y + r), r, pi, pi * 1.5f, col);
  CornerFan(dl, ImVec2(b.x - r, a.y + r), r, pi * 1.5f, pi * 2.0f, col);
  if (roundBottom) {
    CornerFan(dl, ImVec2(b.x - r, b.y - r), r, 0.0f, pi * 0.5f, col);
    CornerFan(dl, ImVec2(a.x + r, b.y - r), r, pi * 0.5f, pi, col);
  }
}

inline void Glass(ImDrawList *dl, ImVec2 a, ImVec2 b, float radius = kRadius,
                  float opacity = 1.0f, bool band = true) {
  FillRoundRect(dl, a, b, alpha(glassFill(), opacity), radius);
  if (band)
    FillRoundRect(dl, a, ImVec2(b.x, a.y + kBandH),
                  alpha(glassSheen(), opacity), radius, false); // title band
  dl->AddRect(a, b, alpha(glassEdge(), opacity), radius, 0, 1.5f);
}

// Console-style focus: white ring with a soft cyan halo. `t` = 0..1 strength.
inline void FocusRing(ImDrawList *dl, ImVec2 a, ImVec2 b, float radius, float t,
                      float pulse = 0.0f) {
  if (t <= 0.01f)
    return;
  for (int i = 3; i >= 1; --i) {
    float g = 5.0f * i;
    dl->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g),
                rgba(0x00, 0xA2, 0xFF, (int)(26 * t / i)), radius + g, 0, 6.0f);
  }
  dl->AddRect(a, b,
              rgba(255, 255, 255, (int)(255 * t * (0.86f + 0.14f * pulse))),
              radius, 0, 3.5f);
}

inline void RowHighlight(ImDrawList *dl, ImVec2 a, ImVec2 b, float t,
                         float pulse = 0.0f) {
  if (t <= 0.01f)
    return;
  FillRoundRect(dl, a, b, rgba(255, 255, 255, (int)(30 * t)), 16.0f);
  FocusRing(dl, a, b, 16.0f, t, pulse);
}

inline float Pill(ImDrawList *dl, float x, float y, float size,
                  const char *label, ImU32 bg, ImU32 fg) {
  ImVec2 ts = Measure(size, label);
  float h = size + 12.0f, w = ts.x + 26.0f;
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, h * 0.5f);
  Text(dl, x + 13.0f, y + (h - ts.y) * 0.5f, size, fg, label);
  return w;
}

enum class Icon {
  Library,
  Download,
  Gear,
  Disk,
  Check,
  Alert,
  Pin,
  Folder,
  EyeOff,
  Chevron,
  ChevronDown,
  Network,
  Thermo,
  Plug
};

inline void DrawIcon(ImDrawList *dl, ImVec2 p, float s, Icon icon, ImU32 col) {
  const float th = s * 0.09f;
  ImVec2 c(p.x + s * 0.5f, p.y + s * 0.5f);
  switch (icon) {
  case Icon::Library: {
    float g = s * 0.12f, q = (s - g) * 0.5f;
    for (int i = 0; i < 4; ++i) {
      float x = p.x + (i % 2) * (q + g), y = p.y + (i / 2) * (q + g);
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + q, y + q), col, s * 0.08f);
    }
    break;
  }
  case Icon::Download:
    dl->AddLine(ImVec2(c.x, p.y + s * 0.08f), ImVec2(c.x, p.y + s * 0.62f), col,
                th);
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.24f, p.y + s * 0.44f),
                          ImVec2(c.x + s * 0.24f, p.y + s * 0.44f),
                          ImVec2(c.x, p.y + s * 0.72f), col);
    dl->AddRectFilled(ImVec2(p.x + s * 0.1f, p.y + s * 0.84f),
                      ImVec2(p.x + s * 0.9f, p.y + s * 0.84f + th), col, th);
    break;
  case Icon::Gear: {
    float ro = s * 0.46f, ri = s * 0.30f;
    for (int i = 0; i < 8; ++i) {
      float a = i * 3.14159265f / 4.0f;
      ImVec2 d(std::cos(a), std::sin(a));
      dl->AddLine(ImVec2(c.x + d.x * ri, c.y + d.y * ri),
                  ImVec2(c.x + d.x * ro, c.y + d.y * ro), col, s * 0.15f);
    }
    dl->AddCircle(c, ri, col, 24, th * 1.6f);
    break;
  }
  case Icon::Disk:
    dl->AddRect(ImVec2(p.x + s * 0.08f, p.y + s * 0.22f),
                ImVec2(p.x + s * 0.92f, p.y + s * 0.78f), col, s * 0.12f, 0,
                th);
    dl->AddCircleFilled(ImVec2(p.x + s * 0.74f, c.y), s * 0.07f, col);
    dl->AddLine(ImVec2(p.x + s * 0.24f, c.y), ImVec2(p.x + s * 0.52f, c.y), col,
                th);
    break;
  case Icon::Check:
    dl->AddLine(ImVec2(p.x + s * 0.18f, p.y + s * 0.52f),
                ImVec2(p.x + s * 0.42f, p.y + s * 0.74f), col, s * 0.13f);
    dl->AddLine(ImVec2(p.x + s * 0.42f, p.y + s * 0.74f),
                ImVec2(p.x + s * 0.84f, p.y + s * 0.28f), col, s * 0.13f);
    break;
  case Icon::Alert:
    dl->AddLine(ImVec2(c.x, p.y + s * 0.18f), ImVec2(c.x, p.y + s * 0.6f), col,
                s * 0.13f);
    dl->AddCircleFilled(ImVec2(c.x, p.y + s * 0.8f), s * 0.08f, col);
    break;
  case Icon::Pin:
    dl->AddCircleFilled(ImVec2(c.x, p.y + s * 0.36f), s * 0.24f, col);
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.16f, p.y + s * 0.5f),
                          ImVec2(c.x + s * 0.16f, p.y + s * 0.5f),
                          ImVec2(c.x, p.y + s * 0.95f), col);
    break;
  case Icon::Folder:
    dl->AddRectFilled(ImVec2(p.x + s * 0.06f, p.y + s * 0.18f),
                      ImVec2(p.x + s * 0.46f, p.y + s * 0.34f), col, s * 0.05f);
    dl->AddRectFilled(ImVec2(p.x + s * 0.06f, p.y + s * 0.28f),
                      ImVec2(p.x + s * 0.94f, p.y + s * 0.84f), col, s * 0.08f);
    break;
  case Icon::EyeOff:
    dl->AddCircle(c, s * 0.2f, col, 20, th);
    dl->AddLine(ImVec2(p.x + s * 0.12f, p.y + s * 0.88f),
                ImVec2(p.x + s * 0.88f, p.y + s * 0.12f), col, th);
    break;
  case Icon::Chevron:
    dl->AddLine(ImVec2(p.x + s * 0.36f, p.y + s * 0.2f),
                ImVec2(p.x + s * 0.66f, c.y), col, th * 1.2f);
    dl->AddLine(ImVec2(p.x + s * 0.66f, c.y),
                ImVec2(p.x + s * 0.36f, p.y + s * 0.8f), col, th * 1.2f);
    break;
  case Icon::ChevronDown:
    dl->AddLine(ImVec2(p.x + s * 0.2f, p.y + s * 0.36f),
                ImVec2(c.x, p.y + s * 0.66f), col, th * 1.2f);
    dl->AddLine(ImVec2(c.x, p.y + s * 0.66f),
                ImVec2(p.x + s * 0.8f, p.y + s * 0.36f), col, th * 1.2f);
    break;
  case Icon::Plug: // plug: two prongs, body, cable
    dl->AddLine(ImVec2(p.x + s * 0.36f, p.y + s * 0.06f),
                ImVec2(p.x + s * 0.36f, p.y + s * 0.3f), col, th * 1.3f);
    dl->AddLine(ImVec2(p.x + s * 0.64f, p.y + s * 0.06f),
                ImVec2(p.x + s * 0.64f, p.y + s * 0.3f), col, th * 1.3f);
    dl->AddRectFilled(ImVec2(p.x + s * 0.2f, p.y + s * 0.3f),
                      ImVec2(p.x + s * 0.8f, p.y + s * 0.62f), col, s * 0.1f);
    dl->AddLine(ImVec2(c.x, p.y + s * 0.62f), ImVec2(c.x, p.y + s * 0.94f), col,
                th * 1.3f);
    break;
  case Icon::Thermo:
    dl->AddLine(ImVec2(c.x, p.y + s * 0.12f), ImVec2(c.x, p.y + s * 0.62f), col,
                s * 0.16f);
    dl->AddCircleFilled(ImVec2(c.x, p.y + s * 0.74f), s * 0.17f, col, 16);
    break;
  case Icon::Network:
    for (int i = 0; i < 3; ++i) {
      float h = s * (0.3f + 0.3f * i);
      float x = p.x + s * (0.12f + 0.3f * i);
      dl->AddRectFilled(ImVec2(x, p.y + s - h), ImVec2(x + s * 0.18f, p.y + s),
                        col, s * 0.04f);
    }
    break;
  }
}

inline void Spinner(ImDrawList *dl, ImVec2 c, float r, float t, ImU32 col) {
  dl->AddCircle(c, r, alpha(col, 0.18f), 48, r * 0.16f);
  float a0 = t * 5.0f;
  dl->PathArcTo(c, r, a0, a0 + 1.9f, 32);
  dl->PathStroke(col, 0, r * 0.16f);
}

enum class Glyph { Cross, Circle, Square, Triangle, L1, R1 };

inline float ButtonGlyph(ImDrawList *dl, ImVec2 p, float d, Glyph g,
                         float a = 1.0f) {
  ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
  float k = d * 0.21f, th = d * 0.085f;
  if (g == Glyph::L1 || g == Glyph::R1) {
    float w = d * 1.55f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + d),
                      rgba(255, 255, 255, (int)(34 * a)), d * 0.32f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + d), rgba(255, 255, 255, (int)(60 * a)),
                d * 0.32f, 0, 1.5f);
    const char *t = g == Glyph::L1 ? "L1" : "R1";
    float fs = d * 0.52f;
    ImVec2 ts = Measure(fs, t);
    Text(dl, p.x + (w - ts.x) * 0.5f, p.y + (d - ts.y) * 0.5f, fs,
         alpha(textPrimary(), a), t);
    return w;
  }
  dl->AddCircleFilled(c, d * 0.5f, rgba(255, 255, 255, (int)(30 * a)), 32);
  switch (g) {
  case Glyph::Cross:
    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k),
                rgba(0x8C, 0xC0, 0xF2, (int)(255 * a)), th);
    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k),
                rgba(0x8C, 0xC0, 0xF2, (int)(255 * a)), th);
    break;
  case Glyph::Circle:
    dl->AddCircle(c, k * 1.05f, rgba(0xFF, 0x76, 0x86, (int)(255 * a)), 24, th);
    break;
  case Glyph::Square:
    dl->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k),
                rgba(0xE8, 0x9C, 0xDE, (int)(255 * a)), 0, 0, th);
    break;
  case Glyph::Triangle:
    dl->AddTriangle(ImVec2(c.x, c.y - k * 1.1f),
                    ImVec2(c.x + k * 1.1f, c.y + k * 0.8f),
                    ImVec2(c.x - k * 1.1f, c.y + k * 0.8f),
                    rgba(0x5C, 0xE0, 0xC2, (int)(255 * a)), th);
    break;
  default:
    break;
  }
  return d;
}

} // namespace Theme
