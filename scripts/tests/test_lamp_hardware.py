"""Production hardware executor with emulated UART/RTOS/NVS; no real serial access."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/apple-lamp-ov3660'


class HardwareExecutorTests(unittest.TestCase):
    def test_production_hardware_execution_and_failures(self):
        for board in [BOARD, ROOT / "main/boards/waveshare/apple-lamp-ov2640"]:
            compiler = shutil.which('g++') or shutil.which('c++')
            if not compiler: self.skipTest('C++23 compiler unavailable')
            stubs = ROOT / 'build/lamp_hw_stubs'; stubs.mkdir(exist_ok=True)
            for name in ['sdkconfig.h','driver/uart.h','driver/ledc.h','driver/gpio.h','esp_log.h','esp_timer.h','esp_random.h',
                         'nvs.h','psa/crypto.h','application.h','freertos/FreeRTOS.h','freertos/task.h']:
                p=stubs/name;p.parent.mkdir(parents=True,exist_ok=True)
                p.write_text('#include "lamp_hardware_platform.h"\n',encoding='utf-8')
            cjson=ROOT/'managed_components/espressif__cjson/cJSON'
            binary=ROOT/'build/lamp_hardware_cli.exe'
            sources=[str(board/name) for name in ['lamp_hardware_executor.cc','lamp_local_executor.cc',
                     'lamp_bus_protocol.cc','lamp_pose_plan.cc','lamp_protocol.cc','lamp_delivery_model.cc']]
            # Only the OV2640 board carries the PWM lamp head dimmer.
            dimmer = board/'lamp_light.cc'
            defines = ['-DLAMP_TEST_PWM_LIGHT=1'] if dimmer.is_file() else []
            if dimmer.is_file(): sources.append(str(dimmer))
            result=subprocess.run([compiler,'-std=c++23','-static','-pthread','-finput-charset=UTF-8',
                                   '-DCONFIG_APPLE_LAMP_DELIVERY=1','-DCONFIG_SPIRAM_USE_MALLOC=1',
                                   '-DCONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024',*defines,
                                   '-I',str(stubs),'-I',str(ROOT/'scripts/tests/fixtures'),'-I',str(ROOT/'main'),
                                   '-I',str(board),'-I',str(cjson),
                                   str(ROOT/'scripts/tests/lamp_hardware_cli.cc'),
                                   *sources,
                                   str(cjson/'cJSON.c'),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout.strip())
