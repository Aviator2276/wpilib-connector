// Mirror HAL core: transport, local HAL callbacks, SimDevice bookkeeping and
// the link-time wrappers for the Timing window. See mirror.h.

#include "mirror.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <emscripten.h>
#include <emscripten/websocket.h>
#include <hal/DriverStationTypes.h>
#include <hal/HALBase.h>
#include <hal/simulation/AddressableLEDData.h>
#include <hal/simulation/DriverStationData.h>
#include <hal/simulation/MockHooks.h>
#include <hal/simulation/SimDeviceData.h>
#include <mockdata/EncoderDataInternal.h>

#include "websim.h"

using namespace websim;
using namespace websim::mirror;

namespace {

int gSocket = 0;
bool gOpen = false;
int gApplyDepth = 0;
bool gBridgeActive = false;
double gNextConnect = 0;
std::string gUrl;

std::map<std::string, std::unique_ptr<SimDev>, std::less<>> gSimDevices;

JoystickState gSentSticks[HAL_kMaxJoysticks];
bool gSticksSent = false;
std::string gSentGameData;
bool gGameDataKnown = false;

RemoteTiming gTiming;

Codec& ActiveCodec() {
  return gBridgeActive ? BridgeCodec() : JsonCodec();
}

// ---------------------------------------------------------------- transport

bool OnOpen(int, const EmscriptenWebSocketOpenEvent*, void*) {
  gOpen = true;
  gSticksSent = false;
  gGameDataKnown = false;
  std::puts("HAL mirror: connected");
  JsonCodec().OnOpen();
  BridgeCodec().OnOpen();
  websim_wake(250);
  return true;
}

bool OnClose(int, const EmscriptenWebSocketCloseEvent*, void*) {
  if (gOpen) {
    std::puts("HAL mirror: disconnected");
    JsonCodec().OnClose();
    BridgeCodec().OnClose();
  }
  gOpen = false;
  gBridgeActive = false;
  gTiming = RemoteTiming{};
  if (gSocket > 0) {
    emscripten_websocket_delete(gSocket);
    gSocket = 0;
  }
  gNextConnect = emscripten_get_now() + 1000;
  websim_wake(250);
  return true;
}

bool OnError(int, const EmscriptenWebSocketErrorEvent*, void*) {
  return true;  // a close event follows
}

bool OnMessage(int, const EmscriptenWebSocketMessageEvent* e, void*) {
  if (e->isText) {
    HandleJsonFrame({reinterpret_cast<const char*>(e->data),
                     e->numBytes > 0 ? e->numBytes - 1 : 0});
    websim_wake(0);
  } else {
    HandleBridgeFrame({e->data, e->numBytes});  // wakes only when needed
  }
  return true;
}

void TryConnect() {
  if (gUrl.empty() || gSocket > 0) {
    return;
  }
  EmscriptenWebSocketCreateAttributes attr;
  emscripten_websocket_init_create_attributes(&attr);
  attr.url = gUrl.c_str();
  attr.createOnMainThread = true;
  gSocket = emscripten_websocket_new(&attr);
  if (gSocket <= 0) {
    gSocket = 0;
    return;
  }
  emscripten_websocket_set_onopen_callback(gSocket, nullptr, OnOpen);
  emscripten_websocket_set_onclose_callback(gSocket, nullptr, OnClose);
  emscripten_websocket_set_onerror_callback(gSocket, nullptr, OnError);
  emscripten_websocket_set_onmessage_callback(gSocket, nullptr, OnMessage);
}

void Tick(void*) {
  if (!gOpen) {
    if (gSocket == 0 && emscripten_get_now() >= gNextConnect) {
      gNextConnect = emscripten_get_now() + 1000;
      TryConnect();
    }
    return;
  }
  ActiveCodec().Flush();
}

// ------------------------------------------------------- local HAL callbacks

// param packs (field << 16 | index); index fits 16 bits (module * 256 + ch).
void OnFieldChanged(const char*, void* param, const HAL_Value* value) {
  if (gApplyDepth > 0 || !gOpen) {
    return;
  }
  auto packed = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
  ActiveCodec().OnLocalField(packed >> 16, packed & 0xffff, *value);
}

void OnSimValueChanged(const char*, void* param, HAL_SimValueHandle,
                       int32_t, const HAL_Value* value) {
  if (gApplyDepth > 0 || !gOpen) {
    return;
  }
  ActiveCodec().OnLocalSimValue(*static_cast<SimValue*>(param), *value);
}

JoystickState ReadStick(int stick) {
  JoystickState s;
  HAL_JoystickAxes axes{};
  HALSIM_GetJoystickAxes(stick, &axes);
  s.axes.assign(axes.axes, axes.axes + axes.count);
  HAL_JoystickPOVs povs{};
  HALSIM_GetJoystickPOVs(stick, &povs);
  s.povs.assign(povs.povs, povs.povs + povs.count);
  HAL_JoystickButtons buttons{};
  HALSIM_GetJoystickButtons(stick, &buttons);
  s.buttons = buttons.buttons;
  s.buttonCount = buttons.count;
  return s;
}

// The DS window calls HALSIM_NotifyDriverStationNewData() about every 20 ms
// while attached. Joystick and match data have no per-value callbacks, so
// they ride on this tick, and only when they differ from what was last sent
// (which also keeps two open tabs from fighting over the sticks).
void OnNewData(const char*, void*, const HAL_Value*) {
  if (gApplyDepth > 0 || !gOpen) {
    return;
  }
  static JoystickState current[HAL_kMaxJoysticks];
  std::pair<int, const JoystickState*> changed[HAL_kMaxJoysticks];
  size_t numChanged = 0;
  for (int i = 0; i < HAL_kMaxJoysticks; ++i) {
    current[i] = ReadStick(i);
    if (!gSticksSent ? !(current[i] == JoystickState{})
                     : !(current[i] == gSentSticks[i])) {
      changed[numChanged++] = {i, &current[i]};
    }
    gSentSticks[i] = current[i];
  }
  gSticksSent = true;

  HAL_MatchInfo info;
  HALSIM_GetMatchInfo(&info);
  std::string gameData{reinterpret_cast<const char*>(info.gameSpecificMessage),
                       info.gameSpecificMessageSize};
  const std::string* gameDataChanged = nullptr;
  if (gGameDataKnown && gameData != gSentGameData) {
    gameDataChanged = &gameData;
  }
  gSentGameData = gameData;
  gGameDataKnown = true;

  ActiveCodec().OnLocalNewData({changed, numChanged}, gameDataChanged);
}

void RegisterLocalCallbacks() {
  for (int f = 0; f < kNumHalFields; ++f) {
    const HalField& field = kHalFields[f];
    if (field.dir == Dir::OUT) {
      continue;
    }
    int32_t count = ClassOf(field).count();
    int32_t channels = field.channels ? field.channels() : 0;
    for (int32_t i = 0; i < count; ++i) {
      for (int32_t ch = 0; ch < (channels ? channels : 1); ++ch) {
        int32_t index = channels ? (i << 8 | ch) : i;
        field.reg(index, OnFieldChanged,
                  reinterpret_cast<void*>(
                      static_cast<uintptr_t>(f << 16 | index)),
                  false);
      }
    }
  }
  HALSIM_RegisterDriverStationNewDataCallback(OnNewData, nullptr, false);
}

}  // namespace

// ------------------------------------------------------------ mirror:: API

bool mirror::Applying() {
  return gApplyDepth > 0;
}
mirror::ApplyScope::ApplyScope() {
  ++gApplyDepth;
}
mirror::ApplyScope::~ApplyScope() {
  --gApplyDepth;
}

bool mirror::Connected() {
  return gOpen;
}

void mirror::SendText(std::string_view text) {
  if (gOpen) {
    std::string s{text};
    emscripten_websocket_send_utf8_text(gSocket, s.c_str());
  }
}

void mirror::SendBinary(std::span<const uint8_t> data) {
  if (gOpen) {
    emscripten_websocket_send_binary(
        gSocket, const_cast<uint8_t*>(data.data()), data.size());
  }
}

void mirror::SetBridgeActive(bool active) {
  gBridgeActive = active;
}

RemoteTiming& mirror::Timing() {
  return gTiming;
}

bool mirror::ValidIndex(int field, int32_t index) {
  if (field < 0 || field >= kNumHalFields || index < 0) {
    return false;
  }
  const HalField& f = kHalFields[field];
  if (f.channels) {
    return (index >> 8) < ClassOf(f).count() && (index & 0xff) < f.channels();
  }
  return index < ClassOf(f).count();
}

void mirror::ApplyField(int field, int32_t index, const HAL_Value& value) {
  if (!ValidIndex(field, index)) {
    return;
  }
  ApplyScope scope;
  kHalFields[field].set(index, value);
}

SimDev* mirror::GetSimDevice(std::string_view name, bool create) {
  auto it = gSimDevices.find(name);
  if (it != gSimDevices.end()) {
    return it->second.get();
  }
  if (!create) {
    return nullptr;
  }
  auto dev = std::make_unique<SimDev>();
  dev->name = name;
  {
    ApplyScope scope;
    dev->handle = HAL_CreateSimDevice(dev->name.c_str());
  }
  if (dev->handle == 0) {
    return nullptr;
  }
  return gSimDevices.emplace(dev->name, std::move(dev)).first->second.get();
}

void mirror::FreeSimDevice(std::string_view name) {
  auto it = gSimDevices.find(name);
  if (it == gSimDevices.end()) {
    return;
  }
  ApplyScope scope;
  HAL_FreeSimDevice(it->second->handle);
  gSimDevices.erase(it);
}

SimValue* mirror::ApplySimValue(SimDev& dev, std::string_view name,
                                int32_t direction, const HAL_Value& value,
                                std::span<const std::string> enumOptions,
                                std::span<const double> enumValues) {
  ApplyScope scope;
  auto it = dev.values.find(name);
  if (it != dev.values.end()) {
    SimValue& sv = *it->second;
    HAL_Value v = value;
    // The JSON protocol cannot tell an integer from a double; keep the type
    // the value was created with.
    if (v.type != sv.type) {
      double d = v.type == HAL_DOUBLE    ? v.data.v_double
                 : v.type == HAL_BOOLEAN ? v.data.v_boolean
                 : v.type == HAL_LONG    ? static_cast<double>(v.data.v_long)
                                         : v.data.v_int;
      switch (sv.type) {
        case HAL_DOUBLE:
          v = HAL_MakeDouble(d);
          break;
        case HAL_BOOLEAN:
          v = HAL_MakeBoolean(d != 0);
          break;
        case HAL_INT:
          v = HAL_MakeInt(static_cast<int32_t>(d));
          break;
        case HAL_LONG:
          v = HAL_MakeLong(static_cast<int64_t>(d));
          break;
        case HAL_ENUM:
          v = HAL_MakeEnum(static_cast<int32_t>(d));
          break;
        default:
          return &sv;
      }
    }
    HAL_SetSimValue(sv.handle, &v);
    return &sv;
  }

  auto sv = std::make_unique<SimValue>();
  sv->dev = &dev;
  sv->name = name;
  sv->type = value.type;
  sv->direction = direction;
  if (value.type == HAL_ENUM && !enumOptions.empty()) {
    std::vector<const char*> options;
    for (auto& o : enumOptions) {
      options.push_back(o.c_str());
    }
    sv->enumValues.assign(enumValues.begin(), enumValues.end());
    if (enumValues.size() == enumOptions.size()) {
      sv->handle = HAL_CreateSimValueEnumDouble(
          dev.handle, sv->name.c_str(), direction, options.size(),
          options.data(), enumValues.data(), value.data.v_enum);
    } else {
      sv->handle = HAL_CreateSimValueEnum(dev.handle, sv->name.c_str(),
                                          direction, options.size(),
                                          options.data(), value.data.v_enum);
    }
  } else {
    sv->handle =
        HAL_CreateSimValue(dev.handle, sv->name.c_str(), direction, &value);
  }
  if (sv->handle == 0) {
    return nullptr;
  }
  SimValue* raw = sv.get();
  if (direction != HAL_SimValueOutput) {
    HALSIM_RegisterSimValueChangedCallback(raw->handle, raw, OnSimValueChanged,
                                           false);
  }
  dev.values.emplace(raw->name, std::move(sv));
  return raw;
}

void mirror::ApplyJoystickOutputs(int stick, int64_t outputs,
                                  int32_t leftRumble, int32_t rightRumble) {
  if (stick < 0 || stick >= HAL_kMaxJoysticks) {
    return;
  }
  ApplyScope scope;
  HALSIM_SetJoystickOutputs(stick, outputs, leftRumble, rightRumble);
}

void mirror::ApplyGameData(std::string_view data) {
  ApplyScope scope;
  WPI_String str{data.data(), data.size()};
  HALSIM_SetGameSpecificMessage(&str);
  gSentGameData = data;
  gGameDataKnown = true;
}

void mirror::ApplyEncoderChannels(int32_t index, int32_t a, int32_t b) {
  if (index < 0 || index >= HAL_GetNumEncoders()) {
    return;
  }
  // There is no public setter for these; the robot's HAL assigns them when
  // the encoder is created.
  hal::SimEncoderData[index].digitalChannelA = a;
  hal::SimEncoderData[index].digitalChannelB = b;
}

void mirror::ApplyLedData(int32_t index, std::span<const uint8_t> rgb) {
  if (index < 0 || index >= HAL_GetNumAddressableLEDs()) {
    return;
  }
  static std::vector<HAL_AddressableLEDData> leds;
  size_t n = rgb.size() / 3;
  if (n > HAL_kAddressableLEDMaxLength) {
    n = HAL_kAddressableLEDMaxLength;
  }
  leds.resize(n);
  for (size_t i = 0; i < n; ++i) {
    leds[i].r = rgb[i * 3];
    leds[i].g = rgb[i * 3 + 1];
    leds[i].b = rgb[i * 3 + 2];
    leds[i].padding = 0;
  }
  ApplyScope scope;
  HALSIM_SetAddressableLEDData(index, leds.data(), n);
}

// --------------------------------------------------------------- public API

void websim::StartHalMirror() {
  gUrl = ModuleString("halUrl");
  RegisterLocalCallbacks();
  emscripten_set_interval(Tick, 10, nullptr);
  if (gUrl.empty()) {
    std::puts("HAL mirror: no Module.halUrl; hardware windows stay local");
  }
}

bool websim::BridgeActive() {
  return gBridgeActive;
}

// --------------------------------------------------- Timing window wrappers
//
// TimingGui.cpp and DriverStationGui.cpp call these HAL functions directly.
// The linker's --wrap redirects those calls here (and nowhere else changes).
// With the native bridge connected they act on the robot process; without it
// (stock halsim_ws has no timing messages) the clock is the browser-local
// HAL's and Run / Pause / Step do nothing, rather than pausing a clock that
// the robot does not follow.

extern "C" {

uint64_t __real_HAL_GetFPGATime(int32_t* status);
uint64_t __real_HALSIM_GetNextNotifierTimeout(void);
int32_t __real_HALSIM_GetNumNotifiers(void);
int32_t __real_HALSIM_GetNotifierInfo(struct HALSIM_NotifierInfo* arr,
                                      int32_t size);

uint64_t __wrap_HAL_GetFPGATime(int32_t* status) {
  if (!gTiming.valid) {
    return __real_HAL_GetFPGATime(status);
  }
  if (status) {
    *status = 0;
  }
  if (gTiming.paused) {
    return gTiming.fpgaTime;
  }
  double elapsedMs = emscripten_get_now() - gTiming.receivedAtMs;
  if (elapsedMs < 0) {
    elapsedMs = 0;
  }
  // Extrapolate between bridge updates, but never run away if they stop.
  if (elapsedMs > 250) {
    elapsedMs = 250;
  }
  return gTiming.fpgaTime + static_cast<uint64_t>(elapsedMs * 1000.0);
}

void __wrap_HALSIM_PauseTiming(void) {
  if (gTiming.valid) {
    gTiming.paused = true;
    BridgeCodec().RequestPause(true);
  }
}

void __wrap_HALSIM_ResumeTiming(void) {
  if (gTiming.valid) {
    gTiming.paused = false;
    gTiming.receivedAtMs = emscripten_get_now();
    BridgeCodec().RequestPause(false);
  }
}

HAL_Bool __wrap_HALSIM_IsTimingPaused(void) {
  return gTiming.valid && gTiming.paused;
}

void __wrap_HALSIM_StepTiming(uint64_t delta) {
  if (gTiming.valid) {
    BridgeCodec().RequestStep(delta);
  }
}

void __wrap_HALSIM_StepTimingAsync(uint64_t delta) {
  if (gTiming.valid) {
    BridgeCodec().RequestStep(delta);
  }
}

uint64_t __wrap_HALSIM_GetNextNotifierTimeout(void) {
  return gTiming.valid ? gTiming.nextTimeout
                       : __real_HALSIM_GetNextNotifierTimeout();
}

int32_t __wrap_HALSIM_GetNumNotifiers(void) {
  return gTiming.valid ? static_cast<int32_t>(gTiming.notifiers.size())
                       : __real_HALSIM_GetNumNotifiers();
}

int32_t __wrap_HALSIM_GetNotifierInfo(struct HALSIM_NotifierInfo* arr,
                                      int32_t size) {
  if (!gTiming.valid) {
    return __real_HALSIM_GetNotifierInfo(arr, size);
  }
  int32_t n = static_cast<int32_t>(gTiming.notifiers.size());
  for (int32_t i = 0; i < n && i < size; ++i) {
    arr[i] = gTiming.notifiers[i];
  }
  return n;
}

}  // extern "C"
