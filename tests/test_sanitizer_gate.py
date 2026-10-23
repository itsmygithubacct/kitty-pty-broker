"""Exercise the production report gate using inert report files."""
import pathlib
import subprocess
import tempfile
import unittest


class SanitizerGateTests(unittest.TestCase):
    def test_report_families(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        for families in ((), ("asan",), ("ubsan",), ("asan", "ubsan")):
            with self.subTest(families=families), tempfile.TemporaryDirectory(prefix="sanitizer gate ") as directory:
                for family in families:
                    pathlib.Path(directory, family + ".123").write_text("inert fixture report\n")
                result = subprocess.run(
                    ["make", "--no-print-directory", "check-sanitizer-reports", "SANITIZE_BUILD_DIR=" + directory],
                    cwd=root, capture_output=True, text=True, check=False,
                )
                self.assertEqual(result.returncode == 0, not families, result.stdout + result.stderr)
                for family in families:
                    self.assertIn(family + ".123", result.stderr)
                self.assertEqual("no sanitizer reports" in result.stdout, not families)


if __name__ == "__main__":
    unittest.main()
