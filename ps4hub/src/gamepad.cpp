#include "gamepad.h"

namespace {

constexpr uint32_t kRepeatDelayMs = 380;
constexpr uint32_t kRepeatRateMs = 70;
constexpr int kStickDeadzone = 16000;

// Raw button indices of the OpenOrbis SDL2 joystick driver (see samples/SDL2).
enum RawButton {
  RAW_CROSS = 0,
  RAW_CIRCLE,
  RAW_SQUARE,
  RAW_TRIANGLE,
  RAW_L1,
  RAW_R1,
  RAW_OPTIONS = 9,
  RAW_L3 = 11,
  RAW_R3,
  RAW_UP,
  RAW_DOWN,
  RAW_LEFT,
  RAW_RIGHT,
};

bool mapRaw(int b, Pad &out) {
  switch (b) {
  case RAW_CROSS:
    out = Pad::Cross;
    return true;
  case RAW_CIRCLE:
    out = Pad::Circle;
    return true;
  case RAW_SQUARE:
    out = Pad::Square;
    return true;
  case RAW_TRIANGLE:
    out = Pad::Triangle;
    return true;
  case RAW_L1:
    out = Pad::L1;
    return true;
  case RAW_R1:
    out = Pad::R1;
    return true;
  case RAW_OPTIONS:
    out = Pad::Options;
    return true;
  case RAW_UP:
    out = Pad::Up;
    return true;
  case RAW_DOWN:
    out = Pad::Down;
    return true;
  case RAW_LEFT:
    out = Pad::Left;
    return true;
  case RAW_RIGHT:
    out = Pad::Right;
    return true;
  }
  return false;
}

bool mapController(int b, Pad &out) {
  switch (b) {
  case SDL_CONTROLLER_BUTTON_A:
    out = Pad::Cross;
    return true;
  case SDL_CONTROLLER_BUTTON_B:
    out = Pad::Circle;
    return true;
  case SDL_CONTROLLER_BUTTON_X:
    out = Pad::Square;
    return true;
  case SDL_CONTROLLER_BUTTON_Y:
    out = Pad::Triangle;
    return true;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    out = Pad::L1;
    return true;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    out = Pad::R1;
    return true;
  case SDL_CONTROLLER_BUTTON_START:
    out = Pad::Options;
    return true;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    out = Pad::Up;
    return true;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    out = Pad::Down;
    return true;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    out = Pad::Left;
    return true;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    out = Pad::Right;
    return true;
  }
  return false;
}

bool repeats(Pad b) {
  return b == Pad::Up || b == Pad::Down || b == Pad::L1 || b == Pad::R1;
}

} // namespace

bool Gamepad::open() {
  close();
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) {
      controller_ = SDL_GameControllerOpen(i);
      if (controller_)
        return true;
    }
  }
  if (SDL_NumJoysticks() > 0)
    joystick_ = SDL_JoystickOpen(0);
  return joystick_ != nullptr;
}

void Gamepad::close() {
  if (controller_)
    SDL_GameControllerClose(controller_);
  if (joystick_)
    SDL_JoystickClose(joystick_);
  controller_ = nullptr;
  joystick_ = nullptr;
}

void Gamepad::set(Pad b, bool down, uint32_t nowMs) {
  int i = (int)b;
  if (down && !held_[i]) {
    pressed_[i] = true;
    nextRepeat_[i] = nowMs + kRepeatDelayMs;
  }
  held_[i] = down;
  activity_ = true;
}

bool Gamepad::handleEvent(const SDL_Event &ev) {
  uint32_t now = SDL_GetTicks();
  Pad b;
  switch (ev.type) {
  case SDL_CONTROLLERDEVICEADDED:
  case SDL_JOYDEVICEADDED:
    if (!connected())
      open();
    return true;
  case SDL_CONTROLLERDEVICEREMOVED:
  case SDL_JOYDEVICEREMOVED:
    open();
    return true;

  // Exactly one of the two paths is active, never both, so an opened
  // GameController does not double-report through its joystick events.
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP:
    if (!controller_ || !mapController(ev.cbutton.button, b))
      return false;
    set(b, ev.type == SDL_CONTROLLERBUTTONDOWN, now);
    return true;
  case SDL_CONTROLLERAXISMOTION:
    if (!controller_ || ev.caxis.axis != SDL_CONTROLLER_AXIS_LEFTY)
      return false;
    {
      bool up =
          ev.caxis.value<-kStickDeadzone, down = ev.caxis.value> kStickDeadzone;
      if (up != stickUp_) {
        stickUp_ = up;
        set(Pad::Up, up, now);
      }
      if (down != stickDown_) {
        stickDown_ = down;
        set(Pad::Down, down, now);
      }
    }
    return true;

  case SDL_JOYBUTTONDOWN:
  case SDL_JOYBUTTONUP:
    if (controller_ || !mapRaw(ev.jbutton.button, b))
      return false;
    set(b, ev.type == SDL_JOYBUTTONDOWN, now);
    return true;
  case SDL_JOYHATMOTION:
    if (controller_)
      return false;
    set(Pad::Up, (ev.jhat.value & SDL_HAT_UP) != 0, now);
    set(Pad::Down, (ev.jhat.value & SDL_HAT_DOWN) != 0, now);
    set(Pad::Left, (ev.jhat.value & SDL_HAT_LEFT) != 0, now);
    set(Pad::Right, (ev.jhat.value & SDL_HAT_RIGHT) != 0, now);
    return true;
  case SDL_JOYAXISMOTION:
    if (controller_ || ev.jaxis.axis != 1)
      return false;
    {
      bool up =
          ev.jaxis.value<-kStickDeadzone, down = ev.jaxis.value> kStickDeadzone;
      if (up != stickUp_) {
        stickUp_ = up;
        set(Pad::Up, up, now);
      }
      if (down != stickDown_) {
        stickDown_ = down;
        set(Pad::Down, down, now);
      }
    }
    return true;
  }
  return false;
}

void Gamepad::update(uint32_t nowMs) {
  for (int i = 0; i < (int)Pad::Count; ++i) {
    if (!held_[i] || !repeats((Pad)i))
      continue;
    if ((int32_t)(nowMs - nextRepeat_[i]) >= 0) {
      pressed_[i] = true;
      nextRepeat_[i] = nowMs + kRepeatRateMs;
      activity_ = true;
    }
  }
}

void Gamepad::endFrame() {
  for (bool &p : pressed_)
    p = false;
  activity_ = false;
}
