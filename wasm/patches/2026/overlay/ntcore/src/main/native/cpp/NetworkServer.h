// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

// Web build: a browser cannot listen for connections, so the NT server is a
// stub that satisfies InstanceImpl. StartServer() in the browser is a no-op.

#pragma once

#include <functional>
#include <string_view>

#include "net/NetworkInterface.h"

namespace wpi {
class Logger;
}  // namespace wpi

namespace nt {

class IConnectionList;

class NetworkServer {
 public:
  NetworkServer(std::string_view persistentFilename,
                std::string_view listenAddress, unsigned int port3,
                unsigned int port4, net::ILocalStorage& localStorage,
                IConnectionList& connList, wpi::Logger& logger,
                std::function<void()> initDone) {
    if (initDone) {
      initDone();
    }
  }

  void FlushLocal() {}
  void Flush() {}
};

}  // namespace nt
