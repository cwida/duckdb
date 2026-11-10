#include "duckdb/common/exception.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/exception/list.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/expression.hpp"

#ifdef DUCKDB_CRASH_ON_ASSERT
#include "duckdb/common/printer.hpp"
#include <stdio.h>
#include <stdlib.h>
#endif
#include "duckdb/common/stacktrace.hpp"

namespace duckdb {

Exception::Exception(ExceptionType exception_type, const string &message)
    : std::runtime_error(ToJSON(exception_type, message)) {
}

Exception::Exception(const unordered_map<string, string> &extra_info, ExceptionType exception_type,
                     const string &message)
    : std::runtime_error(ToJSON(extra_info, exception_type, message)) {
}

string Exception::ToJSON(ExceptionType type, const string &message) {
	unordered_map<string, string> extra_info;
	return ToJSON(extra_info, type, message);
}

string Exception::ToJSON(const unordered_map<string, string> &extra_info, ExceptionType type, const string &message) {
#ifndef DUCKDB_DEBUG_STACKTRACE
	// by default we only enable stack traces for internal exceptions
	if (type == ExceptionType::INTERNAL)
#endif
	{
		auto extended_extra_info = extra_info;
		// We only want to add the stack trace pointers if they are not already present, otherwise the original
		// stack traces are lost
		if (extended_extra_info.find("stack_trace_pointers") == extended_extra_info.end() &&
		    extended_extra_info.find("stack_trace") == extended_extra_info.end()) {
			extended_extra_info["stack_trace_pointers"] = StackTrace::GetStacktracePointers();
		}
		return StringUtil::ExceptionToJSONMap(type, message, extended_extra_info);
	}
	return StringUtil::ExceptionToJSONMap(type, message, extra_info);
}

bool Exception::UncaughtException() {
#if __cplusplus >= 201703L
	return std::uncaught_exceptions() > 0;
#else
	return std::uncaught_exception();
#endif
}

bool Exception::InvalidatesTransaction(ExceptionType exception_type) {
	switch (exception_type) {
	case ExceptionType::BINDER:
	case ExceptionType::CATALOG:
	case ExceptionType::CONNECTION:
	case ExceptionType::PARAMETER_NOT_ALLOWED:
	case ExceptionType::PARSER:
	case ExceptionType::PERMISSION:
	case ExceptionType::RESOURCE_IN_USE:
		return false;
	default:
		return true;
	}
}

bool Exception::InvalidatesDatabase(ExceptionType exception_type) {
	switch (exception_type) {
	case ExceptionType::FATAL:
		return true;
	default:
		return false;
	}
}

string Exception::GetStackTrace(idx_t max_depth) {
	return StackTrace::GetStackTrace(max_depth);
}

string Exception::ConstructMessageRecursive(const string &msg, std::vector<ExceptionFormatValue> &values) {
#ifdef DEBUG
	// Verify that we have the required amount of values for the message
	idx_t parameter_count = 0;
	for (idx_t i = 0; i + 1 < msg.size(); i++) {
		if (msg[i] != '%') {
			continue;
		}
		if (msg[i + 1] == '%') {
			i++;
			continue;
		}
		parameter_count++;
	}
	if (parameter_count != values.size()) {
		throw InternalException("Primary exception: %s\nSecondary exception in ConstructMessageRecursive: Expected %d "
		                        "parameters, received %d",
		                        msg.c_str(), parameter_count, values.size());
	}

#endif
	return ExceptionFormatValue::Format(msg, values);
}

struct ExceptionEntry {
	ExceptionType type;
	char text[48];
};

static constexpr ExceptionEntry EXCEPTION_MAP[] = {{ExceptionType::INVALID, "Invalid"},
                                                   {ExceptionType::OUT_OF_RANGE, "Out of Range"},
                                                   {ExceptionType::CONVERSION, "Conversion"},
                                                   {ExceptionType::UNKNOWN_TYPE, "Unknown Type"},
                                                   {ExceptionType::DECIMAL, "Decimal"},
                                                   {ExceptionType::MISMATCH_TYPE, "Mismatch Type"},
                                                   {ExceptionType::DIVIDE_BY_ZERO, "Divide by Zero"},
                                                   {ExceptionType::OBJECT_SIZE, "Object Size"},
                                                   {ExceptionType::INVALID_TYPE, "Invalid type"},
                                                   {ExceptionType::SERIALIZATION, "Serialization"},
                                                   {ExceptionType::TRANSACTION, "TransactionContext"},
                                                   {ExceptionType::NOT_IMPLEMENTED, "Not implemented"},
                                                   {ExceptionType::EXPRESSION, "Expression"},
                                                   {ExceptionType::CATALOG, "Catalog"},
                                                   {ExceptionType::PARSER, "Parser"},
                                                   {ExceptionType::BINDER, "Binder"},
                                                   {ExceptionType::PLANNER, "Planner"},
                                                   {ExceptionType::SCHEDULER, "Scheduler"},
                                                   {ExceptionType::EXECUTOR, "Executor"},
                                                   {ExceptionType::CONSTRAINT, "Constraint"},
                                                   {ExceptionType::INDEX, "Index"},
                                                   {ExceptionType::STAT, "Stat"},
                                                   {ExceptionType::CONNECTION, "Connection"},
                                                   {ExceptionType::SYNTAX, "Syntax"},
                                                   {ExceptionType::SETTINGS, "Settings"},
                                                   {ExceptionType::OPTIMIZER, "Optimizer"},
                                                   {ExceptionType::NULL_POINTER, "NullPointer"},
                                                   {ExceptionType::IO, "IO"},
                                                   {ExceptionType::INTERRUPT, "INTERRUPT"},
                                                   {ExceptionType::FATAL, "FATAL"},
                                                   {ExceptionType::INTERNAL, "INTERNAL"},
                                                   {ExceptionType::INVALID_INPUT, "Invalid Input"},
                                                   {ExceptionType::OUT_OF_MEMORY, "Out of Memory"},
                                                   {ExceptionType::PERMISSION, "Permission"},
                                                   {ExceptionType::PARAMETER_NOT_RESOLVED, "Parameter Not Resolved"},
                                                   {ExceptionType::PARAMETER_NOT_ALLOWED, "Parameter Not Allowed"},
                                                   {ExceptionType::DEPENDENCY, "Dependency"},
                                                   {ExceptionType::MISSING_EXTENSION, "Missing Extension"},
                                                   {ExceptionType::HTTP, "HTTP"},
                                                   {ExceptionType::AUTOLOAD, "Extension Autoloading"},
                                                   {ExceptionType::SEQUENCE, "Sequence"},
                                                   {ExceptionType::INVALID_CONFIGURATION, "Invalid Configuration"},
                                                   {ExceptionType::DATA_CORRUPTION, "Data Corruption"},
                                                   {ExceptionType::RESOURCE_IN_USE, "Resource In Use"}};

string Exception::ExceptionTypeToString(ExceptionType type) {
	for (auto &e : EXCEPTION_MAP) {
		if (e.type == type) {
			return e.text;
		}
	}
	return "Unknown";
}

ExceptionType Exception::StringToExceptionType(const string &type) {
	for (auto &e : EXCEPTION_MAP) {
		if (e.text == type) {
			return e.type;
		}
	}
	return ExceptionType::INVALID;
}

unordered_map<string, string> Exception::InitializeExtraInfo(const Expression &expr) {
	return InitializeExtraInfo(expr.GetQueryLocation());
}

unordered_map<string, string> Exception::InitializeExtraInfo(const ParsedExpression &expr) {
	return InitializeExtraInfo(expr.GetQueryLocation());
}

unordered_map<string, string> Exception::InitializeExtraInfo(const QueryErrorContext &error_context) {
	return InitializeExtraInfo(error_context.query_location);
}

unordered_map<string, string> Exception::InitializeExtraInfo(const TableRef &ref) {
	return InitializeExtraInfo(ref.query_location);
}

unordered_map<string, string> Exception::InitializeExtraInfo(QueryLocation error_location) {
	unordered_map<string, string> result;
	SetQueryLocation(error_location, result);
	return result;
}

bool Exception::IsExecutionError(ExceptionType type) {
	switch (type) {
	case ExceptionType::INVALID_INPUT:
	case ExceptionType::OUT_OF_RANGE:
	case ExceptionType::CONVERSION:
	case ExceptionType::PARAMETER_NOT_ALLOWED:
		return true;
	default:
		return false;
	}
}

unordered_map<string, string> Exception::InitializeExtraInfo(const string &subtype, QueryLocation error_location) {
	unordered_map<string, string> result;
	result["error_subtype"] = subtype;
	SetQueryLocation(error_location, result);
	return result;
}

void Exception::SetQueryLocation(QueryLocation error_location, unordered_map<string, string> &extra_info) {
	if (error_location.IsValid()) {
		// "position" is the start offset (kept for backwards compatibility with existing consumers)
		extra_info["position"] = to_string(error_location.offset);
		// "location" is the structured [start, length] source range
		extra_info["location"] = "[" + to_string(error_location.offset) + "," + to_string(error_location.length) + "]";
	}
}

InvalidTypeException::InvalidTypeException(PhysicalType type, const string &msg)
    : Exception(ExceptionType::INVALID_TYPE, "Invalid Type [" + TypeIdToString(type) + "]: " + msg) {
}

InvalidTypeException::InvalidTypeException(const LogicalType &type, const string &msg)
    : Exception(ExceptionType::INVALID_TYPE, "Invalid Type [" + type.ToString() + "]: " + msg) {
}

InvalidTypeException::InvalidTypeException(const string &msg) : Exception(ExceptionType::INVALID_TYPE, msg) {
}

TypeMismatchException::TypeMismatchException(const PhysicalType type_1, const PhysicalType type_2, const string &msg)
    : Exception(ExceptionType::MISMATCH_TYPE,
                "Type " + TypeIdToString(type_1) + " does not match with " + TypeIdToString(type_2) + ". " + msg) {
}

TypeMismatchException::TypeMismatchException(const LogicalType &type_1, const LogicalType &type_2, const string &msg)
    : TypeMismatchException(QueryLocation(), type_1, type_2, msg) {
}

TypeMismatchException::TypeMismatchException(QueryLocation error_location, const LogicalType &type_1,
                                             const LogicalType &type_2, const string &msg)
    : Exception(Exception::InitializeExtraInfo(error_location), ExceptionType::MISMATCH_TYPE,
                "Type " + type_1.ToString() + " does not match with " + type_2.ToString() + ". " + msg) {
}

TypeMismatchException::TypeMismatchException(const string &msg) : Exception(ExceptionType::MISMATCH_TYPE, msg) {
}

TransactionException::TransactionException(const string &msg) : Exception(ExceptionType::TRANSACTION, msg) {
}

NotImplementedException::NotImplementedException(const string &msg) : Exception(ExceptionType::NOT_IMPLEMENTED, msg) {
}

OutOfRangeException::OutOfRangeException(const string &msg) : Exception(ExceptionType::OUT_OF_RANGE, msg) {
}

OutOfRangeException::OutOfRangeException(const int64_t value, const PhysicalType orig_type, const PhysicalType new_type)
    : Exception(ExceptionType::OUT_OF_RANGE, "Type " + TypeIdToString(orig_type) + " with value " +
                                                 to_string((intmax_t)value) +
                                                 " can't be cast because the value is out of range "
                                                 "for the destination type " +
                                                 TypeIdToString(new_type)) {
}

OutOfRangeException::OutOfRangeException(const double value, const PhysicalType orig_type, const PhysicalType new_type)
    : Exception(ExceptionType::OUT_OF_RANGE, "Type " + TypeIdToString(orig_type) + " with value " +
                                                 StringUtil::ToString(value) +
                                                 " can't be cast because the value is out of range "
                                                 "for the destination type " +
                                                 TypeIdToString(new_type)) {
}

OutOfRangeException::OutOfRangeException(const hugeint_t value, const PhysicalType orig_type,
                                         const PhysicalType new_type)
    : Exception(ExceptionType::OUT_OF_RANGE, "Type " + TypeIdToString(orig_type) + " with value " + value.ToString() +
                                                 " can't be cast because the value is out of range "
                                                 "for the destination type " +
                                                 TypeIdToString(new_type)) {
}

OutOfRangeException::OutOfRangeException(const PhysicalType var_type, const idx_t length)
    : Exception(ExceptionType::OUT_OF_RANGE,
                "The value is too long to fit into type " + TypeIdToString(var_type) + "(" + to_string(length) + ")") {
}

ConnectionException::ConnectionException(const string &msg) : Exception(ExceptionType::CONNECTION, msg) {
}

PermissionException::PermissionException(const string &msg) : Exception(ExceptionType::PERMISSION, msg) {
}

SyntaxException::SyntaxException(const string &msg) : Exception(ExceptionType::SYNTAX, msg) {
}

ExecutorException::ExecutorException(const string &msg) : Exception(ExceptionType::EXECUTOR, msg) {
}

ConstraintException::ConstraintException(const string &msg) : Exception(ExceptionType::CONSTRAINT, msg) {
}

DependencyException::DependencyException(const string &msg) : Exception(ExceptionType::DEPENDENCY, msg) {
}

IOException::IOException(const string &msg) : Exception(ExceptionType::IO, msg) {
}

IOException::IOException(const unordered_map<string, string> &extra_info, const string &msg)
    : Exception(extra_info, ExceptionType::IO, msg) {
}

DataCorruptionException::DataCorruptionException(const string &msg) : Exception(ExceptionType::DATA_CORRUPTION, msg) {
}

ResourceInUseException::ResourceInUseException(const string &msg) : Exception(ExceptionType::RESOURCE_IN_USE, msg) {
}

NotImplementedException::NotImplementedException(const unordered_map<string, string> &extra_info, const string &msg)
    : Exception(extra_info, ExceptionType::NOT_IMPLEMENTED, msg) {
}

MissingExtensionException::MissingExtensionException(const string &msg)
    : Exception(ExceptionType::MISSING_EXTENSION, msg) {
}

AutoloadException::AutoloadException(const string &extension_name, const string &message)
    : Exception(
          ExceptionType::AUTOLOAD,
          StringUtil::Format("An error occurred while trying to automatically install the required extension '%s:\n%s",
                             extension_name, message)) {
}

SerializationException::SerializationException(const string &msg) : Exception(ExceptionType::SERIALIZATION, msg) {
}

SequenceException::SequenceException(const string &msg) : Exception(ExceptionType::SEQUENCE, msg) {
}

InterruptException::InterruptException() : Exception(ExceptionType::INTERRUPT, INTERRUPT_MESSAGE) {
}

InterruptException::InterruptException(const string &message) : Exception(ExceptionType::INTERRUPT, message) {
}

FatalException::FatalException(ExceptionType type, const string &msg) : Exception(type, msg) {
	// FIXME: Make any log context available to add error logging.
}

void InternalException::Throw(const char *message) {
	throw InternalException(message);
}

void NotImplementedException::Throw(const char *message) {
	throw NotImplementedException(message);
}

void NotImplementedException::Throw(const char *message, PhysicalType type) {
	throw NotImplementedException(message, EnumUtil::ToString(type));
}

void InvalidInputException::Throw(const char *message) {
	throw InvalidInputException(message);
}

void OutOfRangeException::Throw(const char *message) {
	throw OutOfRangeException(message);
}

void OutOfRangeException::ThrowConversion(double value, PhysicalType orig_type, PhysicalType new_type) {
	throw OutOfRangeException(value, orig_type, new_type);
}

void DataCorruptionException::Throw(const char *message) {
	throw DataCorruptionException(message);
}

InternalException::InternalException(const string &msg) : Exception(ExceptionType::INTERNAL, msg) {
	// FIXME: Make any log context available to add error logging.
#ifdef DUCKDB_CRASH_ON_ASSERT
	Printer::Print("ABORT THROWN BY INTERNAL EXCEPTION: " + msg + "\n" + StackTrace::GetStackTrace());
	abort();
#endif
}

InternalException::InternalException(const unordered_map<string, string> &extra_info, const string &msg)
    : Exception(extra_info, ExceptionType::INTERNAL, msg) {
}

InvalidInputException::InvalidInputException(const string &msg) : Exception(ExceptionType::INVALID_INPUT, msg) {
}

InvalidInputException::InvalidInputException(const unordered_map<string, string> &extra_info, const string &msg)
    : Exception(extra_info, ExceptionType::INVALID_INPUT, msg) {
}

InvalidConfigurationException::InvalidConfigurationException(const string &msg)
    : Exception(ExceptionType::INVALID_CONFIGURATION, msg) {
}

InvalidConfigurationException::InvalidConfigurationException(const unordered_map<string, string> &extra_info,
                                                             const string &msg)
    : Exception(extra_info, ExceptionType::INVALID_CONFIGURATION, msg) {
}

OutOfMemoryException::OutOfMemoryException(const string &msg)
    : Exception(ExceptionType::OUT_OF_MEMORY, ExtendOutOfMemoryError(msg)) {
}

string OutOfMemoryException::ExtendOutOfMemoryError(const string &msg) {
	string link = "https://duckdb.org/docs/current/guides/performance/how_to_tune_workloads";
	if (StringUtil::Contains(msg, link)) {
		// already extended
		return msg;
	}
	string new_msg = msg;
	new_msg += "\n\nPossible solutions:\n";
	new_msg += "* Reducing the number of threads (SET threads=X)\n";
	new_msg += "* Disabling insertion-order preservation (SET preserve_insertion_order=false)\n";
	new_msg += "* Increasing the memory limit (SET memory_limit='...GB')\n";
	new_msg += "\nSee also " + link;
	return new_msg;
}

ParameterNotAllowedException::ParameterNotAllowedException(const string &msg)
    : Exception(ExceptionType::PARAMETER_NOT_ALLOWED, msg) {
}

ParameterNotResolvedException::ParameterNotResolvedException()
    : Exception(ExceptionType::PARAMETER_NOT_RESOLVED, "Parameter types could not be resolved") {
}

} // namespace duckdb
