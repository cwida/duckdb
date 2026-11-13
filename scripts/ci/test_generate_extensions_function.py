#!/usr/bin/env python3

import re
import runpy
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]


class ExtensionEntriesTest(unittest.TestCase):
    CLOUD_EXTENSIONS = {"avro", "aws", "azure", "ducklake", "glue", "httpfs", "iceberg", "json", "parquet"}
    ENTRY_FIELDS = ("functions", "function_overloads", "settings", "secret_types", "external_resource_types")

    @classmethod
    def setUpClass(cls):
        with mock.patch.object(sys, "argv", ["generate_extensions_function.py"]), mock.patch("os.chdir"):
            namespace = runpy.run_path(str(REPO_ROOT / "scripts" / "generate_extensions_function.py"))
        cls.generator = namespace["main"].__globals__

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repository"
        self.repository.mkdir()
        self.shell = self.root / "duckdb"
        self.shell.touch()
        self.header = self.root / "extension_entries.hpp"
        self.header.write_text(
            (REPO_ROOT / self.generator["HEADER_PATH"]).read_text(encoding="utf-8"), encoding="utf-8"
        )
        self.original = self.generator["ParsedEntries"](self.header)
        self.queried_extensions = []

    def loaded_extensions(self, sql):
        return re.findall(r"LOAD '[^']*/([^/']+)\.duckdb_extension';", sql)

    def functions(self, sql=""):
        loaded = self.loaded_extensions(sql)
        if loaded:
            self.queried_extensions.append(loaded[-1])
        extensions = set(loaded)
        keys = {key for key, value in self.original.functions.items() if value.extension in extensions}
        overloads = {
            key: [self.generator["FunctionOverload"](key.name, key.type, (), self.generator["LogicalType"]("ANY"))]
            for key in keys
        }
        for key, values in self.original.function_overloads.items():
            matches = [value for value in values if value.extension in extensions]
            if matches:
                keys.add(key)
                overloads[key] = matches
        return keys, overloads

    def names(self, field):
        def query(sql=""):
            extensions = set(self.loaded_extensions(sql))
            return {key for key, value in getattr(self.original, field).items() if value.extension in extensions}

        return query

    def regenerate(self, extensions, artifacts=None, functions=None):
        for name in extensions if artifacts is None else artifacts:
            (self.repository / f"{name}.duckdb_extension").touch()
        args = self.generator["argparse"].Namespace(
            extensions=",".join(sorted(extensions)),
            extension_repository=str(self.repository),
            shell=str(self.shell),
            validate=False,
        )
        overrides = {
            "args": args,
            "DUCKDB_PATH": str(self.shell),
            "HEADER_PATH": str(self.header),
            "get_functions": functions or self.functions,
            "get_settings": self.names("settings"),
            "get_secret_types": self.names("secret_types"),
            "get_external_resource_types": self.names("external_resource_types"),
        }
        with mock.patch.dict(self.generator, overrides), redirect_stderr(StringIO()):
            self.generator["main"]()
        return self.generator["ParsedEntries"](self.header)

    def test_cloud_subset_preserves_other_entries_and_processes_dependencies_once(self):
        regenerated = self.regenerate(self.CLOUD_EXTENSIONS)
        for field in self.ENTRY_FIELDS:
            with self.subTest(field=field):
                self.assertEqual(getattr(regenerated, field), getattr(self.original, field))
        self.assertEqual(sorted(self.queried_extensions), sorted(self.CLOUD_EXTENSIONS))

    def test_subset_regeneration_detects_added_and_removed_functions(self):
        new_function = self.generator["Function"]("new_avro_function", self.generator["CatalogType"].TABLE)

        def functions(sql=""):
            if not self.loaded_extensions(sql):
                return set(), {}
            overload = self.generator["FunctionOverload"](
                new_function.name, new_function.type, (), self.generator["LogicalType"]("ANY")
            )
            return {new_function}, {new_function: [overload]}

        regenerated = self.regenerate({"avro"}, functions=functions)
        self.assertEqual(regenerated.functions[new_function].extension, "avro")
        self.assertNotIn(
            self.generator["Function"]("read_avro", self.generator["CatalogType"].TABLE), regenerated.functions
        )
        self.assertEqual(
            {key: value for key, value in regenerated.functions.items() if value.extension != "avro"},
            {key: value for key, value in self.original.functions.items() if value.extension != "avro"},
        )

    def test_missing_expected_artifact_fails_without_rewriting_header(self):
        original_content = self.header.read_text(encoding="utf-8")
        with self.assertRaises(SystemExit) as error:
            self.regenerate(self.CLOUD_EXTENSIONS, artifacts=self.CLOUD_EXTENSIONS - {"avro"})
        self.assertEqual(error.exception.code, 1)
        self.assertEqual(self.header.read_text(encoding="utf-8"), original_content)

    def test_missing_dependency_fails(self):
        with self.assertRaises(SystemExit) as error:
            self.regenerate({"iceberg"})
        self.assertEqual(error.exception.code, 1)


if __name__ == "__main__":
    unittest.main()
