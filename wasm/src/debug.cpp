// Introspection for tests and troubleshooting: what the in-browser HAL holds.
//
// The GUI draws straight from this HAL, so "the device is in here" is the
// same statement as "the device is in the Other Devices window" without
// having to read pixels. Call from the page:
//   JSON.parse(Module.ccall("websim_debug_state", "string", [], []))

#include <cstdlib>
#include <cstring>
#include <string>

#include <emscripten.h>
#include <hal/HALBase.h>
#include <hal/simulation/DriverStationData.h>
#include <hal/simulation/MockHooks.h>
#include <hal/simulation/NotifierData.h>
#include <hal/simulation/PWMData.h>
#include <hal/simulation/SimDeviceData.h>
#include <networktables/NetworkTableInstance.h>
#include <wpi/json.h>

#include "mirror.h"
#include "websim.h"

extern "C" EMSCRIPTEN_KEEPALIVE const char* websim_debug_state() {
  static std::string out;
  wpi::json devices = wpi::json::array();
  HALSIM_EnumerateSimDevices(
      "", &devices,
      [](const char* name, void* param, HAL_SimDeviceHandle handle) {
        wpi::json values = wpi::json::object();
        HALSIM_EnumerateSimValues(
            handle, &values,
            [](const char* valueName, void* p, HAL_SimValueHandle,
               int32_t direction, const HAL_Value* value) {
              wpi::json v;
              switch (value->type) {
                case HAL_BOOLEAN:
                  v = static_cast<bool>(value->data.v_boolean);
                  break;
                case HAL_DOUBLE:
                  v = value->data.v_double;
                  break;
                case HAL_ENUM:
                  v = value->data.v_enum;
                  break;
                case HAL_INT:
                  v = value->data.v_int;
                  break;
                case HAL_LONG:
                  v = value->data.v_long;
                  break;
                default:
                  break;
              }
              (*static_cast<wpi::json*>(p))[valueName] = {
                  {"direction", direction},
                  {"type", static_cast<int>(value->type)},
                  {"value", v}};
            });
        static_cast<wpi::json*>(param)->push_back(
            {{"name", name}, {"values", values}});
      });

  wpi::json pwm = wpi::json::array();
  for (int i = 0; i < 10; ++i) {
    if (HALSIM_GetPWMInitialized(i)) {
      pwm.push_back({{"channel", i}, {"speed", HALSIM_GetPWMSpeed(i)}});
    }
  }

  wpi::json notifiers = wpi::json::array();
  HALSIM_NotifierInfo info[32];
  int32_t n = HALSIM_GetNotifierInfo(info, 32);
  for (int32_t i = 0; i < n && i < 32; ++i) {
    notifiers.push_back(info[i].name);
  }

  int32_t status = 0;
  wpi::json state = {
      {"hal",
       {{"connected", websim::mirror::Connected()},
        {"bridge", websim::BridgeActive()}}},
      {"nt",
       {{"connected",
         nt::NetworkTableInstance::GetDefault().IsConnected()}}},
      {"ds",
       {{"enabled", static_cast<bool>(HALSIM_GetDriverStationEnabled())},
        {"autonomous", static_cast<bool>(HALSIM_GetDriverStationAutonomous())},
        {"dsAttached", static_cast<bool>(HALSIM_GetDriverStationDsAttached())}}},
      {"timing",
       {{"fpgaTime", HAL_GetFPGATime(&status)},
        {"paused", static_cast<bool>(HALSIM_IsTimingPaused())},
        {"notifiers", notifiers}}},
      {"pwm", pwm},
      {"simDevices", devices},
  };
  out = state.dump();
  return out.c_str();
}
