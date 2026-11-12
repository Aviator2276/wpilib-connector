// C++ view of hal_fields.def: one row per mirrored HAL value, with typed
// accessors generated from the HALSIM function names. Header-only so the
// browser mirror and the native bridge share the exact same table.

#pragma once

#include <stdint.h>

#include <span>
#include <string_view>

#include <hal/Accelerometer.h>
#include <hal/DriverStationTypes.h>
#include <hal/LEDs.h>
#include <hal/Ports.h>
#include <hal/REVPH.h>
#include <hal/Value.h>
#include <hal/simulation/AccelerometerData.h>
#include <hal/simulation/AddressableLEDData.h>
#include <hal/simulation/AnalogGyroData.h>
#include <hal/simulation/AnalogInData.h>
#include <hal/simulation/AnalogOutData.h>
#include <hal/simulation/CTREPCMData.h>
#include <hal/simulation/DIOData.h>
#include <hal/simulation/DigitalPWMData.h>
#include <hal/simulation/DriverStationData.h>
#include <hal/simulation/DutyCycleData.h>
#include <hal/simulation/EncoderData.h>
#include <hal/simulation/PWMData.h>
#include <hal/simulation/PowerDistributionData.h>
#include <hal/simulation/REVPHData.h>
#include <hal/simulation/RelayData.h>
#include <hal/simulation/RoboRioData.h>

namespace websim {

enum class Dir : uint8_t { OUT, IN, BIDIR };

// Device classes, numbered by position in hal_fields.def.
enum class Cls : uint8_t {
#define WEBSIM_CLASS(cls, wsType, count) cls,
#define WEBSIM_FIELD(...)
#define WEBSIM_FIELD0(...)
#define WEBSIM_FIELD2(...)
#include "hal_fields.def"
  kCount
};

struct HalClass {
  Cls cls;
  const char* name;
  const char* wsType;  // halsim_ws "type", or nullptr
  int32_t (*count)();  // number of instances
};

struct HalField {
  Cls cls;
  const char* name;
  HAL_Type type;
  Dir dir;
  const char* wsKey;      // halsim_ws data key, or nullptr
  int32_t (*channels)();  // > 0 for (index, channel) fields
  // `index` is the instance, or instance * 256 + channel for channel fields.
  void (*set)(int32_t index, const HAL_Value& value);
  int32_t (*reg)(int32_t index, HAL_NotifyCallback cb, void* param,
                 HAL_Bool initialNotify);
  void (*cancel)(int32_t index, int32_t uid);
};

inline constexpr HalClass kHalClasses[] = {
#define WEBSIM_CLASS(cls, wsType, count) \
  {Cls::cls, #cls, wsType, []() -> int32_t { return count; }},
#define WEBSIM_FIELD(...)
#define WEBSIM_FIELD0(...)
#define WEBSIM_FIELD2(...)
#include "hal_fields.def"
};

#define WEBSIM_HALTYPE_boolean HAL_BOOLEAN
#define WEBSIM_HALTYPE_double HAL_DOUBLE
#define WEBSIM_HALTYPE_int HAL_INT
#define WEBSIM_HALTYPE_long HAL_LONG
#define WEBSIM_HALTYPE_enum HAL_ENUM

inline constexpr HalField kHalFields[] = {
#define WEBSIM_CLASS(cls, wsType, count)
#define WEBSIM_FIELD(cls, mod, name, haltype, ctype, dir, wsKey)             \
  {Cls::cls, #name, WEBSIM_HALTYPE_##haltype, Dir::dir, wsKey, nullptr,      \
   [](int32_t i, const HAL_Value& v) {                                       \
     HALSIM_Set##mod##name(i, static_cast<ctype>(v.data.v_##haltype));       \
   },                                                                        \
   [](int32_t i, HAL_NotifyCallback cb, void* p, HAL_Bool init) -> int32_t { \
     return HALSIM_Register##mod##name##Callback(i, cb, p, init);            \
   },                                                                        \
   [](int32_t i, int32_t uid) { HALSIM_Cancel##mod##name##Callback(i, uid); }},
#define WEBSIM_FIELD0(cls, mod, name, haltype, ctype, dir, wsKey)            \
  {Cls::cls, #name, WEBSIM_HALTYPE_##haltype, Dir::dir, wsKey, nullptr,      \
   [](int32_t, const HAL_Value& v) {                                         \
     HALSIM_Set##mod##name(static_cast<ctype>(v.data.v_##haltype));          \
   },                                                                        \
   [](int32_t, HAL_NotifyCallback cb, void* p, HAL_Bool init) -> int32_t {   \
     return HALSIM_Register##mod##name##Callback(cb, p, init);               \
   },                                                                        \
   [](int32_t, int32_t uid) { HALSIM_Cancel##mod##name##Callback(uid); }},
#define WEBSIM_FIELD2(cls, mod, name, haltype, ctype, dir, wsKey, nch)       \
  {Cls::cls, #name, WEBSIM_HALTYPE_##haltype, Dir::dir, wsKey,               \
   []() -> int32_t { return nch; },                                          \
   [](int32_t i, const HAL_Value& v) {                                       \
     HALSIM_Set##mod##name(i >> 8, i & 0xff,                                 \
                           static_cast<ctype>(v.data.v_##haltype));          \
   },                                                                        \
   [](int32_t i, HAL_NotifyCallback cb, void* p, HAL_Bool init) -> int32_t { \
     return HALSIM_Register##mod##name##Callback(i >> 8, i & 0xff, cb, p,    \
                                                 init);                      \
   },                                                                        \
   [](int32_t i, int32_t uid) {                                              \
     HALSIM_Cancel##mod##name##Callback(i >> 8, i & 0xff, uid);              \
   }},
#include "hal_fields.def"
};

inline constexpr int kNumHalClasses = sizeof(kHalClasses) / sizeof(HalClass);
inline constexpr int kNumHalFields = sizeof(kHalFields) / sizeof(HalField);

inline const HalClass& ClassOf(const HalField& f) {
  return kHalClasses[static_cast<int>(f.cls)];
}

// FNV-1a over the table's names and types. Both ends compare this on
// connect so a mirror never decodes a bridge built from a different table.
inline constexpr uint32_t HalTableHash() {
  uint32_t h = 2166136261u;
  auto mix = [&h](std::string_view s) {
    for (char c : s) {
      h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    h = (h ^ 0xffu) * 16777619u;
  };
  for (auto& c : kHalClasses) {
    mix(c.name);
  }
  for (auto& f : kHalFields) {
    mix(kHalClasses[static_cast<int>(f.cls)].name);
    mix(f.name);
    h = (h ^ static_cast<uint8_t>(f.type)) * 16777619u;
    h = (h ^ static_cast<uint8_t>(f.dir)) * 16777619u;
  }
  return h;
}

}  // namespace websim
