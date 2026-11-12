// Web build replacement for portable-file-dialogs.cpp.
//
// A browser has no native file dialogs and no access to the workspace's
// files. Each dialog is handed to the page (Module.websimFileDialog), which
// shows a picker over the robot project directory, makes the chosen file or
// folder available in the in-memory filesystem under its real path, and then
// reports that path back through websim_dialog_result(). Glass already polls
// dialogs with ready(0), so nothing blocks.
//
// The public interface is that of portable-file-dialogs.h (WTFPL, Sam
// Hocevar); only the implementation differs.

#include "portable-file-dialogs.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <emscripten.h>

extern "C" void websim_wake(int ms);

namespace pfd {
namespace internal {

class executor {
 public:
  int id = 0;
  bool done = false;
  std::string result;
};

}  // namespace internal
}  // namespace pfd

static int gNextDialog = 1;
static std::map<int, std::weak_ptr<pfd::internal::executor>> gDialogs;

extern "C" EMSCRIPTEN_KEEPALIVE void websim_dialog_result(int id,
                                                          const char* path) {
  auto it = gDialogs.find(id);
  if (it == gDialogs.end()) {
    return;
  }
  if (auto exec = it->second.lock()) {
    exec->result = path ? path : "";
    exec->done = true;
  }
  gDialogs.erase(it);
  websim_wake(250);
}

namespace pfd {

// one slot per settings::flag value; none are ever set in the browser
static bool gFlags[16];

settings::settings(bool resync) {}
bool settings::available() { return true; }
void settings::verbose(bool value) {}
void settings::rescan() {}
bool settings::check_program(std::string const& program) { return false; }
bool const& settings::flags(flag in_flag) const {
  return gFlags[size_t(in_flag)];
}
bool& settings::flags(flag in_flag) { return gFlags[size_t(in_flag)]; }

namespace internal {

dialog::dialog() : m_async{std::make_shared<executor>()} {
  // Until a request is issued the dialog counts as finished ("cancelled").
  m_async->done = true;
}

dialog::~dialog() {
  if (m_async && !m_async->done) {
    EM_ASM(
        {
          if (Module["websimFileDialogCancel"]) {
            Module["websimFileDialogCancel"]($0);
          }
        },
        m_async->id);
    gDialogs.erase(m_async->id);
  }
}

bool dialog::ready(int timeout) const { return m_async->done; }

bool dialog::kill() const {
  m_async->done = true;
  m_async->result.clear();
  return true;
}

std::vector<std::string> dialog::desktop_helper() const { return {}; }
std::string dialog::buttons_to_name(choice) { return {}; }
std::string dialog::get_icon_name(icon) { return {}; }
std::string dialog::powershell_quote(std::string const& str) const {
  return str;
}
std::string dialog::osascript_quote(std::string const& str) const {
  return str;
}
std::string dialog::shell_quote(std::string const& str) const { return str; }

class file_dialog::Impl {};

file_dialog::file_dialog(type in_type, std::string const& title,
                         std::string const& default_path,
                         std::vector<std::string> const& filters,
                         opt options) {
  static const char* const kKinds[] = {"open", "save", "folder"};
  // filters are (name, glob list) pairs; pass the globs, newline separated
  std::string globs;
  for (size_t i = 1; i < filters.size(); i += 2) {
    if (!globs.empty()) {
      globs += '\n';
    }
    globs += filters[i];
  }
  m_async->id = gNextDialog++;
  int handled = EM_ASM_INT(
      {
        if (!Module["websimFileDialog"]) {
          return 0;
        }
        Module["websimFileDialog"]({
          id : $0,
          kind : UTF8ToString($1),
          title : UTF8ToString($2),
          defaultPath : UTF8ToString($3),
          filters : UTF8ToString($4).split("\n").filter(Boolean),
        });
        return 1;
      },
      m_async->id, kKinds[in_type], title.c_str(), default_path.c_str(),
      globs.c_str());
  if (handled) {
    m_async->done = false;
    gDialogs[m_async->id] = m_async;
  }
}

std::string file_dialog::string_result() { return m_async->result; }

std::vector<std::string> file_dialog::vector_result() {
  if (m_async->result.empty()) {
    return {};
  }
  return {m_async->result};
}

}  // namespace internal

notify::notify(std::string const& title, std::string const& message,
               icon _icon) {
  EM_ASM({ console.info(UTF8ToString($0) + ": " + UTF8ToString($1)); },
         title.c_str(), message.c_str());
}

message::message(std::string const& title, std::string const& text,
                 choice _choice, icon _icon) {
  EM_ASM({ console.info(UTF8ToString($0) + ": " + UTF8ToString($1)); },
         title.c_str(), text.c_str());
}

button message::result() { return button::ok; }

}  // namespace pfd
