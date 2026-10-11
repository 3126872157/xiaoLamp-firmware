"""Portable production parser/planner tests, no serial or cloud requests."""
import copy
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/apple-lamp-ov3660'
KEYS = ['baseYaw', 'shoulder', 'elbow', 'wrist', 'headRoll']

class DeliveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++')
        if not compiler: raise unittest.SkipTest('C++23 host compiler required')
        cls.binary = ROOT / 'build/lamp_delivery_cli.exe'
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        cmd = [compiler, '-std=c++23', '-static', '-finput-charset=UTF-8', '-I', str(ROOT/'main'), '-I', str(BOARD), '-I', str(cjson),
               str(ROOT/'scripts/tests/lamp_delivery_cli.cc'), *[str(BOARD/x) for x in ['lamp_delivery_model.cc','lamp_pose_plan.cc','lamp_protocol.cc']],
               str(cjson/'cJSON.c'), '-o', str(cls.binary)]
        p = subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8')
        if p.returncode: raise AssertionError(p.stderr)
        text = (BOARD/'lamp_pose_model.h').read_text(encoding='utf-8')
        cls.config = json.loads(re.search(r'R"lampjson\((.*?)\)lampjson"',text,re.S)[1])
        cls.model = cls.config['model_version']
        cls.entry = cls.config['homes']['homes']['stand']['pose']

    def action(self, delta=.04):
        return dict(format='lamp-relative-action/1', id='my-nod', name='Nod', description='Express agreement',
                    model_version=self.model, binding='a'*64, angle_unit='urdf_rad', joints=[False,False,False,True,False],
                    segments=[dict(delta_rad=dict(zip(KEYS,[0,0,0,delta,0])),duration_sec=1)], end_mode='return_to_reference')

    def run_model(self,**data):
        p = subprocess.run([str(self.binary)],input=json.dumps(data),capture_output=True,text=True,encoding='utf-8',check=True)
        return json.loads(p.stdout)

    def test_relative_playback_two_entry_poses(self):
        for yaw in [0,.2]:
            q = copy.deepcopy(self.entry);q['baseYaw']=yaw
            result=self.run_model(op='play',action=self.action(),entry=q,end_mode='hold_at_end')
            self.assertTrue(result['ok'],result)
            for i,key in enumerate(KEYS): self.assertAlmostEqual(result['final'][i],q[key]+(.04 if key=='wrist' else 0))

    def test_return_uses_current_entry_not_recorded_pose(self):
        result=self.run_model(op='play',action=self.action(),entry=self.entry,end_mode='return_to_reference')
        self.assertTrue(result['ok'],result)
        self.assertEqual(result['duration'],3)
        for i,key in enumerate(KEYS):self.assertAlmostEqual(result['final'][i],self.entry[key])

    def test_increments_not_absolute_pose_limits(self):
        self.assertTrue(self.run_model(op='parse',action=self.action(2.5))['ok'])
        self.assertFalse(self.run_model(op='play',action=self.action(2.5),entry=self.entry,end_mode='hold_at_end')['ok'])

    def test_short_unsafe_middle_segment_rejects_entire_action(self):
        a=self.action(4);a['segments'][0]['duration_sec']=.2
        a['segments'].append(dict(delta_rad=dict(zip(KEYS,[0,0,0,-4,0])),duration_sec=10))
        self.assertFalse(self.run_model(op='play',action=a,entry=self.entry,end_mode='return_to_reference')['ok'])

    def test_file_paths_metadata_and_selection_validation(self):
        for field,value in [('id','../escape'),('id',''),('model_version','old-model'),('binding','x'),('angle_unit','ticks')]:
            a=self.action();a[field]=value;self.assertFalse(self.run_model(op='parse',action=a)['ok'])
        a=self.action();a['joints'][3]=False;self.assertFalse(self.run_model(op='parse',action=a)['ok'])
        a=self.action();a['segments'][0]['delta_rad']['baseYaw']=.1;self.assertFalse(self.run_model(op='parse',action=a)['ok'])

    def test_times_frame_count_and_unknown_fields(self):
        for value in [0,11]:
            a=self.action();a['segments'][0]['duration_sec']=value;self.assertFalse(self.run_model(op='parse',action=a)['ok'])
        a=self.action();a['segments']*=32;self.assertFalse(self.run_model(op='parse',action=a)['ok'])
        a=self.action();a['extra']=1;self.assertFalse(self.run_model(op='parse',action=a)['ok'])

    def test_observation_wrappers_and_invalid_results(self):
        v=json.dumps(dict(found=True,cx=.7,cy=.3,confidence=.9))
        for response in [v,json.dumps({'text':v}),json.dumps({'content':[{'type':'text','text':v}]})]:
            self.assertTrue(self.run_model(op='vision',response=response)['ok'])
        for response in ['book on the left','{}',json.dumps(dict(found=True,cx=2,cy=.4,confidence=.9))]:
            self.assertFalse(self.run_model(op='vision',response=response)['ok'])

    def test_tracking_deadzone_signs_and_step_cap(self):
        options=dict(yaw_sign=1,pitch_sign=-1,gain_deg=20,max_step_deg=2)
        v=json.dumps(dict(found=True,cx=1,cy=1,confidence=.95))
        r=self.run_model(op='tracking',options=options,response=v,entry=self.entry)
        self.assertTrue(r['ok'],r)
        self.assertAlmostEqual(r['target'][0]-self.entry['baseYaw'],math.radians(2))
        self.assertAlmostEqual(r['target'][3]-self.entry['wrist'],-math.radians(2))
        for i in [1,2,4]:self.assertAlmostEqual(r['target'][i],self.entry[KEYS[i]])
        for cx,confidence,found in [(.54,.95,True),(.9,.4,True),(.9,.95,False)]:
            r=self.run_model(op='tracking',options=options,response=json.dumps(dict(found=found,cx=cx,cy=.5,confidence=confidence)),entry=self.entry)
            for i,key in enumerate(KEYS):self.assertAlmostEqual(r['target'][i],self.entry[key])

    def test_tracking_requires_operator_directions(self):
        r=self.run_model(op='tracking',options={},response='{}',entry=self.entry)
        self.assertFalse(r['ok'])

    def test_vision_trailing_data_and_duplicate_fields_rejected(self):
        for v in ['{"found":false,"confidence":0} garbage','{"found":true,"found":false,"cx":.5,"cy":.5,"confidence":.9}']:
            self.assertFalse(self.run_model(op='vision',response=v)['ok'])

    def test_fenced_model_result_and_absent_book(self):
        body=json.dumps(dict(found=True,cx=.6,cy=.4,confidence=.9))
        self.assertTrue(self.run_model(op='vision',response=json.dumps({'answer':'  ```json\n'+body+'\n```\n'}))['ok'])
        self.assertTrue(self.run_model(op='vision',response=json.dumps(dict(found=False,cx=None,cy=None,confidence=0)))['ok'])
