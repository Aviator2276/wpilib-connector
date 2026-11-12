// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

// Web build: open URLs in a new browser tab instead of spawning xdg-open.

#include "wpigui_openurl.h"

#include <string>

#include <emscripten.h>

void wpi::gui::OpenURL(const std::string& url) {
  EM_ASM({ window.open(UTF8ToString($0), "_blank", "noopener"); }, url.c_str());
}
