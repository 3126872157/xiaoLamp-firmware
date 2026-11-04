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
        # Page 0 is the capability summary; pages 1..13 are one primitive each.
        self.assertEqual(len(pages), 14)
        summary, primitives = pages[0], pages[1:]
        self.assertEqual(summary["primitives"], [])
        self.assertEqual(summary["total"], 13)
        self.assertEqual(summary["next_cursor"], 1)
        # The summary page also carries postures/completion/enable/expression, so it
        # is allowed 1000 bytes; a primitive page must stay at 900 or below.
        self.assertLessEqual(len(json.dumps(summary, ensure_ascii=False, separators=(",", ":")).encode()), 1000)
        for page in pages:
            self.assertEqual(page["executor"], "web_sim")
        for page in primitives:
            self.assertLessEqual(len(json.dumps(page, ensure_ascii=False, separators=(",", ":")).encode()), 900)
        self.assertEqual(summary["postures"]["work"], "降低/俯视")
        self.assertIn("completed", summary["completion"])
        self.assertIn("使能", summary["enable"])
        self.assertIn("A01", summary["expression"])
        self.assertIn("executor", summary)
        self.assertNotIn("mode_targets", summary)
        actions = {a["action_id"]: a for a in summary["actions"]}
        self.assertEqual(actions["set_mode"]["mode"], ["sit", "stand", "work"])
        self.assertIn("站起", actions["set_mode"]["use"])
        self.assertEqual(actions["execute_primitive"]["skill_id"], "string")
        self.assertEqual(actions["adjust_pose"]["direction"],
                         ["up", "down", "yaw_positive", "yaw_negative"])
        self.assertEqual(actions["wait"]["duration_sec"], [0, 10])
        for action in ("restore_home", "restore_initial", "gaze_calibrated"):
            self.assertIn(action, actions)
        self.assertNotIn("controls", summary)
        self.assertEqual(len({p["primitives"][0]["id"] for p in primitives}), 13)
        names = {p["primitives"][0]["id"]: p["primitives"][0]["name"] for p in primitives}
        self.assertIn("非站起", names["A03"])
        self.assertIn("非坐下", names["A04"])
        self.assertIn("否定", names["B01"])
        for page in primitives:
            self.assertEqual(len(page["primitives"]), 1)
            self.assertNotIn("actions", page)
            self.assertNotIn("postures", page)
            for key in page["primitives"][0]["inputSchema"]["properties"]:
                self.assertIn(key, ("end_mode", "repetitions", "look_away", "hold_mode"))
        self.assertEqual(primitives[-1]["next_cursor"], -1)
        self.assertEqual([p["next_cursor"] for p in primitives[:-1]], list(range(2, 14)))

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

    def test_dialogue_context_allows_short_expression_sequences(self):
        context = {"source": "dialogue", "turn_id": "turn-42", "cue": "question"}
        # One expression, and up to three, are allowed; the cue must match any step.
        self.run_adapter(self.sequence([{"action_id": "execute_primitive", "skill_id": "B02"}], context=context))
        self.run_adapter(self.sequence([{"action_id": "execute_primitive", "skill_id": "A01"},
                                        {"action_id": "execute_primitive", "skill_id": "B02"},
                                        {"action_id": "execute_primitive", "skill_id": "B01"}], context=context))
        # `neutral` no longer forces A01: the reply may pick any expression that fits.
        neutral = {"source": "dialogue", "turn_id": "turn-43", "cue": "neutral"}
        self.run_adapter(self.sequence([{"action_id": "execute_primitive", "skill_id": "A01"}], context=neutral))
        self.run_adapter(self.sequence([{"action_id": "execute_primitive", "skill_id": "B02"}], context=neutral))
        for steps in [
            # A cue that matches no step in the turn.
            [{"action_id": "execute_primitive", "skill_id": "A01"}],
            # More than three expressions.
            [{"action_id": "execute_primitive", "skill_id": "B02"}] * 4,
            [{"action_id": "execute_primitive", "skill_id": "B02", "options": {"end_mode": "hold_at_end"}}],
            # A posture change is not an expression.
            [{"action_id": "set_mode", "mode": "stand"}],
            # A04 routes to the sit home pose, so it is a posture change too.
            [{"action_id": "execute_primitive", "skill_id": "A04"}],
            # Two nods are `repetitions`, not two A01 steps in one turn.
            [{"action_id": "execute_primitive", "skill_id": "B02"},
             {"action_id": "execute_primitive", "skill_id": "B02"}],
        ]: self.run_adapter(self.sequence(steps, context=context), valid=False)
        self.run_adapter(self.sequence([{"action_id": "execute_primitive", "skill_id": "A01"}],
                                       context={"source": "dialogue", "turn_id": "turn-44", "cue": "angry"}), valid=False)

    def test_dialogue_cue_families_allow_variety(self):
        # Each cue offers more than one expression, so a reply can vary instead of
        # nodding on every turn; target-required skills still need a target.
        for cue, skill, target in [("agreement", "A16", None), ("question", "A03", None),
                                   ("denial", "A14", "user"), ("excitement", "A01", None),
                                   ("regret", "B02", None), ("emphasis", "A01", None),
                                   ("neutral", "B02", None), ("attention", "A09", "user"),
                                   ("avoidance", "A11", "book"), ("surprise", "A02", None)]:
            step = {"action_id": "execute_primitive", "skill_id": skill}
            if target:
                step["target_id"] = target
            with self.subTest(cue=cue):
                self.run_adapter(self.sequence([step], context={"source": "dialogue", "turn_id": "v-" + cue, "cue": cue}))
        # A09/A11/A14 need a calibrated target even inside their cue family.
        self.run_adapter(self.sequence([{"action_id": "execute_primitive", "skill_id": "A09"}],
                                       context={"source": "dialogue", "turn_id": "turn-46", "cue": "attention"}),
                         valid=False)

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
