"""Build and exercise portable production planning; never opens a serial port."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/apple-lamp-ov3660'


def compile_cli():
    compiler = shutil.which('g++') or shutil.which('c++')
    if not compiler:
        raise unittest.SkipTest('C++23 compiler unavailable')
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    binary = ROOT / ('build/lamp_pose_cli.exe' if os.name == 'nt' else 'build/lamp_pose_cli')
    binary.parent.mkdir(exist_ok=True)
    subprocess.run([compiler, '-std=c++23', '-static', '-finput-charset=UTF-8',
                    '-I', str(ROOT / 'main'), '-I', str(BOARD), '-I', str(cjson),
                    str(ROOT / 'scripts/tests/lamp_pose_cli.cc'), str(BOARD / 'lamp_pose_plan.cc'),
                    str(BOARD / 'lamp_protocol.cc'), str(cjson / 'cJSON.c'), '-o', str(binary)],
                   check=True, capture_output=True)
    return binary


class LampPoseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = compile_cli()
        cls.config = json.loads(re.search(r'R"lampjson\((.*?)\)lampjson"',
                                         (BOARD / 'lamp_pose_model.h').read_text(encoding='utf-8'), re.S)[1])

    def run_plan(self, **args):
        result = subprocess.run([str(self.binary)], input=json.dumps(args), text=True,
                                encoding='utf-8', capture_output=True, check=True)
        return json.loads(result.stdout)

    def test_model_and_calibration_trust_boundaries(self):
        q = self.config['homes']['homes']['sit']['pose']
        self.assertTrue(self.run_plan(pose=q)['ok'])
        for mutate in [lambda c: c.update(angle_unit='deg'),
                       lambda c: c['joints'][0].update(upper=100),
                       lambda c: c['tool'].update(xyz=[0, 0, 0]),
                       lambda c: c['action_presets']['A01'].update(amplitudeDeg=180),
                       lambda c: c['profiles']['modes']['sit']['targets']['user'].update(pitch=1),
                       lambda c: c['hardware']['uart'].update(tx_gpio=1),
                       lambda c: c['hardware']['servos'][0].update(direction=0)]:
            c = json.loads(json.dumps(self.config)); mutate(c)
            with self.subTest(config=c): self.assertFalse(self.run_plan(pose=q, config=c)['ok'])

    def test_heading_home_semantics_and_quantized_limits(self):
        q = dict(self.config['homes']['homes']['stand']['pose'], baseYaw=.7)
        for action in ['set_mode', 'restore_home', 'restore_initial']:
            step = {'action_id': action}
            if action == 'set_mode': step['mode'] = 'sit'
            plan = self.run_plan(operation='plan', mode='stand', pose=q, step=step, times=[1.8])
            self.assertTrue(plan['ok'], plan)
            self.assertAlmostEqual(plan['samples'][-1]['pose']['baseYaw'], 0 if action == 'restore_initial' else .7)
        boundary = dict(baseYaw=1.57, shoulder=-1.57, elbow=1.57, wrist=-2.09, headRoll=2.09)
        ticks = self.run_plan(pose=boundary)['ticks']
        import math
        for i, joint in enumerate(self.config['joints']):
            sign = self.config['hardware']['servos'][i]['direction']
            decoded = (ticks[i]-500)*.24*math.pi/180*sign
            self.assertGreaterEqual(decoded, joint['lower']); self.assertLessEqual(decoded, joint['upper'])

    def test_stale_target_and_raw_angles_are_not_silently_accepted(self):
        q = self.config['homes']['homes']['stand']['pose']
        plan = self.run_plan(operation='plan', mode='stand', pose=q,
                             step={'action_id': 'gaze_calibrated', 'target_id': 'user'})
        self.assertFalse(plan['ok']); self.assertIn('scene changed', plan['error'])
        plan = self.run_plan(operation='plan', mode='stand', pose=q,
                             step={'action_id': 'adjust_pose', 'axis': 'wrist', 'delta_rad': .1})
        self.assertFalse(plan['ok'])


if __name__ == '__main__':
    unittest.main()
