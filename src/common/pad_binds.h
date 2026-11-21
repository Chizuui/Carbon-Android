#pragma once
#include <cstddef>
#include <string>

#include "common/mouse_keyboard.h"

// a controller's buttons, hats and axes onto the Xbox pad's, for the CONTROLLER page (native/input.cpp). An action
// with no input of its own keeps SDL's mapping; an input bound to an action no longer presses its mapped one.
namespace input {

const Action* PadActions(size_t* count);  // `defaults` names the mapping's input
std::string PadBinding(size_t action);     // the bound input's name, empty for the mapping's
void PadBind(size_t action, const std::string& source);  // empty goes back to the mapping
void ResetPadBindings();
void LoadPadBindings();      // after the settings load
std::string PressedPadInput();  // an input of any controller pressed now, empty for none
bool PadBindingIsAxis(size_t action);  // the bound input is an axis, shown as "(axis)"

// the first connected gamepad's live state, for the CONTROLLER page's input test
struct PadLive {
    bool connected = false;
    std::string name;
    bool south = false, east = false, west = false, north = false;  // A B X Y
    bool lb = false, rb = false;                                     // L1 R1
    bool start = false, back = false, ls = false, rs = false;        // L3 R3
    bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, y up positive
    float lt = 0, rt = 0;                  // 0..1
};
bool PadLiveState(PadLive& out);

}  // namespace input
