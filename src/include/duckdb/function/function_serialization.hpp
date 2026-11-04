//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/function_serialization.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {
class ClientContext;

class FunctionSerializer {
private:
	static QualifiedName DeserializeQualifiedName(Deserializer &deserializer, Identifier name) {
		auto catalog = deserializer.ReadPropertyWithDefault<Identifier>(505, "catalog_name");
		auto schema = deserializer.ReadPropertyWithDefault<Identifier>(506, "schema_name");
		auto qname = deserializer.ReadPropertyWithExplicitDefault<QualifiedName>(507, "qname", QualifiedName());
		if (!qname.Name().empty()) {
			if (qname.Schema().empty()) {
				return QualifiedName(qname.Catalog().empty() ? Identifier::SystemCatalog() : qname.Catalog(),
				                     Identifier::DefaultSchema(), qname.Name());
			}
			return qname.Catalog().empty() ? qname.WithCatalog(Identifier::SystemCatalog()) : qname;
		}
		return QualifiedName(catalog.empty() ? Identifier::SystemCatalog() : catalog,
		                     schema.empty() ? Identifier::DefaultSchema() : schema, std::move(name));
	}

	class DeserializeContext {
	public:
		DeserializeContext(Deserializer &deserializer_p, const LogicalType &return_type,
		                   const const_expression_list_t &children)
		    : deserializer(deserializer_p) {
			deserializer.Set<const LogicalType &>(return_type);
			try {
				deserializer.Set<const const_expression_list_t &>(children);
			} catch (...) {
				deserializer.Unset<LogicalType>();
				throw;
			}
		}
		~DeserializeContext() { // NOLINT(bugprone-exception-escape): Unset only throws if the stack invariant is
			                    // broken.
			deserializer.Unset<const_expression_list_t>();
			deserializer.Unset<LogicalType>();
		}
		DeserializeContext(const DeserializeContext &) = delete;
		DeserializeContext &operator=(const DeserializeContext &) = delete;

	private:
		Deserializer &deserializer;
	};

	template <class FUNC>
	static void RestoreLogicalSignature(FUNC &function, const vector<unique_ptr<Expression>> &children,
	                                    const LogicalType &return_type) {
		auto arguments = function.GetArguments();
		for (idx_t index = 0; index < arguments.size() && index < children.size(); index++) {
			if (!arguments[index].IsComplete()) {
				arguments[index] = children[index]->GetReturnType();
			}
		}
		function.SetLogicalArguments(std::move(arguments));
		function.SetLogicalReturnType(return_type.IsAggregateState() ? function.GetReturnType() : return_type);
	}

public:
	template <class FUNC>
	static void Serialize(Serializer &serializer, const FUNC &function, optional_ptr<FunctionData> bind_info) {
		D_ASSERT(!function.GetName().empty());
		if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
			serializer.WriteProperty(500, "name", function.GetName());
		}
		serializer.WriteProperty(501, "arguments", GetSerializedArguments(serializer, function));
		if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
			// binds no longer erase the arguments they fold into their bind data, so the argument list above is
			// always the full one - older versions read this field unconditionally, so write it (empty) for them
			serializer.WriteProperty(502, "original_arguments", vector<LogicalType>());
		}

		if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
			serializer.WriteProperty(507, "qname", function.GetQualifiedName());
		} else {
			serializer.WritePropertyWithDefault(505, "catalog_name", function.GetCatalogName(), Identifier());
			serializer.WritePropertyWithDefault(506, "schema_name", function.GetSchemaName(), Identifier());
		}

		bool has_serialize = function.HasSerializationCallbacks();
		serializer.WriteProperty(503, "has_serialize", has_serialize);
		if constexpr (std::is_base_of_v<BoundSimpleFunction, FUNC>) {
			SerializeNamedArguments(serializer, function);
		}
		if (has_serialize) {
			serializer.WriteObject(504, "function_data",
			                       [&](Serializer &obj) { function.GetSerializeCallback()(obj, bind_info, function); });
			D_ASSERT(function.GetDeserializeCallback());
		}
	}

	//! Older versions select a table function by its positional arguments alone
	template <class FUNC>
	static vector<LogicalType> GetSerializedArguments(Serializer &serializer, const FUNC &function) {
		if constexpr (std::is_same_v<FUNC, BoundTableFunction>) {
			if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
				VerifyLegacyTableFunctionArguments(function);
				auto &arguments = function.GetArguments();
				auto positional_end = arguments.begin() + NumericCast<int64_t>(function.GetPositionalArgumentCount());
				return vector<LogicalType>(arguments.begin(), positional_end);
			}
		}
		return function.GetArguments();
	}

	//! A required keyword-only option selects its overload, which an older version cannot be told about
	static void VerifyLegacyTableFunctionArguments(const BoundTableFunction &function) {
		auto &signature = function.GetSignature();
		for (auto &name : function.GetNamedArguments()) {
			auto param_idx = signature.GetParameterIndexByName(name);
			if (param_idx.IsValid() && !signature.GetParameter(param_idx.GetIndex()).HasDefaultValue()) {
				throw SerializationException("Table function %s has a required keyword-only parameter %s, which "
				                             "cannot be serialized to a storage version older than v2.0.0",
				                             function.GetName(), name);
			}
		}
	}

	//! How the arguments split into positional and named ones. Always written for current-version plans, so that a
	//! reader can tell an all-positional call from a plan written before the names were serialized
	template <class FUNC>
	static void SerializeNamedArguments(Serializer &serializer, const FUNC &function) {
		auto &named_arguments = function.GetNamedArguments();
		if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
			if (named_arguments.empty()) {
				return;
			}
			if constexpr (std::is_same_v<FUNC, BoundTableFunction>) {
				// older versions read a table function's options from the operator's named parameters instead
				return;
			}
			if (named_arguments.size() > function.GetKwargsCount()) {
				throw SerializationException("Function %s has keyword-only parameters, which cannot be serialized "
				                             "to a storage version older than v2.0.0",
				                             function.GetName());
			}
			// the "**kwargs" arguments are matched to "*args" instead, or named after their alias
			return;
		}
		serializer.WriteProperty(508, "positional_arguments", function.GetPositionalArgumentCount());
		serializer.WritePropertyWithDefault(509, "named_arguments", named_arguments);
	}

	//! Plans written by versions whose binds erased the arguments they folded into their bind data record the
	//! pre-erase list separately - use that as the argument list, so that the function looks the same either way
	static void RestoreErasedArguments(vector<LogicalType> &arguments, vector<LogicalType> &original_arguments) {
		if (!original_arguments.empty()) {
			arguments = std::move(original_arguments);
		}
	}

	//! Splits the argument types a plan recorded into those passed by position and those passed by name
	static void SplitArgumentTypes(const QualifiedName &qualified_name, const vector<LogicalType> &arguments,
	                               idx_t positional_count, const vector<Identifier> &named_arguments,
	                               vector<LogicalType> &positional_types,
	                               vector<pair<Identifier, LogicalType>> &keyword_types) {
		if (positional_count + named_arguments.size() != arguments.size()) {
			throw SerializationException("Function %s has %llu argument types, but %llu positional and %llu named "
			                             "arguments",
			                             qualified_name.ToString(), arguments.size(), positional_count,
			                             named_arguments.size());
		}
		for (idx_t arg_idx = 0; arg_idx < arguments.size(); arg_idx++) {
			if (arg_idx < positional_count) {
				positional_types.push_back(arguments[arg_idx]);
			} else {
				keyword_types.emplace_back(named_arguments[arg_idx - positional_count], arguments[arg_idx]);
			}
		}
	}

	//! Selects the overload of a plan written before v2.0.0, which records its named arguments only as the operator's
	//! named parameters. Required keyword-only parameters are treated as if they had a default - the bind receives
	//! those named parameters and rejects a missing one, as it did when the plan was written
	static FunctionBindResult SelectWithoutNamedArguments(ClientContext &context, const Identifier &name,
	                                                      const TableFunctionSet &functions,
	                                                      const vector<LogicalType> &positional_types) {
		auto relaxed = functions;
		relaxed.ApplyToFunctions([](TableFunction &function) {
			auto &signature = function.GetSignature();
			for (idx_t i = 0; i < signature.GetParameterCount(); i++) {
				auto &param = signature.GetParameter(i);
				if (param.GetKind() == FunctionParameterKind::KEYWORD_ONLY && !param.HasDefaultValue()) {
					// selection only checks that there is a default - an untyped NULL suits a parameter of any type
					param.SetDefaultValue(Value());
				}
			}
		});
		return FunctionBinder::SelectFunction(context, relaxed, positional_types);
	}

	//! Deserializes a table function, and whether the plan holds its bind data. Without it, the caller binds again
	static pair<BoundTableFunction, bool> DeserializeTableFunction(Deserializer &deserializer) {
		auto &context = deserializer.Get<ClientContext &>();
		auto name = deserializer.ReadPropertyWithDefault<Identifier>(500, "name");
		auto arguments = deserializer.ReadProperty<vector<LogicalType>>(501, "arguments");
		auto original_arguments = deserializer.ReadPropertyWithDefault<vector<LogicalType>>(502, "original_arguments");
		auto qualified_name = DeserializeQualifiedName(deserializer, std::move(name));
		auto has_serialize = deserializer.ReadProperty<bool>(503, "has_serialize");
		auto positional_count = deserializer.ReadPropertyWithDefault<idx_t>(508, "positional_arguments");
		auto named_arguments = deserializer.ReadPropertyWithDefault<vector<Identifier>>(509, "named_arguments");

		RestoreErasedArguments(arguments, original_arguments);
		if (named_arguments.empty()) {
			positional_count = arguments.size();
		}

		auto &func_catalog = Catalog::GetEntry(context, CatalogType::TABLE_FUNCTION_ENTRY, qualified_name);
		if (func_catalog.type != CatalogType::TABLE_FUNCTION_ENTRY) {
			throw InternalException("DeserializeTableFunction - cant find catalog entry for function %s",
			                        qualified_name.Name().GetIdentifierName());
		}
		auto &functions = func_catalog.Cast<TableFunctionCatalogEntry>().functions;

		// the named arguments take part in selecting the overload, exactly as they did when the call was bound
		vector<LogicalType> positional_types;
		vector<pair<Identifier, LogicalType>> keyword_types;
		SplitArgumentTypes(qualified_name, arguments, positional_count, named_arguments, positional_types,
		                   keyword_types);
		auto func_idx = FunctionBinder::SelectFunction(context, functions, {positional_types, keyword_types});
		// the error of the selection with the named arguments describes the plan best
		ErrorData error;
		if (!func_idx) {
			error = func_idx.GetError();
			if (named_arguments.empty()) {
				func_idx = SelectWithoutNamedArguments(context, qualified_name.Name(), functions, positional_types);
			}
		}
		if (!func_idx) {
			throw SerializationException("Failed to find function %s(%s)\n%s", qualified_name.ToString(),
			                             StringUtil::ToString(arguments, ","), error.RawMessage());
		}

		// by index: a selection without named arguments binds to a relaxed copy of the overload
		BoundTableFunction function(functions.GetFunctionByOffset(func_idx.GetBinding().GetOverloadIndex()));
		function.GetArguments() = std::move(arguments);
		function.SetNamedArguments(positional_count, std::move(named_arguments));
		return make_pair(std::move(function), has_serialize);
	}

	template <class FUNC>
	static unique_ptr<FunctionData> FunctionDeserialize(Deserializer &deserializer, FUNC &function) {
		if (!function.GetDeserializeCallback()) {
			throw SerializationException("Function requires deserialization but no deserialization function for %s",
			                             function.GetName());
		}
		unique_ptr<FunctionData> result;
		deserializer.ReadObject(504, "function_data",
		                        [&](Deserializer &obj) { result = function.GetDeserializeCallback()(obj, function); });
		return result;
	}

	static bool TypeRequiresAssignment(const LogicalType &type) {
		switch (type.id()) {
		case LogicalTypeId::SQLNULL:
		case LogicalTypeId::ANY:
		case LogicalTypeId::INVALID:
			return true;
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::UNION:
		case LogicalTypeId::VARIANT:
		case LogicalTypeId::MAP:
			if (!type.HasParameters()) {
				return true;
			}
			return false;
		case LogicalTypeId::LIST:
			if (!type.HasParameters()) {
				return true;
			}
			return TypeRequiresAssignment(ListType::GetChildType(type));
		case LogicalTypeId::ARRAY:
			if (!type.HasParameters()) {
				return true;
			}
			return TypeRequiresAssignment(ArrayType::GetChildType(type));
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::TUPLE:
			if (!type.HasParameters()) {
				return true;
			}
			if (StructType::GetChildCount(type) == 0) {
				return true;
			}
			return false;
		default:
			return false;
		}
	}

	//! Plans written before the argument names were serialized hold the names of the named arguments in the aliases
	//! of the trailing arguments. Only the arguments behind the standard parameters can have been named, and only
	//! where the function cannot take them positionally - a "*args" function folds its "**kwargs" into "*args", so
	//! a trailing alias there is a genuine alias, not an argument name.
	template <class FUNCTION_SET>
	static void RestoreNamesFromAliases(const FUNCTION_SET &functions, const vector<unique_ptr<Expression>> &children,
	                                    idx_t &positional_count, vector<Identifier> &named_arguments) {
		bool takes_named = false;
		idx_t standard_count = 0;
		for (auto &function : functions.functions) {
			auto &signature = function->GetSignature();
			if (!signature.GetKwargs() || signature.GetArgs()) {
				continue;
			}
			takes_named = true;
			standard_count = MaxValue(standard_count, signature.GetPositionalParameterCount());
		}
		if (!takes_named || children.size() != positional_count) {
			return;
		}
		idx_t named_offset = children.size();
		while (named_offset > standard_count && !children[named_offset - 1]->GetAlias().empty()) {
			named_offset--;
		}
		for (idx_t i = named_offset; i < children.size(); i++) {
			named_arguments.push_back(children[i]->GetAlias());
		}
		positional_count = named_offset;
	}

	template <class FUNC>
	static pair<FUNC, unique_ptr<FunctionData>>
	ResolveFunction(FunctionBinder &binder, const FunctionBinding &binding, vector<unique_ptr<Expression>> &children,
	                vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args) {
		if constexpr (std::is_same_v<FUNC, BoundScalarFunction>) {
			return binder.ResolveScalarFunction(binding, children, keyword_args);
		} else if constexpr (std::is_same_v<FUNC, BoundAggregateFunction>) {
			return binder.ResolveAggregateFunction(binding, children, keyword_args);
		} else {
			static_assert(std::is_same_v<FUNC, BoundWindowFunction>, "unsupported bound function type");
			return binder.ResolveWindowFunction(binding, children, keyword_args);
		}
	}

	template <class FUNC, class CATALOG_ENTRY>
	static pair<FUNC, unique_ptr<FunctionData>> Deserialize(Deserializer &deserializer, CatalogType catalog_type,
	                                                        vector<unique_ptr<Expression>> &children,
	                                                        LogicalType return_type) { // NOLINT: clang-tidy bug
		auto &context = deserializer.Get<ClientContext &>();

		auto name = deserializer.ReadPropertyWithDefault<Identifier>(500, "name");
		auto arguments = deserializer.ReadProperty<vector<LogicalType>>(501, "arguments");
		auto original_arguments = deserializer.ReadPropertyWithDefault<vector<LogicalType>>(502, "original_arguments");
		auto qualified_name = DeserializeQualifiedName(deserializer, std::move(name));
		auto has_serialize = deserializer.ReadProperty<bool>(503, "has_serialize");
		auto positional_count = deserializer.ReadPropertyWithDefault<idx_t>(508, "positional_arguments");
		auto named_arguments = deserializer.ReadPropertyWithDefault<vector<Identifier>>(509, "named_arguments");

		RestoreErasedArguments(arguments, original_arguments);

		if (arguments.empty() && !children.empty()) {
			// The function is specified as having no arguments, but somehow expressions were passed anyway
			// Assume this is a "varargs" function and use the types of the expressions as the arguments
			// This can happen when we change a function that used to take varargs, to no longer do so.
			arguments.reserve(children.size());
			for (auto &child : children) {
				arguments.push_back(child->GetReturnType());
			}
		}

		// Now lookup the function in the catalog.
		auto &func_catalog = Catalog::GetEntry(context, catalog_type, qualified_name);

		if (func_catalog.type != catalog_type) {
			throw InternalException("DeserializeFunction - cant find catalog entry for function %s",
			                        qualified_name.Name().GetIdentifierName());
		}

		auto &functions = func_catalog.Cast<CATALOG_ENTRY>().functions;

		// If there are no argument names serialized, treat all arguments as positional
		if (named_arguments.empty()) {
			// a current-version plan always records its positional count - only a plan written before the argument
			// names were serialized leaves both fields empty and holds the names in the aliases of its children
			const bool argument_names_serialized = positional_count != 0;
			positional_count = arguments.size();
			if (!argument_names_serialized) {
				RestoreNamesFromAliases(functions, children, positional_count, named_arguments);
			}
		}

		// Sanity check: The named arguments are the last arguments, so the number of arguments has to add up
		const auto has_valid_count = positional_count + named_arguments.size() == arguments.size();
		if (!has_valid_count || (!named_arguments.empty() && children.size() != arguments.size())) {
			throw SerializationException(
			    "Function %s has %llu argument types and %llu arguments, but %llu positional and %llu named arguments",
			    qualified_name.ToString(), arguments.size(), children.size(), positional_count, named_arguments.size());
		}

		// Split types by positional and keyword arguments
		vector<LogicalType> positional_types;
		vector<pair<Identifier, LogicalType>> keyword_types;

		for (idx_t arg_idx = 0; arg_idx < arguments.size(); arg_idx++) {
			if (arg_idx < positional_count) {
				positional_types.push_back(arguments[arg_idx]);
			} else {
				const auto kw_idx = arg_idx - positional_count;
				keyword_types.emplace_back(named_arguments[kw_idx], arguments[arg_idx]);
			}
		}

		// Lookup the function (and the correct overload)
		FunctionBinder binder(context);
		binder.arguments_collated = true;
		const auto func_idx = FunctionBinder::SelectFunction(context, functions, {positional_types, keyword_types});

		// Ensure the function overload was found
		if (!func_idx.IsSuccess()) {
			throw SerializationException("Failed to find function %s(%s)\n%s", qualified_name.ToString(),
			                             StringUtil::ToString(arguments, ","), func_idx.GetError().RawMessage());
		}

		const auto function = functions.GetFunctionByOffset(func_idx.GetBinding().GetOverloadIndex());

		// Does this function support serializing its bound data?
		if (!has_serialize) {
			// No, then just rebind the function
			try {
				// Split children into positional and keyword args expression
				vector<pair<Identifier, unique_ptr<Expression>>> keyword_args;
				if (!named_arguments.empty()) {
					for (idx_t i = positional_count; i < children.size(); i++) {
						keyword_args.emplace_back(named_arguments[i - positional_count], std::move(children[i]));
					}
					children.resize(positional_count);
				}

				// The bind callback may have changed the argument types, so the selected overload is bound to the
				// arguments it is called with
				std::decay_t<decltype(functions)> overload(functions.name);
				overload.AddFunction(function);
				auto rebind = FunctionBinder::SelectFunction(context, overload, {children, keyword_args}, true);
				if (!rebind) {
					rebind.GetError().Throw();
				}

				// Resolve function
				auto [bound_function, bound_data] =
				    ResolveFunction<FUNC>(binder, rebind.GetBinding(), children, keyword_args);

				if (TypeRequiresAssignment(bound_function.GetReturnType())) {
					bound_function.SetReturnType(std::move(return_type));
				}

				return make_pair(std::move(bound_function), std::move(bound_data));
			} catch (std::exception &ex) {
				ErrorData inner_error(ex);
				throw SerializationException("Error during bind of function in deserialization: %s",
				                             inner_error.RawMessage());
			}
		}

		// Otherwise, construct the bound function from its parts
		FUNC bound_function(function);
		bound_function.GetArguments() = std::move(arguments);

		bound_function.SetNamedArguments(positional_count, std::move(named_arguments));
		RestoreLogicalSignature(bound_function, children, return_type);

		// Invoke deserialization function
		const_expression_list_t child_references;
		for (auto &child : children) {
			child_references.emplace_back(*child);
		}
		unique_ptr<FunctionData> bound_data;
		{
			DeserializeContext scope(deserializer, return_type, child_references);
			bound_data = FunctionDeserialize(deserializer, bound_function);
		}

		if (TypeRequiresAssignment(bound_function.GetReturnType())) {
			bound_function.SetReturnType(std::move(return_type));
		}

		return make_pair(std::move(bound_function), std::move(bound_data));
	}
};

} // namespace duckdb
