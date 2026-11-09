#include "duckdb/planner/bind_context.hpp"

#include "duckdb/catalog/catalog_entry/table_column_type.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/positional_reference_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/binder.hpp"

#include <algorithm>

namespace duckdb {

BindContext::BindContext(Binder &binder) : binder(binder) {
}

string MinimumUniqueAlias(const BindingAlias &alias, const BindingAlias &other) {
	if (alias.GetAlias() != other.GetAlias()) {
		return SQLIdentifier::ToString(alias.GetAlias().GetIdentifierName());
	}
	// the table names are equal - qualify with as much of the (nested) schema path as needed to disambiguate,
	// walking from the innermost schema outwards
	auto schema_path = alias.GetSchemaPath();
	auto other_path = other.GetSchemaPath();
	string qualification = SQLIdentifier::ToString(alias.GetAlias().GetIdentifierName());
	for (idx_t i = 0; i < schema_path.size(); i++) {
		auto &schema = schema_path[schema_path.size() - 1 - i];
		qualification = SQLIdentifier::ToString(schema.GetIdentifierName()) + "." + qualification;
		bool other_matches = i < other_path.size() && other_path[other_path.size() - 1 - i] == schema;
		if (!other_matches) {
			// this schema component distinguishes the two - we are done
			return qualification;
		}
	}
	// the schema paths are identical - fall back to the fully qualified name
	return alias.ToString();
}

optional_ptr<Binding> BindContext::GetMatchingBinding(const Identifier &column_name, QueryErrorContext context) {
	optional_ptr<Binding> result;
	for (auto &binding_ptr : bindings_list) {
		auto &binding = *binding_ptr;
		auto is_using_binding = GetUsingBinding(column_name, binding.GetBindingAlias());
		if (is_using_binding) {
			continue;
		}
		if (binding.HasMatchingBinding(column_name)) {
			if (result) {
				throw BinderException(context,
				                      "Ambiguous reference to column name %s (use: '%s.%s' "
				                      "or '%s.%s')",
				                      column_name,
				                      MinimumUniqueAlias(result->GetBindingAlias(), binding.GetBindingAlias()),
				                      SQLIdentifier(column_name),
				                      MinimumUniqueAlias(binding.GetBindingAlias(), result->GetBindingAlias()),
				                      SQLIdentifier(column_name));
			}
			result = &binding;
		}
	}
	return result;
}

vector<Identifier> BindContext::GetSimilarBindings(const Identifier &column_name) {
	vector<pair<string, double>> scores;
	for (auto &binding_ptr : bindings_list) {
		auto &binding = *binding_ptr;
		for (auto &name : binding.GetColumnNames()) {
			double distance = StringUtil::SimilarityRating(name.GetIdentifierName(), column_name.GetIdentifierName());
			// check if we need to qualify the column
			auto matching_bindings = GetMatchingBindings(name);
			if (matching_bindings.size() > 1) {
				scores.emplace_back(binding.GetAlias() + "." + name.GetIdentifierName(), distance);
			} else {
				scores.emplace_back(name, distance);
			}
		}
	}
	return StringsToIdentifiers(StringUtil::TopNStrings(scores));
}

void BindContext::AddUsingBinding(const Identifier &column_name, UsingColumnSet &set) {
	using_columns[column_name].insert(set);
}

optional_ptr<UsingColumnSet> BindContext::GetUsingBinding(const Identifier &column_name) {
	auto entry = using_columns.find(column_name);
	if (entry == using_columns.end()) {
		return nullptr;
	}
	auto &using_bindings = entry->second;
	if (using_bindings.empty()) {
		throw InternalException("Using binding found but no entries");
	}

	if (using_bindings.size() > 1) {
		string error = "Ambiguous column reference: column \"" + column_name + "\" can refer to either:\n";
		for (auto &using_set_ref : using_bindings) {
			auto &using_set = using_set_ref.get();
			string result_bindings;
			for (auto &binding : using_set.bindings) {
				if (result_bindings.empty()) {
					result_bindings = "[";
				} else {
					result_bindings += ", ";
				}
				result_bindings += binding.GetAlias();
				result_bindings += ".";
				result_bindings += GetActualColumnName(binding, column_name);
			}
			error += result_bindings + "]";
		}
		throw BinderException(error);
	}
	return &using_bindings.begin()->get();
}

optional_ptr<UsingColumnSet> BindContext::GetUsingBinding(const Identifier &column_name, const BindingAlias &binding) {
	if (!binding.IsSet()) {
		throw InternalException("GetUsingBinding: expected non-empty binding_name");
	}
	auto entry = using_columns.find(column_name);
	if (entry == using_columns.end()) {
		return nullptr;
	}
	auto &using_bindings = entry->second;
	for (auto &using_set_ref : using_bindings) {
		auto &using_set = using_set_ref.get();
		auto &bindings = using_set.bindings;
		for (auto &using_binding : bindings) {
			if (using_binding == binding) {
				return &using_set;
			}
		}
	}
	return nullptr;
}

void BindContext::RemoveUsingBinding(const Identifier &column_name, UsingColumnSet &set) {
	auto entry = using_columns.find(column_name);
	if (entry == using_columns.end()) {
		throw InternalException("Attempting to remove using binding that is not there");
	}
	auto &bindings = entry->second;
	if (bindings.find(set) != bindings.end()) {
		bindings.erase(set);
	}
	if (bindings.empty()) {
		using_columns.erase(column_name);
	}
}

void BindContext::TransferUsingBinding(BindContext &current_context, optional_ptr<UsingColumnSet> current_set,
                                       UsingColumnSet &new_set, const Identifier &using_column) {
	AddUsingBinding(using_column, new_set);
	if (current_set) {
		current_context.RemoveUsingBinding(using_column, *current_set);
	}
}

string BindContext::GetActualColumnName(Binding &binding, const Identifier &column_name) {
	column_t binding_index;
	if (!binding.TryGetBindingIndex(column_name, binding_index)) { // LCOV_EXCL_START
		throw InternalException("Binding with name %s does not have a column named %s", binding.GetAlias(),
		                        column_name);
	} // LCOV_EXCL_STOP
	return binding.GetColumnNames()[binding_index].GetIdentifierName();
}

string BindContext::GetActualColumnName(const BindingAlias &binding_alias, const Identifier &column_name) {
	ErrorData error;
	auto binding = GetBinding(binding_alias, error);
	if (!binding) {
		throw InternalException("No binding with name %s: %s", binding_alias.GetAlias(), error.RawMessage());
	}
	return GetActualColumnName(*binding, column_name);
}

vector<reference<Binding>> BindContext::GetMatchingBindings(const Identifier &column_name) {
	vector<reference<Binding>> result;
	for (auto &binding_ptr : bindings_list) {
		auto &binding = *binding_ptr;
		if (binding.HasMatchingBinding(column_name)) {
			result.push_back(binding);
		}
	}
	return result;
}

unique_ptr<ParsedExpression> BindContext::ExpandGeneratedColumn(TableBinding &table_binding,
                                                                const Identifier &column_name) {
	auto result = table_binding.ExpandGeneratedColumn(column_name);
	result->SetAlias(column_name);
	return result;
}

static bool ColumnIsGenerated(Binding &binding, column_t index) {
	if (binding.GetBindingType() != BindingType::TABLE) {
		return false;
	}
	auto &table_binding = binding.Cast<TableBinding>();
	auto catalog_entry = table_binding.GetStandardEntry();
	if (!catalog_entry) {
		return false;
	}
	if (IsVirtualColumn(index)) {
		return false;
	}
	D_ASSERT(catalog_entry->type == CatalogType::TABLE_ENTRY);
	auto &table_entry = catalog_entry->Cast<TableCatalogEntry>();
	return table_entry.GetColumn(LogicalIndex(index)).Generated();
}

unique_ptr<ParsedExpression> BindContext::CreateColumnReference(const BindingAlias &table_alias,
                                                                const Identifier &column_name,
                                                                ColumnBindType bind_type) {
	ErrorData error;
	// emit the full (possibly nested) schema path so the produced reference is unambiguous
	vector<Identifier> names;
	if (!table_alias.GetCatalog().empty()) {
		names.push_back(table_alias.GetCatalog());
	}
	for (auto &schema : table_alias.GetSchemaPath()) {
		names.push_back(schema);
	}
	names.push_back(table_alias.GetAlias());
	names.push_back(column_name);

	auto result = make_uniq<ColumnRefExpression>(names);
	auto binding = GetBinding(table_alias, column_name, error);
	if (!binding) {
		return std::move(result);
	}
	auto column_index = binding->GetBindingIndex(column_name);
	if (bind_type == ColumnBindType::EXPAND_GENERATED_COLUMNS && ColumnIsGenerated(*binding, column_index)) {
		return ExpandGeneratedColumn(binding->Cast<TableBinding>(), column_name);
	}
	auto &registered_name = binding->GetRegisteredColumnName(column_name);
	if (registered_name.GetIdentifierName() != column_name.GetIdentifierName()) {
		// because of case insensitivity in the binder we rename the column to the original name
		// as it appears in the binding itself
		result->SetAlias(registered_name);
	}
	return std::move(result);
}

unique_ptr<ParsedExpression> BindContext::CreateColumnReference(const Identifier &table_name,
                                                                const Identifier &column_name,
                                                                ColumnBindType bind_type) {
	string schema_name;
	return CreateColumnReference(Identifier(schema_name), table_name, column_name, bind_type);
}

unique_ptr<ParsedExpression> BindContext::CreateColumnReference(const Identifier &catalog_name,
                                                                const Identifier &schema_name,
                                                                const Identifier &table_name,
                                                                const Identifier &column_name,
                                                                ColumnBindType bind_type) {
	ErrorData error;
	vector<Identifier> names;
	if (!catalog_name.empty()) {
		names.push_back(catalog_name);
	}
	if (!schema_name.empty()) {
		names.push_back(schema_name);
	}
	names.push_back(table_name);
	names.push_back(column_name);

	BindingAlias alias {catalog_name, schema_name, table_name};
	auto result = make_uniq<ColumnRefExpression>(names);
	auto binding = GetBinding(alias, column_name, error);
	if (!binding) {
		return std::move(result);
	}
	auto column_index = binding->GetBindingIndex(column_name);
	if (bind_type == ColumnBindType::EXPAND_GENERATED_COLUMNS && ColumnIsGenerated(*binding, column_index)) {
		return ExpandGeneratedColumn(binding->Cast<TableBinding>(), column_name);
	}
	auto &registered_name = binding->GetRegisteredColumnName(column_name);
	if (registered_name.GetIdentifierName() != column_name.GetIdentifierName()) {
		// because of case insensitivity in the binder we rename the column to the original name
		// as it appears in the binding itself
		result->SetAlias(registered_name);
	}
	return std::move(result);
}

unique_ptr<ParsedExpression> BindContext::CreateColumnReference(const Identifier &schema_name,
                                                                const Identifier &table_name,
                                                                const Identifier &column_name,
                                                                ColumnBindType bind_type) {
	Identifier catalog_name;
	return CreateColumnReference(catalog_name, schema_name, table_name, column_name, bind_type);
}

string GetCandidateAlias(const BindingAlias &main_alias, const BindingAlias &new_alias) {
	string candidate;
	if (!main_alias.GetCatalog().empty() && !new_alias.GetCatalog().empty()) {
		candidate += new_alias.GetCatalog() + ".";
	}
	// include as many (nested) schema components as the reference alias provides
	auto main_path = main_alias.GetSchemaPath();
	auto new_path = new_alias.GetSchemaPath();
	idx_t schema_components = MinValue<idx_t>(main_path.size(), new_path.size());
	for (idx_t i = 0; i < schema_components; i++) {
		candidate += new_path[new_path.size() - schema_components + i].GetIdentifierName() + ".";
	}
	candidate += new_alias.GetAlias();
	return candidate;
}

vector<reference<Binding>> BindContext::GetBindings(const BindingAlias &alias, ErrorData &out_error) {
	if (!alias.IsSet()) {
		throw InternalException("BindingAlias is not set");
	}
	vector<reference<Binding>> matching_bindings;
	for (auto &binding : bindings_list) {
		if (binding->GetBindingAlias().Matches(alias)) {
			matching_bindings.push_back(*binding);
		}
	}
	if (matching_bindings.empty()) {
		// alias not found in this BindContext
		vector<string> candidates;
		for (auto &binding : bindings_list) {
			candidates.push_back(GetCandidateAlias(alias, binding->GetBindingAlias()));
		}
		auto main_alias = GetCandidateAlias(alias, alias);
		string candidate_str =
		    StringUtil::CandidatesMessage(StringUtil::TopNJaroWinkler(candidates, main_alias), "Candidate tables");
		out_error = ErrorData(ExceptionType::BINDER,
		                      StringUtil::Format("Referenced table \"%s\" not found!%s", main_alias, candidate_str));
	}
	return matching_bindings;
}

string BindContext::AmbiguityException(const BindingAlias &alias, const vector<reference<Binding>> &bindings) {
	D_ASSERT(bindings.size() > 1);
	// found multiple matching aliases
	string result = "(use: ";
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (i > 0) {
			if (i + 1 == bindings.size()) {
				result += " or ";
			} else {
				result += ", ";
			}
		}
		// find the minimum alias that uniquely describes this table reference
		auto &current_alias = bindings[i].get().GetBindingAlias();
		string minimum_alias;
		bool duplicate_alias = false;
		for (idx_t k = 0; k < bindings.size(); k++) {
			if (k == i) {
				continue;
			}
			auto &other_alias = bindings[k].get().GetBindingAlias();
			if (current_alias == other_alias) {
				duplicate_alias = true;
			}
			string new_minimum_alias = MinimumUniqueAlias(current_alias, other_alias);
			if (new_minimum_alias.size() > minimum_alias.size()) {
				minimum_alias = std::move(new_minimum_alias);
			}
		}
		if (duplicate_alias) {
			result = "(duplicate alias \"" + alias.ToString() +
			         "\", explicitly alias one of the tables using \"AS my_alias\"";
		} else {
			result += minimum_alias;
		}
	}
	result += ")";
	return result;
}

optional_ptr<Binding> BindContext::GetBinding(const BindingAlias &alias, const Identifier &column_name,
                                              ErrorData &out_error) {
	auto matching_bindings = GetBindings(alias, out_error);
	if (matching_bindings.empty()) {
		// no bindings found
		return nullptr;
	}

	optional_ptr<Binding> result;
	// find the binding that this column name belongs to
	for (auto &binding_ref : matching_bindings) {
		auto &binding = binding_ref.get();
		if (!binding.HasMatchingBinding(column_name)) {
			continue;
		}
		if (result) {
			// we found multiple bindings that this column name belongs to - ambiguity
			string helper_message = AmbiguityException(alias, matching_bindings);
			throw BinderException("Ambiguous reference to table \"%s\" %s", alias.ToString(), helper_message);
		} else {
			result = &binding;
		}
	}
	if (!result) {
		// found the table binding - but could not find the column
		out_error = matching_bindings[0].get().ColumnNotFoundError(column_name);
	}
	return result;
}

optional_ptr<Binding> BindContext::GetBinding(const BindingAlias &alias, ErrorData &out_error) {
	auto matching_bindings = GetBindings(alias, out_error);
	if (matching_bindings.empty()) {
		return nullptr;
	}
	if (matching_bindings.size() > 1) {
		string helper_message = AmbiguityException(alias, matching_bindings);
		throw BinderException("Ambiguous reference to table \"%s\" %s", alias.ToString(), helper_message);
	}
	// found a single matching alias
	return &matching_bindings[0].get();
}

optional_ptr<Binding> BindContext::GetBinding(const Identifier &name, ErrorData &out_error) {
	return GetBinding(BindingAlias(name), out_error);
}

BindingAlias GetBindingAlias(ColumnRefExpression &colref) {
	auto &names = colref.ColumnNames();
	if (names.size() <= 1) {
		throw InternalException("Cannot get binding alias from column ref unless it has 2 or more entries");
	}
	// the last entry is the column name, the second-to-last is the table name
	// the remaining leading entries are the qualification: [catalog, schema path...] (a real table's reference always
	// carries the catalog, so for 4+ entries the leading entry is the catalog and the rest are the schema path)
	auto &table_name = names[names.size() - 2];
	if (names.size() == 2) {
		return BindingAlias(table_name);
	}
	if (names.size() == 3) {
		// schema.table.column
		return BindingAlias(names[0], table_name);
	}
	auto &catalog = names[0];
	vector<Identifier> schema_path(names.begin() + 1, names.end() - 2);
	return BindingAlias(catalog, schema_path, table_name);
}

BindResult BindContext::BindColumn(ColumnRefExpression &colref, idx_t depth) {
	if (!colref.IsQualified()) {
		throw InternalException("Could not bind alias %s!", colref.GetColumnName());
	}

	ErrorData error;
	BindingAlias alias;
	auto binding = GetBinding(GetBindingAlias(colref), colref.GetColumnName(), error);
	if (!binding) {
		return BindResult(std::move(error));
	}
	return binding->Bind(colref, depth);
}

string BindContext::BindColumn(PositionalReferenceExpression &ref, Identifier &table_name, Identifier &column_name) {
	idx_t total_columns = 0;
	idx_t current_position = ref.Index() - 1;
	for (auto &entry : bindings_list) {
		auto &binding = *entry;
		auto &column_names = binding.GetColumnNames();
		idx_t entry_column_count = column_names.size();
		if (ref.Index() == 0) {
			// this is a row id
			table_name = binding.GetAlias();
			column_name = "rowid";
			return string();
		}
		if (current_position < entry_column_count) {
			table_name = binding.GetAlias();
			column_name = column_names[current_position];
			return string();
		} else {
			total_columns += entry_column_count;
			current_position -= entry_column_count;
		}
	}
	return StringUtil::Format("Positional reference %d out of range (total %d columns)", ref.Index(), total_columns);
}

unique_ptr<ColumnRefExpression> BindContext::PositionToColumn(PositionalReferenceExpression &ref) {
	Identifier table_name, column_name;

	string error = BindColumn(ref, table_name, column_name);
	if (!error.empty()) {
		throw BinderException(error);
	}
	return make_uniq<ColumnRefExpression>(column_name, table_name);
}

struct StarBindState {
	explicit StarBindState(vector<unique_ptr<ParsedExpression>> &new_select_list) : new_select_list(new_select_list) {
	}

	vector<unique_ptr<ParsedExpression>> &new_select_list;
	identifier_set_t excluded_columns;
	qualified_column_set_t excluded_qualified_columns;
	identifier_set_t replaced_columns;
	vector<Identifier> candidate_columns;
};

bool CheckExclusionList(StarExpression &expr, const QualifiedColumnName &qualified_name, StarBindState &state) {
	if (expr.ExcludeList().find(qualified_name) != expr.ExcludeList().end()) {
		state.excluded_qualified_columns.insert(qualified_name);
		return true;
	}
	return false;
}

bool HandleRename(StarExpression &expr, const QualifiedColumnName &qualified_name,
                  unique_ptr<ParsedExpression> &new_expr, StarBindState &state) {
	auto replace_entry = expr.ReplaceList().find(qualified_name.column);
	if (replace_entry != expr.ReplaceList().end()) {
		if (state.replaced_columns.find(replace_entry->first) == state.replaced_columns.end()) {
			new_expr = replace_entry->second->Copy();
			new_expr->SetAlias(replace_entry->first);
			state.replaced_columns.insert(replace_entry->first);
			state.excluded_columns.insert(replace_entry->first);
		} else {
			return false;
		}
	}
	auto rename_entry = expr.RenameList().find(qualified_name);
	if (rename_entry != expr.RenameList().end()) {
		new_expr->SetAlias(rename_entry->second);
	}
	return true;
}

string CandidateColumnMessage(const vector<Identifier> &candidate_columns, const Identifier &missing_column) {
	identifier_set_t unique_columns;
	vector<Identifier> candidates;
	for (auto &candidate_column : candidate_columns) {
		if (unique_columns.find(candidate_column) != unique_columns.end()) {
			continue;
		}
		unique_columns.insert(candidate_column);
		candidates.push_back(candidate_column);
	}
	auto closest_candidates = StringUtil::TopNJaroWinkler(IdentifiersToStrings(candidates), missing_column);
	return StringUtil::CandidatesMessage(closest_candidates, "Candidate bindings");
}

void BindContext::GenerateAllColumnExpressions(StarExpression &expr,
                                               vector<unique_ptr<ParsedExpression>> &new_select_list) {
	if (bindings_list.empty() && expr.RelationName().empty()) {
		throw BinderException("* expression without FROM clause!");
	}
	StarBindState star_state(new_select_list);
	if (expr.RelationName().empty()) {
		// SELECT * case
		// bind all expressions of each table in-order
		reference_set_t<UsingColumnSet> handled_using_columns;
		for (auto &entry : bindings_list) {
			auto &binding = *entry;
			auto &column_names = binding.GetColumnNames();
			auto &binding_alias = binding.GetBindingAlias();
			for (auto &column_name : column_names) {
				star_state.candidate_columns.push_back(column_name);
				QualifiedColumnName qualified_column(binding_alias, column_name);
				if (CheckExclusionList(expr, qualified_column, star_state)) {
					continue;
				}
				// check if this column is a USING column
				auto using_binding_ptr = GetUsingBinding(column_name, binding_alias);
				if (using_binding_ptr) {
					auto &using_binding = *using_binding_ptr;
					// it is!
					// check if we have already emitted the using column
					if (handled_using_columns.find(using_binding) != handled_using_columns.end()) {
						// we have! bail out
						continue;
					}
					// we have not! output the using column
					if (!using_binding.primary_binding.IsSet()) {
						// no primary binding: output a coalesce
						auto coalesce =
						    make_uniq_base<ParsedExpression, OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
						for (auto &child_binding : using_binding.bindings) {
							coalesce->Cast<OperatorExpression>().GetChildrenMutable().push_back(
							    make_uniq<ColumnRefExpression>(column_name, child_binding));
						}
						coalesce->SetAlias(column_name);
						if (HandleRename(expr, qualified_column, coalesce, star_state)) {
							new_select_list.push_back(std::move(coalesce));
						}
					} else {
						// primary binding: output the qualified column ref
						auto new_expr = make_uniq_base<ParsedExpression, ColumnRefExpression>(
						    column_name, using_binding.primary_binding);
						if (HandleRename(expr, qualified_column, new_expr, star_state)) {
							new_select_list.push_back(std::move(new_expr));
						}
					}
					handled_using_columns.insert(using_binding);
					continue;
				}
				auto new_expr =
				    CreateColumnReference(binding_alias, column_name, ColumnBindType::DO_NOT_EXPAND_GENERATED_COLUMNS);
				if (HandleRename(expr, qualified_column, new_expr, star_state)) {
					new_select_list.push_back(std::move(new_expr));
				}
			}
		}
	} else {
		// SELECT tbl.* case
		// SELECT struct.* case
		ErrorData error;
		auto binding = GetBinding(expr.RelationName(), error);
		bool is_struct_ref = false;
		if (!binding) {
			binding = GetMatchingBinding(expr.RelationName(), expr);
			if (!binding) {
				// Expand the nearest enclosing match into column references. Expression binding then
				// resolves their correlation depth, just as it does for explicit qualified columns.
				auto &scopes = binder.GetEnclosingScopes();
				for (idx_t i = scopes.size(); i > 0; i--) {
					auto &scope_context = scopes[i - 1].get().GetBinder().bind_context;
					ErrorData scope_error;
					if (scope_context.GetBinding(expr.RelationName(), scope_error) ||
					    scope_context.GetMatchingBinding(expr.RelationName(), expr)) {
						scope_context.GenerateAllColumnExpressions(expr, new_select_list);
						return;
					}
				}
				if (bindings_list.empty()) {
					throw BinderException("* expression without FROM clause!");
				}
				error.Throw();
			}
			is_struct_ref = true;
		}
		auto &binding_alias = binding->GetBindingAlias();
		auto &column_names = binding->GetColumnNames();
		auto &column_types = binding->GetColumnTypes();

		if (is_struct_ref) {
			auto col_idx = binding->GetBindingIndex(expr.RelationName());
			auto col_type = column_types[col_idx];
			if (col_type.id() != LogicalTypeId::STRUCT) {
				throw BinderException(StringUtil::Format(
				    "Cannot extract field from expression \"%s\" because it is not a struct", expr.ToString()));
			}
			auto &struct_children = StructType::GetChildTypes(col_type);
			vector<Identifier> column_names(3);
			column_names[0] = binding->GetAlias();
			column_names[1] = expr.RelationName();
			for (auto &child : struct_children) {
				star_state.candidate_columns.push_back(child.first);
				QualifiedColumnName qualified_name(child.first);
				if (CheckExclusionList(expr, qualified_name, star_state)) {
					continue;
				}
				column_names[2] = child.first;
				unique_ptr<ParsedExpression> new_expr = make_uniq<ColumnRefExpression>(column_names);
				if (HandleRename(expr, qualified_name, new_expr, star_state)) {
					new_select_list.push_back(std::move(new_expr));
				}
			}
		} else {
			for (auto &column_name : column_names) {
				star_state.candidate_columns.push_back(column_name);
				QualifiedColumnName qualified_name(binding_alias, column_name);
				if (CheckExclusionList(expr, qualified_name, star_state)) {
					continue;
				}
				auto new_expr =
				    CreateColumnReference(binding_alias, column_name, ColumnBindType::DO_NOT_EXPAND_GENERATED_COLUMNS);
				if (HandleRename(expr, qualified_name, new_expr, star_state)) {
					new_select_list.push_back(std::move(new_expr));
				}
			}
		}
	}

	if (binder.GetBindingMode() == BindingMode::EXTRACT_NAMES ||
	    binder.GetBindingMode() == BindingMode::EXTRACT_QUALIFIED_NAMES) {
		//! We only care about extracting the names of the referenced columns
		//! remove the exclude + replace lists
		expr.ExcludeListMutable().clear();
		expr.ReplaceListMutable().clear();
	}

	//! Verify correctness of the exclude list
	for (auto &excluded : expr.ExcludeList()) {
		if (star_state.excluded_qualified_columns.find(excluded) == star_state.excluded_qualified_columns.end()) {
			auto candidate_str = CandidateColumnMessage(star_state.candidate_columns, excluded.column);
			throw BinderException("Column %s in EXCLUDE list not found in %s%s", excluded.ToDisplayString(),
			                      expr.RelationName().empty() ? "FROM clause" : expr.RelationName().c_str(),
			                      candidate_str);
		}
	}

	//! Verify correctness of the replace list
	for (auto &entry : expr.ReplaceList()) {
		if (star_state.excluded_columns.find(entry.first) == star_state.excluded_columns.end()) {
			auto candidate_str = CandidateColumnMessage(star_state.candidate_columns, entry.first);
			throw BinderException("Column %s in REPLACE list not found in %s%s", entry.first,
			                      expr.RelationName().empty() ? string("FROM clause")
			                                                  : expr.RelationName().GetIdentifierName(),
			                      candidate_str);
		}
	}
}

void BindContext::GetTypesAndNames(vector<Identifier> &result_names, vector<LogicalType> &result_types) {
	for (auto &binding_entry : bindings_list) {
		auto &binding = *binding_entry;
		auto &column_names = binding.GetColumnNames();
		auto &column_types = binding.GetColumnTypes();
		D_ASSERT(column_names.size() == column_types.size());
		for (idx_t i = 0; i < column_names.size(); i++) {
			result_names.push_back(column_names[i]);
			result_types.push_back(column_types[i]);
		}
	}
}

void BindContext::AddBinding(unique_ptr<Binding> binding) {
	bindings_list.push_back(std::move(binding));
}

void BindContext::AddBaseTable(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
                               const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids,
                               TableCatalogEntry &entry, virtual_column_map_t virtual_columns) {
	AddBinding(
	    make_uniq<TableBinding>(alias, types, names, bound_column_ids, &entry, index, std::move(virtual_columns)));
}

void BindContext::AddBaseTable(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
                               const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids,
                               TableCatalogEntry &entry, bool add_virtual_columns) {
	virtual_column_map_t virtual_columns;
	if (add_virtual_columns) {
		virtual_columns = entry.GetVirtualColumns();
	}
	AddBaseTable(index, alias, names, types, bound_column_ids, entry, std::move(virtual_columns));
}

void BindContext::AddBaseTable(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
                               const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids,
                               const Identifier &table_name) {
	virtual_column_map_t virtual_columns;
	AddBinding(make_uniq<TableBinding>(alias.empty() ? table_name : alias, types, names, bound_column_ids, nullptr,
	                                   index, std::move(virtual_columns)));
}

void BindContext::AddTableFunction(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
                                   const vector<LogicalType> &types, vector<ColumnIndex> &bound_column_ids,
                                   optional_ptr<StandardEntry> entry, virtual_column_map_t virtual_columns) {
	AddBinding(
	    make_uniq<TableBinding>(alias, types, names, bound_column_ids, entry, index, std::move(virtual_columns)));
}

static Identifier AddColumnNameToBinding(const Identifier &base_name, identifier_set_t &current_names) {
	idx_t index = 1;
	Identifier name = base_name;
	while (current_names.find(name) != current_names.end()) {
		name = Identifier(base_name + "_" + std::to_string(index++));
	}
	current_names.insert(name);
	return name;
}

vector<Identifier> BindContext::AliasColumnNames(const Identifier &table_name, const vector<Identifier> &names,
                                                 const vector<Identifier> &column_aliases) {
	vector<Identifier> result;
	if (column_aliases.size() > names.size()) {
		throw BinderException("table %s has %lld columns available but %lld columns specified", table_name,
		                      names.size(), column_aliases.size());
	}
	identifier_set_t current_names;
	// use any provided column aliases first
	for (idx_t i = 0; i < column_aliases.size(); i++) {
		result.push_back(AddColumnNameToBinding(column_aliases[i], current_names));
	}
	// if not enough aliases were provided, use the default names for remaining columns
	for (idx_t i = column_aliases.size(); i < names.size(); i++) {
		result.push_back(AddColumnNameToBinding(names[i], current_names));
	}
	return result;
}

void BindContext::AddSubquery(TableIndex index, const Identifier &alias, SubqueryRef &ref, BoundStatement &subquery) {
	auto names = AliasColumnNames(alias, subquery.names, ref.column_name_alias);
	AddGenericBinding(index, alias, names, subquery.types);
}

void BindContext::AddEntryBinding(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
                                  const vector<LogicalType> &types, StandardEntry &entry) {
	AddBinding(make_uniq<EntryBinding>(alias, types, names, index, entry));
}

void BindContext::AddView(TableIndex index, const Identifier &alias, SubqueryRef &ref, BoundStatement &subquery,
                          ViewCatalogEntry &view) {
	auto names = AliasColumnNames(alias, subquery.names, ref.column_name_alias);
	AddEntryBinding(index, alias, names, subquery.types, view.Cast<StandardEntry>());
}

void BindContext::AddSubquery(TableIndex index, const Identifier &alias, TableFunctionRef &ref,
                              BoundStatement &subquery) {
	auto names = AliasColumnNames(alias, subquery.names, ref.column_name_alias);
	AddGenericBinding(index, alias, names, subquery.types);
}

void BindContext::AddGenericBinding(TableIndex index, const Identifier &alias, const vector<Identifier> &names,
                                    const vector<LogicalType> &types) {
	AddBinding(make_uniq<Binding>(BindingType::BASE, BindingAlias(alias), types, names, index));
}

void BindContext::AddColumnAlias(TableIndex index, const Identifier &column_alias, column_t column_index) {
	for (auto &binding : bindings_list) {
		if (binding->GetIndex() == index) {
			binding->AddColumnAlias(column_alias, column_index);
			return;
		}
	}
	throw InternalException("AddColumnAlias - no binding found with the given table index");
}

void BindContext::AddCTEBinding(unique_ptr<CTEBinding> binding) {
	for (auto &cte_binding : cte_bindings) {
		if (cte_binding->GetBindingAlias() == binding->GetBindingAlias()) {
			throw BinderException("Duplicate CTE binding \"%s\" in query!", binding->GetBindingAlias().ToString());
		}
	}
	cte_bindings.push_back(std::move(binding));
}

void BindContext::AddCTEBinding(TableIndex index, BindingAlias alias_p, const vector<Identifier> &names,
                                const vector<LogicalType> &types, CTEType cte_type) {
	auto binding = make_uniq<CTEBinding>(std::move(alias_p), types, names, index, cte_type);
	AddCTEBinding(std::move(binding));
}

optional_ptr<CTEBinding> BindContext::GetCTEBinding(const BindingAlias &ctename) {
	for (auto &binding : cte_bindings) {
		if (binding->GetBindingAlias().Matches(ctename)) {
			return binding.get();
		}
	}
	return nullptr;
}

void BindContext::AddContext(BindContext other) {
	for (auto &binding : other.bindings_list) {
		AddBinding(std::move(binding));
	}
	for (auto &entry : other.using_columns) {
		for (auto &alias : entry.second) {
			using_columns[entry.first].insert(alias);
		}
	}
}

vector<BindingAlias> BindContext::GetBindingAliases() {
	vector<BindingAlias> result;
	for (auto &binding : bindings_list) {
		result.push_back(binding->GetBindingAlias());
	}
	return result;
}

void BindContext::RemoveContext(const vector<BindingAlias> &aliases) {
	for (auto &alias : aliases) {
		// remove the binding from any USING columns
		vector<Identifier> removed_using_columns;
		for (auto &using_sets : using_columns) {
			for (auto &using_set_ref : using_sets.second) {
				auto &using_set = using_set_ref.get();
				auto it = std::remove_if(using_set.bindings.begin(), using_set.bindings.end(),
				                         [&](const BindingAlias &using_alias) { return using_alias == alias; });
				using_set.bindings.erase(it, using_set.bindings.end());
				if (using_set.bindings.empty() || using_set.primary_binding == alias) {
					// if the using column is no longer referred to - remove it entirely
					removed_using_columns.push_back(using_sets.first);
				}
			}
		}
		for (auto &removed_col : removed_using_columns) {
			using_columns.erase(removed_col);
		}

		// remove the binding from the list of bindings
		auto it = std::remove_if(bindings_list.begin(), bindings_list.end(),
		                         [&](unique_ptr<Binding> &x) { return x->GetBindingAlias() == alias; });
		bindings_list.erase(it, bindings_list.end());
	}
}

} // namespace duckdb
