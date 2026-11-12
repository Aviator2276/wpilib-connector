// Mirror codec for the halsim_webbridge binary protocol
// (protocol/bridge_protocol.h, PROTOCOL.md).
//
// Unlike the JSON codec this one is lossless: every table field has a wire
// form, SimValues keep their type and enum options, and the Timing window
// reads and controls the robot's clock.

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <emscripten.h>

#include "bridge_protocol.h"
#include "hal_value.h"
#include "mirror.h"
#include "websim.h"

using namespace websim;
using namespace websim::mirror;
using namespace websim::proto;

namespace {

bool gHello = false;      // a compatible bridge has introduced itself
bool gRejected = false;   // an incompatible one has; ignore its frames
std::unordered_map<int32_t, SimDev*> gDevs;      // bridge device id -> local
std::unordered_map<int32_t, SimValue*> gVals;    // bridge value id -> local

void Reset() {
  gHello = false;
  gRejected = false;
  gDevs.clear();
  gVals.clear();
  SetBridgeActive(false);
}

// Returns true if the message changed something a window other than Timing
// would show.
bool ApplyMessage(Reader& r, int64_t elements) {
  Value op = r.Next();
  int64_t rest = elements - 1;
  if (op.kind != Value::kInt) {
    r.Skip(rest);
    return false;
  }

  if (op.i == kHello) {
    Value version = r.Next(), hash = r.Next(), wpilib = r.Next();
    r.Skip(rest - 3);
    if (!r.ok()) {
      return false;
    }
    if (version.AsInt() != kProtocolVersion ||
        static_cast<uint32_t>(hash.AsInt()) != HalTableHash()) {
      std::printf(
          "HAL mirror: the robot's bridge (protocol %d, table %08x) does not "
          "match this GUI (protocol %d, table %08x); hardware is not "
          "mirrored\n",
          static_cast<int>(version.AsInt()),
          static_cast<unsigned>(hash.AsInt()), kProtocolVersion,
          static_cast<unsigned>(HalTableHash()));
      gRejected = true;
      return false;
    }
    gHello = true;
    SetBridgeActive(true);
    std::printf("HAL mirror: native bridge connected (WPILib %.*s)\n",
                static_cast<int>(wpilib.s.size()), wpilib.s.data());
    return true;
  }
  if (!gHello) {
    r.Skip(rest);
    return false;
  }

  switch (op.i) {
    case kField: {
      if (rest != 3) {
        break;
      }
      Value field = r.Next(), index = r.Next(), value = r.Next();
      if (!r.ok() || field.i < 0 || field.i >= kNumHalFields) {
        return false;
      }
      HAL_Value v;
      if (ToHalValue(value, kHalFields[field.i].type, &v)) {
        ApplyField(static_cast<int>(field.i), static_cast<int32_t>(index.i),
                   v);
      }
      return true;
    }
    case kSimDevNew: {
      if (rest != 2) {
        break;
      }
      Value id = r.Next(), name = r.Next();
      if (!r.ok() || name.kind != Value::kStr) {
        return false;
      }
      if (SimDev* dev = GetSimDevice(name.s, true)) {
        dev->remoteId = static_cast<int32_t>(id.i);
        gDevs[dev->remoteId] = dev;
      }
      return true;
    }
    case kSimDevFree: {
      if (rest != 1) {
        break;
      }
      Value id = r.Next();
      auto it = gDevs.find(static_cast<int32_t>(id.i));
      if (!r.ok() || it == gDevs.end()) {
        return false;
      }
      SimDev* dev = it->second;
      std::erase_if(gVals, [&](auto& kv) { return kv.second->dev == dev; });
      std::string name = dev->name;
      gDevs.erase(it);
      FreeSimDevice(name);
      return true;
    }
    case kSimValNew: {
      if (rest != 8) {
        break;
      }
      Value devId = r.Next(), valId = r.Next(), name = r.Next(),
            direction = r.Next(), type = r.Next(), value = r.Next();
      std::vector<std::string> options;
      Value optArr = r.Next();
      for (int64_t i = 0; optArr.kind == Value::kArray && i < optArr.i; ++i) {
        options.emplace_back(r.Next().s);
      }
      std::vector<double> optionValues;
      Value valArr = r.Next();
      for (int64_t i = 0; valArr.kind == Value::kArray && i < valArr.i; ++i) {
        optionValues.push_back(r.Next().AsDouble());
      }
      auto it = gDevs.find(static_cast<int32_t>(devId.i));
      HAL_Value v;
      if (!r.ok() || it == gDevs.end() || name.kind != Value::kStr ||
          !ToHalValue(value, static_cast<HAL_Type>(type.AsInt()), &v)) {
        return false;
      }
      if (SimValue* sv =
              ApplySimValue(*it->second, name.s,
                            static_cast<int32_t>(direction.AsInt()), v,
                            options, optionValues)) {
        sv->remoteId = static_cast<int32_t>(valId.i);
        gVals[sv->remoteId] = sv;
      }
      return true;
    }
    case kSimVal: {
      if (rest != 2) {
        break;
      }
      Value id = r.Next(), value = r.Next();
      auto it = gVals.find(static_cast<int32_t>(id.i));
      HAL_Value v;
      if (!r.ok() || it == gVals.end() ||
          !ToHalValue(value, it->second->type, &v)) {
        return false;
      }
      SimValue* sv = it->second;
      ApplySimValue(*sv->dev, sv->name, sv->direction, v);
      return true;
    }
    case kEncoderCh: {
      if (rest != 3) {
        break;
      }
      Value index = r.Next(), a = r.Next(), b = r.Next();
      if (r.ok()) {
        ApplyEncoderChannels(static_cast<int32_t>(index.i),
                             static_cast<int32_t>(a.AsInt()),
                             static_cast<int32_t>(b.AsInt()));
      }
      return true;
    }
    case kLedData: {
      if (rest != 2) {
        break;
      }
      Value index = r.Next(), data = r.Next();
      if (r.ok() && data.kind == Value::kBin) {
        ApplyLedData(static_cast<int32_t>(index.i),
                     {reinterpret_cast<const uint8_t*>(data.s.data()),
                      data.s.size()});
      }
      return true;
    }
    case kJoyOutputs: {
      if (rest != 4) {
        break;
      }
      Value stick = r.Next(), outputs = r.Next(), left = r.Next(),
            right = r.Next();
      if (r.ok()) {
        ApplyJoystickOutputs(static_cast<int>(stick.i), outputs.AsInt(),
                             static_cast<int32_t>(left.AsInt()),
                             static_cast<int32_t>(right.AsInt()));
      }
      return true;
    }
    case kGameData: {
      if (rest != 1) {
        break;
      }
      Value s = r.Next();
      if (r.ok() && s.kind == Value::kStr) {
        ApplyGameData(s.s);
      }
      return true;
    }
    case kTiming: {
      if (rest != 4) {
        break;
      }
      Value now = r.Next(), paused = r.Next(), next = r.Next();
      Value list = r.Next();
      std::vector<HALSIM_NotifierInfo> notifiers;
      for (int64_t i = 0; list.kind == Value::kArray && i < list.i; ++i) {
        Value item = r.Next();
        if (item.kind != Value::kArray || item.i != 3) {
          r.Skip(item.kind == Value::kArray ? item.i : 0);
          continue;
        }
        Value name = r.Next(), timeout = r.Next(), valid = r.Next();
        HALSIM_NotifierInfo info{};
        info.handle = static_cast<HAL_NotifierHandle>(i + 1);
        size_t n = name.s.size() < sizeof(info.name) - 1 ? name.s.size()
                                                         : sizeof(info.name) - 1;
        std::memcpy(info.name, name.s.data(), n);
        info.timeout = static_cast<uint64_t>(timeout.AsInt());
        info.waitTimeValid = valid.AsBool();
        notifiers.push_back(info);
      }
      if (!r.ok()) {
        return false;
      }
      RemoteTiming& t = Timing();
      bool changed = !t.valid || t.paused != paused.AsBool() ||
                     t.notifiers.size() != notifiers.size();
      t.valid = true;
      t.paused = paused.AsBool();
      t.fpgaTime = static_cast<uint64_t>(now.AsInt());
      t.receivedAtMs = emscripten_get_now();
      t.nextTimeout = static_cast<uint64_t>(next.AsInt());
      t.notifiers = std::move(notifiers);
      // The clock itself only needs a frame if someone is looking at it.
      return changed || WindowVisible("Timing");
    }
    case kSnapshotEnd:
      r.Skip(rest);
      return true;
    default:
      break;
  }
  r.Skip(rest);
  return false;
}

class BridgeCodecImpl final : public Codec {
 public:
  void OnOpen() final {
    Reset();
    m_w.Begin();
  }
  void OnClose() final {
    Reset();
    m_w.Begin();
  }

  void OnLocalField(int field, int32_t index, const HAL_Value& value) final {
    m_w.Message(kField, 4);
    m_w.UInt(field);
    m_w.UInt(index);
    WriteHalValue(m_w, value);
  }

  void OnLocalSimValue(const SimValue& value, const HAL_Value& v) final {
    if (value.remoteId < 0) {
      return;
    }
    m_w.Message(kSimVal, 3);
    m_w.Int(value.remoteId);
    WriteHalValue(m_w, v);
  }

  void OnLocalNewData(
      std::span<const std::pair<int, const JoystickState*>> sticks,
      const std::string* gameData) final {
    for (auto& [index, state] : sticks) {
      m_w.Message(kJoystick, 6);
      m_w.Int(index);
      m_w.Array(state->axes.size());
      for (float a : state->axes) {
        m_w.Double(a);
      }
      m_w.Array(state->povs.size());
      for (int16_t p : state->povs) {
        m_w.Int(p);
      }
      m_w.UInt(state->buttons);
      m_w.UInt(state->buttonCount);
    }
    if (gameData) {
      m_w.Message(kGameData, 2);
      m_w.Str(*gameData);
    }
    m_w.Message(kNewData, 1);
  }

  void RequestPause(bool pause) final {
    m_w.Message(kPause, 2);
    m_w.Bool(pause);
  }

  void RequestStep(uint64_t deltaUs) final {
    m_w.Message(kStep, 2);
    m_w.UInt(deltaUs);
  }

  void Flush() final {
    if (m_w.empty() || !Connected()) {
      return;
    }
    m_w.Finish();
    SendBinary(m_w.buf());
    m_w.Begin();
  }

 private:
  Writer m_w;
};

}  // namespace

Codec& mirror::BridgeCodec() {
  static BridgeCodecImpl codec;
  return codec;
}

void mirror::HandleBridgeFrame(std::span<const uint8_t> data) {
  if (gRejected) {
    return;
  }
  Reader r{data.data(), data.size()};
  Value frame = r.Next();
  if (frame.kind != Value::kArray) {
    return;
  }
  bool visible = false;
  for (int64_t i = 0; i < frame.i && r.ok(); ++i) {
    Value msg = r.Next();
    if (msg.kind != Value::kArray || msg.i < 1) {
      break;
    }
    visible |= ApplyMessage(r, msg.i);
  }
  if (!r.ok()) {
    std::puts("HAL mirror: malformed bridge frame");
  }
  if (visible) {
    websim_wake(0);
  }
}
