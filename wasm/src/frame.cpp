// Redraw-on-demand.
//
// Dear ImGui redraws every frame by default. In a browser tab that wastes a
// Chromebook's battery when nothing is happening, so a frame is drawn only
// when one of these holds:
//   - input arrived in the last 250 ms (the page calls websim_wake on DOM
//     events), or something asked for a burst of frames;
//   - something is animating (the robot is enabled, or a plot is on screen):
//     about 30 frames a second, the full display rate only during input;
//   - NetworkTables or HAL data arrived, at most kDataFps times a second
//     (vendor devices report sensor noise continuously, so "data arrived" is
//     true all the time and must not mean "draw at the display rate");
//   - the idle heartbeat is due (keeps clocks and cursors roughly current).
// This changes when frames are drawn, never what they contain.

#include <string_view>

#include <emscripten.h>
#include <hal/simulation/DriverStationData.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <wpigui_internal.h>

#include "websim.h"

namespace {
constexpr double kIdleIntervalMs = 500;
constexpr double kDataIntervalMs = 100;  // kDataFps = 10
constexpr double kAnimationIntervalMs = 32;  // about 30 fps without input
double gWakeUntil = 1000;  // draw freely while the GUI starts up
bool gDataPending = true;
double gLastFrame = 0;
bool gAlways = false;
}  // namespace

bool websim::WindowVisible(std::string_view prefix) {
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx) {
    return false;
  }
  for (ImGuiWindow* win : ctx->Windows) {
    if (win->WasActive && !win->Hidden && !win->Collapsed &&
        std::string_view{win->Name}.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

extern "C" {

// ms > 0: draw every frame for the next `ms` milliseconds (input, state
// changes). ms == 0: new data is waiting to be shown (rate-limited).
EMSCRIPTEN_KEEPALIVE void websim_wake(int ms) {
  if (ms > 0) {
    double until = emscripten_get_now() + ms;
    if (until > gWakeUntil) {
      gWakeUntil = until;
    }
  } else {
    gDataPending = true;
  }
}

// Write the GUI's layout files now instead of at ImGui's next periodic save.
// A browser tab gives no reliable "window closed" moment, so the page calls
// this when it is hidden or unloaded.
EMSCRIPTEN_KEEPALIVE void websim_save_settings() {
  if (wpi::gui::gContext && wpi::gui::gContext->saveSettings) {
    wpi::gui::gContext->saveSettings(false);
  }
}

// Whole-number pixel density the font atlas is rasterized at (patched
// wpigui): 1 on ordinary screens, 2 or 3 on HiDPI ones.
int websim_font_density(void) {
  double dpr = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1; });
  int density = static_cast<int>(dpr + 0.75);  // 1.25 and up rounds to 2
  return density < 1 ? 1 : density > 3 ? 3 : density;
}

// Debug / test aid: 1 disables redraw-on-demand.
EMSCRIPTEN_KEEPALIVE void websim_set_always_redraw(int enable) {
  gAlways = enable != 0;
}

}  // extern "C"

// Called by the patched wpigui main loop on every requestAnimationFrame tick.
extern "C" int websim_frame_wanted(int fps) {
  double now = emscripten_get_now();
  // FPS cap from the saved GUI settings (requestAnimationFrame already caps
  // at the display rate). 2 ms of slack so a 60 Hz display is not halved.
  if (fps > 0 && (now - gLastFrame) < (1000.0 / fps) - 2.0) {
    return false;
  }
  double sinceLast = now - gLastFrame;
  bool wanted = gAlways || now < gWakeUntil ||
                (gDataPending && sinceLast >= kDataIntervalMs) ||
                sinceLast >= kIdleIntervalMs ||
                (sinceLast >= kAnimationIntervalMs &&
                 (HALSIM_GetDriverStationEnabled() ||
                  websim::WindowVisible("Plot <")));  // glass plot windows
  if (!wanted) {
    return false;
  }
  gDataPending = false;
  gLastFrame = now;
  return true;
}
