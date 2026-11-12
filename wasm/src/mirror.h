// Mirror HAL: internal interface shared by the core and the two wire codecs.
//
// The Sim GUI reads and writes hardware state through HALSIM_* calls in its
// own process. In the browser that process has no robot in it, so the module
// carries its own sim HAL and the mirror keeps it in step with the robot's:
//
//   robot -> browser   values are applied to the local HAL (Apply*), so the
//                      unmodified GUI code sees them
//   browser -> robot   local HAL callbacks fire when the GUI changes an
//                      input; the mirror sends the change (OnLocal*)
//
// Changes applied from the network never echo back (see ApplyScope).
//
// Two codecs speak to the robot through the daemon's /hal WebSocket:
//   mirror_json.cpp    stock halsim_ws JSON (text frames)
//   mirror_bridge.cpp  halsim_webbridge binary protocol (binary frames)
#pragma once

#include <stdint.h>

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <hal/SimDevice.h>
#include <hal/Value.h>
#include <hal/simulation/NotifierData.h>

#include "hal_table.h"

namespace websim::mirror {

// ---- echo guard -----------------------------------------------------------
// True while a network value is being written into the local HAL.
bool Applying();
struct ApplyScope {
  ApplyScope();
  ~ApplyScope();
};

// ---- transport ------------------------------------------------------------
bool Connected();
void SendText(std::string_view text);
void SendBinary(std::span<const uint8_t> data);

// ---- table-driven HAL values ----------------------------------------------
// index: instance, or instance * 256 + channel for channel fields.
bool ValidIndex(int field, int32_t index);
void ApplyField(int field, int32_t index, const HAL_Value& value);

// ---- SimDevices -----------------------------------------------------------
struct SimValue {
  struct SimDev* dev = nullptr;
  std::string name;  // without direction prefix
  HAL_SimValueHandle handle = 0;
  HAL_Type type = HAL_UNASSIGNED;
  int32_t direction = HAL_SimValueOutput;
  std::vector<double> enumValues;
  int32_t remoteId = -1;  // bridge value id
};

struct SimDev {
  std::string name;  // full name as created in the robot's HAL
  HAL_SimDeviceHandle handle = 0;
  std::string wsType;    // halsim_ws "type" this device arrived under
  std::string wsDevice;  // halsim_ws "device"
  int32_t remoteId = -1;  // bridge device id
  std::map<std::string, std::unique_ptr<SimValue>, std::less<>> values;
};

SimDev* GetSimDevice(std::string_view name, bool create);
void FreeSimDevice(std::string_view name);
// Creates the value on first sight, otherwise updates it.
SimValue* ApplySimValue(SimDev& dev, std::string_view name, int32_t direction,
                        const HAL_Value& value,
                        std::span<const std::string> enumOptions = {},
                        std::span<const double> enumValues = {});

// ---- joysticks / driver station ---------------------------------------------
struct JoystickState {
  std::vector<float> axes;
  std::vector<int16_t> povs;
  uint32_t buttons = 0;
  uint8_t buttonCount = 0;
  bool operator==(const JoystickState&) const = default;
};
void ApplyJoystickOutputs(int stick, int64_t outputs, int32_t leftRumble,
                          int32_t rightRumble);
void ApplyGameData(std::string_view data);
void ApplyEncoderChannels(int32_t index, int32_t a, int32_t b);
void ApplyLedData(int32_t index, std::span<const uint8_t> rgb);

// ---- remote timing (bridge only) ----------------------------------------------
struct RemoteTiming {
  bool valid = false;
  bool paused = false;
  uint64_t fpgaTime = 0;     // robot FPGA time at receipt, microseconds
  double receivedAtMs = 0;   // local clock at receipt
  uint64_t nextTimeout = 0;
  std::vector<HALSIM_NotifierInfo> notifiers;
};
RemoteTiming& Timing();

// ---- codec interface ----------------------------------------------------------
class Codec {
 public:
  virtual ~Codec() = default;
  virtual void OnOpen() {}
  virtual void OnClose() {}
  // A GUI edit of a table field / SimValue in the local HAL.
  virtual void OnLocalField(int field, int32_t index,
                            const HAL_Value& value) = 0;
  virtual void OnLocalSimValue(const SimValue& value,
                               const HAL_Value& halValue) = 0;
  // Driver station "new data" tick; `sticks` holds only joysticks whose state
  // changed since the last tick, and gameData is set only if it changed.
  virtual void OnLocalNewData(
      std::span<const std::pair<int, const JoystickState*>> sticks,
      const std::string* gameData) = 0;
  // Timing window requests (no-ops unless the codec can reach the robot).
  virtual void RequestPause(bool pause) {}
  virtual void RequestStep(uint64_t deltaUs) {}
  // Called every few milliseconds; send anything queued.
  virtual void Flush() {}
};

Codec& JsonCodec();
Codec& BridgeCodec();
void HandleJsonFrame(std::string_view text);
void HandleBridgeFrame(std::span<const uint8_t> data);
// The bridge codec calls this once the robot-side bridge has said hello.
void SetBridgeActive(bool active);

}  // namespace websim::mirror
