"""Temporary PR28341 diagnostic: retain the normal Windows batch and capture its stack overflow."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile


FIXTURE = Path("test/sql/aggregate/aggregates/test_entropy.test")
OUTPUT = Path("windows-entropy-diagnostic").resolve()
ORIGINAL = OUTPUT / "test_entropy.original"


def prepare():
    OUTPUT.mkdir(exist_ok=True)
    if ORIGINAL.exists():
        raise RuntimeError("An entropy diagnostic is already prepared")
    archive = OUTPUT / "Procdump.zip"
    urllib.request.urlretrieve("https://download.sysinternals.com/files/Procdump.zip", archive)
    with zipfile.ZipFile(archive) as files:
        # Extract only the executable used below.
        with files.open("procdump64.exe") as source, (OUTPUT / "procdump64.exe").open("wb") as destination:
            shutil.copyfileobj(source, destination)
    provenance = {
        "url": "https://download.sysinternals.com/files/Procdump.zip",
        "sha256": {
            name: hashlib.sha256((OUTPUT / name).read_bytes()).hexdigest()
            for name in ("Procdump.zip", "procdump64.exe")
        },
    }
    # The per-attempt debugger banner records the executable's version.
    (OUTPUT / "procdump-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    shutil.copyfile(FIXTURE, ORIGINAL)
    FIXTURE.write_text("mode debug\n\n" + ORIGINAL.read_text())


def restore():
    if ORIGINAL.exists():
        shutil.copyfile(ORIGINAL, FIXTURE)


def run_batch(binary, batch_file, flags):
    command = [binary, *flags, "--use-colour", "yes", "-f", batch_file]
    batch = Path(batch_file).read_text()
    if FIXTURE.as_posix() not in batch.replace("\\", "/"):
        return subprocess.run(command).returncode

    attempt = OUTPUT / str(os.getpid())
    attempt.mkdir()
    shutil.copyfile(batch_file, attempt / "batch.txt")
    (attempt / "command.txt").write_text(subprocess.list2cmdline(command) + "\n")
    # First-chance stack overflow, one mini-dump (stacks and referenced memory), not full process memory.
    dump_command = [
        str(OUTPUT / "procdump64.exe"),
        "-accepteula",
        "-mm",
        "-e",
        "1",
        "-f",
        "C00000FD",
        "-n",
        "1",
        "-x",
        str(attempt),
        *command,
    ]
    with (attempt / "output.log").open("w", encoding="utf-8") as log:
        with subprocess.Popen(
            dump_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace"
        ) as process:
            passed = False
            for line in process.stdout:
                log.write(line)
                log.flush()
                sys.stdout.write(line)
                sys.stdout.flush()
                passed |= "All tests passed" in line
            result = process.wait()
    (attempt / "debugger_exit.txt").write_text(str(result) + "\n")
    # ProcDump's exit status describes the debugger. Require the test's own successful verdict too.
    return 0 if result == 0 and passed and not list(attempt.glob("*.dmp")) else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--restore", action="store_true")
    parser.add_argument("--binary")
    parser.add_argument("--batch-file")
    args, flags = parser.parse_known_args()
    if args.prepare:
        prepare()
        return 0
    if args.restore:
        restore()
        return 0
    if not args.binary or not args.batch_file:
        parser.error("--binary and --batch-file are required")
    return run_batch(args.binary, args.batch_file, flags)


if __name__ == "__main__":
    sys.exit(main())
