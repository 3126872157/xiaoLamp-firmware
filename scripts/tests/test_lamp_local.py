import importlib.util
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare/apple-lamp-ov3660"


class LocalBusTests(unittest.TestCase):
    def test_production_readonly_frames_fragments_corruption_noise_and_bounds(self):
        compiler = shutil.which("g++") or shutil.which("c++")
        if not compiler: self.skipTest("C++23 host compiler unavailable")
        output = ROOT / "build/lamp_bus_cli.exe"
        output.parent.mkdir(exist_ok=True)
        subprocess.run([compiler, "-std=c++23", "-static", "-I", str(BOARD),
                        str(ROOT / "scripts/tests/lamp_bus_cli.cc"), str(BOARD / "lamp_bus_protocol.cc"),
                        "-o", str(output)], check=True, capture_output=True)
        subprocess.run([str(output)], check=True)

    def test_artifact_roles_cannot_be_confused_or_reused(self):
        spec = importlib.util.spec_from_file_location("lamp_roles", ROOT / "scripts/build_lamp_variant.py")
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        for role, (name, marker) in module.ROLES.items():
            sdk = "CONFIG_APPLE_LAMP_LOCAL_EXECUTOR=y\nCONFIG_APPLE_LAMP_UART_PROBE=y" if role == "local-readonly" else ""
            image = name.encode() + b"\0" + marker
            module.validate_role(image, role, sdk)
            other = "simulator" if role == "local-readonly" else "local-readonly"
            with self.assertRaises(ValueError): module.validate_role(image, other, sdk)
            with self.assertRaises(ValueError): module.validate_role(image + module.ROLES[other][1], role, sdk)
