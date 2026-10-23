#!/usr/bin/env python3
"""Check build concurrency without compiling or installing dependencies."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class BuildParallelismTest(unittest.TestCase):
    def test_defaults_overrides_and_invalid_values(self):
        with tempfile.TemporaryDirectory(prefix="apigate-build-jobs-") as directory:
            temporary = Path(directory)
            binary = temporary / "bin"
            binary.mkdir()
            calls = temporary / "calls.jsonl"
            cmake = binary / "cmake"
            cmake.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys\n"
                "with open(os.environ['APIGATE_TEST_BUILD_CALLS'], 'a') as output:\n"
                "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n",
                encoding="utf-8",
            )
            cmake.chmod(0o755)
            for name in ("ninja", "vcpkg"):
                stub = binary / name
                stub.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                stub.chmod(0o755)
            environment = os.environ.copy()
            environment.update(
                PATH=f"{binary}{os.pathsep}{environment['PATH']}",
                VCPKG_ROOT=str(binary),
                APIGATE_TEST_BUILD_CALLS=str(calls),
            )
            cases = [
                ("linux-analysis", None, "1"),
                ("linux-debug", None, "2"),
                ("linux-release", None, "2"),
                ("linux-analysis", "3", "3"),
                ("linux-debug", "1", "1"),
                ("linux-analysis", "", None),
                ("linux-analysis", "0", None),
                ("linux-analysis", "-1", None),
                ("linux-analysis", "1.5", None),
                ("linux-analysis", "auto", None),
            ]
            for preset, override, expected in cases:
                with self.subTest(preset=preset, override=override):
                    calls.unlink(missing_ok=True)
                    environment.pop("CMAKE_BUILD_PARALLEL_LEVEL", None)
                    if override is not None:
                        environment["CMAKE_BUILD_PARALLEL_LEVEL"] = override
                    result = subprocess.run(
                        ["bash", str(ROOT / "scripts/build.sh"), preset],
                        env=environment,
                        capture_output=True,
                        text=True,
                        timeout=5,
                        check=False,
                    )
                    if expected is None:
                        self.assertEqual(result.returncode, 2, result.stderr)
                        self.assertFalse(calls.exists())
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        commands = [json.loads(line) for line in calls.read_text().splitlines()]
                        self.assertEqual(len(commands), 2)
                        self.assertEqual(
                            commands[-1],
                            ["--build", "--preset", preset, "--parallel", expected],
                        )

    def test_direct_build_presets_are_bounded(self):
        presets = json.loads((ROOT / "CMakePresets.json").read_text())
        jobs = {preset["name"]: preset["jobs"] for preset in presets["buildPresets"]}
        self.assertEqual(jobs, {"linux-debug": 2, "linux-release": 2, "linux-analysis": 1})


if __name__ == "__main__":
    unittest.main()
