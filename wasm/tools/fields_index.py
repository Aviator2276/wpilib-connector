#!/usr/bin/env python3
"""Write the index of WPILib's built-in field images as JSON on stdout.

The desktop GUI embeds these as resources and lists them in fields.cpp. The
web build serves the files instead; this keeps upstream's names and order.

usage: fields_index.py <fieldImages/.../fields.cpp> <dir with the resources>
"""
import json
import re
import sys
from pathlib import Path


def main() -> int:
    fields_cpp, resources = Path(sys.argv[1]), Path(sys.argv[2])
    entries = re.findall(
        r'\{\s*"([^"]+)"\s*,\s*GetResource_(\w+)_json\s*,', fields_cpp.read_text()
    )
    if not entries:
        print("no fields found in fields.cpp; upstream layout changed", file=sys.stderr)
        return 1
    out = []
    for name, ident in entries:
        json_file = ident.replace("_", "-", 1) + ".json"
        path = resources / json_file
        if not path.is_file():
            print(f"missing field resource {json_file}", file=sys.stderr)
            return 1
        image = json.loads(path.read_text())["field-image"]
        if not (resources / image).is_file():
            print(f"missing field image {image}", file=sys.stderr)
            return 1
        out.append({"name": name, "json": json_file, "image": image})
    json.dump(out, sys.stdout, indent=1)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
