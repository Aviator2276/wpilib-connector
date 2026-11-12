// halsim_webbridge: a HAL simulation extension that publishes the robot
// program's complete HAL state to the web Sim GUI and accepts its inputs.
//
// It exists because the stock halsim_ws protocol has no messages for timing
// (pause / resume / step, FPGA time, notifiers), Power Distribution or REV
// PH, loses SimValue types and enum options, allows one client, and sends a
// JSON message per change. This extension runs inside the robot process, so
// it can call the HALSIM functions the desktop Sim GUI calls.
//
//   - WebSocket server on 127.0.0.1:3301 (WEBSIM_BRIDGE_PORT), any number of
//     clients on "/", JSON commands on "/control".
//   - Every table value (protocol/hal_fields.def) is cached from HAL
//     callbacks; changes are sent last-value-wins at 50 Hz.
//   - A new client first receives the whole state (snapshot).
//   - A change made by one client is sent to the others, not back to it.
//
// Loaded through HALSIM_EXTENSIONS like any other extension; see
// images/gradle/wpilib-websim.gradle.

#include <stdint.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hal/DriverStationTypes.h>
#include <hal/Extensions.h>
#include <hal/HALBase.h>
#include <hal/SimDevice.h>
#include <hal/simulation/MockHooks.h>
#include <hal/simulation/NotifierData.h>
#include <hal/simulation/SimDeviceData.h>
#include <wpinet/EventLoopRunner.h>
#include <wpinet/HttpWebSocketServerConnection.h>
#include <wpinet/raw_uv_ostream.h>
#include <wpinet/uv/Buffer.h>
#include <wpinet/uv/Loop.h>
#include <wpinet/uv/Tcp.h>
#include <wpinet/uv/Timer.h>

#include "bridge_protocol.h"
#include "hal_table.h"
#include "hal_value.h"

#ifndef WEBSIM_WPILIB_VERSION
#define WEBSIM_WPILIB_VERSION "unknown"
#endif

namespace uv = wpi::uv;
using namespace websim;
using namespace websim::proto;

namespace {

constexpr int kFlushMs = 20;       // 50 Hz
constexpr int kTimingEveryN = 5;   // timing snapshot at 10 Hz

// Which client the HAL write currently in progress on this thread came
// from (0 = the robot program). HAL callbacks run synchronously inside the
// setter, so they see it.
thread_local uint32_t tOrigin = 0;

struct OriginScope {
  explicit OriginScope(uint32_t id) { tOrigin = id; }
  ~OriginScope() { tOrigin = 0; }
};

// One encoded message plus who caused it.
struct Msg {
  std::vector<uint8_t> bytes;
  uint32_t origin = 0;
};

// Encode a single message (not a frame).
template <typename F>
std::vector<uint8_t> Encode(Op op, uint32_t n, F&& body) {
  Writer w;
  w.Message(op, n);
  body(w);
  return std::move(w.buf());
}

std::vector<uint8_t> Frame(const std::vector<const Msg*>& msgs) {
  size_t size = 5;
  for (auto m : msgs) {
    size += m->bytes.size();
  }
  std::vector<uint8_t> out;
  out.reserve(size);
  uint32_t n = msgs.size();
  out.push_back(0xdd);
  out.push_back(n >> 24);
  out.push_back(n >> 16);
  out.push_back(n >> 8);
  out.push_back(n);
  for (auto m : msgs) {
    out.insert(out.end(), m->bytes.begin(), m->bytes.end());
  }
  return out;
}

class Bridge;

class Connection final
    : public wpi::HttpWebSocketServerConnection<Connection> {
 public:
  Connection(Bridge& bridge, std::shared_ptr<uv::Stream> stream, uint32_t id)
      : wpi::HttpWebSocketServerConnection<Connection>(stream, {}),
        m_bridge{bridge},
        m_id{id} {}

  uint32_t id() const { return m_id; }
  bool data() const { return m_open && !m_control; }

  void Send(const std::vector<uint8_t>& frame) {
    if (!m_open || frame.empty()) {
      return;
    }
    uv::Buffer buf = uv::Buffer::Dup(
        {reinterpret_cast<const char*>(frame.data()), frame.size()});
    m_websocket->SendBinary({{buf}}, [](auto bufs, uv::Error) {
      for (auto& b : bufs) {
        b.Deallocate();
      }
    });
  }

 protected:
  void ProcessRequest() override {
    // Not a WebSocket upgrade: this port serves nothing else.
    wpi::SmallVector<uv::Buffer, 4> toSend;
    wpi::raw_uv_ostream os{toSend, 256};
    BuildHeader(os, 404, "Not Found", "text/plain", 0);
    SendData(os.bufs(), true);
  }

  bool IsValidWsUpgrade(std::string_view) override {
    std::string_view url = m_request.GetUrl();
    m_control = url == "/control";
    return m_control || url == "/" || url.empty();
  }

  void ProcessWsUpgrade() override;

 private:
  Bridge& m_bridge;
  uint32_t m_id;
  bool m_open = false;
  bool m_control = false;
};

class Bridge {
 public:
  bool Start();
  void Stop() { m_runner.Stop(); }

  // ---- called on the loop thread by connections
  void OnOpen(const std::shared_ptr<Connection>& conn);
  void OnClose(Connection* conn);
  void OnFrame(Connection& conn, std::span<const uint8_t> data);
  void OnControl(std::string_view text);

 private:
  struct FieldEntry {
    HAL_Value value{};
    uint32_t origin = 0;
    bool dirty = false;
    bool known = false;
  };
  struct SimVal {
    int32_t id = 0;
    int32_t dev = 0;
    std::string name;
    int32_t direction = 0;
    HAL_Value value{};
    std::vector<std::string> options;
    std::vector<double> optionValues;
    uint32_t origin = 0;
    bool dirty = false;
  };
  struct SimDev {
    int32_t id = 0;
    std::string name;
  };
  struct Blob {  // LED strips, joystick outputs: small keyed payloads
    std::vector<uint8_t> msg;
    bool dirty = false;
  };

  void RegisterHal();
  void Flush();
  std::vector<uint8_t> Snapshot();
  std::vector<uint8_t> TimingMsg();
  void Apply(Connection& conn, Reader& r, int64_t elements);

  static std::vector<uint8_t> FieldMsg(int field, int32_t index,
                                       const HAL_Value& v);
  static std::vector<uint8_t> EncoderChMsg(int32_t index);
  static std::vector<uint8_t> SimDevMsg(const SimDev& d);
  static std::vector<uint8_t> SimValNewMsg(const SimVal& v);
  static std::vector<uint8_t> SimValMsg(const SimVal& v);

  // HAL callbacks (any thread)
  static void FieldCb(const char*, void* param, const HAL_Value* value);
  static void LedCb(const char*, void* param, const unsigned char* buffer,
                    unsigned int count);
  static void JoyOutCb(const char*, void* param, int32_t stick,
                       int64_t outputs, int32_t left, int32_t right);
  static void MatchInfoCb(const char*, void* param, const HAL_MatchInfo* info);
  static void DevCreatedCb(const char* name, void* param,
                           HAL_SimDeviceHandle handle);
  static void DevFreedCb(const char* name, void* param,
                         HAL_SimDeviceHandle handle);
  static void ValCreatedCb(const char* name, void* param,
                           HAL_SimValueHandle handle, int32_t direction,
                           const HAL_Value* value);
  static void ValChangedCb(const char* name, void* param,
                           HAL_SimValueHandle handle, int32_t direction,
                           const HAL_Value* value);

  wpi::EventLoopRunner m_runner;
  std::shared_ptr<uv::Tcp> m_server;
  std::shared_ptr<uv::Timer> m_timer;
  std::vector<std::shared_ptr<Connection>> m_conns;  // loop thread only
  uint32_t m_nextConn = 1;
  int m_tick = 0;
  int m_port = kDefaultPort;
  int m_encoderInitField = -1;

  std::mutex m_mutex;  // guards everything below
  std::unordered_map<uint32_t, FieldEntry> m_fields;  // field << 16 | index
  std::vector<uint32_t> m_dirtyFields;
  std::map<int32_t, SimDev> m_devs;
  std::map<int32_t, SimVal> m_vals;
  std::vector<int32_t> m_dirtyVals;
  std::vector<Msg> m_structural;  // device / value creation, in order
  std::map<int32_t, Blob> m_leds;
  std::map<int32_t, Blob> m_joyOut;
  Blob m_gameData;
  bool m_timingNow = false;
};

Bridge* gBridge = nullptr;

// ------------------------------------------------------------- connection

void Connection::ProcessWsUpgrade() {
  m_websocket->open.connect_extended([this](auto conn, auto) {
    conn.disconnect();  // one-shot
    m_open = true;
    if (!m_control) {
      m_bridge.OnOpen(shared_from_this());
    }
  });
  m_websocket->binary.connect([this](auto data, bool) {
    if (m_open && !m_control) {
      m_bridge.OnFrame(*this, data);
    }
  });
  m_websocket->text.connect([this](auto text, bool) {
    if (m_open && m_control) {
      m_bridge.OnControl(text);
    }
  });
  m_websocket->closed.connect([this](uint16_t, auto) {
    m_open = false;
    m_bridge.OnClose(this);
  });
}

// ---------------------------------------------------------------- encoding

std::vector<uint8_t> Bridge::FieldMsg(int field, int32_t index,
                                      const HAL_Value& v) {
  return Encode(kField, 4, [&](Writer& w) {
    w.UInt(field);
    w.UInt(index);
    WriteHalValue(w, v);
  });
}

std::vector<uint8_t> Bridge::EncoderChMsg(int32_t index) {
  return Encode(kEncoderCh, 4, [&](Writer& w) {
    w.UInt(index);
    w.Int(HALSIM_GetEncoderDigitalChannelA(index));
    w.Int(HALSIM_GetEncoderDigitalChannelB(index));
  });
}

std::vector<uint8_t> Bridge::SimDevMsg(const SimDev& d) {
  return Encode(kSimDevNew, 3, [&](Writer& w) {
    w.Int(d.id);
    w.Str(d.name);
  });
}

std::vector<uint8_t> Bridge::SimValNewMsg(const SimVal& v) {
  return Encode(kSimValNew, 9, [&](Writer& w) {
    w.Int(v.dev);
    w.Int(v.id);
    w.Str(v.name);
    w.Int(v.direction);
    w.Int(v.value.type);
    WriteHalValue(w, v.value);
    w.Array(v.options.size());
    for (auto& o : v.options) {
      w.Str(o);
    }
    w.Array(v.optionValues.size());
    for (double d : v.optionValues) {
      w.Double(d);
    }
  });
}

std::vector<uint8_t> Bridge::SimValMsg(const SimVal& v) {
  return Encode(kSimVal, 3, [&](Writer& w) {
    w.Int(v.id);
    WriteHalValue(w, v.value);
  });
}

std::vector<uint8_t> Bridge::TimingMsg() {
  int32_t status = 0;
  uint64_t now = HAL_GetFPGATime(&status);
  bool paused = HALSIM_IsTimingPaused();
  uint64_t next = HALSIM_GetNextNotifierTimeout();
  HALSIM_NotifierInfo info[32];
  int32_t n = HALSIM_GetNotifierInfo(info, 32);
  if (n > 32) {
    n = 32;
  }
  return Encode(kTiming, 5, [&](Writer& w) {
    w.UInt(now);
    w.Bool(paused);
    w.UInt(next);
    w.Array(n);
    for (int32_t i = 0; i < n; ++i) {
      w.Array(3);
      w.Str(info[i].name);
      w.UInt(info[i].timeout);
      w.Bool(info[i].waitTimeValid);
    }
  });
}

// ------------------------------------------------------------ HAL callbacks

void Bridge::FieldCb(const char*, void* param, const HAL_Value* value) {
  Bridge* self = gBridge;
  if (!self) {
    return;
  }
  auto key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
  std::scoped_lock lock{self->m_mutex};
  FieldEntry& e = self->m_fields[key];
  e.value = *value;
  e.known = true;
  e.origin = tOrigin;
  if (!e.dirty) {
    e.dirty = true;
    self->m_dirtyFields.push_back(key);
  }
}

void Bridge::LedCb(const char*, void* param, const unsigned char* buffer,
                   unsigned int count) {
  Bridge* self = gBridge;
  if (!self) {
    return;
  }
  auto index = static_cast<int32_t>(reinterpret_cast<intptr_t>(param));
  auto leds = reinterpret_cast<const HAL_AddressableLEDData*>(buffer);
  size_t n = count / sizeof(HAL_AddressableLEDData);
  std::vector<uint8_t> rgb(n * 3);
  for (size_t i = 0; i < n; ++i) {
    rgb[i * 3] = leds[i].r;
    rgb[i * 3 + 1] = leds[i].g;
    rgb[i * 3 + 2] = leds[i].b;
  }
  auto msg = Encode(kLedData, 3, [&](Writer& w) {
    w.UInt(index);
    w.Bin(rgb.data(), rgb.size());
  });
  std::scoped_lock lock{self->m_mutex};
  Blob& b = self->m_leds[index];
  b.msg = std::move(msg);
  b.dirty = true;
}

void Bridge::JoyOutCb(const char*, void* param, int32_t stick, int64_t outputs,
                      int32_t left, int32_t right) {
  Bridge* self = gBridge;
  if (!self) {
    return;
  }
  auto msg = Encode(kJoyOutputs, 5, [&](Writer& w) {
    w.Int(stick);
    w.Int(outputs);
    w.Int(left);
    w.Int(right);
  });
  std::scoped_lock lock{self->m_mutex};
  Blob& b = self->m_joyOut[stick];
  b.msg = std::move(msg);
  b.dirty = true;
}

void Bridge::MatchInfoCb(const char*, void*, const HAL_MatchInfo* info) {
  Bridge* self = gBridge;
  if (!self || tOrigin != 0) {
    return;  // a client's own edit is not echoed to anyone as "robot" state
  }
  auto msg = Encode(kGameData, 2, [&](Writer& w) {
    w.Str({reinterpret_cast<const char*>(info->gameSpecificMessage),
           info->gameSpecificMessageSize});
  });
  std::scoped_lock lock{self->m_mutex};
  self->m_gameData.msg = std::move(msg);
  self->m_gameData.dirty = true;
}

void Bridge::DevCreatedCb(const char* name, void*, HAL_SimDeviceHandle handle) {
  Bridge* self = gBridge;
  if (!self) {
    return;
  }
  {
    std::scoped_lock lock{self->m_mutex};
    SimDev& d = self->m_devs[handle];
    d.id = handle;
    d.name = name;
    self->m_structural.push_back({SimDevMsg(d), 0});
  }
  // Registering for the device's values from inside this callback would
  // re-enter the SimDevice registry; do it from the loop instead.
  self->m_runner.ExecAsync([handle](uv::Loop&) {
    HALSIM_RegisterSimValueCreatedCallback(handle, nullptr, ValCreatedCb, true);
  });
}

void Bridge::DevFreedCb(const char*, void*, HAL_SimDeviceHandle handle) {
  Bridge* self = gBridge;
  if (!self) {
    return;
  }
  std::scoped_lock lock{self->m_mutex};
  if (self->m_devs.erase(handle) == 0) {
    return;
  }
  std::erase_if(self->m_vals,
                [&](auto& kv) { return kv.second.dev == handle; });
  self->m_structural.push_back(
      {Encode(kSimDevFree, 2, [&](Writer& w) { w.Int(handle); }), 0});
}

void Bridge::ValCreatedCb(const char* name, void*, HAL_SimValueHandle handle,
                          int32_t direction, const HAL_Value* value) {
  Bridge* self = gBridge;
  if (!self) {
    return;
  }
  SimVal v;
  v.id = handle;
  v.dev = HALSIM_GetSimValueDeviceHandle(handle);
  v.name = name;
  v.direction = direction;
  v.value = *value;
  if (value->type == HAL_ENUM) {
    int32_t n = 0;
    const char** options = HALSIM_GetSimValueEnumOptions(handle, &n);
    for (int32_t i = 0; options && i < n; ++i) {
      v.options.emplace_back(options[i]);
    }
    const double* values = HALSIM_GetSimValueEnumDoubleValues(handle, &n);
    if (values && n > 0) {
      v.optionValues.assign(values, values + n);
    }
  }
  {
    std::scoped_lock lock{self->m_mutex};
    if (self->m_vals.contains(handle) || !self->m_devs.contains(v.dev)) {
      return;
    }
    self->m_structural.push_back({SimValNewMsg(v), 0});
    self->m_vals.emplace(handle, std::move(v));
  }
  HALSIM_RegisterSimValueChangedCallback(handle, nullptr, ValChangedCb, false);
}

void Bridge::ValChangedCb(const char*, void*, HAL_SimValueHandle handle,
                          int32_t, const HAL_Value* value) {
  Bridge* self = gBridge;
  if (!self) {
    return;
  }
  std::scoped_lock lock{self->m_mutex};
  auto it = self->m_vals.find(handle);
  if (it == self->m_vals.end()) {
    return;
  }
  SimVal& v = it->second;
  v.value = *value;
  v.origin = tOrigin;
  if (!v.dirty) {
    v.dirty = true;
    self->m_dirtyVals.push_back(handle);
  }
}

void Bridge::RegisterHal() {
  for (int f = 0; f < kNumHalFields; ++f) {
    const HalField& field = kHalFields[f];
    if (field.cls == Cls::Encoder &&
        std::string_view{field.name} == "Initialized") {
      m_encoderInitField = f;
    }
    int32_t count = ClassOf(field).count();
    int32_t channels = field.channels ? field.channels() : 0;
    for (int32_t i = 0; i < count; ++i) {
      for (int32_t ch = 0; ch < (channels ? channels : 1); ++ch) {
        int32_t index = channels ? (i << 8 | ch) : i;
        field.reg(index, FieldCb,
                  reinterpret_cast<void*>(
                      static_cast<uintptr_t>(static_cast<uint32_t>(f) << 16 |
                                             static_cast<uint32_t>(index))),
                  true);
      }
    }
  }
  for (int32_t i = 0; i < HAL_GetNumAddressableLEDs(); ++i) {
    HALSIM_RegisterAddressableLEDDataCallback(
        i, LedCb, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }
  for (int32_t i = 0; i < HAL_kMaxJoysticks; ++i) {
    HALSIM_RegisterJoystickOutputsCallback(i, JoyOutCb, nullptr, true);
  }
  HALSIM_RegisterMatchInfoCallback(MatchInfoCb, nullptr, true);
  HALSIM_RegisterSimDeviceCreatedCallback("", nullptr, DevCreatedCb, true);
  HALSIM_RegisterSimDeviceFreedCallback("", nullptr, DevFreedCb, false);
}

// ------------------------------------------------------------------ server

bool Bridge::Start() {
  if (const char* port = std::getenv("WEBSIM_BRIDGE_PORT")) {
    m_port = std::atoi(port);
    if (m_port <= 0 || m_port > 65535) {
      std::fprintf(stderr, "Web Sim Bridge: bad WEBSIM_BRIDGE_PORT '%s'\n",
                   port);
      return false;
    }
  }
  bool ok = true;
  m_runner.ExecSync([&](uv::Loop& loop) {
    m_server = uv::Tcp::Create(loop);
    m_timer = uv::Timer::Create(loop);
    if (!m_server || !m_timer) {
      ok = false;
      return;
    }
    m_server->error.connect([this](uv::Error err) {
      std::fprintf(stderr, "Web Sim Bridge: cannot listen on port %d: %s\n",
                   m_port, err.str());
    });
    // loopback only: the workspace daemon is the one client
    m_server->Bind("127.0.0.1", m_port);
    m_server->connection.connect([this, srv = m_server.get()] {
      auto tcp = srv->Accept();
      if (!tcp) {
        return;
      }
      tcp->SetNoDelay(true);
      auto conn = std::make_shared<Connection>(*this, tcp, m_nextConn++);
      tcp->SetData(conn);
    });
    m_server->Listen();
    m_timer->timeout.connect([this] { Flush(); });
    m_timer->Start(uv::Timer::Time{kFlushMs}, uv::Timer::Time{kFlushMs});
  });
  if (!ok) {
    return false;
  }
  RegisterHal();
  return true;
}

void Bridge::OnOpen(const std::shared_ptr<Connection>& conn) {
  conn->Send(Snapshot());
  m_conns.push_back(conn);
}

void Bridge::OnClose(Connection* conn) {
  std::erase_if(m_conns, [&](auto& c) { return c.get() == conn; });
}

std::vector<uint8_t> Bridge::Snapshot() {
  std::vector<Msg> msgs;
  msgs.push_back({Encode(kHello, 4,
                         [](Writer& w) {
                           w.UInt(kProtocolVersion);
                           w.UInt(HalTableHash());
                           w.Str(WEBSIM_WPILIB_VERSION);
                         }),
                  0});
  {
    std::scoped_lock lock{m_mutex};
    for (auto& [id, d] : m_devs) {
      msgs.push_back({SimDevMsg(d), 0});
    }
    for (auto& [id, v] : m_vals) {
      msgs.push_back({SimValNewMsg(v), 0});
    }
    for (auto& [key, e] : m_fields) {
      if (!e.known) {
        continue;
      }
      int field = key >> 16;
      int32_t index = key & 0xffff;
      msgs.push_back({FieldMsg(field, index, e.value), 0});
      if (field == m_encoderInitField && e.value.data.v_boolean) {
        msgs.push_back({EncoderChMsg(index), 0});
      }
    }
    for (auto& [id, b] : m_joyOut) {
      if (!b.msg.empty()) {
        msgs.push_back({b.msg, 0});
      }
    }
    if (!m_gameData.msg.empty()) {
      msgs.push_back({m_gameData.msg, 0});
    }
  }
  // LED strips have no "tell me the current data" callback; read them.
  for (int32_t i = 0; i < HAL_GetNumAddressableLEDs(); ++i) {
    static thread_local std::vector<HAL_AddressableLEDData> leds(
        HAL_kAddressableLEDMaxLength);
    int32_t n = HALSIM_GetAddressableLEDData(i, leds.data());
    if (n <= 0) {
      continue;
    }
    std::vector<uint8_t> rgb(n * 3);
    for (int32_t k = 0; k < n; ++k) {
      rgb[k * 3] = leds[k].r;
      rgb[k * 3 + 1] = leds[k].g;
      rgb[k * 3 + 2] = leds[k].b;
    }
    msgs.push_back({Encode(kLedData, 3,
                           [&](Writer& w) {
                             w.UInt(i);
                             w.Bin(rgb.data(), rgb.size());
                           }),
                    0});
  }
  msgs.push_back({TimingMsg(), 0});
  msgs.push_back({Encode(kSnapshotEnd, 1, [](Writer&) {}), 0});
  std::vector<const Msg*> all;
  for (auto& m : msgs) {
    all.push_back(&m);
  }
  return Frame(all);
}

void Bridge::Flush() {
  std::vector<Msg> msgs;
  {
    std::scoped_lock lock{m_mutex};
    msgs = std::move(m_structural);
    m_structural.clear();
    for (uint32_t key : m_dirtyFields) {
      FieldEntry& e = m_fields[key];
      e.dirty = false;
      int field = key >> 16;
      int32_t index = key & 0xffff;
      msgs.push_back({FieldMsg(field, index, e.value), e.origin});
      if (field == m_encoderInitField && e.value.data.v_boolean) {
        msgs.push_back({EncoderChMsg(index), 0});
      }
    }
    m_dirtyFields.clear();
    for (int32_t id : m_dirtyVals) {
      auto it = m_vals.find(id);
      if (it != m_vals.end()) {
        it->second.dirty = false;
        msgs.push_back({SimValMsg(it->second), it->second.origin});
      }
    }
    m_dirtyVals.clear();
    for (auto* blobs : {&m_leds, &m_joyOut}) {
      for (auto& [id, b] : *blobs) {
        if (b.dirty) {
          b.dirty = false;
          msgs.push_back({b.msg, 0});
        }
      }
    }
    if (m_gameData.dirty) {
      m_gameData.dirty = false;
      msgs.push_back({m_gameData.msg, 0});
    }
    if (++m_tick >= kTimingEveryN || m_timingNow) {
      m_tick = 0;
      m_timingNow = false;
      if (!m_conns.empty()) {
        msgs.push_back({TimingMsg(), 0});
      }
    }
  }
  if (msgs.empty() || m_conns.empty()) {
    return;
  }
  std::vector<const Msg*> forClient;
  forClient.reserve(msgs.size());
  for (auto& conn : m_conns) {
    if (!conn->data()) {
      continue;
    }
    forClient.clear();
    for (auto& m : msgs) {
      if (m.origin != conn->id()) {
        forClient.push_back(&m);
      }
    }
    if (!forClient.empty()) {
      conn->Send(Frame(forClient));
    }
  }
}

// ------------------------------------------------------- client -> robot

void Bridge::OnControl(std::string_view text) {
  // {"cmd":"disable"}: sent by the daemon when the last browser tab is gone.
  if (text.find("\"disable\"") != std::string_view::npos) {
    HALSIM_SetDriverStationEnabled(false);
    HALSIM_NotifyDriverStationNewData();
  }
}

void Bridge::OnFrame(Connection& conn, std::span<const uint8_t> data) {
  Reader r{data.data(), data.size()};
  Value frame = r.Next();
  if (frame.kind != Value::kArray) {
    return;
  }
  OriginScope origin{conn.id()};
  for (int64_t i = 0; i < frame.i && r.ok(); ++i) {
    Value msg = r.Next();
    if (msg.kind != Value::kArray || msg.i < 1) {
      return;
    }
    Apply(conn, r, msg.i);
  }
}

void Bridge::Apply(Connection&, Reader& r, int64_t elements) {
  Value op = r.Next();
  int64_t rest = elements - 1;
  if (op.kind != Value::kInt) {
    r.Skip(rest);
    return;
  }
  switch (op.i) {
    case kField: {
      if (rest != 3) {
        break;
      }
      Value field = r.Next(), index = r.Next(), value = r.Next();
      if (!r.ok() || field.kind != Value::kInt || index.kind != Value::kInt ||
          field.i < 0 || field.i >= kNumHalFields) {
        return;
      }
      const HalField& f = kHalFields[field.i];
      if (f.dir == Dir::OUT) {
        return;  // robot outputs are not writable from the GUI
      }
      int32_t idx = static_cast<int32_t>(index.i);
      bool valid = idx >= 0 &&
                   (f.channels ? (idx >> 8) < ClassOf(f).count() &&
                                     (idx & 0xff) < f.channels()
                               : idx < ClassOf(f).count());
      HAL_Value v;
      if (valid && ToHalValue(value, f.type, &v)) {
        f.set(idx, v);
      }
      return;
    }
    case kSimVal: {
      if (rest != 2) {
        break;
      }
      Value id = r.Next(), value = r.Next();
      if (!r.ok() || id.kind != Value::kInt) {
        return;
      }
      HAL_Type type;
      {
        std::scoped_lock lock{m_mutex};
        auto it = m_vals.find(static_cast<int32_t>(id.i));
        if (it == m_vals.end() ||
            it->second.direction == HAL_SimValueOutput) {
          return;
        }
        type = it->second.value.type;
      }
      HAL_Value v;
      if (ToHalValue(value, type, &v)) {
        HAL_SetSimValue(static_cast<int32_t>(id.i), &v);
      }
      return;
    }
    case kJoystick: {
      if (rest != 5) {
        break;
      }
      Value stick = r.Next();
      HAL_JoystickAxes axes{};
      Value a = r.Next();
      for (int64_t i = 0; a.kind == Value::kArray && i < a.i; ++i) {
        Value x = r.Next();
        if (i < HAL_kMaxJoystickAxes) {
          axes.axes[i] = static_cast<float>(x.AsDouble());
          axes.count = i + 1;
        }
      }
      HAL_JoystickPOVs povs{};
      Value p = r.Next();
      for (int64_t i = 0; p.kind == Value::kArray && i < p.i; ++i) {
        Value x = r.Next();
        if (i < HAL_kMaxJoystickPOVs) {
          povs.povs[i] = static_cast<int16_t>(x.AsInt());
          povs.count = i + 1;
        }
      }
      Value buttons = r.Next(), count = r.Next();
      if (!r.ok() || stick.kind != Value::kInt || stick.i < 0 ||
          stick.i >= HAL_kMaxJoysticks) {
        return;
      }
      HAL_JoystickButtons b{};
      b.buttons = static_cast<uint32_t>(buttons.AsInt());
      b.count = static_cast<uint8_t>(count.AsInt() > 32 ? 32 : count.AsInt());
      int s = static_cast<int>(stick.i);
      HALSIM_SetJoystickAxes(s, &axes);
      HALSIM_SetJoystickPOVs(s, &povs);
      HALSIM_SetJoystickButtons(s, &b);
      return;
    }
    case kNewData:
      r.Skip(rest);
      HALSIM_NotifyDriverStationNewData();
      return;
    case kGameData: {
      if (rest != 1) {
        break;
      }
      Value s = r.Next();
      if (r.ok() && s.kind == Value::kStr) {
        WPI_String str{s.s.data(), s.s.size()};
        HALSIM_SetGameSpecificMessage(&str);
      }
      return;
    }
    case kPause: {
      if (rest != 1) {
        break;
      }
      Value p = r.Next();
      if (r.ok()) {
        if (p.AsBool()) {
          HALSIM_PauseTiming();
        } else {
          HALSIM_ResumeTiming();
        }
        std::scoped_lock lock{m_mutex};
        m_timingNow = true;
      }
      return;
    }
    case kStep: {
      if (rest != 1) {
        break;
      }
      Value d = r.Next();
      if (r.ok() && d.IsNumber() && d.AsInt() > 0) {
        HALSIM_StepTimingAsync(static_cast<uint64_t>(d.AsInt()));
        std::scoped_lock lock{m_mutex};
        m_timingNow = true;
      }
      return;
    }
    default:
      break;
  }
  r.Skip(rest);  // unknown or malformed: ignore, keep the frame in step
}

}  // namespace

extern "C" {
// The one symbol the HAL looks up; everything else stays hidden.
__attribute__((visibility("default"))) int HALSIM_InitExtension(void) {
  std::puts("Web Sim Bridge Initializing.");
  static Bridge bridge;
  gBridge = &bridge;
  if (!bridge.Start()) {
    gBridge = nullptr;
    return -1;
  }
  HAL_OnShutdown(nullptr, [](void*) {
    if (gBridge) {
      Bridge* b = gBridge;
      gBridge = nullptr;
      b->Stop();
    }
  });
  HAL_RegisterExtension("halsim_webbridge", &bridge);
  std::puts("Web Sim Bridge Initialized!");
  return 0;
}
}  // extern "C"
