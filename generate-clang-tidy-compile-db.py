#!/usr/bin/env python3

#
# Generate compile_commands.json for clang-tidy based on a GCC compile database.
#
# ESP-IDF v6.0+ uses picolibc via -specs=picolibc.specs, which Clang does not
# support.  This script queries GCC for the system include paths that the specs
# file would add and injects them as -isystem flags so that clang-tidy sees the
# correct (picolibc) headers instead of the legacy newlib ones.
#

import json
import subprocess
import re
from pathlib import Path

# Flags from GCC that are not supported by Clang
UNSUPPORTED_FLAGS = [
    "-fanalyzer",
    "-fno-shrink-wrap",
    "-fno-tree-switch-conversion",
    "-freorder-blocks",
    "-fstrict-volatile-bitfields",
    "-mlong-calls",
    "-mlongcalls",
    "-nostartfiles",
]

# Flag prefixes that need to be stripped (matched as startswith)
UNSUPPORTED_FLAG_PREFIXES = [
    "-specs=",
]

SCRIPT_DIR = Path(__file__).resolve().parent

# Extra args appended to each compile command
EXTRA_ARGS = [
    "-Wno-extern-c-compat",
    "-include", str(SCRIPT_DIR / "tools" / "clang-tidy-compat.h"),
]

def run(cmd):
    return subprocess.check_output(cmd, shell=True, text=True).strip()

def get_gcc_system_include_dirs(gcc_binary):
    """Query GCC for its system include search paths, including any
    paths injected by -specs=picolibc.specs."""
    output = subprocess.run(
        [gcc_binary, "-specs=picolibc.specs", "-E", "-x", "c++", "-v", "/dev/null"],
        capture_output=True, text=True
    ).stderr

    # Parse the "#include <...> search starts here:" block
    in_search = False
    dirs = []
    for line in output.splitlines():
        if "#include <...> search starts here:" in line:
            in_search = True
            continue
        if "End of search list" in line:
            break
        if in_search:
            path = line.strip()
            if path:
                dirs.append(str(Path(path).resolve()))
    return dirs

def extract_gcc_binary(command):
    """Extract the GCC binary path from a compile command."""
    return command.split()[0]

def patch_command(command, system_includes):
    tokens = command.split()
    tokens = [tok for tok in tokens
              if tok not in UNSUPPORTED_FLAGS
              and not any(tok.startswith(p) for p in UNSUPPORTED_FLAG_PREFIXES)]
    return tokens + [f"-isystem{inc}" for inc in system_includes] + EXTRA_ARGS

def fix_compile_commands(input_path, output_path):
    with open(input_path, "r", encoding="utf-8") as f:
        commands = json.load(f)

    # Discover include dirs from the first entry's compiler
    gcc_binary = extract_gcc_binary(commands[0]["command"])
    system_includes = get_gcc_system_include_dirs(gcc_binary)
    print(f"   GCC binary: {gcc_binary}")
    print(f"   System include dirs ({len(system_includes)}):")
    for d in system_includes:
        print(f"     {d}")

    for entry in commands:
        if "command" in entry:
            entry["command"] = " ".join(patch_command(entry["command"], system_includes))

    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(commands, f, indent=2)

    print(f"✅ Fixed compile_commands.json written to {output_path}")

# --- Main ---
if __name__ == "__main__":
    input_file = "build/compile_commands.json"
    output_file = "build/clang-tidy/compile_commands.json"

    fix_compile_commands(input_file, output_file)
