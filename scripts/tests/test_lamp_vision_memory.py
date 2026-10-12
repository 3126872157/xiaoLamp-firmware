"""Exercise camera resource admission without opening a camera or network."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VisionMemoryTests(unittest.TestCase):
    def test_conversation_heap_and_retry_classification(self):
        compiler = shutil.which("g++") or shutil.which("c++")
        if not compiler:
            self.skipTest("C++23 compiler unavailable")
        binary = ROOT / "build/lamp_vision_memory_cli.exe"
        subprocess.run([compiler, "-std=c++23", "-static", "-I", str(ROOT / "main"),
                        str(ROOT / "scripts/tests/lamp_vision_memory_cli.cc"), "-o", str(binary)],
                       check=True, capture_output=True)
        subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)
