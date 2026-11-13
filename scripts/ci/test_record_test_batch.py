import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("record_test_batch.py")


class RecordTestBatchTests(unittest.TestCase):
    def run_batch(self, root, code):
        batch = root / "batch.txt"
        batch.write_text('"C++ test, with commas"\ntest/sql/example.test\n', encoding="utf-8")
        output = root / "failures"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--output",
                str(output),
                "--",
                sys.executable,
                "-c",
                code,
                "-f",
                str(batch),
            ],
            capture_output=True,
        )
        return result, batch, output

    def test_success_preserves_output_without_failure_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            result, batch, output = self.run_batch(
                Path(directory),
                "import sys; sys.stdout.buffer.write(b'passed\\n'); sys.stderr.buffer.write(b'notice\\n')",
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"passed\n")
            self.assertEqual(result.stderr, b"notice\n")
            self.assertFalse(output.exists())

    def test_failure_keeps_exit_code_and_replayable_batch_after_input_removal(self):
        with tempfile.TemporaryDirectory() as directory:
            result, batch, output = self.run_batch(
                Path(directory),
                "import sys; sys.stdout.buffer.write(b'failed\\n'); sys.stderr.buffer.write(b'raw\\xff\\n'); sys.exit(7)",
            )
            self.assertEqual(result.returncode, 7)
            self.assertEqual(result.stdout, b"failed\n")
            self.assertEqual(result.stderr, b"raw\xff\n")
            saved = output / batch.stem
            self.assertEqual((saved / "tests.txt").read_bytes(), batch.read_bytes())
            batch.unlink()
            self.assertEqual((saved / "stdout.log").read_bytes(), result.stdout)
            self.assertEqual((saved / "stderr.log").read_bytes(), result.stderr)
            manifest = json.loads((saved / "command.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["returncode"], 7)
            self.assertEqual(manifest["command"][-1], str(saved / "tests.txt"))
            replay = subprocess.run(manifest["command"], capture_output=True)
            self.assertEqual(replay.returncode, 7)
            self.assertEqual(replay.stdout, result.stdout)
            self.assertEqual(replay.stderr, result.stderr)


if __name__ == "__main__":
    unittest.main()
