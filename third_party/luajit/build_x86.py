"""Build the vendored pinned, client-gated x86/FR1 LuaJIT static archive."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


SOURCE = Path(__file__).resolve().parent
COMMIT = "63c8f1a9b479ba5518cf6a170ff7729178bc3cfd"
ARCHIVE_SHA256 = "9d1f1e5e4c7b91d7a047fb22a6bb0c1b074f6d29e011c7940c5b4000e7391bc1"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", required=True, type=Path,
                        help="Directory containing the i686 compiler and mingw32-make")
    parser.add_argument("--compiler", type=Path,
                        help="C compiler executable "
                             "(default: <toolchain>/i686-w64-mingw32-clang.exe)")
    parser.add_argument("--compiler-flag", action="append", default=[],
                        help="Extra flag passed to the compiler (repeatable)")
    parser.add_argument("--make", type=Path,
                        help="Windows-native GNU make executable "
                             "(default: <toolchain>/mingw32-make.exe)")
    parser.add_argument("--archiver", type=Path,
                        help="Archiver executable (default: <toolchain>/llvm-ar.exe)")
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    tools, build = args.toolchain.resolve(), args.build_dir.resolve()
    if build == SOURCE or SOURCE.is_relative_to(build) or build.is_relative_to(SOURCE):
        raise ValueError("LuaJIT generated files require a separate build directory")
    manifest = json.loads((SOURCE / "pinned_sources.json").read_text(encoding="utf-8"))
    if manifest["commit"] != COMMIT or manifest["archive_sha256"] != ARCHIVE_SHA256:
        raise ValueError("The vendored LuaJIT pin differs from the validated client runtime")
    for name, expected in manifest["files"].items():
        source = (SOURCE / name).resolve()
        target = (build / name).resolve()
        if not source.is_relative_to(SOURCE) or not target.is_relative_to(build):
            raise ValueError("Pinned LuaJIT input escapes its source or build directory")
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Pinned LuaJIT input changed: {name}")
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
    compiler = (args.compiler or tools / "i686-w64-mingw32-clang.exe").resolve()
    make = (args.make or tools / "mingw32-make.exe").resolve()
    archiver = (args.archiver or tools / "llvm-ar.exe").resolve()
    for tool in (compiler, make, archiver):
        if not tool.is_file():
            raise FileNotFoundError(tool)
    compiler_command = [str(compiler), *args.compiler_flag]
    compiler_value = subprocess.list2cmdline(compiler_command)
    tool_dirs = []
    for tool in (compiler, make, archiver):
        if tool.parent not in tool_dirs:
            tool_dirs.append(tool.parent)
    env = os.environ.copy()
    env.update(PATH=os.pathsep.join(str(path) for path in tool_dirs) + os.pathsep
                    + env["PATH"], TERM="", MSYSTEM="", OS="Windows_NT")
    command = [str(make), "-j2", "BUILDMODE=static", f"CC={compiler_value}",
               f"HOST_CC={compiler_value}", f"TARGET_AR={archiver.name} rcus",
               "GIT_RELVER=type ..\\.relver >luajit_relver.txt", "GIT_DEP=",
               "HOST_SYS=Windows", "TARGET_SYS=Windows", "XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT",
               "libluajit.a"]
    subprocess.run(command, cwd=build / "src", env=env, check=True)
    layout_object = build / "skeet_layout.o"
    subprocess.run([*compiler_command, "-std=c11", "-O2", "-m32", "-msse2",
                    "-DLUAJIT_ENABLE_LUA52COMPAT", "-Wall", "-Wextra", "-Werror",
                    "-I", str(build / "src"), "-c", str(SOURCE / "skeet_layout.c"),
                    "-o", str(layout_object)], env=env, check=True)
    archive = build / "src/libluajit.a"
    subprocess.run([str(archiver), "rcs", str(archive), str(layout_object)], env=env, check=True)
    shutil.copy2(SOURCE / "COPYRIGHT", build / "COPYRIGHT")
    print(f"Pinned LuaJIT x86/FR1/Lua52 static archive: {archive}", flush=True)


if __name__ == "__main__":
    main()
