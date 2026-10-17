"""Regression tests for private configuration loading (synthetic values only)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from run_local import load_settings


class PrivateSettingsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "private.json"

    def write(self, value, mode=0o600):
        self.path.write_text(json.dumps(value))
        self.path.chmod(mode)

    def test_private_configuration(self):
        self.write({"BOX3_TEST": "synthetic"})
        self.assertEqual(load_settings(self.path), {"BOX3_TEST": "synthetic"})

    def test_rejects_group_readable_settings(self):
        self.write({"BOX3_TEST": "synthetic"}, 0o640)
        with self.assertRaises(ValueError):
            load_settings(self.path)

    def test_rejects_arbitrary_environment_and_nonstring_values(self):
        for value in [{"PATH": "synthetic"}, {"BOX3_TEST": 1}, [], {"BOX3_TEST": "a\0b"}]:
            self.write(value)
            with self.assertRaises(ValueError):
                load_settings(self.path)

    def test_environment_override_wins_without_dumping_configuration(self):
        self.write({"BOX3_TEST": "synthetic-file", "BOX3_UNUSED": "synthetic-not-for-output"})
        result = subprocess.run(
            [sys.executable, str(Path(__file__).with_name("run_local.py")), sys.executable,
             "-c", "import os; print(os.environ['BOX3_TEST'])"],
            env={**os.environ, "BOX3_LOCAL_CONFIG": str(self.path), "BOX3_TEST": "synthetic-env"},
            capture_output=True, text=True, check=True,
        )
        self.assertEqual(result.stdout.strip(), "synthetic-env")
        self.assertEqual(result.stderr, "")


class PublicAuditTest(unittest.TestCase):
    def test_rejects_literal_credentials(self):
        from audit_public import findings
        value = b"pass" + b"word = " + bytes([34]) + b"a" * 16 + bytes([34])
        self.assertIn("literal-credential", findings(value))

    def test_network_examples_are_not_deployment_addresses(self):
        from audit_public import findings
        self.assertNotIn("private-network-address", findings(b"192.0.2.10"))
        self.assertIn("private-network-address", findings(b"10" + b".1.2.3"))

    def test_private_values_are_detected_without_returning_them(self):
        from audit_public import findings
        self.assertEqual(findings(b"prefix synthetic-value", [b"synthetic-value"]),
                         ["configured-private-value"])


if __name__ == "__main__":
    unittest.main()
