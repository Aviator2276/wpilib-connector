// fields::GetFields() for the web build.
//
// Upstream embeds 9.7 MB of field images as resources. Here the page lists
// what the server offers (Module.websimFields = [{name, json, image}], with
// absolute or page-relative URLs) and each file is downloaded the first time
// Glass asks for it, i.e. when the user picks that field.

#include <fields/fields.h>

#include <array>
#include <span>
#include <string_view>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <emscripten.h>

namespace {

constexpr int kMaxFields = 48;

struct Entry {
  std::string name;
  std::string data[2];  // json, image
  bool loaded[2] = {false, false};
};

std::vector<Entry> gEntries;
std::vector<fields::Field> gFields;

// Synchronous fetch: Glass's Field interface returns the bytes directly.
// Runs only on an explicit user choice, for one or two same-origin files.
EM_JS(char*, websim_fetch_field, (int index, int which, int* size), {
  try {
    var list = Module["websimFields"] || [];
    var f = list[index];
    if (!f) return 0;
    var xhr = new XMLHttpRequest();
    xhr.open("GET", which ? f.image : f.json, false);
    xhr.overrideMimeType("text/plain; charset=x-user-defined");
    xhr.send();
    if (xhr.status !== 200) return 0;
    var text = xhr.responseText;
    var n = text.length;
    var ptr = _malloc(n + 1);
    for (var i = 0; i < n; i++) HEAPU8[ptr + i] = text.charCodeAt(i) & 0xff;
    HEAPU8[ptr + n] = 0;
    HEAP32[size >> 2] = n;
    return ptr;
  } catch (e) {
    console.warn("field download failed", e);
    return 0;
  }
});

std::string_view Load(int index, int which) {
  if (index >= static_cast<int>(gEntries.size())) {
    return {};
  }
  auto& e = gEntries[index];
  if (!e.loaded[which]) {
    int size = 0;
    if (char* p = websim_fetch_field(index, which, &size)) {
      e.data[which].assign(p, size);
      std::free(p);
      e.loaded[which] = true;
    }
  }
  return e.data[which];
}

template <int N>
std::string_view Json() {
  return Load(N, 0);
}
template <int N>
std::string_view Image() {
  return Load(N, 1);
}

using Getter = std::string_view (*)();

template <int... N>
constexpr std::array<std::pair<Getter, Getter>, sizeof...(N)> MakeGetters(
    std::integer_sequence<int, N...>) {
  return {{{&Json<N>, &Image<N>}...}};
}

constexpr auto kGetters =
    MakeGetters(std::make_integer_sequence<int, kMaxFields>{});

}  // namespace

std::span<const fields::Field> fields::GetFields() {
  static bool init = false;
  if (!init) {
    init = true;
    int count = EM_ASM_INT({ return (Module["websimFields"] || []).length; });
    if (count > kMaxFields) {
      count = kMaxFields;
    }
    gEntries.resize(count);
    for (int i = 0; i < count; ++i) {
      char* name = static_cast<char*>(EM_ASM_PTR(
          { return stringToNewUTF8(Module["websimFields"][$0].name || ""); },
          i));
      gEntries[i].name = name;
      std::free(name);
    }
    for (int i = 0; i < count; ++i) {
      gFields.push_back(
          {gEntries[i].name.c_str(), kGetters[i].first, kGetters[i].second});
    }
  }
  return gFields;
}
