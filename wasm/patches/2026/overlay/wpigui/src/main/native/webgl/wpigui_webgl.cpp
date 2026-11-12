// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

// Web build of the wpigui platform layer: wpigui_opengl3.cpp adapted to
// WebGL2 (OpenGL ES 3.0). Differences from upstream are limited to the GL
// header, the context hints, the GLSL version and the missing BGRA format.

#include <cstdio>

#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "wpigui.h"
#include "wpigui_internal.h"

using namespace wpi::gui;

static bool gPlatformValid = false;

namespace wpi {

void gui::PlatformCreateContext() {}

void gui::PlatformDestroyContext() {}

void gui::PlatformGlfwInitHints() {}

void gui::PlatformGlfwWindowHints() {
  // Decide GL versions
  // WebGL2 == OpenGL ES 3.0 + GLSL ES 300
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

  // enable 4xMSAA
  glfwWindowHint(GLFW_SAMPLES, 4);
}

bool gui::PlatformInitRenderer() {
  glfwMakeContextCurrent(gContext->window);

  glfwSwapInterval(1);  // Enable vsync

  // Web build: WebGL2 needs no loader, and multisampling is a context
  // attribute (requested through GLFW_SAMPLES) rather than a GL enable.

  // Setup Platform/Renderer bindings
  ImGui_ImplGlfw_InitForOpenGL(gContext->window, true);

  // Decide GLSL versions
  const char* glsl_version = "#version 300 es";
  ImGui_ImplOpenGL3_Init(glsl_version);

  gPlatformValid = true;
  return true;
}

void gui::PlatformRenderFrame() {
  if (gContext->reloadFonts) {
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
    gContext->reloadFonts = false;
  }
  ImGui_ImplOpenGL3_NewFrame();

  CommonRenderFrame();

  int display_w, display_h;
  glfwGetFramebufferSize(gContext->window, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  glClearColor(gContext->clearColor.x, gContext->clearColor.y,
               gContext->clearColor.z, gContext->clearColor.w);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  glfwSwapBuffers(gContext->window);
}

void gui::PlatformShutdown() {
  gPlatformValid = false;
  ImGui_ImplOpenGL3_Shutdown();
}

void gui::PlatformFramebufferSizeChanged(int width, int height) {}

static inline GLenum GLPixelFormat(PixelFormat format) {
  switch (format) {
    case kPixelRGBA:
      return GL_RGBA;
    case kPixelBGRA:
      // WebGL2 has no BGRA upload format; nothing in the Sim GUI uses it.
      return GL_RGBA;
    default:
      return GL_RGBA;
  }
}

ImTextureID gui::CreateTexture(PixelFormat format, int width, int height,
                               const unsigned char* data) {
  if (!gPlatformValid) {
    return nullptr;
  }

  // Create a OpenGL texture identifier
  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);

  // Setup filtering parameters for display
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  // Upload pixels into texture
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
               GLPixelFormat(format), GL_UNSIGNED_BYTE, data);

  return reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(texture));
}

void gui::UpdateTexture(ImTextureID texture, PixelFormat format, int width,
                        int height, const unsigned char* data) {
  GLuint glTexture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture));
  if (glTexture == 0) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, glTexture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GLPixelFormat(format),
                  GL_UNSIGNED_BYTE, data);
}

void gui::DeleteTexture(ImTextureID texture) {
  if (!gPlatformValid) {
    return;
  }
  GLuint glTexture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture));
  if (glTexture != 0) {
    glDeleteTextures(1, &glTexture);
  }
}

}  // namespace wpi
