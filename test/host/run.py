#!/usr/bin/env python3
"""Build and run the host-side tests for the learner core.

Compiles NavienLearner, PeakFinder, BucketStore, SchedulerBase and TimeUtils
with a plain C++17 compiler behind the platform seam (LearnerPlatform.h with
LEARNER_PLATFORM_CUSTOM), plus upstream's two standalone harnesses
(TimeUtils_test.cpp, test/PeakFinder_test.cpp). No Arduino, HomeSpan or
ESP32 toolchain involved.

    python test/host/run.py        # build + run everything
    python test/host/run.py -v     # echo LEARNER_LOG output from the core
    CXX=clang++ python test/host/run.py

Compiler lookup: $CXX, then g++ / clang++ on PATH, then the MSYS2 UCRT
toolchain at C:\\msys64\\ucrt64\\bin (the dev machine).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BUILD = HERE / "build"

CORE = ["NavienLearner.cpp", "PeakFinder.cpp", "BucketStore.cpp", "SchedulerBase.cpp", "TimeUtils.cpp"]


def find_compiler() -> str:
    if env := os.environ.get("CXX"):
        return env
    for name in ("g++", "clang++"):
        if found := shutil.which(name):
            return found
    msys = Path(r"C:\msys64\ucrt64\bin\g++.exe")
    if msys.is_file():
        return str(msys)
    sys.exit(
        "No C++ compiler found. Install g++ (Linux: build-essential; "
        "Windows: MSYS2 + mingw-w64-ucrt-x86_64-gcc) or set CXX."
    )


def compiler_env(cxx: str) -> dict[str, str]:
    """MinGW's cc1plus/ld need the compiler's own bin directory on PATH or g++
    exits 1 with no message."""
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
    return env


def exe(name: str) -> Path:
    return BUILD / (name + (".exe" if os.name == "nt" else ""))


def build(cxx: str, out: Path, sources: list[str], extra: list[str]) -> int:
    cmd = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-Wno-missing-field-initializers", f"-I{ROOT}", *extra, "-o", str(out), *sources]
    if os.name == "nt":
        # mingw-w64 only declares the POSIX gmtime_r/localtime_r (which the
        # core uses, as newlib on the ESP32 does) behind this feature macro.
        cmd += ["-D_POSIX_THREAD_SAFE_FUNCTIONS", "-static-libgcc", "-static-libstdc++", "-static"]
    else:
        cmd += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    print("compiling", out.name)
    return subprocess.call(cmd, env=compiler_env(cxx), cwd=str(ROOT))


def run(out: Path, args: list[str]) -> int:
    try:
        return subprocess.call([str(out), *args], cwd=str(ROOT), timeout=120)
    except subprocess.TimeoutExpired:
        print(f"FAIL: {out.name} exceeded 120 s", file=sys.stderr)
        return 2


def main(argv: list[str]) -> int:
    verbose = "-v" in argv
    cxx = find_compiler()
    BUILD.mkdir(exist_ok=True)
    print("compiling with", cxx)

    core = exe("learner_core_test")
    rc = build(cxx, core, [str(HERE / "learner_core_test.cpp"), *[str(ROOT / c) for c in CORE]],
               ["-DLEARNER_PLATFORM_CUSTOM", f"-I{HERE}", f"-I{HERE / 'stubs'}"])
    if rc:
        return rc
    if rc := run(core, ["-v"] if verbose else []):
        return rc

    peak = exe("PeakFinder_test")
    if rc := build(cxx, peak, [str(ROOT / "test" / "PeakFinder_test.cpp"), str(ROOT / "PeakFinder.cpp")], []):
        return rc
    if rc := run(peak, []):
        return rc

    tu = exe("TimeUtils_test")
    if rc := build(cxx, tu, [str(ROOT / "TimeUtils_test.cpp"), str(ROOT / "TimeUtils.cpp")], []):
        return rc
    return run(tu, [])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
