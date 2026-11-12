// Entry point of the web Sim GUI.
//
// Boots upstream's halsim_gui extension, unmodified, against an in-browser
// copy of the sim HAL. NetworkTables comes from an NT4 client connected to
// the robot's server; hardware state comes from the HAL mirror.

#include <cstdio>
#include <cstdlib>

#include <GLFW/emscripten_glfw3.h>
#include <emscripten.h>
#include <hal/HAL.h>
#include <hal/Main.h>
#include <networktables/NetworkTableInstance.h>
#include <wpigui.h>

#include "websim.h"

extern "C" int HALSIM_InitExtension(void);

std::string websim::ModuleString(const char* key) {
  char* raw = static_cast<char*>(EM_ASM_PTR(
      {
        var v = Module[UTF8ToString($0)];
        return stringToNewUTF8(typeof v === "string" ? v : "");
      },
      key));
  std::string out = raw;
  std::free(raw);
  return out;
}

int main() {
  std::printf("WPILib Web Sim GUI (WPILib %s)\n", WEBSIM_WPILIB_VERSION);

  if (!HAL_Initialize(500, 0)) {
    std::puts("HAL_Initialize failed");
    return 1;
  }

  // The desktop Sim GUI shares the robot's NT server instance. Here the GUI
  // is an NT4 client; the address comes from Module.ntUrl (see the ntcore
  // NetworkClient replacement), so the host and port below are placeholders.
  auto inst = nt::NetworkTableInstance::GetDefault();
  inst.StartClient4("websim");
  inst.SetServer("localhost", 5810);

  HALSIM_InitExtension();

  // Follow the size of the host element (the shell's canvas container, or
  // the window) instead of the fixed 1280x720 the GUI asks for.
  if (GLFWwindow* window = wpi::gui::GetSystemWindow()) {
    std::string selector = websim::ModuleString("resizeSelector");
    emscripten_glfw_make_canvas_resizable(
        window, selector.empty() ? "window" : selector.c_str(), nullptr);
  }

  websim::StartHalMirror();

  EM_ASM({
    if (Module["onSimGuiReady"]) {
      Module["onSimGuiReady"]();
    }
  });

  // Hands control to the GUI's main loop, which the browser drives.
  HAL_RunMain();
  return 0;
}
