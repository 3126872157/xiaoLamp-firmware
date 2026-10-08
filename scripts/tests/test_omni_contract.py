"""Exercise production cloud schema and credential/tool trust boundaries without a key."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]

class OmniContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('Host C++ compiler required')
        cls.binary = ROOT/'build'/('omni_contract_cli.exe' if os.name == 'nt' else 'omni_contract_cli')
        cjson = ROOT/'managed_components/espressif__cjson/cJSON'
        subprocess.run([compiler, '-std=c++23', '-static', '-fno-rtti', '-finput-charset=UTF-8',
                        '-I', str(ROOT/'main/protocols'), '-I', str(cjson),
                        str(ROOT/'scripts/tests/omni_contract_cli.cc'), str(cjson/'cJSON.c'),
                        '-o', str(cls.binary)], check=True, capture_output=True)

    def check(self, op, value):
        r = subprocess.run([str(self.binary), op], input=json.dumps(value),
                           capture_output=True, text=True, encoding='utf-8', check=True)
        return json.loads(r.stdout)

    def test_session_uses_current_pcm_format_and_readonly_function(self):
        j = self.check('session', {})
        s = j['session']
        self.assertIsNone(s['turn_detection'])
        self.assertIsNone(s['input_audio_transcription'])
        self.assertEqual(s['audio']['input']['format'], dict(type='pcm',sample_rate=16000,
            sample_format='s16le',channels=1,packing='interleaved',channel_layout='mono'))
        self.assertEqual(s['audio']['output']['format'], dict(type='pcm',sample_rate=24000))
        self.assertEqual([t['function']['name'] for t in s['tools']],
                         ['lamp_get_state', 'lamp_end_conversation'])

    def test_rest_accepts_only_empty_object(self):
        self.assertTrue(self.check('tool',dict(name='lamp_end_conversation',arguments='{}')))
        for args in ('null','[]','{"torque":false}','{"in_use":false}','{} trailing'):
            self.assertFalse(self.check('tool',dict(name='lamp_end_conversation',arguments=args)))

    def test_allowed_regions(self):
        for region in ('cn-beijing', 'ap-southeast-1'):
            self.assertTrue(self.check('credentials',dict(workspace='test-123',key='dummy-key',region=region)))

    def test_header_and_host_injection_are_rejected(self):
        for w,k,r in [('evil.example/path','dummy-key','cn-beijing'),
                      ('test','dummy\r\nInjected: yes','cn-beijing'),
                      ('test','dummy-key','evil.example'),
                      ('x'*129,'dummy-key','cn-beijing'),
                      ('test','x'*513,'cn-beijing')]:
            self.assertFalse(self.check('credentials',dict(workspace=w,key=k,region=r)))

    def test_state_accepts_only_empty_json_object(self):
        self.assertTrue(self.check('tool',dict(name='lamp_get_state',arguments=' {} ')))
        for args in ('null','[]','{"enable":true}','{} trailing','{','"{}"',' '*257):
            self.assertFalse(self.check('tool',dict(name='lamp_get_state',arguments=args)))

    def test_motion_and_unknown_tools_are_rejected(self):
        for name in ('self.lamp.run_sequence','enable_torque','lamp_get_state\n',''):
            self.assertFalse(self.check('tool',dict(name=name,arguments='{}')))

    def test_archive_cannot_mislabel_omni_as_stable_twin(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('roles', ROOT/'scripts/build_lamp_variant.py')
        m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        sdk='\n'.join('CONFIG_'+s+'=y' for s in ('APPLE_LAMP_LOCAL_EXECUTOR','APPLE_LAMP_UART_PROBE',
            'COMPILER_CXX_EXCEPTIONS','APPLE_LAMP_HARDWARE_MOTION','APPLE_LAMP_TWIN_WEB','APPLE_LAMP_OMNI_REALTIME'))
        image=b'apple-lamp-ov3660-omni-poc Firmware role: OMNI-POC'
        m.validate_role(image,'omni-poc',sdk)
        with self.assertRaises(ValueError): m.validate_role(image,'local-twin',sdk)

    def test_edge_role_cannot_be_mislabeled_as_poc(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('roles_edge', ROOT/'scripts/build_lamp_variant.py')
        m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        sdk = '\n'.join('CONFIG_'+s+'=y' for s in ('APPLE_LAMP_LOCAL_EXECUTOR', 'APPLE_LAMP_UART_PROBE',
            'COMPILER_CXX_EXCEPTIONS', 'APPLE_LAMP_HARDWARE_MOTION', 'APPLE_LAMP_TWIN_WEB',
            'APPLE_LAMP_OMNI_REALTIME', 'APPLE_LAMP_EDGE_RUNTIME'))
        image = b'apple-lamp-ov3660-edge-runtime Firmware role: EDGE-RUNTIME'
        m.validate_role(image, 'edge-runtime', sdk)
        with self.assertRaises(ValueError): m.validate_role(image, 'omni-poc', sdk)

    def test_production_event_replay_and_cancellation(self):
        cjson = ROOT/'managed_components/espressif__cjson/cJSON'
        fixture = ROOT/'scripts/tests/fixtures/omni'
        binary = ROOT/'build'/('omni_events_cli.exe' if os.name == 'nt' else 'omni_events_cli')
        subprocess.run([shutil.which('g++'), '-std=c++23', '-static', '-fno-rtti', '-finput-charset=UTF-8',
                        '-I', str(fixture), '-I', str(ROOT/'main/protocols'), '-I', str(cjson),
                        str(ROOT/'scripts/tests/omni_events_cli.cc'),str(cjson/'cJSON.c'),
                        '-o', str(binary)], check=True, capture_output=True)
        result = subprocess.run([str(binary)],capture_output=True,text=True,check=True)
        self.assertIn('PASS handshake',result.stdout)

if __name__ == '__main__': unittest.main()
