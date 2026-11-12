#!/usr/bin/env python3
"""Assemble the licence notices for everything compiled into simgui.wasm.

usage: collect_licenses.py <allwpilib checkout>   (writes to stdout)
"""
import os
import sys
from pathlib import Path

EMSCRIPTEN_NOTE = """\
Emscripten runtime and system libraries (musl, libc++, libc++abi, compiler-rt)
are linked into the module. Emscripten is available under the MIT license and
the University of Illinois/NCSA Open Source License; see
https://github.com/emscripten-core/emscripten/blob/main/LICENSE
"""


def section(title: str, body: str) -> str:
    bar = "=" * 78
    return f"{bar}\n{title}\n{bar}\n\n{body.strip()}\n\n"


def main() -> int:
    src = Path(sys.argv[1])
    out = [
        "This file lists the third-party software compiled into the WPILib Web\n"
        "Sim GUI bundle (simgui.js / simgui.wasm) and the licences it is\n"
        "distributed under.\n\n"
    ]
    out.append(section("WPILib (allwpilib)", (src / "LICENSE.md").read_text()))
    out.append(
        section(
            "Third-party software distributed with WPILib",
            (src / "ThirdPartyNotices.txt").read_text(),
        )
    )
    emsdk = os.environ.get("EMSDK", "/emsdk")
    glfw = Path(emsdk) / "upstream/emscripten/cache/ports/contrib.glfw3/LICENSE.txt"
    if glfw.is_file():
        out.append(section("emscripten-glfw (contrib.glfw3 port)", glfw.read_text()))
    else:
        out.append(
            section(
                "emscripten-glfw (contrib.glfw3 port)",
                "Apache License 2.0; see https://github.com/pongasoft/emscripten-glfw",
            )
        )
    out.append(section("Emscripten", EMSCRIPTEN_NOTE))
    sys.stdout.write("".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
