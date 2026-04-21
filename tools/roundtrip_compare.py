"""
Compare the C++ codec (MusicDataCheck.exe) against musicdata_tool.py.

Prerequisites:
  - Python 3 with musicdata_tool.py on sys.path (repo root).
  - Built MusicDataCheck.exe (Release|x64) at plugin/bin/x64/Release/MusicDataCheck.exe.

Procedure:
  1) Pick a real music_*.bin (supported data_ver per musicdata_tool.handlers).
  2) Python extract -> py.json; C++ extract -> cpp.json; normalize JSON and diff (or compare key fields).
  3) Python create from py.json -> py.bin; C++ create from same json -> cpp.bin; compare files in binary (fc /b).

Example (PowerShell from repo root):
  python plugin/tools/roundtrip_compare.py path\\to\\music_foo.bin
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser(description="Compare Python musicdata_tool vs MusicDataCheck.exe")
    p.add_argument("bin", type=Path, help="Input music_*.bin")
    p.add_argument("--exe", type=Path, default=None, help="Path to MusicDataCheck.exe")
    args = p.parse_args()

    root = Path(__file__).resolve().parents[1]
    exe = args.exe or (root / "bin" / "x64" / "Release" / "MusicDataCheck.exe")
    if not exe.is_file():
        print(f"MusicDataCheck not found: {exe}", file=sys.stderr)
        return 2

    repo = root.parent
    sys.path.insert(0, str(repo))
    import musicdata_tool as mdt  # type: ignore

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        py_json = td / "py.json"
        cpp_json = td / "cpp.json"
        py_bin = td / "py.bin"
        cpp_bin = td / "cpp.bin"

        mdt.extract_file(str(args.bin), str(py_json), in_memory=False)
        subprocess.check_call([str(exe), "extract", str(args.bin), str(cpp_json)])

        with py_json.open("r", encoding="utf-8") as f:
            jp = json.load(f)
        with cpp_json.open("r", encoding="utf-8") as f:
            jc = json.load(f)
        if jp != jc:
            print("JSON extract mismatch between Python and C++.", file=sys.stderr)
            return 3

        mdt.create_file(str(py_json), str(py_bin), None)
        subprocess.check_call([str(exe), "create", str(py_json), str(cpp_bin)])

        a = py_bin.read_bytes()
        b = cpp_bin.read_bytes()
        if a != b:
            print(f"Binary create mismatch: len {len(a)} vs {len(b)}", file=sys.stderr)
            for i, (x, y) in enumerate(zip(a, b)):
                if x != y:
                    print(f"First diff at {i}: {x:#x} vs {y:#x}", file=sys.stderr)
                    break
            return 4

    print("OK: extract JSON identical; create .bin byte-identical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
