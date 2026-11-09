import os
import csv
import re
import argparse
import glob

os.chdir(os.path.dirname(__file__))

# Dont generate serialization for these enums
blacklist = [
    "CMExpressionType",
    "RegexOptions",
    "Flags",
    "FlushState",
    "ReclaimMode",
    "PoolState",
    "ContainerType",
    "Type",
    "DictionaryAppendState",
    "DictFSSTMode",
    "DictFSSTCompressResult",
    "ComplexJSONType",
    "UnavailableReason",
    "VirtualColumnBindingType",
    "Slot",
    "State",
    "ScheduleMode",
    "MemoryUpdateMode",
    "SchedulePolicy",
    "BatchDrainMode",
    "AccountedWriteAdoption",
    "FileWritePublicationState",
    "IdleFilter",
    "IdentifierConversionPolicy",
    "CreateDirectoryMode",
    "MatchKind",
    "MatchRecognizeScope",
    "RemoveDirectoryMode",
    "CopyOutputOwnership",
    "CopyOutputPublicationState",
    "PendingTaskCountMode",
    "PartitionKeyTrackerState",
    "ClaimState",
    "MoveBufferResult",
    "CSVBufferResidency",
    "AppendAdmission",
    "AppendReservationState",
    "BufferedPushState",
    "CTEPipelineSelectionState",
    "ConsumerLifecycle",
    "ConsumerMode",
    "ConsumerReadState",
    "DataflowDependencyMode",
    "ExchangeLogEvent",
    "ExternalInputEventState",
    "MetaPipelineDependencyMode",
    "PipelineBroadcastExchangeCompletionMode",
    "PipelineBroadcastExchangeDirectPushState",
    "PipelineBroadcastExchangeLocalMode",
    "PipelineDependencyType",
    "PipelineInputChunkMode",
    "PendingBatchAdvanceState",
    "PipelineExternalInputCost",
    "PipelineExternalInputSupport",
    "PipelineSourceConsumption",
    "ProducerState",
    "ProjectionMode",
    "ReaderWakeMode",
    "RecursiveDependencyMode",
    "ReservationKind",
    "RowGroupBatchType",
    "RuntimeFilterCastMode",
    "SkippedTo",
    "SourceFinishNotificationState",
    "WatermarkState",
    "WriterWakeMode",
    # Internal to the Variant binary encoding; ParquetVariantNode::Kind is a nested enum,
    # which this script would emit unqualified.
    "Kind",
    "ParquetGroupKind",
    "VariantBasicType",
    "VariantPrimitiveType",
    # ClientContext::Retention, nested and private to the client context
    "Retention",
    # HTTPRetryBudget::RecordedOutcome is a nested enum of the HTTP retry budget,
    # which this script would emit unqualified.
    "RecordedOutcome",
    # PEGRuleId is generated from the grammar and ships its own PEGRuleIdToString/PEGRuleIdFromString
    "PEGRuleId",
]

enum_util_header_file = os.path.join("..", "src", "include", "duckdb", "common", "enum_util.hpp")
enum_util_source_file = os.path.join("..", "src", "common", "enum_util.cpp")

# Overrides conversions for the following enums:
overrides = {
    "LogicalTypeId": {
        "SQLNULL": "NULL",
        "TIMESTAMP_TZ": "TIMESTAMP WITH TIME ZONE",
        "TIME_TZ": "TIME WITH TIME ZONE",
        "TIMESTAMP_SEC": "TIMESTAMP_S",
        "TIMESTAMP_TZ_NS": "TIMESTAMPTZ_NS",
    },
    "JoinType": {"OUTER": "FULL"},
    "OrderType": {
        "ORDER_DEFAULT": ["ORDER_DEFAULT", "DEFAULT"],
        "DESCENDING": ["DESCENDING", "DESC"],
        "ASCENDING": ["ASCENDING", "ASC"],
    },
    "AllowParserOverride": {
        "DEFAULT_OVERRIDE": "DEFAULT",
        "FALLBACK_OVERRIDE": "FALLBACK",
        "STRICT_OVERRIDE": "STRICT",
    },
    "OrderByNullType": {
        "ORDER_DEFAULT": ["ORDER_DEFAULT", "DEFAULT"],
        "NULLS_FIRST": ["NULLS FIRST", "NULLS_FIRST"],
        "NULLS_LAST": ["NULLS LAST", "NULLS_LAST"],
    },
    "CheckpointAbort": {
        "NO_ABORT": "NONE",
        "DEBUG_ABORT_BEFORE_TRUNCATE": "BEFORE_TRUNCATE",
        "DEBUG_ABORT_BEFORE_HEADER": "BEFORE_HEADER",
        "DEBUG_ABORT_AFTER_FREE_LIST_WRITE": "AFTER_FREE_LIST_WRITE",
        "DEBUG_ABORT_BEFORE_WAL_FINISH": "BEFORE_WAL_FINISH",
        "DEBUG_ABORT_BEFORE_MOVING_RECOVERY": "BEFORE_MOVING_RECOVERY",
        "DEBUG_ABORT_BEFORE_DELETING_CHECKPOINT_WAL": "BEFORE_DELETING_CHECKPOINT_WAL",
        "DEBUG_ABORT_BEFORE_HEADER_NON_FATAL": "BEFORE_HEADER_NON_FATAL",
        "DEBUG_ABORT_IN_MEMORY_CHECKPOINT": "IN_MEMORY_CHECKPOINT",
        "DEBUG_ABORT_BEFORE_WAL_FLUSH": "BEFORE_WAL_FLUSH",
    },
    "SampleMethod": {"SYSTEM_SAMPLE": "System", "BERNOULLI_SAMPLE": "Bernoulli", "RESERVOIR_SAMPLE": "Reservoir"},
    "TableReferenceType": {"EMPTY_FROM": "EMPTY"},
    "LogLevel": {
        "LOG_TRACE": "TRACE",
        "LOG_DEBUG": "DEBUG",
        "LOG_INFO": "INFO",
        "LOG_WARNING": "WARNING",
        "LOG_ERROR": "ERROR",
        "LOG_FATAL": "FATAL",
    },
    "RequestType": {
        "GET_REQUEST": "GET",
        "PUT_REQUEST": "PUT",
        "HEAD_REQUEST": "HEAD",
        "DELETE_REQUEST": "DELETE",
        "POST_REQUEST": "POST",
        "OPTIONS_REQUEST": "OPTIONS",
    },
    "CompressionType": {
        "COMPRESSION_AUTO": "AUTO",
        "COMPRESSION_UNCOMPRESSED": "UNCOMPRESSED",
        "COMPRESSION_CONSTANT": "CONSTANT",
        "COMPRESSION_RLE": "RLE",
        "COMPRESSION_DICTIONARY": "DICTIONARY",
        "COMPRESSION_PFOR_DELTA": "PFOR",
        "COMPRESSION_BITPACKING": "BITPACKING",
        "COMPRESSION_FSST": "FSST",
        "COMPRESSION_CHIMP": "CHIMP",
        "COMPRESSION_PATAS": "PATAS",
        "COMPRESSION_ALP": "ALP",
        "COMPRESSION_ALPRD": "ALPRD",
        "COMPRESSION_ZSTD": "ZSTD",
        "COMPRESSION_ROARING": "ROARING",
        "COMPRESSION_EMPTY": "EMPTY",
        "COMPRESSION_DICT_FSST": "DICT_FSST",
    },
    "ArrowFormatVersion": {"V1_0": "1.0", "V1_1": "1.1", "V1_2": "1.2", "V1_3": "1.3", "V1_4": "1.4", "V1_5": "1.5"},
    "TriggerTiming": {"BEFORE": "BEFORE", "AFTER": "AFTER", "INSTEAD_OF": "INSTEAD OF"},
    "TriggerEventType": {"INSERT_EVENT": "INSERT", "DELETE_EVENT": "DELETE", "UPDATE_EVENT": "UPDATE"},
    "TriggerForEach": {"STATEMENT": "STATEMENT", "ROW": "ROW"},
}

# get all the headers
hpp_files = []
for root, dirs, files in os.walk(os.path.join("..", "src")):
    for file in files:
        # Dont include the generated header itself recursively
        if file == "enum_util.hpp":
            continue

        if file.endswith(".hpp"):
            hpp_files.append(os.path.join(root, file))


def remove_prefix(str, prefix):
    if str.startswith(prefix):
        return str[len(prefix) :]
    return str


# get all the enum classes
enums = []
enum_paths = []
enum_path_set = set()

for hpp_file in hpp_files:
    with open(hpp_file, "r") as f:
        text = f.read()
        for res in re.finditer(r"enum class (\w*)\s*:\s*(\w*)\s*{((?:\s*[^}])*)}", text, re.MULTILINE):
            file_path = remove_prefix(os.path.relpath(hpp_file, os.path.join("..", "src")), "include/")
            enum_name = res.group(1)

            if enum_name in blacklist:
                print(f"Skipping {enum_name} because it is blacklisted")
                continue

            enum_type = res.group(2)

            enum_members = []
            # Capture All members: \w+(\s*\=\s*-?\w*)?
            # group one is the member name
            # group two is the member value
            # First clean group from comments
            s = res.group(3)
            s = re.sub(r"\/\/.*", "", s)
            s = re.sub(r"\/\*.*\*\/", "", s)

            enum_values = {}
            for member in re.finditer(r"(\w+)(\s*\=\s*-?\w*)?", s):
                key = member.group(1)
                strings = [key]
                if enum_name in overrides and key in overrides[enum_name]:
                    override = overrides[enum_name][key]
                    if isinstance(override, list):
                        print(f"Overriding {enum_name}::{key} to one of {override}")
                        strings = override
                    else:
                        print(f"Overriding {enum_name}::{key} to {override}")
                        strings = [override]

                if member.group(2):
                    # If the member has a value, make sure it isnt already covered by another member
                    # If it is, we cant do anything else than ignore it
                    value = remove_prefix(member.group(2).strip(), "=").strip()
                    if value not in enum_values and value not in dict(enum_members):
                        enum_members.append((key, strings))
                    else:
                        print(f"Skipping {enum_name}::{key} because it has a duplicate value {value}")
                else:
                    enum_members.append((key, strings))

            if not file_path in enum_path_set:
                enum_path_set.add(file_path)
                enum_paths.append(file_path)

            enums.append((enum_name, enum_type, enum_members))

enum_paths.sort()
enums.sort(key=lambda x: x[0])

header = """//-------------------------------------------------------------------------
// This file is automatically generated by scripts/generate_enum_util.py
// Do not edit this file manually, your changes will be overwritten
// If you want to exclude an enum from serialization, add it to the blacklist in the script
//
// Note: The generated code will only work properly if the enum is a top level item in the duckdb namespace
// If the enum is nested in a class, or in another namespace, the generated code will not compile.
// You should move the enum to the duckdb namespace, manually write a specialization or add it to the blacklist
//-------------------------------------------------------------------------\n\n
"""

# Write the enum util header
with open(enum_util_header_file, "w") as f:
    f.write(header)

    f.write('#pragma once\n\n')
    f.write('#include <stdint.h>\n')
    f.write('#include "duckdb/common/string.hpp"\n\n')

    f.write("namespace duckdb {\n\n")

    f.write(
        """struct EnumUtil {
    // String -> Enum
    template <class T>
    static T FromString(const char *value) = delete;

    template <class T>
    static T FromString(const string &value) { return FromString<T>(value.c_str()); }

    // Enum -> String
    template <class T>
    static const char *ToChars(T value) = delete;

    template <class T>
    static string ToString(T value) { return string(ToChars<T>(value)); }
};\n\n"""
    )

    # Forward declare all enums
    for enum_name, enum_type, _ in enums:
        f.write(f"enum class {enum_name} : {enum_type};\n\n")
    f.write("\n")

    # Forward declare all enum serialization functions
    for enum_name, enum_type, _ in enums:
        f.write(f"template<>\nconst char* EnumUtil::ToChars<{enum_name}>({enum_name} value);\n\n")
    f.write("\n")

    # Forward declare all enum dserialization functions
    for enum_name, enum_type, _ in enums:
        f.write(f"template<>\n{enum_name} EnumUtil::FromString<{enum_name}>(const char *value);\n\n")
    f.write("\n")

    f.write("}\n")


with open(enum_util_source_file, "w") as f:
    f.write(header)

    f.write('#include "duckdb/common/enum_util.hpp"\n')

    # Write the includes
    for enum_path in enum_paths:
        f.write(f'#include "{enum_path}"\n')
    f.write("\n")

    f.write("namespace duckdb {\n\n")

    for enum_name, enum_type, enum_members in enums:
        enum_string_array = "Get" + enum_name + "Values()"
        # Write the enum from string
        f.write(f"const StringUtil::EnumStringLiteral *{enum_string_array} {{\n")
        f.write(f"\tstatic constexpr StringUtil::EnumStringLiteral values[] {{\n")
        member_count = 0
        for key, strings in enum_members:
            for str_val in strings:
                if member_count != 0:
                    f.write(",\n")
                f.write(f"\t\t{{ static_cast<uint32_t>({enum_name}::{key}), \"{str_val}\" }}")
                member_count += 1
        f.write("\n\t};")
        f.write("\n\treturn values;")
        f.write("\n}\n\n")
        # An enum whose last member is ENUM_SIZE counts itself: the sentinel is not one of the values,
        # and appending a member no longer changes the generated count.
        last_member = enum_members[-1][0] if enum_members else ""
        if last_member == "ENUM_SIZE":
            member_count = f"static_cast<uint32_t>({enum_name}::{last_member})"
        f.write(f"template<>\nconst char* EnumUtil::ToChars<{enum_name}>({enum_name} value) {{\n")
        f.write(
            f"\treturn StringUtil::EnumToString({enum_string_array}, {member_count}, \"{enum_name}\", static_cast<uint32_t>(value));\n"
        )
        f.write("}\n\n")

        # Write the string to enum
        f.write(f"template<>\n{enum_name} EnumUtil::FromString<{enum_name}>(const char *value) {{\n")
        f.write(
            f"\treturn static_cast<{enum_name}>(StringUtil::StringToEnum({enum_string_array}, {member_count}, \"{enum_name}\", value));"
        )
        f.write("\n}\n\n")

    f.write("}\n\n")
