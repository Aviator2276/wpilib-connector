// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "NetworkClient.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <emscripten.h>
#include <emscripten/websocket.h>
#include <fmt/format.h>
#include <wpi/raw_ostream.h>
#include <wpi/timestamp.h>

#include "IConnectionList.h"
#include "Log.h"

// Provided by the web sim app: asks for frames to be drawn for the next `ms`.
extern "C" void websim_wake(int ms);

using namespace nt;

namespace nt {

// net::WireConnection on a browser WebSocket.
//
// The browser buffers sends, so writes never report backpressure. Consecutive
// text messages are batched into one JSON array frame and consecutive binary
// messages into one binary frame, preserving order, as the libuv wire does.
//
// Browsers cannot send or observe WebSocket ping frames, which is how NT 4.1
// checks liveness. The wire therefore advertises 4.1, sends no pings, and
// reports "data just received" for as long as the socket is open; a dead
// connection is detected by the socket's close event instead. (Falling back
// to NT 4.0's timestamp keepalive is not an option: that client path
// disconnects itself after a few seconds.)
class BrowserWire final : public net::WireConnection {
 public:
  explicit BrowserWire(int socket) : m_socket{socket} {}

  unsigned int GetVersion() const final { return 0x0401; }

  void SendPing(uint64_t time) final {}

  bool Ready() const final { return true; }

  int WriteText(wpi::function_ref<void(wpi::raw_ostream& os)> writer) final {
    if (m_state == kBinary) {
      FlushBuf();
    }
    wpi::raw_string_ostream os{m_buf};
    os << (m_state == kText ? ',' : '[');
    m_state = kText;
    writer(os);
    return 0;
  }

  int WriteBinary(wpi::function_ref<void(wpi::raw_ostream& os)> writer) final {
    if (m_state == kText) {
      FlushBuf();
    }
    m_state = kBinary;
    wpi::raw_string_ostream os{m_buf};
    writer(os);
    return 0;
  }

  int Flush() final {
    FlushBuf();
    m_lastFlush = wpi::Now();
    return 0;
  }

  void SendText(wpi::function_ref<void(wpi::raw_ostream& os)> writer) final {
    std::string s;
    wpi::raw_string_ostream os{s};
    os << '[';
    writer(os);
    os << ']';
    emscripten_websocket_send_utf8_text(m_socket, s.c_str());
  }

  void SendBinary(wpi::function_ref<void(wpi::raw_ostream& os)> writer) final {
    std::string s;
    wpi::raw_string_ostream os{s};
    writer(os);
    emscripten_websocket_send_binary(m_socket, s.data(), s.size());
  }

  uint64_t GetLastFlushTime() const final { return m_lastFlush; }

  uint64_t GetLastReceivedTime() const final {
    return static_cast<uint64_t>(emscripten_get_now());
  }

  void StopRead() final {}
  void StartRead() final {}

  void Disconnect(std::string_view reason) final {
    emscripten_websocket_close(m_socket, 1000, "");
  }

 private:
  enum State { kEmpty, kText, kBinary };

  void FlushBuf() {
    if (m_state == kText) {
      m_buf += ']';
      emscripten_websocket_send_utf8_text(m_socket, m_buf.c_str());
    } else if (m_state == kBinary) {
      emscripten_websocket_send_binary(m_socket, m_buf.data(), m_buf.size());
    }
    m_buf.clear();
    m_state = kEmpty;
  }

  int m_socket;
  State m_state = kEmpty;
  std::string m_buf;
  uint64_t m_lastFlush = 0;
};

}  // namespace nt

static bool OnOpenCb(int, const EmscriptenWebSocketOpenEvent*, void* ud) {
  static_cast<NetworkClient*>(ud)->OnOpen();
  return true;
}

static bool OnCloseCb(int, const EmscriptenWebSocketCloseEvent* e, void* ud) {
  static_cast<NetworkClient*>(ud)->OnClose(e->reason);
  return true;
}

static bool OnErrorCb(int, const EmscriptenWebSocketErrorEvent*, void*) {
  // A close event always follows; reconnect is handled there.
  return true;
}

static bool OnMessageCb(int, const EmscriptenWebSocketMessageEvent* e,
                        void* ud) {
  auto self = static_cast<NetworkClient*>(ud);
  if (e->isText) {
    // text frames arrive NUL-terminated; numBytes includes the terminator
    self->OnText({reinterpret_cast<const char*>(e->data),
                  e->numBytes > 0 ? e->numBytes - 1 : 0});
  } else {
    self->OnBinary({e->data, e->numBytes});
  }
  return true;
}

NetworkClient::NetworkClient(
    int inst, std::string_view id, net::ILocalStorage& localStorage,
    IConnectionList& connList, wpi::Logger& logger,
    std::function<void(int64_t, int64_t, bool)> timeSyncUpdated)
    : m_inst{inst},
      m_localStorage{localStorage},
      m_connList{connList},
      m_logger{logger},
      m_id{id},
      m_timeSyncUpdated{std::move(timeSyncUpdated)},
      m_localQueue{logger} {
  // One timer drives reconnects and periodic sends. It runs independently of
  // requestAnimationFrame so data keeps flowing while the tab is hidden.
  m_timer = emscripten_set_interval(
      [](void* ud) { static_cast<NetworkClient*>(ud)->Tick(); }, 10, this);
}

NetworkClient::~NetworkClient() {
  emscripten_clear_interval(m_timer);
  if (m_socket > 0) {
    emscripten_websocket_close(m_socket, 1000, "");
    emscripten_websocket_delete(m_socket);
  }
}

void NetworkClient::SetServers(
    std::span<const std::pair<std::string, unsigned int>> servers) {
  // The page says where the NT proxy is (Module.ntUrl, a ws:/wss: URL without
  // the client id). Without it, fall back to the first server given.
  char* fromPage = static_cast<char*>(EM_ASM_PTR({
    var u = (typeof Module !== "undefined" && Module["ntUrl"]) || "";
    return stringToNewUTF8(u);
  }));
  std::string base = fromPage;
  std::free(fromPage);
  if (base.empty() && !servers.empty()) {
    base = fmt::format("ws://{}:{}/nt", servers[0].first,
                       servers[0].second == 0 ? 5810 : servers[0].second);
  }
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  m_url = base.empty() ? std::string{} : fmt::format("{}/{}", base, m_id);
  m_nextConnect = 0;
}

void NetworkClient::TryConnect() {
  if (m_url.empty() || m_socket > 0) {
    return;
  }
  EmscriptenWebSocketCreateAttributes attr;
  emscripten_websocket_init_create_attributes(&attr);
  attr.url = m_url.c_str();
  attr.protocols =
      "v4.1.networktables.first.wpi.edu,networktables.first.wpi.edu";
  attr.createOnMainThread = true;
  m_socket = emscripten_websocket_new(&attr);
  if (m_socket <= 0) {
    m_socket = 0;
    return;
  }
  emscripten_websocket_set_onopen_callback(m_socket, this, OnOpenCb);
  emscripten_websocket_set_onclose_callback(m_socket, this, OnCloseCb);
  emscripten_websocket_set_onerror_callback(m_socket, this, OnErrorCb);
  emscripten_websocket_set_onmessage_callback(m_socket, this, OnMessageCb);
}

void NetworkClient::OnOpen() {
  ConnectionInfo connInfo;
  connInfo.remote_id = "";
  connInfo.remote_ip = m_url;
  connInfo.remote_port = 0;
  connInfo.protocol_version = 0x0401;
  INFO("CONNECTED NT4 to {}", m_url);
  m_connHandle = m_connList.AddConnection(connInfo);

  m_wire = std::make_shared<BrowserWire>(m_socket);
  m_clientImpl = std::make_unique<net::ClientImpl>(
      static_cast<uint64_t>(emscripten_get_now()), *m_wire, m_logger,
      m_timeSyncUpdated,
      [this](uint32_t repeatMs) { m_periodMs = repeatMs; });
  m_clientImpl->SetLocal(&m_localStorage);
  m_localStorage.StartNetwork(&m_localQueue);
  HandleLocal();
  m_clientImpl->SendInitial();
  websim_wake(250);
}

void NetworkClient::OnClose(std::string_view reason) {
  bool wasConnected = static_cast<bool>(m_clientImpl);
  m_clientImpl.reset();
  m_wire.reset();
  if (m_socket > 0) {
    emscripten_websocket_delete(m_socket);
    m_socket = 0;
  }
  if (wasConnected) {
    INFO("DISCONNECTED NT4 connection: {}", reason);
    m_localStorage.ClearNetwork();
    m_localQueue.ClearQueue();
    m_connList.RemoveConnection(m_connHandle);
    m_connHandle = 0;
    m_timeSyncUpdated(0, 0, false);
    websim_wake(250);
  }
  m_nextConnect = emscripten_get_now() + 1000;
}

void NetworkClient::OnText(std::string_view data) {
  if (m_clientImpl) {
    m_clientImpl->ProcessIncomingText(data);
    websim_wake(0);
  }
}

void NetworkClient::OnBinary(std::span<const uint8_t> data) {
  if (m_clientImpl) {
    m_clientImpl->ProcessIncomingBinary(
        static_cast<uint64_t>(emscripten_get_now()), data);
    websim_wake(0);
  }
}

void NetworkClient::Tick() {
  double now = emscripten_get_now();
  if (!m_clientImpl) {
    if (m_socket == 0 && now >= m_nextConnect) {
      m_nextConnect = now + 1000;
      TryConnect();
    }
    return;
  }
  if (now >= m_nextPeriodic) {
    m_nextPeriodic = now + (m_periodMs < 100 ? m_periodMs : 100);
    HandleLocal();
    if (m_clientImpl) {
      m_clientImpl->SendOutgoing(static_cast<uint64_t>(now), false);
    }
  }
}

void NetworkClient::HandleLocal() {
  for (;;) {
    auto msgs = m_localQueue.ReadQueue(m_localMsgs);
    if (msgs.empty()) {
      return;
    }
    if (m_clientImpl) {
      m_clientImpl->HandleLocal(msgs);
    }
  }
}

void NetworkClient::Flush() {
  HandleLocal();
  if (m_clientImpl) {
    m_clientImpl->SendOutgoing(static_cast<uint64_t>(emscripten_get_now()),
                               true);
  }
}

void NetworkClient::Disconnect() {
  if (m_wire) {
    m_wire->Disconnect("requested by application");
  }
}
