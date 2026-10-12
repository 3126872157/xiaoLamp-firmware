"""Stored tool table validation and persistence; never touches a physical device."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/apple-lamp-ov3660'

class ToolTableTests(unittest.TestCase):
    def test_production_table(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        binary = ROOT / 'build/lamp_tool_table_cli.exe'
        result = subprocess.run([compiler, '-std=c++23', '-static', '-pthread', '-fno-rtti',
            '-finput-charset=UTF-8', '-I', str(ROOT/'scripts/tests/fixtures/omni'),
            '-I', str(ROOT/'main'), '-I', str(ROOT/'main/protocols'), '-I', str(BOARD),
            '-I', str(cjson),
            str(ROOT/'scripts/tests/lamp_tool_table_cli.cc'), str(BOARD/'lamp_tool_table.cc'),
            str(BOARD/'lamp_protocol.cc'), str(cjson/'cJSON.c'), '-o', str(binary)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS tool table', result.stdout)

if __name__ == '__main__':
    unittest.main()
