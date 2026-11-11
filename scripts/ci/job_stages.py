#!/usr/bin/env python3

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field, replace
from typing import TextIO

COMMON_JOBS = [
    "linux-relassert",
    "linux-relassert-tests",
    "tidy-check",
    "extensions-build",
    "extensions-deploy",
    "wasm-eh",
    "linux-release",
    "linux-release-tests",
    "linux-release-musl",
    "swift",
    "windows",
    "no-string-inline",
    "no-rtti",
    "vector-sizes",
    "threadsan",
    "linux-configs",
]

PULL_REQUEST_ONLY_JOBS = [
    "regression",
]

PULL_REQUEST_JOBS = COMMON_JOBS + PULL_REQUEST_ONLY_JOBS

NIGHTLY_ONLY_JOBS = [
    "osx",
    "codecov",
]

NIGHTLY_JOBS = COMMON_JOBS + NIGHTLY_ONLY_JOBS

MERGE_GROUP_JOBS = [
    "linux-relassert",
    "linux-release",
    "linux-release-tests",
    "tidy-check",
]

RELEASE_JOBS = [
    "osx",
    "codecov",
    "extensions-install",
]

SKIP_TESTS_JOBS = {
    "linux-relassert-tests",
    "regression",
    "swift",
    "linux-configs",
    "linux-release-tests",
}

PREPARE_JOBS = [
    "prepare",
]

SUMMARY_JOBS = [
    "summary",
]

ALL_JOBS = set(PREPARE_JOBS)
ALL_JOBS |= set(PULL_REQUEST_JOBS)
ALL_JOBS |= set(NIGHTLY_JOBS)
ALL_JOBS |= set(MERGE_GROUP_JOBS)
ALL_JOBS |= set(SUMMARY_JOBS)
ALL_JOBS |= set(RELEASE_JOBS)
SELECTABLE_JOBS = ALL_JOBS - set(SUMMARY_JOBS)


@dataclass(frozen=True)
class JobSelection:
    enabled_jobs: list[str]
    save_cache: bool
    reduced_ci_mode: str
    windows_full_matrix: bool
    extension_opt_in_archs: str = ""
    optimized_release: bool = False
    runners: dict[str, str] = field(default_factory=dict)
    linux_release_matrix: list[dict[str, object]] = field(default_factory=list)
    linux_musl_matrix: list[dict[str, object]] = field(default_factory=list)


@dataclass(frozen=True)
class JobSelectionInput:
    event_name: str
    ref_name: str
    default_branch: str
    repository: str
    skip_tests: bool
    changed_keys: set[str]
    reduced_ci_mode: str
    run_all: bool
    runners: dict[str, str] = field(default_factory=dict)
    runner_provider: str = ""


def should_save_cache(selection_input: JobSelectionInput) -> bool:
    return selection_input.repository != "duckdb/duckdb" or selection_input.event_name == "workflow_dispatch"


def extension_opt_in_archs(selection_input: JobSelectionInput) -> str:
    if selection_input.event_name == "workflow_dispatch":
        return "windows_arm64;osx_amd64;linux_amd64_musl;linux_arm64_musl"
    if selection_input.event_name == "pull_request" and "extensions" in selection_input.changed_keys:
        return "windows_arm64;linux_arm64_musl"
    return ""


def windows_full_matrix(selection_input: JobSelectionInput) -> bool:
    if selection_input.ref_name == selection_input.default_branch or selection_input.repository != "duckdb/duckdb":
        return True
    if selection_input.event_name == "pull_request":
        return "extensions" in selection_input.changed_keys
    return selection_input.run_all


def enabled_jobs(selection_input: JobSelectionInput, reduced_ci_mode: str) -> list[str]:
    if selection_input.event_name == "merge_group":
        selected_jobs = MERGE_GROUP_JOBS.copy()
    elif selection_input.ref_name == selection_input.default_branch:
        selected_jobs = NIGHTLY_JOBS.copy()
    else:
        selected_jobs = PULL_REQUEST_JOBS.copy()

    if selection_input.event_name == "workflow_dispatch":
        selected_jobs = [job for job in selected_jobs if job != "regression"]
        selected_jobs.extend(RELEASE_JOBS)

    if selection_input.skip_tests:
        selected_jobs = [job for job in selected_jobs if job not in SKIP_TESTS_JOBS]

    extensions_need_osx = reduced_ci_mode == "disabled" and "extensions-build" in selected_jobs
    osx_changed = selection_input.event_name in {"push", "pull_request"} and "osx" in selection_input.changed_keys
    extensions_changed = (
        selection_input.event_name in {"push", "pull_request"} and "extensions" in selection_input.changed_keys
    )
    if (extensions_need_osx or osx_changed or extensions_changed) and "osx" not in selected_jobs:
        selected_jobs.append("osx")

    override = parse_job_selection_override(os.getenv("OVERRIDE_JOBS"))
    if override is not None:
        return override

    return selected_jobs


def parse_job_selection_override(value: str | None) -> list[str] | None:
    if value is None:
        return None
    parsed_jobs = [token for token in re.split(r"[\s,]+", value.strip()) if token]
    if not parsed_jobs:
        return PREPARE_JOBS.copy()

    invalid_jobs = sorted(set(parsed_jobs) - SELECTABLE_JOBS)
    if invalid_jobs:
        raise ValueError(f"invalid jobs in OVERRIDE_JOBS: {', '.join(invalid_jobs)}")

    seen: set[str] = set()
    deduplicated = [job for job in parsed_jobs if not (job in seen or seen.add(job))]
    if "prepare" not in seen:
        deduplicated = PREPARE_JOBS + deduplicated
    return deduplicated


def compatibility_release_config(*, runner: str, arch: str, optimized_release: bool) -> dict[str, object]:
    is_amd64 = arch == "amd64"
    artifact_suffix = f"linux-{arch}"
    if is_amd64:
        build_artifact = "linux-release-compat-build" if optimized_release else "linux-release-build"
    else:
        build_artifact = "linux-release-arm64-compat-build" if optimized_release else "linux-release-arm64-build"

    return {
        "runner": runner,
        "arch": arch,
        "image": f"manylinux_{arch}_main",
        "name": f"{arch} compatibility",
        "artifact_suffix": artifact_suffix,
        "build_artifact": build_artifact,
        "extension_config": "linux_release_extensions.cmake" if is_amd64 else "bundled_extensions.cmake",
        "vcpkg_cache_suffix": "" if is_amd64 else "linux-cli-arm64-glibc",
        "build_jemalloc": "1",
        "use_ccache": True,
        "ccache_key": f"linux-cli-{arch}-glibc",
        "save_vcpkg_cache": True,
        "lto": "",
        "lto_jobs": "",
        "extra_cmake_variables": "",
        "is_compatibility_build": True,
        "is_canonical_build": not optimized_release,
        "publish_static": not optimized_release,
        "publish_source": is_amd64,
        "test_static": not optimized_release,
        "run_smoke": is_amd64,
        "run_arm_tests": not is_amd64 and not optimized_release,
    }


def optimized_release_config(*, runner: str, arch: str) -> dict[str, object]:
    is_amd64 = arch == "amd64"
    return {
        "runner": runner,
        "arch": arch,
        "image": f"manylinux_{arch}_main",
        "name": f"{arch} optimized",
        "artifact_suffix": f"linux-{arch}",
        "build_artifact": "linux-release-build" if is_amd64 else "linux-release-arm64-build",
        "extension_config": "linux_release_extensions.cmake" if is_amd64 else "bundled_extensions.cmake",
        "vcpkg_cache_suffix": "" if is_amd64 else "linux-cli-arm64-glibc",
        "build_jemalloc": "1",
        "use_ccache": False,
        "ccache_key": "",
        "save_vcpkg_cache": False,
        "lto": "thin",
        "lto_jobs": "8",
        "extra_cmake_variables": (
            "-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= " "-DNATIVE_LTO_STATIC_LIBRARIES=1"
        ),
        "is_compatibility_build": False,
        "is_canonical_build": True,
        "publish_static": True,
        "publish_source": False,
        "test_static": True,
        "run_smoke": True,
        "run_arm_tests": not is_amd64,
    }


def linux_release_matrix(
    selection_input: JobSelectionInput, optimized_release: bool, full_extension_matrix: bool
) -> list[dict[str, object]]:
    result = [
        compatibility_release_config(
            runner=selection_input.runners.get("linux_x64", ""), arch="amd64", optimized_release=optimized_release
        )
    ]
    if selection_input.event_name not in {"pull_request", "merge_group"} or full_extension_matrix:
        result.append(
            compatibility_release_config(
                runner=selection_input.runners.get("linux_arm64", ""),
                arch="arm64",
                optimized_release=optimized_release,
            )
        )
    if optimized_release:
        result.extend(
            [
                optimized_release_config(runner=selection_input.runners.get("linux_x64", ""), arch="amd64"),
                optimized_release_config(runner=selection_input.runners.get("linux_arm64", ""), arch="arm64"),
            ]
        )
    return result


def linux_musl_matrix(selection_input: JobSelectionInput) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    if selection_input.event_name != "pull_request":
        result.append(
            {
                "runner": selection_input.runners.get("linux_x64", ""),
                "arch": "amd64",
                "image": "alpine_amd64_main",
                "name": "amd64",
                "artifact_suffix": "linux-amd64-musl",
                "build_artifact": "linux-release-musl-amd64-build",
                "cache_suffix": "amd64-musl",
            }
        )
    result.append(
        {
            "runner": selection_input.runners.get("linux_arm64", ""),
            "arch": "arm64",
            "image": "alpine_arm64_main",
            "name": "arm64",
            "artifact_suffix": "linux-arm64-musl",
            "build_artifact": "linux-release-musl-arm64-build",
            "cache_suffix": "arm64-musl",
        }
    )
    return result


def configure_runners(selection_input: JobSelectionInput) -> dict[str, str]:
    if selection_input.event_name != "merge_group" or selection_input.runner_provider != "namespace":
        return selection_input.runners.copy()
    return {name: f"{runner};job.priority=2" for name, runner in selection_input.runners.items()}


def compute_job_selection(selection_input: JobSelectionInput) -> JobSelection:
    reduced_ci_mode = resolve_reduced_ci_mode(
        selection_input.reduced_ci_mode,
        os.getenv("OVERRIDE_REDUCED_CI_MODE"),
        event_name=selection_input.event_name,
        changed_keys=selection_input.changed_keys,
    )
    selected_jobs = enabled_jobs(selection_input, reduced_ci_mode)
    optimized_release = selection_input.event_name == "workflow_dispatch" and "linux-release" in selected_jobs
    full_extension_matrix = reduced_ci_mode == "disabled" and "extensions-build" in selected_jobs
    runners = configure_runners(selection_input)
    configured_input = replace(selection_input, runners=runners)
    return JobSelection(
        enabled_jobs=selected_jobs,
        save_cache=should_save_cache(selection_input),
        reduced_ci_mode=reduced_ci_mode,
        windows_full_matrix=windows_full_matrix(selection_input),
        extension_opt_in_archs=extension_opt_in_archs(selection_input),
        optimized_release=optimized_release,
        runners=runners,
        linux_release_matrix=linux_release_matrix(configured_input, optimized_release, full_extension_matrix),
        linux_musl_matrix=linux_musl_matrix(configured_input),
    )


def write_outputs(selection: JobSelection, out: TextIO, *, include_matrices: bool = True) -> None:
    out.write(f"enabled_jobs={json.dumps(selection.enabled_jobs, separators=(',', ':'))}\n")
    out.write(f"save_cache={'true' if selection.save_cache else 'false'}\n")
    out.write(f"optimized_release={'true' if selection.optimized_release else 'false'}\n")
    out.write(f"reduced_ci_mode={selection.reduced_ci_mode}\n")
    out.write(f"windows_full_matrix={'true' if selection.windows_full_matrix else 'false'}\n")
    out.write(f"extension_opt_in_archs={selection.extension_opt_in_archs}\n")
    out.write(f"runners={json.dumps(selection.runners, separators=(',', ':'))}\n")
    if include_matrices:
        out.write(f"linux_release_matrix={json.dumps(selection.linux_release_matrix, separators=(',', ':'))}\n")
        out.write(f"linux_musl_matrix={json.dumps(selection.linux_musl_matrix, separators=(',', ':'))}\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compute enabled Main CI jobs and cache save policy.")
    parser.add_argument("--event", dest="event_name", required=True)
    parser.add_argument("--ref_name", required=True)
    parser.add_argument("--default_branch", required=True)
    parser.add_argument("--repository", default="duckdb/duckdb")
    parser.add_argument("--skip-tests", default="false")
    parser.add_argument("--changed-keys", default="")
    parser.add_argument("--reduced-ci-mode", required=True)
    parser.add_argument("--run-all", required=True)
    parser.add_argument("--runners", required=True)
    parser.add_argument("--runner-provider", required=True)
    return parser.parse_args()


def parse_bool(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off", ""}:
        return False
    raise ValueError(f"invalid boolean value: {value!r}")


def resolve_reduced_ci_mode(
    value: str,
    override: str | None = None,
    *,
    event_name: str = "",
    changed_keys: set[str] | None = None,
) -> str:
    effective_value = override if override is not None else value
    normalized = effective_value.strip().lower()
    if normalized not in {"enabled", "disabled"}:
        raise ValueError(f"invalid reduced CI mode: {effective_value!r} (must be enabled or disabled)")
    if override is None and event_name == "pull_request" and "extensions" in (changed_keys or set()):
        return "disabled"
    return normalized


def parse_changed_keys(value: str) -> set[str]:
    # changed-files may emit keys separated by spaces/newlines, and can be configured
    # to use commas. Its safe output mode escapes newlines with backslashes.
    return {token.lower() for token in re.split(r"[\\\s,]+", value.strip()) if token}


def parse_runners(value: str) -> dict[str, str]:
    parsed = json.loads(value)
    if not isinstance(parsed, dict) or not all(
        isinstance(key, str) and isinstance(item, str) for key, item in parsed.items()
    ):
        raise ValueError("runners must be a JSON object with string keys and values")
    missing = {"linux_x64", "linux_arm64"} - parsed.keys()
    if missing:
        raise ValueError(f"runners is missing required keys: {', '.join(sorted(missing))}")
    return parsed


def main() -> int:
    args = parse_args()
    selection_input = JobSelectionInput(
        event_name=args.event_name,
        ref_name=args.ref_name,
        default_branch=args.default_branch,
        repository=args.repository,
        skip_tests=parse_bool(args.skip_tests),
        changed_keys=parse_changed_keys(args.changed_keys),
        reduced_ci_mode=args.reduced_ci_mode,
        run_all=parse_bool(args.run_all),
        runners=parse_runners(args.runners),
        runner_provider=args.runner_provider,
    )
    selection = compute_job_selection(selection_input)

    # Emit to stderr so helper output stays visible in CI logs without polluting stdout pipelines.
    write_outputs(selection, sys.stderr, include_matrices=False)

    github_output = os.getenv("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as f:
            write_outputs(selection, f)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
