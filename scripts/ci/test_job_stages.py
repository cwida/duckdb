#!/usr/bin/env python3
import json
import os
import re
import shlex
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.ci import job_stages


class JobStagesTest(unittest.TestCase):
    UNITTEST_TOKEN_RE = re.compile(r"(?:\./)?[\w./-]*unittest(?:\.exe)?$")
    SKIP_IF_OVERRIDE = "OVERRIDE_JOBS is set; baseline job-selection tests are not applicable."

    def _workflow_paths(self) -> list[Path]:
        return sorted((REPO_ROOT / ".github" / "workflows").glob("*.yml"))

    def _extract_run_commands(self, workflow_text: str) -> list[tuple[int, str]]:
        commands: list[tuple[int, str]] = []
        lines = workflow_text.splitlines()
        i = 0
        while i < len(lines):
            line = lines[i]
            match = re.match(r"^(\s*)run:\s*(.*)$", line)
            if not match:
                i += 1
                continue

            indent = len(match.group(1))
            value = match.group(2).strip()
            line_no = i + 1

            if value in {"|", ">"}:
                block_lines: list[str] = []
                i += 1
                while i < len(lines):
                    block_line = lines[i]
                    if not block_line.strip():
                        block_lines.append("")
                        i += 1
                        continue

                    block_indent = len(block_line) - len(block_line.lstrip(" "))
                    if block_indent <= indent:
                        break
                    block_lines.append(block_line[indent + 2 :] if len(block_line) > indent + 2 else "")
                    i += 1

                commands.append((line_no, "\n".join(block_lines)))
                continue

            commands.append((line_no, value))
            i += 1

        return commands

    def _has_direct_unittest_invocation(self, command: str) -> bool:
        command_parts = re.split(r"\n|&&|\|\||;", command)
        for raw_part in command_parts:
            part = raw_part.strip()
            if not part:
                continue
            try:
                tokens = shlex.split(part)
            except ValueError:
                tokens = part.split()
            if not tokens:
                continue

            idx = 0
            while idx < len(tokens) and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=.*$", tokens[idx]):
                idx += 1
            if idx >= len(tokens):
                continue

            if self.UNITTEST_TOKEN_RE.match(tokens[idx]):
                return True
        return False

    def _compute_job_selection(
        self,
        event_name: str,
        ref_name: str,
        repository: str,
        skip_tests: bool = False,
        changed_keys: set[str] | None = None,
        default_branch: str = "main",
        reduced_ci_mode: str | None = None,
        runner_provider: str = "github",
        run_all: bool = True,
    ) -> job_stages.JobSelection:
        if reduced_ci_mode is None:
            reduced_ci_mode = "enabled" if event_name == "pull_request" else "disabled"
        return job_stages.compute_job_selection(
            job_stages.JobSelectionInput(
                event_name=event_name,
                ref_name=ref_name,
                default_branch=default_branch,
                repository=repository,
                skip_tests=skip_tests,
                changed_keys=changed_keys or set(),
                reduced_ci_mode=reduced_ci_mode,
                run_all=run_all,
                runners={"linux_x64": "runner-x64", "linux_arm64": "runner-arm64"},
                runner_provider=runner_provider,
            )
        )

    def _main_workflow_job_ids(self) -> set[str]:
        workflow_path = REPO_ROOT / ".github" / "workflows" / "Main.yml"
        workflow_text = workflow_path.read_text(encoding="utf-8")

        in_jobs = False
        job_ids: set[str] = set()
        for line in workflow_text.splitlines():
            if not in_jobs:
                if line.strip() == "jobs:":
                    in_jobs = True
                continue

            if line and not line.startswith(" "):
                break

            match = re.match(r"^ ([a-zA-Z0-9_-]+):\s*$", line)
            if match:
                job_ids.add(match.group(1))

        self.assertTrue(job_ids, "failed to parse any top-level jobs from .github/workflows/Main.yml")
        return job_ids

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_merge_group_minimal_jobs(self):
        selection = self._compute_job_selection(
            "merge_group", "gh-readonly-queue/main/pr-1-abc", "duckdb/duckdb", changed_keys={"osx"}
        )
        required_jobs = {"linux-relassert", "linux-release", "linux-release-tests", "tidy-check"}
        self.assertTrue(required_jobs.issubset(set(selection.enabled_jobs)))
        self.assertNotIn("osx", selection.enabled_jobs)
        self.assertEqual([config["name"] for config in selection.linux_release_matrix], ["amd64 compatibility"])
        self.assertTrue(selection.linux_release_matrix[0]["publish_static"])
        self.assertTrue(selection.linux_release_matrix[0]["test_static"])
        self.assertFalse(selection.save_cache)

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_default_branch_includes_nightly_only_jobs(self):
        selection = self._compute_job_selection(
            "push", "v2.0-cyanoptera", "duckdb/duckdb", default_branch="v2.0-cyanoptera"
        )
        self.assertIn("codecov", selection.enabled_jobs)
        self.assertEqual(selection.enabled_jobs.count("osx"), 1)
        self.assertFalse(selection.save_cache)

        former_default_selection = self._compute_job_selection(
            "push", "main", "duckdb/duckdb", default_branch="v2.0-cyanoptera"
        )
        self.assertNotIn("codecov", former_default_selection.enabled_jobs)
        self.assertIn("osx", former_default_selection.enabled_jobs)

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_workflow_dispatch_adds_release_jobs(self):
        for ref_name in ["feature/my-branch", "main"]:
            push_selection = self._compute_job_selection("push", ref_name, "duckdb/duckdb")
            workflow_dispatch_selection = self._compute_job_selection("workflow_dispatch", ref_name, "duckdb/duckdb")
            expected_base_jobs = set(push_selection.enabled_jobs) - {"regression"}
            self.assertTrue(expected_base_jobs.issubset(set(workflow_dispatch_selection.enabled_jobs)))
            self.assertTrue(set(job_stages.RELEASE_JOBS).issubset(set(workflow_dispatch_selection.enabled_jobs)))
            self.assertNotIn("regression", workflow_dispatch_selection.enabled_jobs)
            self.assertTrue(workflow_dispatch_selection.optimized_release)
            self.assertEqual(
                workflow_dispatch_selection.extension_opt_in_archs,
                "windows_arm64;osx_amd64;linux_amd64_musl;linux_arm64_musl",
            )
            self.assertEqual(len(workflow_dispatch_selection.linux_release_matrix), 4)
            self.assertEqual(
                [config["ccache_key"] for config in workflow_dispatch_selection.linux_release_matrix[:2]],
                ["linux-cli-amd64-glibc", "linux-cli-arm64-glibc"],
            )
            self.assertEqual(
                [config["build_artifact"] for config in workflow_dispatch_selection.linux_release_matrix],
                [
                    "linux-release-compat-build",
                    "linux-release-arm64-compat-build",
                    "linux-release-build",
                    "linux-release-arm64-build",
                ],
            )
            self.assertEqual(
                [
                    config["name"]
                    for config in workflow_dispatch_selection.linux_release_matrix
                    if config["is_canonical_build"]
                ],
                ["amd64 optimized", "arm64 optimized"],
            )
            self.assertEqual(
                [config["publish_static"] for config in workflow_dispatch_selection.linux_release_matrix],
                [False, False, True, True],
            )
            self.assertEqual(
                [config["test_static"] for config in workflow_dispatch_selection.linux_release_matrix],
                [False, False, True, True],
            )
            self.assertEqual(
                [config["build_jemalloc"] for config in workflow_dispatch_selection.linux_release_matrix],
                ["1", "1", "1", "1"],
            )
            self.assertEqual(
                [config["publish_source"] for config in workflow_dispatch_selection.linux_release_matrix],
                [True, False, False, False],
            )
            self.assertTrue(
                all(
                    "-DNATIVE_LTO_STATIC_LIBRARIES=1" in config["extra_cmake_variables"]
                    for config in workflow_dispatch_selection.linux_release_matrix[2:]
                )
            )
            self.assertTrue(workflow_dispatch_selection.save_cache)

    def test_regular_branch_excludes_main_only_jobs(self):
        selection = self._compute_job_selection("pull_request", "feature/my-branch", "duckdb/duckdb")
        self.assertNotIn("codecov", selection.enabled_jobs)
        self.assertNotIn("osx", selection.enabled_jobs)
        self.assertIn("linux-release-musl", selection.enabled_jobs)
        self.assertEqual([config["name"] for config in selection.linux_release_matrix], ["amd64 compatibility"])
        self.assertTrue(selection.linux_release_matrix[0]["publish_static"])
        self.assertTrue(selection.linux_release_matrix[0]["test_static"])
        self.assertEqual([config["name"] for config in selection.linux_musl_matrix], ["arm64"])
        self.assertEqual(
            [config["build_artifact"] for config in selection.linux_musl_matrix],
            ["linux-release-musl-arm64-build"],
        )
        self.assertEqual(selection.extension_opt_in_archs, "")
        self.assertFalse(selection.save_cache)

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_feature_branch_push_enables_osx_for_full_extension_matrix(self):
        selection = self._compute_job_selection("push", "feature/my-branch", "duckdb/duckdb")
        self.assertIn("extensions-build", selection.enabled_jobs)
        self.assertIn("osx", selection.enabled_jobs)

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_full_extension_ci_on_pull_request_builds_prebuilt_dependencies(self):
        selection = self._compute_job_selection(
            "pull_request", "feature/my-branch", "duckdb/duckdb", reduced_ci_mode="disabled"
        )
        self.assertEqual(selection.reduced_ci_mode, "disabled")
        self.assertIn("osx", selection.enabled_jobs)
        self.assertEqual(
            [config["name"] for config in selection.linux_release_matrix],
            ["amd64 compatibility", "arm64 compatibility"],
        )
        self.assertEqual(
            [config["artifact_suffix"] for config in selection.linux_release_matrix],
            ["linux-amd64", "linux-arm64"],
        )

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_osx_changed_key_enables_osx(self):
        for event_name in ["push", "pull_request"]:
            selection = self._compute_job_selection(
                event_name, "feature/my-branch", "duckdb/duckdb", changed_keys={"osx"}
            )
            self.assertIn("osx", selection.enabled_jobs)

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_extension_changes_enable_osx(self):
        for event_name in ["push", "pull_request"]:
            selection = self._compute_job_selection(
                event_name, "feature/my-branch", "duckdb/duckdb", changed_keys={"extensions"}
            )
            self.assertIn("osx", selection.enabled_jobs)
            self.assertEqual(selection.reduced_ci_mode, "disabled")
            self.assertEqual(
                selection.extension_opt_in_archs,
                "windows_arm64;linux_arm64_musl" if event_name == "pull_request" else "",
            )
        self.assertEqual(
            [config["name"] for config in selection.linux_release_matrix],
            ["amd64 compatibility", "arm64 compatibility"],
        )

    def test_windows_full_matrix_selection(self):
        regular_pr = self._compute_job_selection("pull_request", "feature/my-branch", "duckdb/duckdb")
        self.assertFalse(regular_pr.windows_full_matrix)

        extension_pr = self._compute_job_selection(
            "pull_request", "feature/my-branch", "duckdb/duckdb", changed_keys={"extensions"}
        )
        self.assertTrue(extension_pr.windows_full_matrix)

        push = self._compute_job_selection("push", "feature/my-branch", "duckdb/duckdb")
        self.assertTrue(push.windows_full_matrix)

        reduced_dispatch = self._compute_job_selection(
            "workflow_dispatch", "feature/my-branch", "duckdb/duckdb", run_all=False
        )
        self.assertFalse(reduced_dispatch.windows_full_matrix)

        full_dispatch = self._compute_job_selection(
            "workflow_dispatch", "feature/my-branch", "duckdb/duckdb", run_all=True
        )
        self.assertTrue(full_dispatch.windows_full_matrix)

        default_branch_dispatch = self._compute_job_selection(
            "workflow_dispatch", "main", "duckdb/duckdb", run_all=False
        )
        self.assertTrue(default_branch_dispatch.windows_full_matrix)

        fork_pr = self._compute_job_selection("pull_request", "feature/my-branch", "somefork/duckdb")
        self.assertTrue(fork_pr.windows_full_matrix)

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_workflow_dispatch_with_skipped_tests_includes_optimized_release(self):
        selection = self._compute_job_selection("workflow_dispatch", "main", "duckdb/duckdb", skip_tests=True)
        self.assertIn("linux-release", selection.enabled_jobs)
        self.assertNotIn("linux-release-tests", selection.enabled_jobs)
        self.assertIn("osx", selection.enabled_jobs)
        self.assertIn("codecov", selection.enabled_jobs)
        self.assertTrue(selection.optimized_release)
        self.assertEqual(
            [config["name"] for config in selection.linux_release_matrix],
            ["amd64 compatibility", "arm64 compatibility", "amd64 optimized", "arm64 optimized"],
        )
        self.assertEqual(
            [config["build_artifact"] for config in selection.linux_release_matrix],
            [
                "linux-release-compat-build",
                "linux-release-arm64-compat-build",
                "linux-release-build",
                "linux-release-arm64-build",
            ],
        )
        self.assertEqual(
            [config["ccache_key"] for config in selection.linux_release_matrix[:2]],
            ["linux-cli-amd64-glibc", "linux-cli-arm64-glibc"],
        )
        self.assertEqual(
            [config["name"] for config in selection.linux_release_matrix if config["is_canonical_build"]],
            ["amd64 optimized", "arm64 optimized"],
        )

    def test_fork_saves_cache(self):
        selection = self._compute_job_selection("pull_request", "feature/my-branch", "somefork/duckdb")
        self.assertTrue(selection.save_cache)

    def test_parse_changed_keys(self):
        parsed = job_stages.parse_changed_keys(" tEsts_slow,extensions\\\ngithub tests_slow ")
        self.assertEqual(parsed, {"tests_slow", "extensions", "github"})

    def test_parse_runners(self):
        self.assertEqual(
            job_stages.parse_runners('{"linux_x64":"runner-x64","linux_arm64":"runner-arm64"}'),
            {"linux_x64": "runner-x64", "linux_arm64": "runner-arm64"},
        )
        with self.assertRaises(ValueError):
            job_stages.parse_runners('["runner-x64"]')
        with self.assertRaises(ValueError):
            job_stages.parse_runners('{"linux_x64":"runner-x64"}')

    def test_merge_group_namespace_runner_priority(self):
        selection = self._compute_job_selection(
            "merge_group",
            "gh-readonly-queue/main/pr-1-abc",
            "duckdb/duckdb",
            runner_provider="namespace",
        )
        self.assertEqual(
            selection.runners,
            {
                "linux_x64": "runner-x64;job.priority=2",
                "linux_arm64": "runner-arm64;job.priority=2",
            },
        )
        self.assertEqual(selection.linux_release_matrix[0]["runner"], "runner-x64;job.priority=2")

        pull_request_selection = self._compute_job_selection(
            "pull_request", "feature/my-branch", "duckdb/duckdb", runner_provider="namespace"
        )
        self.assertEqual(pull_request_selection.runners["linux_x64"], "runner-x64")

        github_selection = self._compute_job_selection(
            "merge_group", "gh-readonly-queue/main/pr-1-abc", "somefork/duckdb", runner_provider="github"
        )
        self.assertEqual(github_selection.runners["linux_x64"], "runner-x64")

    def test_writes_github_output(self):
        selection = job_stages.JobSelection(
            enabled_jobs=["linux-relassert"],
            save_cache=False,
            reduced_ci_mode="enabled",
            windows_full_matrix=False,
        )
        with tempfile.NamedTemporaryFile(mode="w+", encoding="utf-8", delete=False) as tmp:
            output_path = tmp.name
        try:
            with open(output_path, "a", encoding="utf-8") as f:
                job_stages.write_outputs(selection, f)
            with open(output_path, "r", encoding="utf-8") as f:
                lines = [line.strip() for line in f.readlines() if line.strip()]
        finally:
            os.unlink(output_path)

        self.assertEqual(lines[0], "enabled_jobs=[\"linux-relassert\"]")
        self.assertEqual(lines[1], "save_cache=false")
        self.assertEqual(lines[2], "optimized_release=false")
        self.assertEqual(lines[3], "reduced_ci_mode=enabled")
        self.assertEqual(lines[4], "windows_full_matrix=false")
        self.assertEqual(lines[5], "extension_opt_in_archs=")
        self.assertEqual(lines[6], "runners={}")
        self.assertEqual(lines[7], "linux_release_matrix=[]")
        self.assertEqual(lines[8], "linux_musl_matrix=[]")

    def test_reduced_ci_mode_override(self):
        self.assertEqual(job_stages.resolve_reduced_ci_mode("enabled", "disabled"), "disabled")
        self.assertEqual(job_stages.resolve_reduced_ci_mode("disabled", "enabled"), "enabled")
        self.assertEqual(
            job_stages.resolve_reduced_ci_mode("enabled", event_name="pull_request", changed_keys={"extensions"}),
            "disabled",
        )
        self.assertEqual(
            job_stages.resolve_reduced_ci_mode(
                "enabled", "enabled", event_name="pull_request", changed_keys={"extensions"}
            ),
            "enabled",
        )
        with self.assertRaisesRegex(ValueError, "must be enabled or disabled"):
            job_stages.resolve_reduced_ci_mode("enabled", "auto")

    @unittest.skipIf(os.getenv("OVERRIDE_JOBS") is not None, SKIP_IF_OVERRIDE)
    def test_main_prints_and_writes_outputs(self):
        with tempfile.NamedTemporaryFile(mode="w+", encoding="utf-8", delete=False) as tmp:
            output_path = tmp.name

        old_env = os.environ.get("GITHUB_OUTPUT")
        old_reduced_ci_override = os.environ.get("OVERRIDE_REDUCED_CI_MODE")
        old_argv = sys.argv
        try:
            os.environ["GITHUB_OUTPUT"] = output_path
            os.environ["OVERRIDE_REDUCED_CI_MODE"] = "disabled"
            sys.argv = [
                "job_stages.py",
                "--event",
                "merge_group",
                "--ref_name",
                "gh-readonly-queue/main/pr-1-abc",
                "--default_branch",
                "v2.0-cyanoptera",
                "--repository",
                "duckdb/duckdb",
                "--reduced-ci-mode",
                "enabled",
                "--run-all",
                "true",
                "--runners",
                '{"linux_x64":"runner-x64","linux_arm64":"runner-arm64"}',
                "--runner-provider",
                "namespace",
            ]
            rc = job_stages.main()
            self.assertEqual(rc, 0)
            with open(output_path, "r", encoding="utf-8") as f:
                out = f.read()
            self.assertIn("enabled_jobs=", out)
            self.assertIn("save_cache=false", out)
            self.assertIn("reduced_ci_mode=disabled", out)
            self.assertIn("extension_opt_in_archs=", out)
            payload = out.splitlines()[0].split("=", 1)[1]
            selected_jobs = json.loads(payload)
            required_jobs = {"linux-relassert", "linux-release", "linux-release-tests", "tidy-check"}
            self.assertTrue(required_jobs.issubset(set(selected_jobs)))
            output_values = dict(line.split("=", 1) for line in out.splitlines())
            release_matrix = json.loads(output_values["linux_release_matrix"])
            self.assertEqual(release_matrix[0]["runner"], "runner-x64;job.priority=2")
        finally:
            sys.argv = old_argv
            if old_env is None:
                os.environ.pop("GITHUB_OUTPUT", None)
            else:
                os.environ["GITHUB_OUTPUT"] = old_env
            if old_reduced_ci_override is None:
                os.environ.pop("OVERRIDE_REDUCED_CI_MODE", None)
            else:
                os.environ["OVERRIDE_REDUCED_CI_MODE"] = old_reduced_ci_override
            os.unlink(output_path)

    def test_job_selection_override_adds_prepare(self):
        old_value = os.environ.get("OVERRIDE_JOBS")
        try:
            os.environ["OVERRIDE_JOBS"] = "extensions-build extensions-deploy extensions-install"
            selection = self._compute_job_selection("pull_request", "feature/my-branch", "duckdb/duckdb")
            self.assertEqual(
                selection.enabled_jobs,
                ["prepare", "extensions-build", "extensions-deploy", "extensions-install"],
            )
        finally:
            if old_value is None:
                os.environ.pop("OVERRIDE_JOBS", None)
            else:
                os.environ["OVERRIDE_JOBS"] = old_value

    def test_job_selection_override_invalid_job_raises(self):
        old_value = os.environ.get("OVERRIDE_JOBS")
        try:
            os.environ["OVERRIDE_JOBS"] = "extensions-deploy,not-a-job"
            with self.assertRaises(ValueError):
                self._compute_job_selection("pull_request", "feature/my-branch", "duckdb/duckdb")
        finally:
            if old_value is None:
                os.environ.pop("OVERRIDE_JOBS", None)
            else:
                os.environ["OVERRIDE_JOBS"] = old_value

    def test_all_jobs_matches_main_workflow(self):
        workflow_jobs = self._main_workflow_job_ids()
        missing_in_workflow = job_stages.ALL_JOBS - workflow_jobs
        missing_in_job_stages = workflow_jobs - job_stages.ALL_JOBS

        self.assertFalse(
            missing_in_workflow,
            f"jobs listed in job_stages.ALL_JOBS but missing from Main.yml: {sorted(missing_in_workflow)}",
        )
        self.assertFalse(
            missing_in_job_stages,
            f"jobs in Main.yml but missing from job_stages.ALL_JOBS: {sorted(missing_in_job_stages)}",
        )

    def test_summary_needs_all_other_jobs(self):
        workflow_path = REPO_ROOT / ".github" / "workflows" / "Main.yml"
        workflow_text = workflow_path.read_text(encoding="utf-8")
        workflow_jobs = self._main_workflow_job_ids()

        lines = workflow_text.splitlines()
        in_summary = False
        in_needs = False
        summary_needs: set[str] = set()

        for line in lines:
            if not in_summary:
                if re.match(r"^ summary:\s*$", line):
                    in_summary = True
                continue

            # Leave summary block on next top-level job.
            if re.match(r"^ [a-zA-Z0-9_-]+:\s*$", line):
                break

            if not in_needs:
                if re.match(r"^\s+needs:\s*$", line):
                    in_needs = True
                continue

            need_match = re.match(r"^\s+-\s+([a-zA-Z0-9_-]+)\s*$", line)
            if need_match:
                summary_needs.add(need_match.group(1))
                continue

            if re.match(r"^\s+steps:\s*$", line):
                break

        self.assertTrue(in_summary, "failed to locate 'summary' job in Main.yml")
        self.assertTrue(in_needs, "failed to locate summary.needs in Main.yml")

        expected_needs = workflow_jobs - {"summary"}
        missing_from_summary = expected_needs - summary_needs
        extra_in_summary = summary_needs - expected_needs

        self.assertFalse(
            missing_from_summary,
            f"summary.needs is missing jobs from Main.yml: {sorted(missing_from_summary)}",
        )
        self.assertFalse(
            extra_in_summary,
            f"summary.needs references unknown jobs: {sorted(extra_in_summary)}",
        )

    def test_workflow_unittest_commands_use_wrapped_runner(self):
        violations: list[str] = []

        for workflow_path in self._workflow_paths():
            workflow_text = workflow_path.read_text(encoding="utf-8")
            for line_no, command in self._extract_run_commands(workflow_text):
                if not self._has_direct_unittest_invocation(command):
                    continue
                if re.search(r"(?:^|[\s\"'])\.?/build/[^/\s\"']+/test/run(?:$|[\s\"'])", command):
                    continue
                snippet = " ".join(command.strip().split())
                if len(snippet) > 180:
                    snippet = snippet[:177] + "..."
                violations.append(f"{workflow_path.relative_to(REPO_ROOT)}:{line_no}: {snippet}")

        if violations:
            formatted = "\n\n".join(f"- {entry}" for entry in violations)
            self.fail(
                "workflow `run:` commands using `unittest` must use `build/*/test/run` as test runner:\n\n" + formatted
            )


if __name__ == "__main__":
    unittest.main()
