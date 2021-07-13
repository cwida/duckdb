# cursor description
from datetime import datetime, date
from pytest import mark


class TestCursorDescription(object):
    @mark.parametrize(
        "query,column_name,string_type,real_type",
        [
            ["SELECT * FROM integers", "i", "NUMBER", int],
            ["SELECT * FROM timestamps", "t", "DATETIME", datetime],
            ["SELECT DATE '1992-09-20';", "CAST(1992-09-20 AS DATE)", "Date", date],
            ["SELECT '\\xAA'::BLOB;", "\\xAA", "BINARY", bytes],
            ["SELECT {'x': 1, 'y': 2, 'z': 3}", "struct_pack(1, 2, 3)", "dict", dict],
            ["SELECT [1, 2, 3]", "list_value(1, 2, 3)", "list", list],
        ],
    )
    def test_description(
        self, query, column_name, string_type, real_type, duckdb_cursor
    ):
        duckdb_cursor.execute(query)
        assert duckdb_cursor.description == [
            (column_name, string_type, None, None, None, None, None)
        ]
        assert isinstance(duckdb_cursor.fetchone()[0], real_type)

    def test_none_description(self, duckdb_empty_cursor):
        assert duckdb_empty_cursor.description is None
