#pragma once
#include <cmath>
#include <map>
#include <string>

#include "imgui.h"

namespace Anim {

// Set whenever a value is still moving; the main loop keeps redrawing while
// true.
inline bool &activeFlag() {
  static bool f = false;
  return f;
}
inline void beginFrame() { activeFlag() = false; }
inline bool active() { return activeFlag(); }

inline float clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline ImVec2 lerp(ImVec2 a, ImVec2 b, float t) {
  return ImVec2(lerp(a.x, b.x, t), lerp(a.y, b.y, t));
}
inline float easeOutCubic(float t) {
  t = clamp01(t);
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}
inline float easeInOutSine(float t) {
  return 0.5f - 0.5f * std::cos(clamp01(t) * 3.14159265f);
}

// Exponential approach: `speed` ~ 1/time-constant (12 = snappy, 6 = soft).
inline float approach(float &v, float target, float speed, float dt,
                      float eps = 0.002f) {
  float k = 1.0f - std::exp(-speed * dt);
  v += (target - v) * k;
  if (std::fabs(target - v) < eps)
    v = target;
  else
    activeFlag() = true;
  return v;
}
inline ImVec2 approach(ImVec2 &v, ImVec2 target, float speed, float dt) {
  approach(v.x, target.x, speed, dt, 0.25f);
  approach(v.y, target.y, speed, dt, 0.25f);
  return v;
}

// Damped spring (semi-implicit Euler, sub-stepped for stability). A damping
// ratio a little under 1 gives the small, quick overshoot console UIs use.
struct Spring {
  float x = 0.0f, v = 0.0f;
  Spring() = default;
  explicit Spring(float start) : x(start) {}
  float step(float target, float dt, float freqHz = 4.5f, float zeta = 0.78f) {
    const float w = 6.2831853f * freqHz;
    int n = dt > 0.0f ? (int)std::ceil(dt / 0.004f) : 0;
    float h = n ? dt / n : 0.0f;
    for (int i = 0; i < n; ++i) {
      float a = -2.0f * zeta * w * v - w * w * (x - target);
      v += a * h;
      x += v * h;
    }
    if (std::fabs(x - target) < 0.0015f && std::fabs(v) < 0.01f) {
      x = target;
      v = 0.0f;
    } else
      activeFlag() = true;
    return x;
  }
};

struct Spring2 {
  Spring x, y;
  Spring2() = default;
  explicit Spring2(ImVec2 p) : x(p.x), y(p.y) {}
  ImVec2 step(ImVec2 t, float dt, float freqHz = 4.0f, float zeta = 0.9f) {
    return ImVec2(x.step(t.x, dt, freqHz, zeta), y.step(t.y, dt, freqHz, zeta));
  }
};

// Per-key animated values (e.g. one per tile) that survive between frames.
template <typename T> struct Bank {
  std::map<std::string, T> values;
  T &get(const std::string &key, const T &initial) {
    auto it = values.find(key);
    if (it == values.end())
      it = values.emplace(key, initial).first;
    return it->second;
  }
};

// Transforms all vertices added to `dl` between construction and apply().
struct VtxScope {
  ImDrawList *dl;
  int start;
  explicit VtxScope(ImDrawList *d) : dl(d), start(d->VtxBuffer.Size) {}
  void apply(float alpha, ImVec2 offset = ImVec2(0, 0), float scale = 1.0f,
             ImVec2 pivot = ImVec2(0, 0)) {
    const bool doScale = scale != 1.0f,
               doMove = offset.x != 0.0f || offset.y != 0.0f;
    const int a = (int)(clamp01(alpha) * 256.0f);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
      ImDrawVert &v = dl->VtxBuffer.Data[i];
      if (doScale) {
        v.pos.x = pivot.x + (v.pos.x - pivot.x) * scale;
        v.pos.y = pivot.y + (v.pos.y - pivot.y) * scale;
      }
      if (doMove) {
        v.pos.x += offset.x;
        v.pos.y += offset.y;
      }
      if (a < 256) {
        ImU32 va = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        v.col =
            (v.col & ~IM_COL32_A_MASK) | (((va * a) >> 8) << IM_COL32_A_SHIFT);
      }
    }
  }
};

} // namespace Anim
