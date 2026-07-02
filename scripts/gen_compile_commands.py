#!/usr/bin/env python3
"""Generate compile_commands.json for editor tooling (VSCode/clangd, Cline, etc.).

Runs `make -Bn` (dry run) against the default build and, if ROCm headers are
found, against the AMDGPU=1 build, then turns the printed gcc invocations
into a JSON compilation database. This project's Makefile has no
autoconf/cmake layer, so this reconstructs the same ROCM_DIR auto-detection
`make` does rather than trying to share it directly.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def detect_rocm_dir():
    for candidate in ("/opt/rocm", "/usr"):
        if (Path(candidate) / "include/amd_smi/amdsmi.h").exists():
            return candidate
    return None


def dry_run(make_args):
    result = subprocess.run(
        ["make", "-Bn", *make_args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.splitlines()


def parse_commands(lines):
    """Map source file -> compile command, for every gcc line mentioning a .c file."""
    entries = {}
    for line in lines:
        line = line.strip()
        if not line.startswith("gcc "):
            continue
        for token in line.split():
            if token.endswith(".c"):
                entries[token] = line
    return entries


def build_database():
    database = {}

    default_lines = dry_run([])
    database.update(parse_commands(default_lines))

    rocm_dir = detect_rocm_dir()
    if rocm_dir:
        gpu_lines = dry_run([f"AMDGPU=1", f"ROCM_DIR={rocm_dir}"])
        # GPU build is a superset (also covers amd_smi.c/amd_sysfs.c), and its
        # flags are a strict superset for shared files, so let it win.
        database.update(parse_commands(gpu_lines))
    else:
        print(
            "note: no ROCm headers found under /opt/rocm or /usr; "
            "amd_smi.c/amd_sysfs.c will be missing from compile_commands.json",
            file=sys.stderr,
        )

    return [
        {"directory": str(REPO_ROOT), "file": file, "command": command}
        for file, command in sorted(database.items())
    ]


def main():
    if shutil.which("gcc") is None:
        print("error: gcc not found in PATH", file=sys.stderr)
        return 1

    compile_commands = build_database()
    out_path = REPO_ROOT / "compile_commands.json"
    out_path.write_text(json.dumps(compile_commands, indent=2) + "\n")
    print(f"wrote {len(compile_commands)} entries to {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
