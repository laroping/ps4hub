#pragma once
#include <SDL2/SDL.h>
#include <cstdint>

enum class Pad : int {
  Up,
  Down,
  Left,
  Right,
  Cross,
  Circle,
  Square,
  Triangle,
  L1,
  R1,
  Options,
  Count
};

class Gamepad {
public:
  bool open();
  void close();

  bool handleEvent(const SDL_Event &ev);

  void update(uint32_t nowMs);

  bool pressed(Pad b) const { return pressed_[(int)b]; }
  bool held(Pad b) const { return held_[(int)b]; }
  bool anyActivity() const { return activity_; }
  bool connected() const { return controller_ || joystick_; }

  void endFrame();

private:
  void set(Pad b, bool down, uint32_t nowMs);

  SDL_GameController *controller_ = nullptr;
  SDL_Joystick *joystick_ = nullptr;

  bool held_[(int)Pad::Count] = {};
  bool pressed_[(int)Pad::Count] = {};
  uint32_t nextRepeat_[(int)Pad::Count] = {};
  bool activity_ = false;
  bool stickUp_ = false, stickDown_ = false;
};
