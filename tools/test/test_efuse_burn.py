#!/usr/bin/env python3
"""
Tests for ../efuse_burn.py, run entirely against espefuse's virtual eFuse
mode (--virt), so no hardware is required.

Requires IDF_PYTHON_ENV_PATH to point at a Python environment with espefuse
installed — source tools/activate_idf.sh first for local runs. CI runs this
as a step in the embedded-test job (.github/workflows/build.yml), inside the
same ESP-IDF Docker image the build job uses.

Usage: python3 -m unittest discover -s tools/test -v
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = os.path.join(os.path.dirname(__file__), "..", "efuse_burn.py")


def run(*args):
    return subprocess.run(
        [sys.executable, SCRIPT, *args],
        capture_output=True, text=True,
    )


class EfuseBurnTest(unittest.TestCase):
    def setUp(self):
        if not os.environ.get("IDF_PYTHON_ENV_PATH"):
            self.skipTest("IDF_PYTHON_ENV_PATH is not set; run '. tools/activate_idf.sh' first")
        self.tmp_dir = tempfile.mkdtemp()
        self.efuse_file = os.path.join(self.tmp_dir, "efuse.bin")

    def tearDown(self):
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    def identity(self, chip="esp32c6", hw_gen=11, hw_rev=0, mfr_id=1, batch=0, serial=1042):
        # --mfr-id, --batch, and --serial are passed as hex to exercise parse_int's "0x..." handling.
        return run(
            "identity", "--chip", chip, "--virt", "--path-efuse-file", self.efuse_file,
            "--hw-gen", str(hw_gen), "--hw-rev", str(hw_rev),
            "--mfr-id", hex(mfr_id), "--batch", hex(batch), "--serial", hex(serial), "--yes",
        )

    def show(self, chip="esp32c6"):
        return run("show", "--chip", chip, "--virt", "--path-efuse-file", self.efuse_file)

    def test_show_on_unburned_board_reports_no_record(self):
        result = self.show()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("unburned or pre-dates this scheme", result.stdout)

    def test_identity_burn_then_show_round_trips(self):
        burn = self.identity(hw_gen=11, hw_rev=0, mfr_id=0x1, batch=0x1, serial=0x1092)
        self.assertEqual(burn.returncode, 0, burn.stderr)

        result = self.show()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hw_gen:    11", result.stdout)
        self.assertIn("hw_rev:    0", result.stdout)
        self.assertIn("mfr_id:    0x01", result.stdout)
        self.assertIn("batch:     0x00000001", result.stdout)
        self.assertIn("serial:    4242", result.stdout)

    def test_identity_burn_round_trips_on_esp32s3_too(self):
        burn = self.identity(chip="esp32s3", hw_gen=9, hw_rev=2, mfr_id=0, serial=7)
        self.assertEqual(burn.returncode, 0, burn.stderr)

        result = self.show(chip="esp32s3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hw_gen:    9", result.stdout)
        self.assertIn("hw_rev:    2", result.stdout)
        self.assertIn("mfr_id:    0x00", result.stdout)
        self.assertIn("batch:     0 (not recorded)", result.stdout)
        self.assertIn("serial:    7", result.stdout)

    def test_0z_base36_prefix_works_for_mfr_id_and_serial_too(self):
        # "0z" is handled by parse_int() itself, not something specific to
        # --batch, so it should work anywhere parse_int() is used.
        burn = run(
            "identity", "--chip", "esp32c6", "--virt", "--path-efuse-file", self.efuse_file,
            "--hw-gen", "11", "--hw-rev", "0", "--mfr-id", "0z1", "--serial", "0z70kbl", "--yes",
        )
        self.assertEqual(burn.returncode, 0, burn.stderr)

        result = self.show()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("mfr_id:    0x01", result.stdout)
        self.assertIn(f"serial:    {int('70kbl', 36)}", result.stdout)

    def test_batch_id_accepts_0z_base36_prefix(self):
        # JLCPCB prints batch/lot codes in base-36, e.g. "70kbl" -- there's
        # no standard prefix for base-36 the way "0x" means hex, so this
        # project uses "0z" by analogy.
        burn = run(
            "identity", "--chip", "esp32c6", "--virt", "--path-efuse-file", self.efuse_file,
            "--hw-gen", "11", "--hw-rev", "0", "--mfr-id", "0x1", "--batch", "0z70kbl",
            "--serial", "0x1092", "--yes",
        )
        self.assertEqual(burn.returncode, 0, burn.stderr)

        result = self.show()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"batch:     0x{int('70kbl', 36):08x}", result.stdout)

    def test_bare_base36_batch_without_0z_prefix_is_rejected(self):
        # A bare JLCPCB code has no recognized prefix and isn't valid
        # decimal, so it must be rejected rather than silently guessed.
        result = run(
            "identity", "--chip", "esp32c6", "--virt", "--path-efuse-file", self.efuse_file,
            "--hw-gen", "11", "--hw-rev", "0", "--mfr-id", "0x1", "--batch", "70kbl",
            "--serial", "0x1092", "--yes",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid parse_int value", result.stderr)

    def test_batch_defaults_to_not_recorded_when_omitted(self):
        burn = run(
            "identity", "--chip", "esp32c6", "--virt", "--path-efuse-file", self.efuse_file,
            "--hw-gen", "11", "--hw-rev", "0", "--mfr-id", "0x1", "--serial", "0x1092", "--yes",
        )
        self.assertEqual(burn.returncode, 0, burn.stderr)

        result = self.show()
        self.assertIn("batch:     0 (not recorded)", result.stdout)

    def test_second_differing_burn_is_rejected_and_original_survives(self):
        first = self.identity(hw_gen=11, hw_rev=0, serial=1)
        self.assertEqual(first.returncode, 0, first.stderr)

        second = self.identity(hw_gen=11, hw_rev=1, serial=2)
        self.assertNotEqual(second.returncode, 0)

        # RS coding only allows a block to be burned once; espefuse must
        # refuse the second, differing burn without corrupting the first.
        result = self.show()
        self.assertIn("hw_rev:    0", result.stdout)
        self.assertIn("serial:    1", result.stdout)

    def test_repeating_identical_burn_is_a_harmless_no_op(self):
        first = self.identity(hw_gen=11, hw_rev=0, serial=1)
        self.assertEqual(first.returncode, 0, first.stderr)

        second = self.identity(hw_gen=11, hw_rev=0, serial=1)
        self.assertEqual(second.returncode, 0, second.stderr)

    def test_virt_requires_explicit_chip(self):
        result = run("show", "--virt", "--path-efuse-file", self.efuse_file)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--chip is required with --virt", result.stderr)

    def test_port_required_without_virt(self):
        # espefuse has no port auto-detection (unlike --chip) — verify we
        # fail fast with a clear error instead of letting espefuse guess.
        result = run("show", "--chip", "esp32c6")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--port is required unless --virt is set", result.stderr)

    def test_out_of_range_field_is_rejected(self):
        result = run(
            "identity", "--chip", "esp32c6", "--virt", "--path-efuse-file", self.efuse_file,
            "--hw-gen", "256", "--hw-rev", "0", "--mfr-id", "0x0", "--serial", "0x0",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("out of range", result.stderr)


if __name__ == "__main__":
    unittest.main()
