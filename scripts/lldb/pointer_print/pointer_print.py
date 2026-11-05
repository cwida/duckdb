"""LLDB helpers for DuckDB smart pointers.

Usage inside LLDB:
    command script import /path/to/duckdb/scripts/lldb/pointer_print/pointer_print.py

After import, LLDB will:
1. Pretty-print DuckDB smart pointers (`unique_ptr`, `shared_ptr`, `optional_ptr`)
   as if they were the pointee.

The formatter path avoids calling `duckdb::unique_ptr<T>::operator*()` or `.get()`,
which makes it robust even when those template symbols are not emitted.
"""

from __future__ import annotations

import re

try:
    import lldb  # type: ignore
except ImportError:  # pragma: no cover - imported by LLDB at runtime
    lldb = None


MODULE_NAME = __name__.rsplit(".", 1)[-1]
CATEGORY_NAME = "duckdb"
SMART_PTR_TYPES = (
    "unique_ptr",
    "shared_ptr",
    "optional_ptr",
    "buffer_ptr",
    "arena_ptr",
    "unsafe_unique_ptr",
    "unsafe_shared_ptr",
    "unsafe_optional_ptr",
    "unsafe_arena_ptr",
)
SMART_PTR_REGEX = r"^duckdb::(" + "|".join(SMART_PTR_TYPES) + r")<.+>$"
PRINT_COMMAND_NAME = "duckdb-p"
_LAST_ROOT_EXPRESSION = ""


def __lldb_init_module(debugger, _internal_dict):
    _handle_lldb_command(debugger, f"type category define {CATEGORY_NAME}")
    _handle_lldb_command(
        debugger,
        f"type synthetic add --category {CATEGORY_NAME} "
        f"-x '{SMART_PTR_REGEX}' --python-class {_callback_name('DuckDBSmartPtrSyntheticProvider')}",
    )
    _handle_lldb_command(
        debugger,
        f"type summary add -e --category {CATEGORY_NAME} "
        f"-x '{SMART_PTR_REGEX}' -F {_callback_name('duckdb_smart_ptr_summary')}",
    )
    _handle_lldb_command(debugger, f"type category enable {CATEGORY_NAME}")
    _handle_lldb_command(debugger, f"command script delete {PRINT_COMMAND_NAME}", ignore_errors=True)
    _handle_lldb_command(
        debugger, f"command script add -f {_callback_name('duckdb_print_command')} {PRINT_COMMAND_NAME}"
    )
    _handle_lldb_command(debugger, f"command alias p {PRINT_COMMAND_NAME}")


def duckdb_print_command(debugger, command, result, _internal_dict):
    global _LAST_ROOT_EXPRESSION
    expr_text = _extract_expression_text(command)
    _LAST_ROOT_EXPRESSION = _normalize_expression_text(expr_text)
    debugger.GetCommandInterpreter().HandleCommand(_build_expression_command(command), result)


def _build_expression_command(command):
    stripped = command.lstrip()
    if stripped.startswith("/"):
        match = re.match(r"^/([A-Za-z]+)\s+(.*)$", stripped, re.DOTALL)
        if match:
            fmt, expr = match.groups()
            return f"expression -d no-run-target -f {fmt} -- {expr}"
        return f"expression {command}"
    return f"expression -d no-run-target -- {command}"


def _extract_expression_text(command):
    stripped = command.lstrip()
    if stripped.startswith("/"):
        match = re.match(r"^/([A-Za-z]+)\s+(.*)$", stripped, re.DOTALL)
        if match:
            return match.group(2)
    return command


def _handle_lldb_command(debugger, command, ignore_errors=False):
    result = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(command, result)
    if ignore_errors or result.Succeeded():
        return result

    error = result.GetError() or result.GetOutput()
    if error:
        print(error.rstrip())
    return result


def _callback_name(name):
    return f"{MODULE_NAME}.{name}"


def duckdb_smart_ptr_summary(valobj, _internal_dict):
    pointer_value = _get_pointer_value(valobj)
    if pointer_value is None or not pointer_value.IsValid():
        return "<unavailable>"

    address = pointer_value.GetValueAsUnsigned(0)
    if address == 0:
        return "nullptr"

    pointee_value = _dereference_pointer(pointer_value)
    pointee_name = _get_pointee_name(valobj, pointer_value, pointee_value)
    return f"{pointee_name} @ 0x{address:016x}"


def _get_pointee_name(valobj, pointer_value, pointee_value):
    if pointee_value is not None and pointee_value.IsValid():
        pointee_type = pointee_value.GetType()
        dynamic_name = pointee_type.GetDisplayTypeName() or pointee_type.GetName()
        if dynamic_name:
            return dynamic_name

    template_type = _get_template_argument_type(valobj)
    if template_type is not None and template_type.IsValid():
        template_name = template_type.GetDisplayTypeName() or template_type.GetName()
        if template_name:
            return template_name

    pointee_type = (
        pointee_value.GetType()
        if pointee_value is not None and pointee_value.IsValid()
        else pointer_value.GetType().GetPointeeType()
    )
    return pointee_type.GetDisplayTypeName() or pointee_type.GetName() or "value"


def _get_template_argument_type(value):
    if value is None or not value.IsValid():
        return None

    type_obj = value.GetType().GetUnqualifiedType()
    try:
        template_type = type_obj.GetTemplateArgumentType(0)
    except Exception:
        return None
    return template_type if template_type.IsValid() else None


def _get_pointer_value(value):
    current = value.GetNonSyntheticValue() if value and value.IsValid() else value
    while current and current.IsValid():
        type_obj = current.GetType()
        if type_obj.IsPointerType():
            return current

        type_name = type_obj.GetUnqualifiedType().GetName() or ""
        if not _is_supported_smart_ptr_type(type_name):
            return None

        pointer_child = _find_pointer_descendant(current)
        if pointer_child is None or not pointer_child.IsValid():
            return None
        current = pointer_child
    return None


def _is_supported_smart_ptr_type(type_name):
    return any(ptr_type + "<" in type_name for ptr_type in SMART_PTR_TYPES)


def _should_expand_children(value):
    path = _normalize_expression_text(_get_expression_path(value))
    if not path:
        return False

    if path == _LAST_ROOT_EXPRESSION:
        return True
    if path.startswith("$") and _is_expression_result(value):
        type_name = value.GetType().GetUnqualifiedType().GetName() or ""
        return _is_supported_smart_ptr_type(type_name)
    return False


def _is_expression_result(value):
    if value is None or not value.IsValid():
        return False
    return value.GetValueType() == getattr(lldb, "eValueTypeConstResult", 7)


def _dereference_pointer(pointer_value):
    if pointer_value is None or not pointer_value.IsValid():
        return None
    if pointer_value.GetValueAsUnsigned(0) == 0:
        return None

    pointee = pointer_value.Dereference()
    error = pointee.GetError()
    if error.Fail():
        return None

    dynamic_pointee = _get_dynamic_value(pointee)
    if dynamic_pointee is not None and dynamic_pointee.IsValid():
        return dynamic_pointee
    return pointee


def _get_dynamic_value(value):
    if value is None or not value.IsValid():
        return None

    try:
        dynamic_value = value.GetDynamicValue(lldb.eDynamicDontRunTarget)
    except Exception:
        return None

    if dynamic_value is None or not dynamic_value.IsValid():
        return None
    return dynamic_value


def _find_pointer_descendant(value):
    if value is None or not value.IsValid():
        return None

    pending = [value]
    seen = set()
    scanned = 0

    while pending and scanned < 64:
        current = pending.pop()
        if current is None or not current.IsValid():
            continue

        key = _value_key(current)
        if key in seen:
            continue
        seen.add(key)
        scanned += 1

        for field_name in ("pointer", "__ptr_"):
            direct = current.GetChildMemberWithName(field_name)
            if direct and direct.IsValid():
                return direct

        child_count = current.GetNumChildren()
        for i in range(child_count):
            child = current.GetChildAtIndex(i)
            if not child.IsValid():
                continue
            child_type = child.GetType()
            if child_type.IsPointerType():
                return child
            pending.append(child)
    return None


def _value_key(value):
    value_id = getattr(value, "GetID", None)
    if callable(value_id):
        return ("id", value_id())

    return (
        value.GetName(),
        value.GetType().GetName() if value.GetType().IsValid() else None,
        value.GetValue(),
        value.GetValueAsUnsigned(0),
        value.GetNumChildren(),
    )


def _get_expression_path(value):
    if value is None or not value.IsValid():
        return ""

    stream = lldb.SBStream()
    ok = value.GetExpressionPath(stream)
    if not ok:
        return ""
    return stream.GetData() or ""


def _normalize_expression_text(text):
    if not text:
        return ""

    normalized = re.sub(r"\s+", "", text).replace("->", ".")
    while normalized.startswith("(") and normalized.endswith(")") and _outer_parens_wrap_entire_expr(normalized):
        normalized = normalized[1:-1]
    return normalized


def _outer_parens_wrap_entire_expr(text):
    depth = 0
    for index, char in enumerate(text):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0 and index != len(text) - 1:
                return False
    return depth == 0


class DuckDBSmartPtrSyntheticProvider:
    def __init__(self, valobj, _internal_dict):
        self.valobj = valobj
        self.pointer_value = None
        self.pointee_value = None
        self.expand_children = False
        self.update()

    def update(self):
        self.expand_children = _should_expand_children(self.valobj)
        self.pointer_value = _get_pointer_value(self.valobj)
        self.pointee_value = None
        if self.expand_children:
            self.pointee_value = _dereference_pointer(self.pointer_value)
        return False

    def has_children(self):
        return self.expand_children and self.pointee_value is not None and self.pointee_value.IsValid()

    def num_children(self):
        if not self.has_children():
            return 0
        return self.pointee_value.GetNumChildren()

    def get_child_at_index(self, index):
        if not self.has_children():
            return None
        if index < 0 or index >= self.pointee_value.GetNumChildren():
            return None
        return self.pointee_value.GetChildAtIndex(index)

    def get_child_index(self, name):
        if not self.has_children():
            return -1
        for index in range(self.pointee_value.GetNumChildren()):
            child = self.pointee_value.GetChildAtIndex(index)
            if child.IsValid() and child.GetName() == name:
                return index
        return -1
