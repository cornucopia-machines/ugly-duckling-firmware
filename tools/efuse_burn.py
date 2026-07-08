#!/usr/bin/env python3
"""
Burn or read back the hardware identity eFuse record described in
docs/specs/Hardware-Version-in-eFuse.md.

Wraps `espefuse burn-block-data`, following the same "shell out to the ESP-IDF
tool" approach as scripts/gen_config_nvs.py. Requires IDF_PATH to be set
(source tools/activate_idf.sh first).

Usage:
  efuse_burn.py identity --port PORT --chip {esp32s3,esp32c6} \\
      --hw-gen N --hw-rev N --mfr-id N --serial N [--virt --path-efuse-file F]
  efuse_burn.py show --port PORT --chip {esp32s3,esp32c6} \\
      [--virt --path-efuse-file F]
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

MAGIC = 0x5544
FMT_VERSION = 1

BLOCK_SIZE = 32    # bytes; eFuse blocks on ESP32-S3/C6 are 256 bits

IDENTITY_BLOCK = "BLOCK_USR_DATA"


def find_espefuse():
    idf_python_env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if not idf_python_env:
        print("Error: IDF_PYTHON_ENV_PATH is not set. Run '. tools/activate_idf.sh' first.", file=sys.stderr)
        sys.exit(1)
    espefuse = os.path.join(idf_python_env, "bin", "espefuse.py")
    if not os.path.isfile(espefuse):
        print(f"Error: espefuse.py not found at {espefuse}", file=sys.stderr)
        sys.exit(1)
    return espefuse


def run_espefuse(args, extra_espefuse_args):
    espefuse = find_espefuse()
    cmd = [sys.executable, espefuse, "--chip", args.chip]
    if extra_espefuse_args:
        cmd += extra_espefuse_args
    if args.virt:
        cmd += ["--virt"]
        if args.path_efuse_file:
            cmd += ["--path-efuse-file", args.path_efuse_file]
    elif args.port:
        cmd += ["--port", args.port]
    return cmd


def burn_block(args, block_name, payload, extra_espefuse_args):
    if len(payload) > BLOCK_SIZE:
        raise ValueError(f"payload ({len(payload)} bytes) does not fit in a {BLOCK_SIZE}-byte block")
    padded = payload.ljust(BLOCK_SIZE, b"\x00")

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(padded)
        data_file = f.name

    try:
        cmd = run_espefuse(args, extra_espefuse_args)
        cmd += ["burn-block-data", "--offset", "0", block_name, data_file]
        if not args.virt:
            print(f"About to burn {block_name}: {padded.hex()}")
        result = subprocess.run(cmd)
        if result.returncode != 0:
            sys.exit(result.returncode)
    finally:
        os.unlink(data_file)


def cmd_identity(args):
    payload = struct.pack("<HBBBBI", MAGIC, FMT_VERSION, args.hw_gen, args.hw_rev, args.mfr_id, args.serial)
    print(f"Burning hardware identity: hw_gen={args.hw_gen} hw_rev={args.hw_rev} "
          f"mfr_id=0x{args.mfr_id:02x} serial={args.serial}")
    burn_block(args, IDENTITY_BLOCK, payload, ["--do-not-confirm"] if args.yes else [])


def read_blocks(args):
    cmd = run_espefuse(args, [])
    cmd += ["summary"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        sys.exit(result.returncode)
    return result.stdout


def parse_block_hex(summary_text, label):
    # espefuse prints the block's data a few lines after its label, e.g.:
    #   "BLOCK_USR_DATA (BLOCK3) ... User data\n   = 44 55 01 ... R/W"
    lines = summary_text.splitlines()
    for i, line in enumerate(lines):
        if line.strip().startswith(label):
            for data_line in lines[i + 1:i + 5]:
                if "=" not in data_line:
                    continue
                hex_bytes = data_line.split("=", 1)[1].strip().split()
                hex_bytes = [b for b in hex_bytes if len(b) == 2 and all(c in "0123456789abcdefABCDEF" for c in b)]
                if hex_bytes:
                    return bytes(int(b, 16) for b in hex_bytes)
            return None
    return None


def cmd_show(args):
    summary_text = read_blocks(args)

    identity_bytes = parse_block_hex(summary_text, "BLOCK_USR_DATA")
    if identity_bytes is None:
        print("Could not parse eFuse summary output:", file=sys.stderr)
        print(summary_text, file=sys.stderr)
        sys.exit(1)

    magic, fmt_version, hw_gen, hw_rev, mfr_id, serial = struct.unpack("<HBBBBI", identity_bytes[:10])

    if magic != MAGIC or fmt_version != FMT_VERSION:
        print(f"No valid hardware identity record found (magic=0x{magic:04x}, fmt_version={fmt_version}) "
              "— board is unburned or pre-dates this scheme.")
        return

    print(f"hw_gen:    {hw_gen}")
    print(f"hw_rev:    {hw_rev}")
    print(f"mfr_id:    0x{mfr_id:02x}")
    print(f"serial:    {serial}")


def add_common_args(parser):
    parser.add_argument("--chip", required=True, choices=["esp32s3", "esp32c6"])
    parser.add_argument("--port", help="Serial port (required unless --virt)")
    parser.add_argument("--virt", action="store_true", help="Dry-run against a virtual eFuse file, no hardware")
    parser.add_argument("--path-efuse-file", help="Virtual eFuse state file (used with --virt)")
    parser.add_argument("--yes", action="store_true", help="Skip espefuse's interactive confirmation prompt")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_identity = subparsers.add_parser("identity", help="Burn the one-time hardware identity record")
    add_common_args(p_identity)
    p_identity.add_argument("--hw-gen", type=int, required=True, help="Hardware generation, e.g. 11 for MK11")
    p_identity.add_argument("--hw-rev", type=int, required=True, help="Hardware sub-revision, 0 = first release")
    p_identity.add_argument("--mfr-id", type=int, required=True, help="Manufacturer/assembler ID (0 = unknown)")
    p_identity.add_argument("--serial", type=int, required=True, help="Unit serial number")
    p_identity.set_defaults(func=cmd_identity)

    p_show = subparsers.add_parser("show", help="Read back and decode the hardware identity record")
    add_common_args(p_show)
    p_show.set_defaults(func=cmd_show)

    args = parser.parse_args()

    if not args.virt and not args.port:
        parser.error("--port is required unless --virt is set")
    for field in ("hw_gen", "hw_rev", "mfr_id", "serial"):
        if hasattr(args, field) and not (0 <= getattr(args, field) <= (2**32 - 1 if field == "serial" else 255)):
            parser.error(f"--{field.replace('_', '-')} out of range")

    args.func(args)


if __name__ == "__main__":
    main()
