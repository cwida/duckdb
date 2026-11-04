#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SUMMARIZER_PATH = REPO_ROOT / "scripts" / "ci" / "summarize_clang_tidy.py"
SPEC = importlib.util.spec_from_file_location("summarize_clang_tidy", SUMMARIZER_PATH)
SUMMARIZE_CLANG_TIDY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARIZE_CLANG_TIDY)


class SummarizeClangTidyTest(unittest.TestCase):
    def test_deduplicates_errors_and_keeps_first_full_diagnostic_block(self):
        lines = [
            "clang-tidy-20 -p=build/tidy /work/z.cpp",
            "/work/z.cpp:8:2: error: later error [check-z,-warnings-as-errors]",
            "  bad();",
            "  ^",
            "/work/z.cpp:2:1: note: declared here",
            "declaration();",
            "^",
            "2 warnings generated.",
            "clang-tidy-20 -p=build/tidy /work/a.cpp",
            "/work/a.cpp:3:4: error: first error [check-a,-warnings-as-errors]",
            "   wrong();",
            "   ^",
            "/work/a.cpp:3:4: error: first error [check-a,-warnings-as-errors]",
            "   duplicate_context();",
            "   ^",
            "Suppressed 12 warnings (12 in non-user code).",
        ]

        self.assertEqual(
            SUMMARIZE_CLANG_TIDY.summarize(lines, 1),
            [
                "Found 2 unique clang-tidy errors:",
                "",
                "/work/a.cpp:3:4: error: first error [check-a,-warnings-as-errors]",
                "   wrong();",
                "   ^",
                "",
                "/work/z.cpp:8:2: error: later error [check-z,-warnings-as-errors]",
                "  bad();",
                "  ^",
                "/work/z.cpp:2:1: note: declared here",
                "declaration();",
                "^",
            ],
        )

    def test_ignores_indented_error_text(self):
        lines = [
            "  example: error: this is source text",
            "No warnings generated.",
        ]

        self.assertEqual(SUMMARIZE_CLANG_TIDY.summarize(lines, 0), ["No clang-tidy errors found."])

    def test_shows_bounded_tail_for_failure_without_diagnostics(self):
        lines = [f"output line {index}" for index in range(125)]

        summary = SUMMARIZE_CLANG_TIDY.summarize(lines, 2)

        self.assertEqual(
            summary[:2],
            ["Clang-tidy failed without a recognized diagnostic; showing the final 100 log lines:", ""],
        )
        self.assertEqual(summary[2:], lines[-100:])

    def test_reports_empty_success_concisely(self):
        self.assertEqual(SUMMARIZE_CLANG_TIDY.summarize(["No warnings generated."], 0), ["No clang-tidy errors found."])


if __name__ == "__main__":
    unittest.main()
