#!/usr/bin/env python3
"""Extract only the 25 Grids drum-map nodes into portable C++.

The source file is Mutable Instruments Grids resources.cc (GPL-3.0).
Generated GridResources.cpp therefore remains GPL-3.0-derived data.
"""
from pathlib import Path
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: extract_grids_resources.py /path/to/grids/resources.cc")

src_path = Path(sys.argv[1])
text = src_path.read_text(encoding="utf-8")

nodes = {}
pattern = re.compile(
    r"const\s+prog_uint8_t\s+node_(\d+)\[\]\s+PROGMEM\s*=\s*\{(.*?)\};",
    re.S,
)
for m in pattern.finditer(text):
    idx = int(m.group(1))
    values = [int(x) for x in re.findall(r"\b\d+\b", m.group(2))]
    if len(values) != 96:
        raise SystemExit(f"node_{idx} has {len(values)} values, expected 96")
    nodes[idx] = values

missing = [i for i in range(25) if i not in nodes]
if missing:
    raise SystemExit(f"missing Grids nodes: {missing}")

header = """/* Auto-generated from Mutable Instruments Grids resources.cc.\n   Original copyright Emilie Gillet. GPL-3.0. */\n#pragma once\n#include <cstdint>\nnamespace gridscape {\n"""
for i in range(25):
    header += f"extern const uint8_t node_{i}[96];\n"
header += "} // namespace gridscape\n"

cpp = """/* Auto-generated from Mutable Instruments Grids resources.cc.\n   Original copyright Emilie Gillet. GPL-3.0. */\n#include \"GridResources.h\"\nnamespace gridscape {\n"""
for i in range(25):
    vals = nodes[i]
    cpp += f"const uint8_t node_{i}[96] = {{\n"
    for row in range(0, 96, 12):
        cpp += "    " + ", ".join(f"{v:3d}" for v in vals[row:row+12]) + ",\n"
    cpp += "};\n"
cpp += "} // namespace gridscape\n"

out_dir = Path(__file__).resolve().parent
(out_dir / "GridResources.h").write_text(header, encoding="utf-8")
(out_dir / "GridResources.cpp").write_text(cpp, encoding="utf-8")
print("Extracted 25 Grids drum-map nodes (2400 bytes) into GridResources.cpp")
