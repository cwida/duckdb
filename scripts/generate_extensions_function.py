import os
import csv
import re
import argparse
import glob
import sys
from typing import Set, Tuple, cast
import pathlib
from typing import NamedTuple
from typing import List, Dict
import json

os.chdir(os.path.join(os.path.dirname(__file__), '..'))

# Example usage:

parser = argparse.ArgumentParser(description='Generates/Validates extension_functions.hpp file')

parser.add_argument(
    '--validate',
    action=argparse.BooleanOptionalAction,
    help='If set will validate that extension_entries.hpp is up to date, otherwise it generates the extension_functions.hpp file.',
)
parser.add_argument(
    '--extension_repository',
    action='store',
    help="The repository to look for the '**/<extension>.duckdb_extension' files",
    default='build/release/repository',
)
parser.add_argument(
    '--shell',
    action='store',
    help="Path to the DuckDB shell",
    default='build/release/duckdb',
)
parser.add_argument(
    '--extensions',
    action='store',
    help="Comma separated list of extensions - if not provided this is read from the extension configuration",
    default='',
)

args = parser.parse_args()

EXTENSIONS_PATH = os.path.join("build", "extension_configuration", "extensions.csv")
DUCKDB_PATH = os.path.join(*args.shell.split('/'))
HEADER_PATH = os.path.join("src", "include", "duckdb", "main", "extension_entries.hpp")

EXTENSION_DEPENDENCIES = {
    'ducklake': [
        'parquet',
    ],
    'glue': [
        'avro',
        'json',
        'parquet',
    ],
    'iceberg': [
        'avro',
        'parquet',
    ],
}

from enum import Enum


class CatalogType(str, Enum):
    SCALAR = "CatalogType::SCALAR_FUNCTION_ENTRY"
    TABLE = "CatalogType::TABLE_FUNCTION_ENTRY"
    AGGREGATE = "CatalogType::AGGREGATE_FUNCTION_ENTRY"
    PRAGMA = "CatalogType::PRAGMA_FUNCTION_ENTRY"
    MACRO = "CatalogType::MACRO_ENTRY"
    TABLE_MACRO = "CatalogType::TABLE_MACRO_ENTRY"


parameter_type_map = {"TIMESTAMP WITH TIME ZONE": "TIMESTAMPTZ", "TIME WITH TIME ZONE": "TIMETZ"}


def log(message: str):
    print(message, file=sys.stderr, flush=True)


def catalog_type_from_type(catalog_type: str) -> CatalogType:
    TYPE_MAP = {
        CatalogType.SCALAR.value: CatalogType.SCALAR,
        CatalogType.TABLE.value: CatalogType.TABLE,
        CatalogType.AGGREGATE.value: CatalogType.AGGREGATE,
        CatalogType.PRAGMA.value: CatalogType.PRAGMA,
        CatalogType.MACRO.value: CatalogType.MACRO,
        CatalogType.TABLE_MACRO.value: CatalogType.TABLE_MACRO,
    }
    if catalog_type not in TYPE_MAP:
        raise Exception(f"Unrecognized function type: '{catalog_type}'")
    return TYPE_MAP[catalog_type]


def catalog_type_from_string(catalog_type: str) -> CatalogType:
    TYPE_MAP = {
        CatalogType.SCALAR.name.lower(): CatalogType.SCALAR,
        CatalogType.TABLE.name.lower(): CatalogType.TABLE,
        CatalogType.AGGREGATE.name.lower(): CatalogType.AGGREGATE,
        CatalogType.PRAGMA.name.lower(): CatalogType.PRAGMA,
        CatalogType.MACRO.name.lower(): CatalogType.MACRO,
        CatalogType.TABLE_MACRO.name.lower(): CatalogType.TABLE_MACRO,
    }
    if catalog_type not in TYPE_MAP:
        raise Exception(f"Unrecognized function type: '{catalog_type}'")
    return TYPE_MAP[catalog_type]


def parse_records(text):
    records = []  # Will hold all parsed records
    current_record = []  # Holds items for the current record
    current_item = []  # Accumulates characters for the current item
    in_quote = False  # True if we're inside a double-quoted string
    inside_braces = False  # True if we're inside a { ... } block

    for char in text:
        if char == '"':
            # Toggle the quote state and always include the quote.
            in_quote = not in_quote
        elif char == '{' and not in_quote:
            # Start of a new record.
            inside_braces = True
            # Reset any previous record state.
            current_record = []
            current_item = []
        elif char == '}' and not in_quote and inside_braces:
            # End of the current record.
            token = ''.join(current_item).strip()
            if token:
                current_record.append(token)
            records.append(current_record)
            # Reset state for subsequent records.
            current_record = []
            current_item = []
            inside_braces = False
        elif char == ',' and not in_quote and inside_braces:
            # A comma outside quotes indicates the end of the current item.
            token = ''.join(current_item).strip()
            if token:
                current_record.append(token)
            current_item = []
        else:
            # Otherwise, just add the character if we're inside braces.
            if inside_braces:
                current_item.append(char)
    return records


class LogicalType(NamedTuple):
    type: str


class Function(NamedTuple):
    name: str
    type: CatalogType


class FunctionOverload(NamedTuple):
    name: str
    type: CatalogType
    parameters: Tuple
    return_type: LogicalType


class ExtensionFunctionOverload(NamedTuple):
    extension: str
    name: str
    type: CatalogType
    parameters: Tuple
    return_type: LogicalType

    @staticmethod
    def create_map(input: List[Tuple[str, str, str, str]]) -> Dict[Function, List["ExtensionFunctionOverload"]]:
        output: Dict[Function, List["ExtensionFunctionOverload"]] = {}
        for x in input:
            function = Function(x[0], catalog_type_from_type(x[2]))
            # parse the signature
            signature = x[3]
            splits = signature.split('>')
            return_type = LogicalType(splits[1])
            parameters = [LogicalType(param) for param in splits[0][1:-1].split(',')]
            extension_function = ExtensionFunctionOverload(x[1], function.name, function.type, parameters, return_type)
            if function not in output:
                output[function] = []
            output[function].append(extension_function)
        return output


class ExtensionFunction(NamedTuple):
    extension: str
    name: str
    type: CatalogType

    @staticmethod
    def create_map(input: List[Tuple[str, str, str]]) -> Dict[Function, "ExtensionFunction"]:
        output: Dict[Function, "ExtensionFunction"] = {}
        for x in input:
            key = Function(x[0], catalog_type_from_type(x[2]))
            output[key] = ExtensionFunction(x[1], key.name, key.type)
        return output


class ExtensionSetting(NamedTuple):
    extension: str
    name: str

    @staticmethod
    def create_map(input: List[Tuple[str, str]]) -> Dict[str, "ExtensionSetting"]:
        output: Dict[str, "ExtensionSetting"] = {}
        for x in input:
            output[x[0]] = ExtensionSetting(x[1], x[0])
        return output


class ExtensionSecretType(NamedTuple):
    extension: str
    name: str

    @staticmethod
    def create_map(input: List[Tuple[str, str]]) -> Dict[str, "ExtensionSecretType"]:
        output: Dict[str, "ExtensionSecretType"] = {}
        for x in input:
            output[x[0]] = ExtensionSecretType(x[1], x[0])
        return output


class ExtensionCopyFunction(NamedTuple):
    extension: str
    name: str

    @staticmethod
    def create_map(input: List[Tuple[str, str]]) -> Dict[str, "ExtensionCopyFunction"]:
        output: Dict[str, "ExtensionCopyFunction"] = {}
        for x in input:
            output[x[0]] = ExtensionCopyFunction(x[1], x[0])
        return output


class ExtensionType(NamedTuple):
    extension: str
    name: str

    @staticmethod
    def create_map(input: List[Tuple[str, str]]) -> Dict[str, "ExtensionType"]:
        output: Dict[str, "ExtensionType"] = {}
        for x in input:
            output[x[0]] = ExtensionType(x[1], x[0])
        return output


class ParsedEntries:
    def __init__(self, file_path):
        self.path = file_path
        self.functions = {}
        self.function_overloads = {}
        self.settings = {}
        self.secret_types = {}
        self.types = {}
        self.copy_functions = {}

        file = open(file_path, 'r')
        file_blob = file.read()

        # Get the extension functions
        ext_functions_file_blob = get_slice_of_file("EXTENSION_FUNCTIONS", file_blob)
        res = parse_records(ext_functions_file_blob)
        res = [(x[0], x[1], x[2]) for x in res]
        self.functions = ExtensionFunction.create_map(res)

        # Get the extension function overloads
        ext_function_overloads_file_blob = get_slice_of_file("EXTENSION_FUNCTION_OVERLOADS", file_blob)
        res = parse_records(ext_function_overloads_file_blob)
        res = [(x[0], x[1], x[2], x[3]) for x in res]
        self.function_overloads = ExtensionFunctionOverload.create_map(res)

        # Get the extension settings
        ext_settings_file_blob = get_slice_of_file("EXTENSION_SETTINGS", file_blob)
        res = parse_records(ext_settings_file_blob)
        res = [(x[0], x[1]) for x in res]
        self.settings = ExtensionSetting.create_map(res)

        # Get the extension secret types
        ext_secret_types_file_blob = get_slice_of_file("EXTENSION_SECRET_TYPES", file_blob)
        res = parse_records(ext_secret_types_file_blob)
        res = [(x[0], x[1]) for x in res]
        self.secret_types = ExtensionSecretType.create_map(res)

        # Get the extension types
        ext_copy_functions_blob = get_slice_of_file("EXTENSION_COPY_FUNCTIONS", file_blob)
        res = parse_records(ext_copy_functions_blob)
        res = [(x[0], x[1]) for x in res]
        self.copy_functions = ExtensionCopyFunction.create_map(res)

        # Get the extension types
        ext_types_file_blob = get_slice_of_file("EXTENSION_TYPES", file_blob)
        res = parse_records(ext_types_file_blob)
        res = [(x[0], x[1]) for x in res]
        self.types = ExtensionType.create_map(res)

    def strip_unloaded_extensions(self, extensions: List[str], functions):
        return [x for x in functions if x.extension not in extensions]

    def filter_entries(self, extensions: List[str]):
        self.functions = {k: v for k, v in self.functions.items() if v.extension not in extensions}
        self.function_overloads = {
            k: self.strip_unloaded_extensions(extensions, v)
            for k, v in self.function_overloads.items()
            if len(self.strip_unloaded_extensions(extensions, v)) > 0
        }
        self.copy_functions = {k: v for k, v in self.copy_functions.items() if v.extension not in extensions}
        self.settings = {k: v for k, v in self.settings.items() if v.extension not in extensions}
        self.secret_types = {k: v for k, v in self.secret_types.items() if v.extension not in extensions}
        self.types = {k: v for k, v in self.types.items() if v.extension not in extensions}


def check_prerequisites():
    if not os.path.isfile(DUCKDB_PATH):
        log(f"{DUCKDB_PATH} not found")
        log("please run 'GENERATE_EXTENSION_ENTRIES=1 BUILD_ALL_EXT=1 make release'")
        exit(1)
    if len(args.extensions) == 0 and not os.path.isfile(EXTENSIONS_PATH):
        log(f"{EXTENSIONS_PATH} not found and --extensions it not set")
        log("Either:")
        log("* run 'GENERATE_EXTENSION_ENTRIES=1 BUILD_ALL_EXT=1 make release'")
        log("* Specify a comma separated list of extensions using --extensions")
        exit(1)
    if not os.path.isdir(args.extension_repository):
        log(f"provided --extension_repository '{args.extension_repository}' is not a valid directory")
        exit(1)


# Parses the extension config files for which extension names there are to be expected
def get_extension_names() -> List[str]:
    if len(args.extensions) > 0:
        return args.extensions.split(',')
    extension_names = []
    with open(EXTENSIONS_PATH) as f:
        # Skip the csv header
        next(f)
        for line in f:
            extension_name = line.split(',')[0].rstrip()
            extension_names.append(extension_name)
    return extension_names


def get_query(sql_query, load_query) -> list:
    # Optionally perform a LOAD of an extension
    # Then perform a SQL query, fetch the output
    import csv
    import io

    query = f'{DUCKDB_PATH} -unsigned -csv -c "{load_query}{sql_query}" '
    query_result = os.popen(query).read()
    f = io.StringIO(query_result)
    reader = csv.reader(f)
    header = next(reader)
    result = []
    for line in reader:
        result_obj = {}
        for i in range(len(header)):
            result_obj[header[i]] = line[i]
        result.append(result_obj)
    return result


def transform_parameter(parameter) -> LogicalType:
    if parameter is None:
        return LogicalType("INVALID")
    parameter = parameter.upper()
    if parameter.endswith('[]'):
        return LogicalType(transform_parameter(parameter[0 : len(parameter) - 2]).type + '[]')
    if parameter in parameter_type_map:
        return LogicalType(parameter_type_map[parameter])
    return LogicalType(parameter)


def transform_parameters(parameters) -> FunctionOverload:
    parameters = [x for x in parameters.lstrip('[').rstrip(']').split(', ') if len(x) > 0]
    return tuple(transform_parameter(param) for param in parameters)


def get_functions(load="") -> (Set[Function], Dict[Function, List[FunctionOverload]]):
    GET_FUNCTIONS_QUERY = """
        select distinct
            function_name,
            function_type,
            parameter_types,
            return_type
        from duckdb_functions()
        where function_type <> 'window'
        ORDER BY function_name, function_type;
    """
    # ['name_1,type_1', ..., 'name_n,type_n']
    results = get_query(GET_FUNCTIONS_QUERY, load)

    functions = set()
    function_overloads = {}
    for func in results:
        function_name = func["function_name"].lower()
        function_type = func["function_type"]
        parameter_types = func["parameter_types"]
        return_type = func["return_type"]
        function_parameters = transform_parameters(parameter_types)
        function_return = transform_parameter(return_type)
        function = Function(function_name, catalog_type_from_string(function_type))
        function_overload = FunctionOverload(
            function_name, catalog_type_from_string(function_type), function_parameters, function_return
        )
        if function not in functions:
            functions.add(function)
            function_overloads[function] = [function_overload]
        else:
            function_overloads[function].append(function_overload)

    return (functions, function_overloads)


def get_settings(load="") -> Set[str]:
    # Debug and deprecated settings still need to trigger autoloading
    GET_SETTINGS_QUERY = """
        select distinct
            name
        from duckdb_settings(include_all := true);
    """
    settings = get_query(GET_SETTINGS_QUERY, load)
    res = set()
    for setting in settings:
        name = setting['name']
        res.add(name)
    return res


def get_secret_types(load="") -> Set[str]:
    GET_SECRET_TYPES_QUERY = """
        select distinct
            type
        from duckdb_secret_types();
    """
    secret_types = get_query(GET_SECRET_TYPES_QUERY, load)
    res = set()
    for secret_type in secret_types:
        type = secret_type['type']
        res.add(type)
    return res


class ExtensionData:
    def __init__(self):
        # Map of extension -> ExtensionFunction
        self.function_map: Dict[Function, ExtensionFunction] = {}
        # Map of extension -> ExtensionSetting
        self.settings_map: Dict[str, ExtensionSetting] = {}
        # Map of extension -> ExtensionSecretType
        self.secret_types_map: Dict[str, ExtensionSecretType] = {}
        # Map of function -> extension function overloads
        self.function_overloads: Dict[Function, List[ExtensionFunctionOverload]] = {}
        # All function overloads (also ones that will not be written to the file)
        self.all_function_overloads: Dict[Function, List[ExtensionFunctionOverload]] = {}

        self.base_settings: Set[str] = set()
        self.base_secret_types: Set[str] = set()
        self.base_functions: Set[Function] = set()

        self.extension_settings: Dict[str, Set[str]] = {}
        self.extension_secret_types: Dict[str, Set[str]] = {}
        self.extension_functions: Dict[str, Set[Function]] = {}

        self.added_extensions: Set[str] = set()

        # Map of extension -> extension_path
        self.extensions: Dict[str, str] = get_extension_path_map()

        self.stored_functions: Dict[str, List[Function]] = {
            'arrow': [Function("scan_arrow_ipc", CatalogType.TABLE), Function("to_arrow_ipc", CatalogType.TABLE)],
            'spatial': [],
        }
        self.stored_settings: Dict[str, List[str]] = {'arrow': [], 'spatial': []}

    def set_base(self):
        (functions, function_overloads) = get_functions()
        self.base_functions: Set[Function] = functions
        self.base_settings: Set[str] = get_settings()
        self.base_secret_types: Set[str] = get_secret_types()

    def add_entries(self, entries: ParsedEntries):
        self.function_map.update(entries.functions)
        self.function_overloads.update(entries.function_overloads)
        self.settings_map.update(entries.settings)
        self.secret_types_map.update(entries.secret_types)

    def load_dependencies(self, extension_name: str) -> str:
        if extension_name not in EXTENSION_DEPENDENCIES:
            return ''

        res = ''
        dependencies = EXTENSION_DEPENDENCIES[extension_name]
        for item in dependencies:
            if item not in self.extensions:
                log(f"Could not load extension '{extension_name}', dependency '{item}' is missing")
                exit(1)
            extension_path = self.extensions[item]
            log(f"Load {item} at {extension_path}")
            res += f"LOAD '{extension_path}';"
        return res

    def add_extension(self, extension_name: str):
        if extension_name in EXTENSION_DEPENDENCIES:
            for item in EXTENSION_DEPENDENCIES[extension_name]:
                if item not in self.added_extensions:
                    self.add_extension(item)

        if extension_name in self.extensions:
            # Perform a LOAD and add the added settings/functions/secret_types
            extension_path = self.extensions[extension_name]

            log(f"Load {extension_name} at {extension_path}")
            load = self.load_dependencies(extension_name)
            load += f"LOAD '{extension_path}';"

            (functions, function_overloads) = get_functions(load)
            extension_functions = list(functions)
            extension_settings = list(get_settings(load))
            extension_secret_types = list(get_secret_types(load))

            self.add_settings(extension_name, extension_settings)
            self.add_secret_types(extension_name, extension_secret_types)
            self.add_functions(extension_name, extension_functions, function_overloads)
        elif extension_name in self.stored_functions or extension_name in self.stored_settings:
            # Retrieve the list of settings/functions from our hardcoded list
            extension_functions = self.stored_functions[extension_name]
            extension_settings = self.stored_settings[extension_name]
            extension_secret_types = self.stored_secret_types[extension_name]

            log(f"Loading {extension_name} from stored functions: {extension_functions}")
            self.add_settings(extension_name, extension_settings)
            self.add_secret_types(extension_name, extension_secret_types)
            self.add_functions(extension_name, extension_functions)
        else:
            error = f"""Missing extension {extension_name} and not found in stored_functions/stored_settings/stored_secret_types
Please double check if '{args.extension_repository}' is the right location to look for ./**/*.duckdb_extension files"""
            log(error)
            exit(1)
        self.added_extensions.add(extension_name)

    def add_settings(self, extension_name: str, settings_list: List[str]):
        extension_name = extension_name.lower()

        base_settings = set()
        base_settings.update(self.base_settings)
        if extension_name in EXTENSION_DEPENDENCIES:
            dependencies = EXTENSION_DEPENDENCIES[extension_name]
            for item in dependencies:
                assert item in self.extension_settings
                base_settings.update(self.extension_settings[item])

        added_settings: Set[str] = set(settings_list) - base_settings

        self.extension_settings[extension_name] = added_settings

        settings_to_add: Dict[str, ExtensionSetting] = {}
        for setting in added_settings:
            setting_name = setting.lower()
            settings_to_add[setting_name] = ExtensionSetting(extension_name, setting_name)

        self.settings_map.update(settings_to_add)

    def add_secret_types(self, extension_name: str, secret_types_list: List[str]):
        extension_name = extension_name.lower()

        base_secret_types = set()
        base_secret_types.update(self.base_secret_types)
        if extension_name in EXTENSION_DEPENDENCIES:
            dependencies = EXTENSION_DEPENDENCIES[extension_name]
            for item in dependencies:
                assert item in self.extension_secret_types
                base_secret_types.update(self.extension_secret_types[item])

        added_secret_types: Set[str] = set(secret_types_list) - base_secret_types

        self.extension_secret_types[extension_name] = added_secret_types

        secret_types_to_add: Dict[str, ExtensionSecretType] = {}
        for secret_type in added_secret_types:
            secret_type_name = secret_type.lower()
            secret_types_to_add[secret_type_name] = ExtensionSecretType(extension_name, secret_type_name)

        self.secret_types_map.update(secret_types_to_add)

    def get_extension_overloads(
        self, extension_name: str, overloads: Dict[Function, List[FunctionOverload]]
    ) -> Dict[Function, List[ExtensionFunctionOverload]]:
        result = {}
        for function, function_overloads in overloads.items():
            extension_overloads = []
            for overload in function_overloads:
                extension_overloads.append(
                    ExtensionFunctionOverload(
                        extension_name, overload.name, overload.type, overload.parameters, overload.return_type
                    )
                )
            result[function] = extension_overloads
        return result

    def add_functions(
        self, extension_name: str, function_list: List[Function], overloads: Dict[Function, List[FunctionOverload]]
    ):
        extension_name = extension_name.lower()

        base_functions = set()
        base_functions.update(self.base_functions)
        if extension_name in EXTENSION_DEPENDENCIES:
            dependencies = EXTENSION_DEPENDENCIES[extension_name]
            for item in dependencies:
                assert item in self.extension_functions
                base_functions.update(self.extension_functions[item])

        overloads = self.get_extension_overloads(extension_name, overloads)
        added_functions: Set[Function] = set(function_list) - base_functions

        self.extension_functions[extension_name] = added_functions

        functions_to_add: Dict[Function, ExtensionFunction] = {}
        for function in added_functions:
            if function in self.function_overloads:
                # function is in overload map - add overloads
                self.function_overloads[function] += overloads[function]
            elif function in self.function_map:
                # function is in function map and we are trying to add it again
                # this means the function is present in multiple extensions
                # remove from function map, and add to overload map
                self.function_overloads[function] = self.all_function_overloads[function] + overloads[function]
                del self.function_map[function]
            else:
                functions_to_add[function] = ExtensionFunction(extension_name, function.name, function.type)

        self.all_function_overloads.update(overloads)
        self.function_map.update(functions_to_add)

    def validate(self):
        parsed_entries = ParsedEntries(HEADER_PATH)
        if self.function_map != parsed_entries.functions:
            log("Function map mismatches:")
            print_map_diff(self.function_map, parsed_entries.functions)
            exit(1)
        if self.settings_map != parsed_entries.settings:
            log("Settings map mismatches:")
            print_map_diff(self.settings_map, parsed_entries.settings)
            exit(1)
        if self.secret_types_map != parsed_entries.secret_types:
            log("SecretTypes map mismatches:")
            print_map_diff(self.secret_types_map, parsed_entries.secret_types)
            exit(1)

        log("All entries found: ")
        log(" > functions: " + str(len(parsed_entries.functions)))
        log(" > settings:  " + str(len(parsed_entries.settings)))
        log(" > secret_types:  " + str(len(parsed_entries.secret_types)))

    def verify_export(self):
        if len(self.function_map) == 0 or len(self.settings_map) == 0 or len(self.secret_types_map) == 0:
            log(
                """
The provided configuration produced an empty function map or empty settings map or empty secret types map
This is likely caused by building DuckDB with extensions linked in
"""
            )
            exit(1)

    def export_functions(self) -> str:
        result = """
static constexpr ExtensionFunctionEntry EXTENSION_FUNCTIONS[] = {\n"""
        sorted_function = sorted(self.function_map)

        for func in sorted_function:
            function: ExtensionFunction = self.function_map[func]
            result += "\t{"
            result += f'"{function.name}", "{function.extension}", {function.type.value}'
            result += "},\n"
        result += "}; // END_OF_EXTENSION_FUNCTIONS\n"
        return result

    def export_function_overloads(self) -> str:
        result = """
static constexpr ExtensionFunctionOverloadEntry EXTENSION_FUNCTION_OVERLOADS[] = {\n"""
        sorted_function = sorted(self.function_overloads)

        for func in sorted_function:
            overloads: List[ExtensionFunctionOverload] = sorted(self.function_overloads[func])
            for overload in overloads:
                result += "\t{"
                result += f'"{overload.name}", "{overload.extension}", {overload.type.value}, "'
                signature = "["
                signature += ",".join([parameter.type for parameter in overload.parameters])
                signature += "]>" + overload.return_type.type
                result += signature
                result += '"},\n'
        result += "}; // END_OF_EXTENSION_FUNCTION_OVERLOADS\n"
        return result

    def export_settings(self) -> str:
        result = """
static constexpr ExtensionEntry EXTENSION_SETTINGS[] = {\n"""
        sorted_settings = sorted(self.settings_map)

        for settings_name in sorted_settings:
            setting: ExtensionSetting = self.settings_map[settings_name]
            result += "\t{"
            result += f'"{settings_name.lower()}", "{setting.extension}"'
            result += "},\n"
        result += "}; // END_OF_EXTENSION_SETTINGS\n"
        return result

    def export_secret_types(self) -> str:
        result = """
static constexpr ExtensionEntry EXTENSION_SECRET_TYPES[] = {\n"""
        sorted_secret_types = sorted(self.secret_types_map)

        for secret_types_name in sorted_secret_types:
            secret_type: ExtensionSecretType = self.secret_types_map[secret_types_name]
            result += "\t{"
            result += f'"{secret_types_name.lower()}", "{secret_type.extension}"'
            result += "},\n"
        result += "}; // END_OF_EXTENSION_SECRET_TYPES\n"
        return result


# Get the slice of the file containing the var (assumes // END_OF_<varname> comment after var)
def get_slice_of_file(var_name, file_str):
    begin = file_str.find(var_name)
    end = file_str.find("END_OF_" + var_name)
    return file_str[begin:end]


def print_map_diff(d1, d2):
    s1 = sorted(set(d1.items()))
    s2 = sorted(set(d2.items()))

    diff1 = str(set(s1) - set(s2))
    diff2 = str(set(s2) - set(s1))
    log("Diff between maps: " + diff1 + "\n")
    log("Diff between maps: " + diff2 + "\n")


def get_extension_path_map() -> Dict[str, str]:
    extension_paths: Dict[str, str] = {}
    # extension_repository = pathlib.Path('../build/release/repository')
    extension_repository = args.extension_repository
    for location in glob.iglob(extension_repository + '/**/*.duckdb_extension', recursive=True):
        name, _ = os.path.splitext(os.path.basename(location))
        log(f"Located extension: {name} in path: '{location}'")
        extension_paths[name] = location
    return extension_paths


def write_header(data: ExtensionData):
    INCLUDE_HEADER = """//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension_entries.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include \"duckdb/common/unordered_map.hpp\"
#include \"duckdb/common/enums/catalog_type.hpp\"

// NOTE: this file is generated by scripts/generate_extensions_function.py.
// Example usage to refresh one extension (replace "icu" with the desired extension):
// GENERATE_EXTENSION_ENTRIES=1 make debug
// python3 scripts/generate_extensions_function.py --extensions icu --shell build/debug/duckdb --extension_repository build/debug/repository

// Check out the check-load-install-extensions  job in .github/workflows/LinuxRelease.yml for more details

namespace duckdb {

struct ExtensionEntry {
    char name[48];
    char extension[48];
};

struct ExtensionFunctionEntry {
    char name[48];
    char extension[48];
    CatalogType type;
};

struct ExtensionFunctionOverloadEntry {
    char name[48];
    char extension[48];
    CatalogType type;
    char signature[96];
};
"""

    INCLUDE_FOOTER = """
// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_COPY_FUNCTIONS[] = {
    {"parquet", "parquet"},
    {"json", "json"},
    {"geojson", "json"},
    {"avro", "avro"},
    {"iceberg", "iceberg"}
}; // END_OF_EXTENSION_COPY_FUNCTIONS

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_TYPES[] = {
    {"json", "json"},
    {"inet", "inet"},
    {"parquet_variant", "parquet"},
}; // END_OF_EXTENSION_TYPES

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_STORAGE_EXTENSIONS[] = {
    {"arn", "aws"},
}; // END_OF_EXTENSION_STORAGE_EXTENSIONS

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_COLLATIONS[] = {
    {"af", "icu"},    {"am", "icu"},    {"ar", "icu"},     {"ar_sa", "icu"}, {"as", "icu"},    {"az", "icu"},
    {"be", "icu"},    {"bg", "icu"},    {"bn", "icu"},     {"bo", "icu"},    {"br", "icu"},    {"bs", "icu"},
    {"ca", "icu"},    {"ceb", "icu"},   {"chr", "icu"},    {"cs", "icu"},    {"cy", "icu"},    {"da", "icu"},
    {"de", "icu"},    {"de_at", "icu"}, {"dsb", "icu"},    {"dz", "icu"},    {"ee", "icu"},    {"el", "icu"},
    {"en", "icu"},    {"en_us", "icu"}, {"eo", "icu"},     {"es", "icu"},    {"et", "icu"},    {"fa", "icu"},
    {"fa_af", "icu"}, {"ff", "icu"},    {"fi", "icu"},     {"fil", "icu"},   {"fo", "icu"},    {"fr", "icu"},
    {"fr_ca", "icu"}, {"fy", "icu"},    {"ga", "icu"},     {"gl", "icu"},    {"gu", "icu"},    {"ha", "icu"},
    {"haw", "icu"},   {"he", "icu"},    {"he_il", "icu"},  {"hi", "icu"},    {"hr", "icu"},    {"hsb", "icu"},
    {"hu", "icu"},    {"hy", "icu"},    {"id", "icu"},     {"id_id", "icu"}, {"ig", "icu"},    {"is", "icu"},
    {"it", "icu"},    {"ja", "icu"},    {"ka", "icu"},     {"kk", "icu"},    {"kl", "icu"},    {"km", "icu"},
    {"kn", "icu"},    {"ko", "icu"},    {"kok", "icu"},    {"ku", "icu"},    {"ky", "icu"},    {"lb", "icu"},
    {"lkt", "icu"},   {"ln", "icu"},    {"lo", "icu"},     {"lt", "icu"},    {"lv", "icu"},    {"mk", "icu"},
    {"ml", "icu"},    {"mn", "icu"},    {"mr", "icu"},     {"ms", "icu"},    {"mt", "icu"},    {"my", "icu"},
    {"nb", "icu"},    {"nb_no", "icu"}, {"ne", "icu"},     {"nl", "icu"},    {"nn", "icu"},    {"om", "icu"},
    {"or", "icu"},    {"pa", "icu"},    {"pa_in", "icu"},  {"pl", "icu"},    {"ps", "icu"},    {"pt", "icu"},
    {"ro", "icu"},    {"ru", "icu"},    {"sa", "icu"},     {"se", "icu"},    {"si", "icu"},    {"sk", "icu"},
    {"sl", "icu"},    {"smn", "icu"},   {"sq", "icu"},     {"sr", "icu"},    {"sr_ba", "icu"}, {"sr_me", "icu"},
    {"sr_rs", "icu"}, {"sv", "icu"},    {"sw", "icu"},     {"ta", "icu"},    {"te", "icu"},    {"th", "icu"},
    {"tk", "icu"},    {"to", "icu"},    {"tr", "icu"},     {"ug", "icu"},    {"uk", "icu"},    {"ur", "icu"},
    {"uz", "icu"},    {"vi", "icu"},    {"wae", "icu"},    {"wo", "icu"},    {"xh", "icu"},    {"yi", "icu"},
    {"yo", "icu"},    {"yue", "icu"},   {"yue_cn", "icu"}, {"zh", "icu"},    {"zh_cn", "icu"}, {"zh_hk", "icu"},
    {"zh_mo", "icu"}, {"zh_sg", "icu"}, {"zh_tw", "icu"},  {"zu", "icu"}}; // END_OF_EXTENSION_COLLATIONS

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_FILE_PREFIXES[] = {
     {"http://", "httpfs"}, {"https://", "httpfs"}, {"s3://", "httpfs"}, {"s3a://", "httpfs"}, {"s3n://", "httpfs"},
     {"gcs://", "httpfs"},  {"gs://", "httpfs"},    {"r2://", "httpfs"}, {"azure://", "azure"}, {"az://", "azure"},
     {"abfss://", "azure"}, {"hf://", "httpfs"}
}; // END_OF_EXTENSION_FILE_PREFIXES

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_FILE_POSTFIXES[] = {
    {".parquet", "parquet"},
    {".json", "json"},
    {".jsonl", "json"},
    {".ndjson", "json"},
    {".geojson", "json"},
    {".geojsonl", "json"},
    {".shp", "spatial"},
    {".gpkg", "spatial"},
    {".fgb", "spatial"},
    {".xlsx", "excel"},
    {".avro", "avro"},
}; // END_OF_EXTENSION_FILE_POSTFIXES

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_FILE_CONTAINS[] = {
    {".parquet?", "parquet"},
    {".json?", "json"},
    {".ndjson?", ".jsonl?"},
    {".jsonl?", ".ndjson?"}
}; // EXTENSION_FILE_CONTAINS

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_SECRET_PROVIDERS[] = {{"s3/config", "httpfs"},
                                                                {"gcs/config", "httpfs"},
                                                                {"r2/config", "httpfs"},
                                                                {"s3/credential_chain", "aws"},
                                                                {"gcs/credential_chain", "aws"},
                                                                {"r2/credential_chain", "aws"},
                                                                {"aws/credential_chain", "aws"},
                                                                {"rds/credential_chain", "aws"},
                                                                {"s3/none", "aws"},
                                                                {"aws/none", "aws"},
                                                                {"azure/access_token", "azure"},
                                                                {"azure/config", "azure"},
                                                                {"azure/credential_chain", "azure"},
                                                                {"azure/service_principal", "azure"},
                                                                {"huggingface/config", "httfps"},
                                                                {"huggingface/credential_chain", "httpfs"},
                                                                {"bearer/config", "httpfs"},
                                                                {"mysql/config", "mysql_scanner"},
                                                                {"postgres/config", "postgres_scanner"}
}; // EXTENSION_SECRET_PROVIDERS

// Note: these are currently hardcoded in scripts/generate_extensions_function.py
// TODO: automate by passing though to script via duckdb
static constexpr ExtensionEntry EXTENSION_LOG_TYPES[] = {
    {"DeltaKernel", "delta"}, {"DuckLakeMetadata", "ducklake"},         {"HTTPFSInfo", "httpfs"},
    {"Iceberg", "iceberg"},   {"PostgresQueryLog", "postgres_scanner"}, {"Quack", "quack"},
}; // END_OF_EXTENSION_LOG_TYPES

static constexpr const char *AUTOLOADABLE_EXTENSIONS[] = {
    "autocomplete",
    "avro",
    "aws",
    "azure",
    "core_functions",
    "delta",
    "ducklake",
    "encodings",
    "excel",
    "fts",
    "httpfs",
    "iceberg",
    "icu",
    "inet",
    "json",
    "legacy_parser",
    "motherduck",
    "mysql_scanner",
    "odbc_scanner",
    "parquet",
    "postgres_scanner",
    "quack",
    "sqlite_scanner",
    "sqlsmith",
    "tpcds",
    "tpch",
    "ui",
    "unity_catalog"
}; // END_OF_AUTOLOADABLE_EXTENSIONS

} // namespace duckdb"""

    data.verify_export()

    file = open(HEADER_PATH, 'w')
    file.write(INCLUDE_HEADER)

    exported_functions = data.export_functions()
    file.write(exported_functions)

    exported_overloads = data.export_function_overloads()
    file.write(exported_overloads)

    exported_settings = data.export_settings()
    file.write(exported_settings)

    exported_secret_types = data.export_secret_types()
    file.write(exported_secret_types)

    file.write(INCLUDE_FOOTER)
    file.close()


# Extensions that can be autoloaded, but are not buildable by DuckDB CI
HARDCODED_EXTENSION_FUNCTIONS = ExtensionFunction.create_map(
    [
        ("delta_scan", "delta", "CatalogType::TABLE_FUNCTION_ENTRY"),
    ]
)


def main():
    check_prerequisites()

    extension_names: List[str] = get_extension_names()

    extension_data = ExtensionData()
    # Collect the list of functions/settings without any extensions loaded
    extension_data.set_base()

    # TODO: add 'purge' option to ignore existing entries ??
    parsed_entries = ParsedEntries(HEADER_PATH)
    parsed_entries.filter_entries(extension_names)

    # Add the entries we parsed from the HEADER_PATH
    extension_data.add_entries(parsed_entries)

    for extension_name in extension_names:
        # For every extension, add the functions/settings added by the extension
        extension_data.add_extension(extension_name)

    # Add hardcoded extension entries (
    for key, value in HARDCODED_EXTENSION_FUNCTIONS.items():
        extension_data.function_map[key] = value

    if args.validate:
        extension_data.validate()
        return

    write_header(extension_data)


if __name__ == '__main__':
    main()
