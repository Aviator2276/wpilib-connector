// Shared declarations for the web sim app code (everything in wasm/src).
#pragma once

#include <string>
#include <string_view>

extern "C" {
// Ask for frames to be drawn for the next `ms` milliseconds (0 = one frame).
// Called on input, on NetworkTables / HAL data, and by the page.
void websim_wake(int ms);

// Redraw-on-demand gate used by the patched wpigui main loop (frame.cpp).
int websim_frame_wanted(int fps);
}

namespace websim {

// Read a string property of the Emscripten Module object ("" when unset).
std::string ModuleString(const char* key);

// True if a GUI window whose name starts with `prefix` was drawn last frame.
bool WindowVisible(std::string_view prefix);

// Connects the in-browser HAL to the robot's HAL (see mirror.cpp).
void StartHalMirror();

// True while the robot-side native bridge is feeding the mirror.
bool BridgeActive();

}  // namespace websim
