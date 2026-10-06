import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import unittest
from unittest.mock import patch
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare/apple-lamp-ov3660"


class LocalBusTests(unittest.TestCase):
    def test_archive_conflict_never_overwrites_existing_images(self):
        spec = importlib.util.spec_from_file_location("lamp_archives", ROOT / "scripts/build_lamp_variant.py")
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='lamp-archive-test-') as directory:
            local = Path(directory).resolve()
            self.assertTrue(local.is_relative_to((ROOT / 'build').resolve()))
            build = local / 'build'; build.mkdir()
            name, marker = module.ROLES['simulator']
            (build / 'xiaozhi.bin').write_bytes(name.encode() + b'\0' + marker)
            (build / 'assets.bin').write_bytes(b'original-assets')
            (local / 'sdkconfig').write_text('', encoding='utf-8')
            (build / 'flasher_args.json').write_text(json.dumps({
                'flash_files': {'0x20000': 'xiaozhi.bin', '0x800000': 'assets.bin'},
                'write_flash_args': []}), encoding='utf-8')
            def git_output(command, **kwargs):
                return '12345678' if command[1] == 'rev-parse' else b''
            with patch.object(module, 'ROOT', local), patch.object(module.subprocess, 'check_output', git_output):
                manifest = module.archive('simulator')
                self.assertEqual(module.archive('simulator'), manifest)
                archived = manifest.parent / 'assets.bin'
                (build / 'assets.bin').write_bytes(b'different-assets')
                with self.assertRaises(ValueError): module.archive('simulator')
                self.assertEqual(archived.read_bytes(), b'original-assets')
                (build / 'assets.bin').write_bytes(b'original-assets')
                archived.write_bytes(b'tampered')
                with self.assertRaises(ValueError): module.archive('simulator')
                self.assertEqual(archived.read_bytes(), b'tampered')

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
            sdk = "CONFIG_APPLE_LAMP_LOCAL_EXECUTOR=y\nCONFIG_APPLE_LAMP_UART_PROBE=y\nCONFIG_COMPILER_CXX_EXCEPTIONS=y" if role != "simulator" else ""
            if role in ("local-hardware", "local-twin"): sdk += "\nCONFIG_APPLE_LAMP_HARDWARE_MOTION=y"
            if role == "local-twin": sdk += "\nCONFIG_APPLE_LAMP_TWIN_WEB=y"
            image = name.encode() + b"\0" + marker
            module.validate_role(image, role, sdk)
            for other in module.ROLES:
                if other == role: continue
                with self.assertRaises(ValueError): module.validate_role(image, other, sdk)
                with self.assertRaises(ValueError): module.validate_role(image + module.ROLES[other][1], role, sdk)
