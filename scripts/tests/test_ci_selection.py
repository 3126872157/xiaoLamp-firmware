import copy
import importlib.util
import json
import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class CiSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.variants = build._collect_variants(idf_version=(6, 1, 0))
        cls.manifest = json.loads(build._CI_REPRESENTATIVE_VARIANTS.read_text())

    def load_manifest(self, manifest):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "representatives.json"
            path.write_text(json.dumps(manifest))
            return build._load_representative_variants(self.variants, path)

    def test_reviewed_list_has_2_builds_and_all_chip_targets(self):
        selected = self.load_manifest(self.manifest)
        self.assertEqual(len(selected), 2)
        self.assertEqual(
            {item["target"] for item in selected},
            {item["target"] for item in self.variants},
        )
        self.assertEqual({item["target"] for item in selected}, {"esp32s3"})
        self.assertEqual(
            len({item.get("ci_name", item["full_name"]) for item in selected}),
            len(selected),
        )

    def test_common_paths_select_only_the_reviewed_list(self):
        expected = self.load_manifest(self.manifest)
        for path in (
            "main/application.cc", "main/boards/common/board.cc",
            "main/Kconfig.projbuild", "main/CMakeLists.txt", "main/idf_component.yml",
            "CMakeLists.txt", "sdkconfig.defaults.esp32p4", "dependencies.lock",
            ".github/workflows/build.yml", "scripts/ci/representative-variants.json",
            "scripts/build.py", "partitions/v2/16m.csv",
        ):
            with self.subTest(path=path):
                self.assertEqual(
                    build._select_variants_for_changes(self.variants, [path]), expected
                )

    def test_board_only_includes_all_its_variants(self):
        board = "waveshare/apple-lamp-ov3660"
        selected = build._select_variants_for_changes(
            self.variants, [f"main/boards/{board}/config.h"]
        )
        self.assertEqual(selected, [v for v in self.variants if v["board"] == board])
        self.assertGreater(len(selected), 0)

    def test_mixed_changes_cover_both_boards_in_either_order(self):
        paths = ["main/application.cc", "main/boards/waveshare/apple-lamp-ov2640/config.h"]
        for changes in (paths, list(reversed(paths))):
            with self.subTest(changes=changes):
                selected = build._select_variants_for_changes(self.variants, changes)
                self.assertEqual(len(selected), 2)
                self.assertIn("waveshare/apple-lamp-ov2640", {v["board"] for v in selected})

    def test_representative_board_changes_do_not_duplicate_default_build(self):
        selected = build._select_variants_for_changes(
            self.variants,
            ["main/application.cc", "main/boards/waveshare/apple-lamp-ov3660/config.h"],
        )
        self.assertEqual(len(selected), 2)
        self.assertEqual(
            sorted(v["name"] for v in selected if v["board"] == "waveshare/apple-lamp-ov3660"),
            ["apple-lamp-ov3660-edge-runtime"],
        )

    def test_no_network_type_option_remains(self):
        supported = [
            v for v in self.variants
            if any(d["key"] == "network_type" for d in v["build_options"])
        ]
        self.assertEqual(supported, [])

    def test_full_selection_includes_every_default(self):
        paths = ["main/application.cc"] + [
            f"main/boards/{board}/config.json"
            for board in {v["board"] for v in self.variants}
        ]
        selected = build._select_variants_for_changes(self.variants, paths)
        self.assertEqual(len(selected), len(self.variants))
        self.assertEqual(
            {(v["board"], v["name"]) for v in selected},
            {(v["board"], v["name"]) for v in self.variants},
        )

    def test_docs_only_and_empty_changes_skip_firmware(self):
        for paths in ([], ["README.md", "docs/custom-board.md"]):
            self.assertEqual(build._select_variants_for_changes(self.variants, paths), [])

    def test_missing_duplicate_and_invalid_option_entries_fail(self):
        invalid = copy.deepcopy(self.manifest)
        invalid["variants"][0]["name"] = "removed-variant"
        with self.assertRaisesRegex(ValueError, "unavailable"):
            self.load_manifest(invalid)
        invalid = copy.deepcopy(self.manifest)
        invalid["variants"].append(invalid["variants"][0])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.load_manifest(invalid)
        invalid = copy.deepcopy(self.manifest)
        invalid["variants"][0]["build_options"] = {"network_type": "ethernet"}
        with self.assertRaisesRegex(ValueError, "Unsupported build option"):
            self.load_manifest(invalid)
        invalid = copy.deepcopy(self.manifest)
        invalid["variants"][0]["build_options"] = {"camera_hmirror": "not-a-bool"}
        with self.assertRaisesRegex(ValueError, "must be a boolean"):
            self.load_manifest(invalid)


class WorkflowDiffTests(unittest.TestCase):
    """Exercise the workflow's actual shell against small local Git histories."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "-b", "main")
        self.git("config", "user.email", "ci-test@example.invalid")
        self.git("config", "user.name", "CI test")
        (self.root / "scripts").mkdir()
        # Record the actual changed paths passed to the selector. Selection
        # policy itself is tested above against real repository variants.
        (self.root / "scripts/build.py").write_text(
            "import json, sys\nprint(json.dumps(sys.stdin.read().splitlines()))\n"
        )
        self.commit_file("README.md")
        self.base = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/origin/main", self.base)
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        select = workflow.split("name: Select variants based on changes", 1)[1]
        self.script = textwrap.dedent(select.split("        run: |\n", 1)[1].split("\n  build:", 1)[0])

    def git(self, *args):
        return subprocess.check_output(
            ["git", *args], cwd=self.root, text=True, stderr=subprocess.DEVNULL
        ).strip()

    def commit_file(self, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name + "\n")
        self.git("add", ".")
        self.git("commit", "-m", "test change")

    def selected_paths(self, event="push", before=None, ref="refs/heads/main"):
        output = self.root / "github-output"
        env = dict(os.environ)
        env.update({
            "EVENT_NAME": event, "BEFORE_SHA": before or self.base,
            "GITHUB_REF": ref, "GITHUB_WORKSPACE": str(self.root),
            "GITHUB_OUTPUT": str(output), "RUNNER_TEMP": str(self.root),
            "GIT_CONFIG_GLOBAL": str(self.root / "gitconfig"),
        })
        subprocess.run(
            ["bash", "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", self.script],
            cwd=self.root, env=env, capture_output=True, text=True, check=True,
        )
        return json.loads(output.read_text().removeprefix("variants="))

    def test_normal_push_includes_only_changed_paths(self):
        self.commit_file("main/boards/example/config.h")
        self.assertEqual(self.selected_paths(), ["main/boards/example/config.h"])

    def test_first_ci_branch_push_uses_main_merge_base(self):
        self.git("checkout", "-b", "ci/example")
        self.commit_file("main/application.cc")
        self.assertEqual(
            self.selected_paths(before="0" * 40, ref="refs/heads/ci/example"),
            ["main/application.cc"],
        )

    def test_pr_excludes_main_changes_since_branching(self):
        self.git("checkout", "-b", "feature")
        self.commit_file("main/boards/changed/config.h")
        self.git("checkout", "main")
        self.commit_file("main/boards/unrelated/config.h")
        self.git("merge", "--no-ff", "feature", "-m", "synthetic merge")
        self.assertEqual(
            self.selected_paths(event="pull_request"),
            ["main/boards/changed/config.h"],
        )

    def test_rename_reports_both_board_paths(self):
        self.commit_file("main/boards/old/config.h")
        before = self.git("rev-parse", "HEAD")
        (self.root / "main/boards/new").mkdir()
        self.git("mv", "main/boards/old/config.h", "main/boards/new/config.h")
        self.git("commit", "-m", "rename")
        self.assertEqual(
            set(self.selected_paths(before=before)),
            {"main/boards/old/config.h", "main/boards/new/config.h"},
        )

    def test_missing_push_baseline_and_manual_run_include_all_files(self):
        self.commit_file("main/boards/example/config.h")
        for event, before in (("push", "1" * 40), ("workflow_dispatch", self.base)):
            with self.subTest(event=event):
                output = self.root / "github-output"
                if output.exists():
                    output.unlink()
                self.assertEqual(
                    set(self.selected_paths(event=event, before=before)),
                    set(self.git("ls-files").splitlines()),
                )


if __name__ == "__main__":
    unittest.main()
