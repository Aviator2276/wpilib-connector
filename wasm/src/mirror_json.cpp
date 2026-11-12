// Mirror codec for the stock halsim_ws JSON protocol
// (allwpilib simulation/halsim_ws_core/doc/hardware_ws_api.md).
//
// The upstream halsim_ws providers are the robot side of this protocol: they
// send every value and accept only ">" keys. This is the other side: it
// accepts every key and sends only GUI edits of inputs.
//
// The daemon batches robot-to-browser messages into JSON arrays; a frame is
// either one message object or an array of them.
//
// What this protocol cannot carry (and the native bridge can): timing,
// notifiers, Power Distribution, REV PH, analog gyros, duty cycle inputs,
// SimValue types (every number arrives as a double) and enum option names.

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hal/DriverStationTypes.h>
#include <wpi/StringExtras.h>
#include <wpi/json.h>

#include "mirror.h"

using namespace websim;
using namespace websim::mirror;

namespace {

struct ClassKeys {
  Cls cls;
  std::unordered_map<std::string, int> fields;  // wsKey -> field index
};

std::unordered_map<std::string, ClassKeys>& WsTypes() {
  static std::unordered_map<std::string, ClassKeys> types = [] {
    std::unordered_map<std::string, ClassKeys> out;
    for (int f = 0; f < kNumHalFields; ++f) {
      const HalField& field = kHalFields[f];
      const HalClass& cls = ClassOf(field);
      if (!cls.wsType) {
        continue;
      }
      auto& entry = out[cls.wsType];
      entry.cls = cls.cls;
      if (field.wsKey) {
        entry.fields[field.wsKey] = f;
      }
    }
    return out;
  }();
  return types;
}

constexpr const char* kStations[] = {"red1",  "red2",  "red3",
                                     "blue1", "blue2", "blue3"};

// "device" string <-> table index, per class.
bool ParseDevice(Cls cls, std::string_view device, int32_t* index) {
  switch (cls) {
    case Cls::DriverStation:
    case Cls::RoboRio:
      *index = 0;
      return device.empty();
    case Cls::Accelerometer:
      *index = 0;
      return device == "BuiltInAccel";
    case Cls::CTREPCMSolenoid: {
      auto [module, channel] = wpi::split(device, ',');
      auto m = wpi::parse_integer<int32_t>(module, 10);
      auto c = wpi::parse_integer<int32_t>(channel, 10);
      if (!m || !c || *m < 0 || *c < 0 || *c > 255) {
        return false;
      }
      *index = (*m << 8) | *c;
      return true;
    }
    default: {
      auto v = wpi::parse_integer<int32_t>(device, 10);
      if (!v || *v < 0) {
        return false;
      }
      *index = *v;
      return true;
    }
  }
}

std::string DeviceName(Cls cls, int32_t index) {
  switch (cls) {
    case Cls::DriverStation:
    case Cls::RoboRio:
      return "";
    case Cls::Accelerometer:
      return "BuiltInAccel";
    case Cls::CTREPCMSolenoid:
      return std::to_string(index >> 8) + "," + std::to_string(index & 0xff);
    default:
      return std::to_string(index);
  }
}

bool ToHalValue(const wpi::json& j, HAL_Type type, HAL_Value* out) {
  double num;
  if (j.is_boolean()) {
    num = j.get<bool>() ? 1 : 0;
  } else if (j.is_number()) {
    num = j.get<double>();
  } else {
    return false;
  }
  switch (type) {
    case HAL_BOOLEAN:
      *out = HAL_MakeBoolean(num != 0);
      return true;
    case HAL_DOUBLE:
      *out = HAL_MakeDouble(num);
      return true;
    case HAL_INT:
      *out = HAL_MakeInt(static_cast<int32_t>(num));
      return true;
    case HAL_LONG:
      *out = HAL_MakeLong(j.is_number_integer() ? j.get<int64_t>()
                                                : static_cast<int64_t>(num));
      return true;
    case HAL_ENUM:
      *out = HAL_MakeEnum(static_cast<int32_t>(num));
      return true;
    default:
      return false;
  }
}

wpi::json FromHalValue(const HAL_Value& v) {
  switch (v.type) {
    case HAL_BOOLEAN:
      return static_cast<bool>(v.data.v_boolean);
    case HAL_DOUBLE:
      return v.data.v_double;
    case HAL_INT:
      return v.data.v_int;
    case HAL_LONG:
      return v.data.v_long;
    case HAL_ENUM:
      return v.data.v_enum;
    default:
      return nullptr;
  }
}

int FieldIndex(Cls cls, std::string_view name) {
  for (int f = 0; f < kNumHalFields; ++f) {
    if (kHalFields[f].cls == cls && name == kHalFields[f].name) {
      return f;
    }
  }
  return -1;
}

// ------------------------------------------------------------------ inbound

void ApplySimDeviceMessage(const std::string& type, const std::string& device,
                           const wpi::json& data) {
  // halsim_ws splits "CANMotor:Talon FX[1]" into type "CANMotor" and device
  // "Talon FX[1]". The GUI sorts devices by that prefix, so the local device
  // gets the full original name back.
  std::string name = type == "SimDevice" ? device : type + ":" + device;
  SimDev* dev = GetSimDevice(name, true);
  if (!dev) {
    return;
  }
  dev->wsType = type;
  dev->wsDevice = device;
  for (auto& [key, val] : data.items()) {
    std::string_view k = key;
    int32_t direction = HAL_SimValueOutput;
    if (k.starts_with("<>")) {
      direction = HAL_SimValueBidir;
      k.remove_prefix(2);
    } else if (k.starts_with("<")) {
      k.remove_prefix(1);
    } else if (k.starts_with(">")) {
      direction = HAL_SimValueInput;
      k.remove_prefix(1);
    }
    HAL_Value v;
    if (val.is_boolean()) {
      v = HAL_MakeBoolean(val.get<bool>());
    } else if (val.is_number()) {
      v = HAL_MakeDouble(val.get<double>());
    } else {
      continue;  // enum sent by option name; not representable here
    }
    ApplySimValue(*dev, k, direction, v);
  }
}

void ApplyMessage(const wpi::json& msg) {
  if (!msg.is_object()) {
    return;
  }
  auto typeIt = msg.find("type");
  auto deviceIt = msg.find("device");
  auto dataIt = msg.find("data");
  if (typeIt == msg.end() || deviceIt == msg.end() || dataIt == msg.end() ||
      !typeIt->is_string() || !deviceIt->is_string() || !dataIt->is_object()) {
    return;
  }
  auto& type = typeIt->get_ref<const std::string&>();
  auto& device = deviceIt->get_ref<const std::string&>();
  auto& data = *dataIt;

  if (type == "HAL") {
    return;  // sim periodic markers (XRP mode); nothing to mirror
  }

  if (type == "Joystick") {
    auto stick = wpi::parse_integer<int>(device, 10);
    if (!stick) {
      return;
    }
    // Stick inputs (">axes" etc.) are this GUI's to produce, not to mirror.
    auto outputs = data.find("<outputs");
    auto left = data.find("<rumble_left");
    auto right = data.find("<rumble_right");
    if (outputs != data.end() || left != data.end() || right != data.end()) {
      ApplyJoystickOutputs(
          *stick, outputs != data.end() ? outputs->get<int64_t>() : 0,
          left != data.end() ? static_cast<int32_t>(left->get<double>()) : 0,
          right != data.end() ? static_cast<int32_t>(right->get<double>())
                              : 0);
    }
    return;
  }

  auto& types = WsTypes();
  auto it = types.find(type);
  int32_t index = 0;
  if (it == types.end() || !ParseDevice(it->second.cls, device, &index)) {
    // Not a HAL device of that type: a SimDevice ("SimDevice", "CANMotor",
    // or e.g. type "Accel" with a device other than the built-in one).
    ApplySimDeviceMessage(type, device, data);
    return;
  }
  Cls cls = it->second.cls;

  for (auto& [key, val] : data.items()) {
    auto fieldIt = it->second.fields.find(key);
    if (fieldIt != it->second.fields.end()) {
      HAL_Value v;
      if (ToHalValue(val, kHalFields[fieldIt->second].type, &v)) {
        ApplyField(fieldIt->second, index, v);
      }
      continue;
    }
    // keys the table cannot express
    if (cls == Cls::DriverStation) {
      if (key == ">station" && val.is_string()) {
        auto& s = val.get_ref<const std::string&>();
        for (int i = 0; i < 6; ++i) {
          if (s == kStations[i]) {
            // HAL_AllianceStationID: unknown = 0, red1..3 = 1..3, blue = 4..6
            ApplyField(FieldIndex(cls, "AllianceStationId"), 0,
                       HAL_MakeEnum(i + 1));
          }
        }
      } else if (key == ">game_data" && val.is_string()) {
        ApplyGameData(val.get_ref<const std::string&>());
      }
    } else if (cls == Cls::Accelerometer) {
      if (key == "<range" && val.is_number()) {
        double g = val.get<double>();
        ApplyField(FieldIndex(cls, "Range"), 0,
                   HAL_MakeEnum(g <= 2 ? 0 : g <= 4 ? 1 : 2));
      }
    } else if (cls == Cls::AddressableLED) {
      if (key == "<data" && val.is_array()) {
        std::vector<uint8_t> rgb;
        rgb.reserve(val.size() * 3);
        for (auto& led : val) {
          if (!led.is_object()) {
            continue;
          }
          rgb.push_back(led.value("r", 0));
          rgb.push_back(led.value("g", 0));
          rgb.push_back(led.value("b", 0));
        }
        ApplyLedData(index, rgb);
      }
    }
  }

  if (cls == Cls::Encoder) {
    auto a = data.find("<channel_a");
    auto b = data.find("<channel_b");
    if (a != data.end() && b != data.end() && a->is_number() &&
        b->is_number()) {
      ApplyEncoderChannels(index, a->get<int32_t>(), b->get<int32_t>());
    }
  }
}

// ----------------------------------------------------------------- outbound

class JsonCodecImpl final : public Codec {
 public:
  void OnOpen() final { m_queue.clear(); }
  void OnClose() final { m_queue.clear(); }

  void OnLocalField(int field, int32_t index, const HAL_Value& value) final {
    const HalField& f = kHalFields[field];
    const HalClass& cls = ClassOf(f);
    if (!cls.wsType) {
      return;
    }
    if (f.wsKey) {
      Queue(cls.wsType, DeviceName(cls.cls, index), f.wsKey,
            FromHalValue(value));
    } else if (cls.cls == Cls::DriverStation &&
               std::string_view{f.name} == "AllianceStationId") {
      int id = value.data.v_enum;
      if (id >= 1 && id <= 6) {
        Queue(cls.wsType, "", ">station", kStations[id - 1]);
      }
    }
  }

  void OnLocalSimValue(const SimValue& value, const HAL_Value& v) final {
    const SimDev& dev = *value.dev;
    if (dev.wsType.empty()) {
      return;
    }
    const char* prefix = value.direction == HAL_SimValueInput    ? ">"
                         : value.direction == HAL_SimValueBidir ? "<>"
                                                                : "<";
    Queue(dev.wsType, dev.wsDevice, prefix + value.name, FromHalValue(v));
  }

  void OnLocalNewData(
      std::span<const std::pair<int, const JoystickState*>> sticks,
      const std::string* gameData) final {
    for (auto& [index, state] : sticks) {
      wpi::json buttons = wpi::json::array();
      for (int b = 0; b < state->buttonCount; ++b) {
        buttons.push_back((state->buttons & (1u << b)) != 0);
      }
      m_queue.push_back({"Joystick", std::to_string(index),
                         wpi::json{{">axes", state->axes},
                                   {">povs", state->povs},
                                   {">buttons", std::move(buttons)}}});
    }
    if (gameData) {
      Queue("DriverStation", "", ">game_data", *gameData);
    }
    // One-shot: tells the robot program to latch the DS and joystick values
    // above. Always its own message so it is never merged away.
    m_queue.push_back(
        {"DriverStation", "", wpi::json{{">new_data", true}}});
  }

  void Flush() final {
    if (m_queue.empty() || !Connected()) {
      return;
    }
    wpi::json frame = wpi::json::array();
    for (auto& m : m_queue) {
      frame.push_back(wpi::json{{"type", std::move(m.type)},
                                {"device", std::move(m.device)},
                                {"data", std::move(m.data)}});
    }
    m_queue.clear();
    SendText(frame.dump());
  }

 private:
  struct Message {
    std::string type;
    std::string device;
    wpi::json data;
  };

  void Queue(std::string_view type, std::string_view device,
             std::string_view key, wpi::json value) {
    if (!m_queue.empty()) {
      Message& last = m_queue.back();
      if (last.type == type && last.device == device &&
          !last.data.contains(">new_data")) {
        last.data[std::string{key}] = std::move(value);
        return;
      }
    }
    m_queue.push_back({std::string{type}, std::string{device},
                       wpi::json{{std::string{key}, std::move(value)}}});
  }

  std::vector<Message> m_queue;
};

}  // namespace

Codec& mirror::JsonCodec() {
  static JsonCodecImpl codec;
  return codec;
}

void mirror::HandleJsonFrame(std::string_view text) {
  try {
    wpi::json frame = wpi::json::parse(text);
    if (frame.is_array()) {
      for (auto& msg : frame) {
        ApplyMessage(msg);
      }
    } else {
      ApplyMessage(frame);
    }
  } catch (const std::exception& e) {
    std::printf("HAL mirror: bad JSON frame: %s\n", e.what());
  }
}
