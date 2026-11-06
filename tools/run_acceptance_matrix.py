#!/usr/bin/env python3
"""Run MiniDB's complete local acceptance matrix into a fresh evidence tree.

The caller supplies a new build root so every configuration is configured from
scratch.  Existing directories are never deleted.  TSan runs are wrapped in
`setarch x86_64 -R` on Linux to avoid the WSL2 high-entropy ASLR runtime
failure; the wrapper also applies during build because GoogleTest discovery
executes test binaries.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys


EXPECTED_TEST_COUNT = 214

CONFIGS = [
    {
        "name": "debug",
        "build_type": "Debug",
        "extra": [],
        "jobs": 2,
        "env": {},
        "runner": [],
    },
    {
        "name": "release",
        "build_type": "Release",
        "extra": [],
        "jobs": 2,
        "env": {},
        "runner": [],
    },
    {
        "name": "asan",
        "build_type": "Debug",
        "extra": ["-DENABLE_ASAN=ON"],
        "jobs": 2,
        "env": {
            "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1:halt_on_error=1",
        },
        "runner": [],
    },
    {
        "name": "ubsan",
        "build_type": "Debug",
        "extra": ["-DENABLE_UBSAN=ON"],
        "jobs": 2,
        "env": {
            "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1",
        },
        "runner": [],
    },
    {
        "name": "asan-ubsan",
        "build_type": "Debug",
        "extra": ["-DENABLE_ASAN=ON", "-DENABLE_UBSAN=ON"],
        "jobs": 2,
        "env": {
            "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1:halt_on_error=1",
            "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1",
        },
        "runner": [],
    },
    {
        "name": "tsan",
        "build_type": "Debug",
        "extra": ["-DENABLE_TSAN=ON"],
        "jobs": 1,
        "env": {
            "TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1",
        },
        "runner": ["setarch", "x86_64", "-R"],
    },
]


def command_output(command):
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False)
    return result.stdout.strip()


def source_digest(source):
    required = [
        source / "CMakeLists.txt",
        source / "CMakePresets.json",
        source / ".clang-format",
        source / ".clang-tidy",
    ]
    files = [path for path in required if path.is_file()]
    for directory in ("app", "bench", "experiments", "fuzz", "src", "tests", "tools"):
        base = source / directory
        if not base.exists():
            continue
        files.extend(path for path in base.rglob("*") if path.is_file())
    filtered = []
    for path in files:
        if any(part in {"build", "__pycache__", ".git"} for part in path.parts):
            continue
        if path.name.endswith((".bak", ".orig", ".before_fix", ".pyc")):
            continue
        relative = path.relative_to(source)
        if relative.parts[:2] == ("bench", "results"):
            continue
        filtered.append(path)
    digest = hashlib.sha256()
    for path in sorted(filtered):
        digest.update(str(path.relative_to(source)).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest(), len(filtered)


def write_provenance(source, evidence):
    digest, file_count = source_digest(source)
    lines = [
        "timestamp_utc=" + datetime.datetime.now(
            datetime.timezone.utc).isoformat(),
        "source=" + str(source),
        "source_sha256=" + digest,
        "source_file_count=" + str(file_count),
        "uname=" + command_output(["uname", "-a"]),
        "cmake=" + command_output(["cmake", "--version"]).splitlines()[0],
        "cxx=" + command_output(["c++", "--version"]).splitlines()[0],
        "clang=" + command_output(["clang++", "--version"]).splitlines()[0],
        "git_head=" + command_output(
            ["git", "-C", str(source), "rev-parse", "HEAD"]),
        "git_status=",
        command_output(["git", "-C", str(source), "status", "--short"]),
        "",
    ]
    (evidence / "provenance.txt").write_text(
        "\n".join(lines), encoding="utf-8")


def run_command(command, cwd, environment, log_path):
    with log_path.open("w", encoding="utf-8") as log:
        log.write("$ " + shlex.join(command) + "\n")
        log.flush()
        process = subprocess.run(
            command, cwd=cwd, env=environment, stdout=log,
            stderr=subprocess.STDOUT)
        log.write(f"\nEXIT_CODE={process.returncode}\n")
    return process.returncode


def combined_sanitizer_evidence(build_dir):
    compile_log = build_dir / "compile_commands.json"
    compile_has_address = False
    compile_has_undefined = False
    if compile_log.is_file():
        entries = json.loads(compile_log.read_text(encoding="utf-8"))
        for entry in entries:
            command = entry.get("command") or " ".join(
                entry.get("arguments", []))
            compile_has_address |= "-fsanitize=address" in command
            compile_has_undefined |= "-fsanitize=undefined" in command

    link_file = None
    link_has_address = False
    link_has_undefined = False
    for candidate in sorted((build_dir / "tests").glob(
            "CMakeFiles/*.dir/link.txt")):
        text = candidate.read_text(encoding="utf-8", errors="replace")
        if "-fsanitize=address" in text:
            link_has_address = True
        if "-fsanitize=undefined" in text:
            link_has_undefined = True
        if link_has_address and link_has_undefined:
            link_file = str(candidate)
            break
    return {
        "compile_has_address": compile_has_address,
        "compile_has_undefined": compile_has_undefined,
        "link_has_address": link_has_address,
        "link_has_undefined": link_has_undefined,
        "link_file": link_file,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default=str(Path.cwd()))
    parser.add_argument("--root", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--gtest-source", default="")
    args = parser.parse_args()

    source = Path(args.source).resolve()
    root = Path(args.root).resolve()
    evidence = Path(args.evidence).resolve()
    if root.exists() and any(root.iterdir()):
        raise SystemExit(f"build root must be absent or empty: {root}")
    if evidence.exists() and any(evidence.iterdir()):
        raise SystemExit(f"evidence directory must be absent or empty: {evidence}")
    root.mkdir(parents=True, exist_ok=True)
    evidence.mkdir(parents=True, exist_ok=True)
    write_provenance(source, evidence)

    if not shutil.which("setarch"):
        raise SystemExit("setarch is required for the TSan WSL2 workaround")

    results = []
    for config in CONFIGS:
        name = config["name"]
        build_dir = root / name
        config_evidence = evidence / name
        config_evidence.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        environment.update(config["env"])
        runner = config["runner"]

        configure = [
            "cmake", "-S", str(source), "-B", str(build_dir),
            f"-DCMAKE_BUILD_TYPE={config['build_type']}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ] + config["extra"]
        if args.gtest_source:
            configure.append(
                "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=" +
                str(Path(args.gtest_source).resolve()))

        print(f"[{name}] configure", flush=True)
        configure_rc = run_command(
            runner + configure, source, environment,
            config_evidence / "configure.log")

        build_rc = None
        test_rc = None
        if configure_rc == 0:
            print(f"[{name}] build -j2", flush=True)
            build_rc = run_command(
                runner + ["cmake", "--build", str(build_dir), "-j2"],
                source, environment, config_evidence / "build.log")
        if build_rc == 0:
            print(f"[{name}] ctest", flush=True)
            test_rc = run_command(
                runner + ["ctest", "--test-dir", str(build_dir),
                          "--output-on-failure", f"-j{config['jobs']}"],
                source, environment, config_evidence / "ctest.log")

        test_count = None
        ctest_log = config_evidence / "ctest.log"
        if ctest_log.is_file():
            for line in ctest_log.read_text(
                    encoding="utf-8", errors="replace").splitlines():
                marker = "tests passed, 0 tests failed out of"
                if marker in line:
                    test_count = int(line.split("out of", 1)[1].strip())
                    break

        sanitizer_flags = None
        if name == "asan-ubsan":
            sanitizer_flags = combined_sanitizer_evidence(build_dir)

        result = {
            "config": name,
            "build_type": config["build_type"],
            "extra": config["extra"],
            "runner": runner,
            "configure_rc": configure_rc,
            "build_rc": build_rc,
            "ctest_rc": test_rc,
            "test_count": test_count,
            "combined_sanitizer": sanitizer_flags,
        }
        results.append(result)
        print(json.dumps(result), flush=True)

    summary = {
        "schema": "minidb-acceptance-matrix-v1",
        "source": str(source),
        "root": str(root),
        "evidence": str(evidence),
        "test_count_expected_per_config": EXPECTED_TEST_COUNT,
        "results": results,
    }
    (evidence / "matrix-summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)

    passed = len(results) == len(CONFIGS) and all(
        item["configure_rc"] == 0 and
        item["build_rc"] == 0 and
        item["ctest_rc"] == 0 and
        item["test_count"] == EXPECTED_TEST_COUNT
        for item in results)
    asan_ubsan = next(
        item for item in results if item["config"] == "asan-ubsan")
    passed = passed and bool(asan_ubsan["combined_sanitizer"]) and all(
        asan_ubsan["combined_sanitizer"].get(key, False)
        for key in ("compile_has_address", "compile_has_undefined",
                    "link_has_address", "link_has_undefined"))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
