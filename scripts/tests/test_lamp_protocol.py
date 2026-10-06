"""Exercise the production C++ adapter, including model-input trust boundaries."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare/apple-lamp-ov3660"


class LampProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("c++")
        if not compiler:
            raise unittest.SkipTest("A host C++23 compiler is required")
        cls.binary = ROOT / "build" / ("lamp_protocol_cli.exe" if os.name == "nt" else "lamp_protocol_cli")
        cls.binary.parent.mkdir(exist_ok=True)
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        if not (cjson / "cJSON.c").is_file():
            raise unittest.SkipTest("Run ESP-IDF configure to download cJSON before native adapter tests")
        if not (cjson / "cJSON.c").is_file():
            raise unittest.SkipTest("Run ESP-IDF configure to download cJSON before native adapter tests")
        subprocess.run([compiler, "-std=c++23", "-static", "-finput-charset=UTF-8",
                        "-I", str(ROOT / "main"), "-I", str(BOARD), "-I", str(cjson),
                        str(ROOT / "scripts/tests/lamp_protocol_cli.cc"), str(BOARD / "lamp_protocol.cc"),
                        str(cjson / "cJSON.c"), "-o", str(cls.binary)], check=True, capture_output=True)

    def run_adapter(self, value, mode="sequence", valid=True):
        result = subprocess.run([str(self.binary), mode],
                                input=value if isinstance(value, str) else json.dumps(value),
                                text=True, encoding="utf-8", capture_output=True)
        self.assertEqual(result.returncode, 0 if valid else 1, result.stderr)
        return json.loads(result.stdout) if valid else result.stderr

    @staticmethod
    def sequence(steps, **extra):
        return {"protocol_version": "lamp-action/2", "request_id": "test-stable-id", "steps": steps, **extra}

    def test_semantic_group_preserves_ids_options_and_wait(self):
        steps = [{"action_id": "set_mode", "mode": "stand"},
                 {"action_id": "execute_primitive", "skill_id": "A01", "options": {"repetitions": 5}},
                 {"action_id": "wait", "duration_sec": .3},
                 {"action_id": "execute_primitive", "skill_id": "B01"}]
        command = self.run_adapter(self.sequence(steps))
        self.assertEqual(command["operation"], "execute_group")
        self.assertEqual(command["args"]["steps"], steps)

    def test_catalog_pages_expose_all_thirteen_schema_without_numeric_tuning(self):
        result = subprocess.run([str(self.binary), "catalog"], text=True, encoding="utf-8", capture_output=True, check=True)
        pages = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(pages), 13)
        self.assertEqual(len({p["primitives"][0]["id"] for p in pages}), 13)
        for page in pages:
            self.assertEqual(page["executor"], "web_sim")
            self.assertLessEqual(len(json.dumps(page, ensure_ascii=False, separators=(",", ":")).encode()), 900)
            for key in page["primitives"][0]["inputSchema"]["properties"]:
                self.assertIn(key, ("end_mode", "repetitions", "look_away", "hold_mode"))
        self.assertEqual(pages[-1]["next_cursor"], -1)

    def test_raw_joint_fields_and_unexposed_options_are_rejected(self):
        for step in [
            {"action_id": "adjust_pose", "axis": "wrist", "delta_rad": .1},
            {"action_id": "execute_primitive", "skill_id": "A01", "params": {"amplitudeDeg": 20}},
            {"action_id": "execute_primitive", "skill_id": "B02", "options": {"angle": 20}},
            {"action_id": "set_mode", "mode": "stand", "duration_sec": 2},
        ]:
            with self.subTest(step=step): self.run_adapter(self.sequence([step]), valid=False)

    def test_disabled_skills_bad_targets_and_bad_options_are_rejected(self):
        for step in [
            {"action_id": "execute_primitive", "skill_id": "A05"},
            {"action_id": "gaze_calibrated", "target_id": "cup"},
            {"action_id": "execute_primitive", "skill_id": "A13"},
            {"action_id": "execute_primitive", "skill_id": "A01", "options": {"repetitions": 1.5}},
            {"action_id": "execute_primitive", "skill_id": "A01", "options": {"repetitions": 6}},
            {"action_id": "adjust_pose", "direction": "up", "size": "huge"},
            {"action_id": "wait", "duration_sec": -1},
        ]:
            with self.subTest(step=step): self.run_adapter(self.sequence([step]), valid=False)

    def test_group_limits_and_json_trailing_data(self):
        self.run_adapter(self.sequence([]), valid=False)
        self.run_adapter(self.sequence([{"action_id": "wait", "duration_sec": 0}] * 17), valid=False)
        self.run_adapter(self.sequence([{"action_id": "wait", "duration_sec": 10}] * 7), valid=False)
        self.run_adapter(json.dumps(self.sequence([{"action_id": "restore_home"}])) + "{}", valid=False)

    def test_old_version_and_reserved_request_ids(self):
        for request in [
            self.sequence([{"action_id": "restore_home"}], protocol_version="lamp-action/1"),
            self.sequence([{"action_id": "restore_home"}], request_id="__lamp:internal"),
            self.sequence([{"action_id": "restore_home"}], request_id=" \t"),
        ]: self.run_adapter(request, valid=False)

    def test_dialogue_context_matches_one_expression_and_returns_reference(self):
        context = {"source": "dialogue", "turn_id": "turn-42", "cue": "question"}
        self.run_adapter(self.sequence([{"action_id": "execute_primitive", "skill_id": "B02"}], context=context))
        for steps in [
            [{"action_id": "execute_primitive", "skill_id": "A01"}],
            [{"action_id": "execute_primitive", "skill_id": "B02"}] * 2,
            [{"action_id": "execute_primitive", "skill_id": "B02", "options": {"end_mode": "hold_at_end"}}],
        ]: self.run_adapter(self.sequence(steps, context=context), valid=False)

    def test_persistent_lighting_requires_single_execute(self):
        step = {"action_id": "execute_primitive", "skill_id": "A10", "target_id": "book", "options": {"hold_mode": "holding"}}
        self.run_adapter(self.sequence([step]), valid=False)
        self.run_adapter({"protocol_version": "lamp-action/2", "request_id": "lighting", "operation": "execute", "args": step}, mode="command")

    def test_query_and_task_scoped_cancel(self):
        for operation, args in [("get_task", {"task_id": "active-task"}), ("cancel", {"task_id": "old-task"}), ("end_lighting", {})]:
            result = self.run_adapter({"protocol_version": "lamp-action/2", "request_id": "query", "operation": operation, "args": args}, mode="command")
            self.assertEqual(result["args"], args)
        summary = self.run_adapter({"protocol_version": "lamp-action/2", "request_id": "state", "ok": True,
                                    "result": {"mode": "stand", "busy": True, "completion_basis": "simulation",
                                               "active_task": {"task_id": "group-1", "status": "paused", "results": [{"large": "x" * 2000}]}}}, mode="summary")
        self.assertEqual(summary["result"]["active_task"]["status"], "paused")
        self.assertEqual(summary["result"]["completion_basis"], "simulation")
        self.assertNotIn("results", summary["result"]["active_task"])

    def test_duplicate_fields_and_command_size_rejected(self):
        self.run_adapter('{"protocol_version":"lamp-action/2","request_id":"first","request_id":"second","steps":[]}', valid=False)
        self.run_adapter(" " * 24577, valid=False)

    def test_rest_torque_and_measured_calibration_contract(self):
        for operation, args in [("rest", {}), ("set_torque", {"enabled": False}),
                                ("set_torque", {"enabled": True}),
                                ("record_calibration", {"target_id": "user"})]:
            value = {"protocol_version": "lamp-action/2", "request_id": "control",
                     "operation": operation, "args": args}
            self.assertEqual(self.run_adapter(value, mode="command")["args"], args)
        for operation, args in [("rest", {"mode": "stand"}), ("set_torque", {"enabled": "false"}),
                                ("record_calibration", {"target_id": "cup"}),
                                ("record_calibration", {"target_id": "user", "pose": {}})]:
            self.run_adapter({"protocol_version": "lamp-action/2", "request_id": "bad-control",
                              "operation": operation, "args": args}, mode="command", valid=False)
        summary = self.run_adapter({"ok": True, "result": {"torque_enabled": False, "teaching": True,
                                   "rest_task": {"request_id": "rest-1", "status": "completed"},
                                   "target_id": "user", "stored_at": "web_sim", "persistent": True}}, mode="summary")
        self.assertFalse(summary["result"]["torque_enabled"])
        self.assertEqual(summary["result"]["rest_task"]["status"], "completed")

    def test_explicit_initial_home_and_body_approach(self):
        for step in [{"action_id": "restore_initial"},
                     {"action_id": "execute_primitive", "skill_id": "A13", "target_id": "user"}]:
            value = {"protocol_version": "lamp-action/2", "request_id": "heading",
                     "operation": "execute", "args": step}
            self.assertEqual(self.run_adapter(value, mode="command")["args"], step)
        self.run_adapter(self.sequence([{"action_id": "restore_initial", "yaw": 0}]), valid=False)


if __name__ == "__main__":
    unittest.main()
