// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

// Web build: NT4 client on the browser WebSocket API. Replaces the libuv
// implementation; the protocol logic (net::ClientImpl) is upstream's.

#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "INetworkClient.h"
#include "net/ClientImpl.h"
#include "net/ClientMessageQueue.h"
#include "net/Message.h"
#include "net/NetworkInterface.h"
#include "net/WireConnection.h"

namespace wpi {
class Logger;
}  // namespace wpi

namespace nt {

class IConnectionList;
class BrowserWire;

class NetworkClient final : public INetworkClient {
 public:
  NetworkClient(
      int inst, std::string_view id, net::ILocalStorage& localStorage,
      IConnectionList& connList, wpi::Logger& logger,
      std::function<void(int64_t serverTimeOffset, int64_t rtt2, bool valid)>
          timeSyncUpdated);
  ~NetworkClient() final;

  void SetServers(
      std::span<const std::pair<std::string, unsigned int>> servers) final;
  void Disconnect() final;
  void StartDSClient(unsigned int port) final {}
  void StopDSClient() final {}
  void FlushLocal() final { HandleLocal(); }
  void Flush() final;

  // Browser event entry points (all on the main thread).
  void OnOpen();
  void OnClose(std::string_view reason);
  void OnText(std::string_view data);
  void OnBinary(std::span<const uint8_t> data);
  void Tick();

 private:
  void HandleLocal();
  void TryConnect();

  int m_inst;
  net::ILocalStorage& m_localStorage;
  IConnectionList& m_connList;
  wpi::Logger& m_logger;
  std::string m_id;
  std::function<void(int64_t, int64_t, bool)> m_timeSyncUpdated;

  using Queue = net::LocalClientMessageQueue;
  net::ClientMessage m_localMsgs[Queue::kBlockSize];
  Queue m_localQueue;

  std::string m_url;
  int m_socket = 0;
  int m_connHandle = 0;
  double m_nextConnect = 0;
  double m_nextPeriodic = 0;
  uint32_t m_periodMs = 100;
  long m_timer = 0;
  std::shared_ptr<BrowserWire> m_wire;
  std::unique_ptr<net::ClientImpl> m_clientImpl;
};

// NT3 is a raw TCP protocol and cannot be reached from a browser. This keeps
// InstanceImpl::StartClient3() compiling; it never connects.
class NetworkClient3 final : public INetworkClient {
 public:
  NetworkClient3(int inst, std::string_view id,
                 net::ILocalStorage& localStorage, IConnectionList& connList,
                 wpi::Logger& logger) {}

  void SetServers(
      std::span<const std::pair<std::string, unsigned int>> servers) final {}
  void Disconnect() final {}
  void StartDSClient(unsigned int port) final {}
  void StopDSClient() final {}
  void FlushLocal() final {}
  void Flush() final {}
};

}  // namespace nt
