#include "duckdb/planner/binder.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/common/enums/cte_materialize.hpp"
#include "duckdb/common/enums/trigger_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/returning_binder.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_trigger.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

#include <algorithm>
#include <unordered_set>

namespace duckdb {

// Defined in binder.cpp — rejects 'excluded'-qualified columns in RETURNING, matching the no-trigger path.
void VerifyNotExcluded(const ParsedExpression &root_expr);

unique_ptr<BoundStatement> Binder::TryExpandTriggers(QueryNode &node, TableCatalogEntry &table,
                                                     TriggerEventType event_type) {
	D_ASSERT(node.type == QueryNodeType::INSERT_QUERY_NODE || node.type == QueryNodeType::UPDATE_QUERY_NODE ||
	         node.type == QueryNodeType::DELETE_QUERY_NODE);
	auto &expanded_tables = global_binder_state->trigger_expanded_tables;
	if (expanded_tables.find(table) != expanded_tables.end()) {
		if (global_binder_state->trigger_creation_table == &table) {
			throw NotImplementedException("Recursive trigger chains are not yet supported (trigger cycle detected "
			                              "through trigger %s on table %s)",
			                              global_binder_state->trigger_creation_name, table.name);
		}
		return nullptr;
	}
	auto txn = table.ParentCatalog().GetCatalogTransaction(context);
	// FOR EACH ROW triggers are expanded separately via TryExpandRowTriggers; this path only fires statement triggers.
	auto before_triggers = table.GetTriggersForEvent(txn, TriggerTiming::BEFORE, event_type, TriggerForEach::STATEMENT);
	auto after_triggers = table.GetTriggersForEvent(txn, TriggerTiming::AFTER, event_type, TriggerForEach::STATEMENT);

	// UPDATE OF <cols>: drop triggers whose OF list is disjoint from the SET list.
	// Triggers without an OF list are unrestricted and always fire.
	if (event_type == TriggerEventType::UPDATE_EVENT && node.type == QueryNodeType::UPDATE_QUERY_NODE) {
		auto &update_node = node.Cast<UpdateQueryNode>();
		identifier_set_t updated_columns;
		if (update_node.set_info) {
			updated_columns.insert(update_node.set_info->columns.begin(), update_node.set_info->columns.end());
		}
		auto trigger_does_not_fire = [&](const_reference<TriggerCatalogEntry> trig) {
			const auto &of_cols = trig.get().columns;
			return !of_cols.empty() && std::none_of(of_cols.begin(), of_cols.end(),
			                                        [&](const Identifier &c) { return updated_columns.count(c) > 0; });
		};
		auto drop_non_firing = [&](vector<const_reference<TriggerCatalogEntry>> &triggers) {
			triggers.erase(std::remove_if(triggers.begin(), triggers.end(), trigger_does_not_fire), triggers.end());
		};
		drop_non_firing(before_triggers);
		drop_non_firing(after_triggers);
	}

	if (before_triggers.empty() && after_triggers.empty()) {
		return nullptr;
	}
	if (node.type == QueryNodeType::INSERT_QUERY_NODE) {
		auto &insert_node = node.Cast<InsertQueryNode>();
		if (insert_node.on_conflict_info && insert_node.on_conflict_info->action_type != OnConflictAction::NOTHING) {
			// Only AFTER triggers can carry REFERENCING NEW TABLE — BEFORE rejects it at CREATE time.
			for (auto &trigger : after_triggers) {
				if (!trigger.get().referencing_new_table.empty()) {
					throw NotImplementedException(
					    "ON CONFLICT DO UPDATE is not yet supported with REFERENCING NEW TABLE AS triggers");
				}
			}
		}
	}
	expanded_tables.insert(table);
	auto bound = ExpandTriggers(node, table, before_triggers, after_triggers);

	// Only track this table as expanded for the duration of its own subtree in the trigger chain.
	expanded_tables.erase(table);
	return make_uniq<BoundStatement>(std::move(bound));
}

static constexpr const char *TRIGGER_BASE_CTE_PREFIX = "__duckdb_trigger_base_";
static constexpr const char *TRIGGER_BODY_CTE_PREFIX = "__duckdb_trigger_body_";
static constexpr const char *TRIGGER_BEFORE_BODY_CTE_PREFIX = "__duckdb_trigger_before_body_";

static unique_ptr<CommonTableExpressionInfo> MakeTransitionTableAliasCTE(const Identifier &base_cte_name,
                                                                         const identifier_set_t &exclude_columns) {
	auto alias_cte = make_uniq<CommonTableExpressionInfo>();
	auto alias_select = make_uniq<SelectNode>();
	auto star = make_uniq<StarExpression>();
	// Hide injected virtual columns from trigger bodies that SELECT * over the transition table.
	for (auto &name : exclude_columns) {
		star->ExcludeListMutable().insert(QualifiedColumnName(name));
	}
	alias_select->select_list.push_back(std::move(star));
	auto alias_ref = make_uniq<BaseTableRef>();
	alias_ref->SetTable(base_cte_name);
	alias_select->from_table = std::move(alias_ref);
	alias_cte->query_node = std::move(alias_select);
	alias_cte->materialized = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
	return alias_cte;
}

// Reserved names under which the base UPDATE CTE exposes the captured OLD physical columns, one per physical
// column in table order. Computed once per expansion and threaded to every consumer (the base UPDATE binding,
// the OLD transition alias, and the NEW-alias exclude set), so no layer re-derives the contract independently.
// The prefix is extended until it is not a case-insensitive prefix of any real column name, so the reserved
// names cannot collide with a user column (which would otherwise make the OLD alias silently read the NEW value).
static vector<Identifier> OldCaptureColumnNames(const TableCatalogEntry &table) {
	string prefix = "__duckdb_old_col_";
	bool collides = true;
	while (collides) {
		collides = false;
		for (auto &column : table.GetColumns().Logical()) {
			auto &name = column.Name();
			if (name.size() >= prefix.size() &&
			    StringUtil::CIEquals(name.c_str(), prefix.size(), prefix.c_str(), prefix.size())) {
				prefix += "_";
				collides = true;
				break;
			}
		}
	}
	vector<Identifier> names;
	idx_t physical_index = 0;
	for (auto &column : table.GetColumns().Physical()) {
		(void)column;
		names.push_back(Identifier(prefix + to_string(physical_index)));
		physical_index++;
	}
	return names;
}

// Recursively replace references to generated columns with a copy of their generated expression, so the result
// references only physical columns. This makes the recomputation independent of generated-column declaration
// order (a generated column may reference another generated column declared later).
static void InlineGeneratedColumnRefs(unique_ptr<ParsedExpression> &expr, const TableCatalogEntry &table) {
	if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (!colref.IsQualified() && table.GetColumns().ColumnExists(colref.GetColumnName())) {
			auto &column = table.GetColumns().GetColumn(colref.GetColumnName());
			if (column.Generated()) {
				expr = column.GeneratedExpression().Copy();
				InlineGeneratedColumnRefs(expr, table);
				return;
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { InlineGeneratedColumnRefs(child, table); });
}

// OLD transition alias for UPDATE: the base CTE exposes the pre-update physical columns under internal names.
// Rename them back to the real column names and (re)compute generated columns over those OLD values. The result
// exposes exactly the logical table columns, in table order — no rowid, physical-only, or capture columns.
static unique_ptr<CommonTableExpressionInfo> MakeUpdateOldAliasCTE(const TableCatalogEntry &table,
                                                                   const Identifier &base_cte_name,
                                                                   const vector<Identifier> &old_capture_names) {
	// Inner select: rename each captured OLD physical column (exposed under its reserved name) to its real name.
	auto inner = make_uniq<SelectNode>();
	idx_t physical_index = 0;
	for (auto &column : table.GetColumns().Physical()) {
		auto colref = make_uniq<ColumnRefExpression>(old_capture_names[physical_index]);
		colref->SetAlias(column.Name());
		inner->select_list.push_back(std::move(colref));
		physical_index++;
	}
	auto inner_from = make_uniq<BaseTableRef>();
	inner_from->SetTable(base_cte_name);
	inner->from_table = std::move(inner_from);

	auto alias_cte = make_uniq<CommonTableExpressionInfo>();
	alias_cte->materialized = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;

	if (!table.HasGeneratedColumns()) {
		// The renamed physical columns already are the full logical row, in table order.
		alias_cte->query_node = std::move(inner);
		return alias_cte;
	}

	// Wrap the renamed OLD image so generated columns can be recomputed over it, producing every logical column.
	auto inner_stmt = make_uniq<SelectStatement>();
	inner_stmt->node = std::move(inner);
	auto subquery = make_uniq<SubqueryRef>(std::move(inner_stmt));

	auto outer = make_uniq<SelectNode>();
	for (auto &column : table.GetColumns().Logical()) {
		if (column.Generated()) {
			auto expr = column.GeneratedExpression().Copy();
			InlineGeneratedColumnRefs(expr, table);
			expr->SetAlias(column.Name());
			outer->select_list.push_back(std::move(expr));
		} else {
			outer->select_list.push_back(make_uniq<ColumnRefExpression>(column.Name()));
		}
	}
	outer->from_table = std::move(subquery);
	alias_cte->query_node = std::move(outer);
	return alias_cte;
}

static vector<unique_ptr<ParsedExpression>> &GetDMLReturningList(QueryNode &node) {
	switch (node.type) {
	case QueryNodeType::INSERT_QUERY_NODE:
		return node.Cast<InsertQueryNode>().returning_list;
	case QueryNodeType::UPDATE_QUERY_NODE:
		return node.Cast<UpdateQueryNode>().returning_list;
	case QueryNodeType::DELETE_QUERY_NODE:
		return node.Cast<DeleteQueryNode>().returning_list;
	default:
		throw InternalException("GetDMLReturningList: unexpected node type");
	}
}

static Identifier GetTableAlias(const QueryNode &node, const Identifier &fallback) {
	switch (node.type) {
	case QueryNodeType::INSERT_QUERY_NODE: {
		auto &n = node.Cast<InsertQueryNode>();
		if (n.table_ref && !n.table_ref->alias.empty()) {
			return n.table_ref->alias;
		}
		return fallback;
	}
	case QueryNodeType::UPDATE_QUERY_NODE: {
		auto &n = node.Cast<UpdateQueryNode>();
		if (n.table && !n.table->alias.empty()) {
			return n.table->alias;
		}
		return fallback;
	}
	case QueryNodeType::DELETE_QUERY_NODE: {
		auto &n = node.Cast<DeleteQueryNode>();
		if (n.table && !n.table->alias.empty()) {
			return n.table->alias;
		}
		return fallback;
	}
	default:
		D_ASSERT(false);
		return fallback;
	}
}

// Raw table_ref alias (empty if none) — an empty alias lets the binding derive
// catalog.schema.table from the entry, so qualified RETURNING columns resolve.
static Identifier GetReturningAlias(const QueryNode &node) {
	switch (node.type) {
	case QueryNodeType::INSERT_QUERY_NODE:
		return node.Cast<InsertQueryNode>().table_ref ? node.Cast<InsertQueryNode>().table_ref->alias : Identifier();
	case QueryNodeType::UPDATE_QUERY_NODE:
		return node.Cast<UpdateQueryNode>().table ? node.Cast<UpdateQueryNode>().table->alias : Identifier();
	case QueryNodeType::DELETE_QUERY_NODE:
		return node.Cast<DeleteQueryNode>().table ? node.Cast<DeleteQueryNode>().table->alias : Identifier();
	default:
		return Identifier();
	}
}

// Virtual columns exposed to RETURNING — only DELETE, mirroring the no-trigger path.
static virtual_column_map_t ReturningVirtualColumns(const QueryNode &node, const TableCatalogEntry &table) {
	if (node.type == QueryNodeType::DELETE_QUERY_NODE) {
		return table.GetVirtualColumns();
	}
	return virtual_column_map_t();
}

// Virtual columns the RETURNING list references, so the base CTE can materialise them (RETURNING *
// does not include rowid).  Matched on the trailing identifier — the only table in scope is the
// target.  Returns (id, name) pairs, deduplicated.
static vector<pair<column_t, Identifier>> ReferencedVirtualColumns(vector<unique_ptr<ParsedExpression>> &returning_list,
                                                                   const virtual_column_map_t &virtual_cols,
                                                                   const TableCatalogEntry &table) {
	vector<pair<column_t, Identifier>> result;
	if (virtual_cols.empty()) {
		return result;
	}
	// A virtual column shadowed by a real column of the same name is not virtual here: RETURNING *
	// already materialises the real column, and injecting would duplicate the name.
	identifier_set_t real_names;
	for (auto &col : table.GetColumns().Logical()) {
		real_names.insert(col.Name());
	}
	unordered_set<column_t> seen;
	for (auto &expr : returning_list) {
		ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(*expr, [&](const ColumnRefExpression &cr) {
			auto &cn = cr.ColumnNames();
			if (cn.empty()) {
				return;
			}
			for (auto &vc : virtual_cols) {
				if (real_names.find(vc.second.name) != real_names.end()) {
					continue;
				}
				if (cn.back() == vc.second.name && seen.insert(vc.first).second) {
					result.emplace_back(vc.first, vc.second.name);
				}
			}
		});
	}
	return result;
}

static void AddAfterTriggerCTEs(SelectNode &outer, const vector<const_reference<TriggerCatalogEntry>> &after_triggers,
                                const Identifier &base_cte_name, const string &uuid_suffix,
                                const identifier_set_t &injected_names, const TableCatalogEntry &table,
                                const vector<Identifier> &old_capture_names) {
	// For UPDATE with OLD capture the base CTE also carries the reserved OLD columns; keep them out of the NEW alias.
	bool update_old_capture = !old_capture_names.empty();
	identifier_set_t new_alias_exclude = injected_names;
	new_alias_exclude.insert(old_capture_names.begin(), old_capture_names.end());

	for (idx_t i = 0; i < after_triggers.size(); i++) {
		auto &trigger = after_triggers[i].get();
		Identifier body_cte_name(string(TRIGGER_BODY_CTE_PREFIX) + to_string(i + 1) + "_" + uuid_suffix);

		auto trig_cte = make_uniq<CommonTableExpressionInfo>();
		trig_cte->query_node = trigger.trigger_action->Copy();
		trig_cte->materialized = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
		trig_cte->is_trigger_generated = true;

		// Scoped to this body so sibling triggers can't see the alias.
		auto &body_map = trig_cte->query_node->cte_map.map;
		if (!trigger.referencing_new_table.empty() && body_map.find(trigger.referencing_new_table) == body_map.end()) {
			body_map[trigger.referencing_new_table] = MakeTransitionTableAliasCTE(base_cte_name, new_alias_exclude);
		}
		if (!trigger.referencing_old_table.empty() && body_map.find(trigger.referencing_old_table) == body_map.end()) {
			// UPDATE captures OLD as reserved columns that must be renamed; INSERT/DELETE expose it as-is.
			body_map[trigger.referencing_old_table] =
			    update_old_capture ? MakeUpdateOldAliasCTE(table, base_cte_name, old_capture_names)
			                       : MakeTransitionTableAliasCTE(base_cte_name, injected_names);
		}

		outer.cte_map.map[body_cte_name] = std::move(trig_cte);
	}
}

// Build the trigger CTE chain as a SelectNode: BEFORE bodies → base DML → AFTER bodies.
// For RETURNING the outer projection is a plain SELECT *, rebound through ReturningBinder later.
static unique_ptr<SelectNode> BuildTriggerChain(const QueryNode &node, const TableCatalogEntry &table,
                                                const vector<const_reference<TriggerCatalogEntry>> &before_triggers,
                                                const vector<const_reference<TriggerCatalogEntry>> &after_triggers,
                                                const string &uuid_suffix, const Identifier &base_cte_name,
                                                bool has_returning,
                                                const vector<pair<column_t, Identifier>> &injected_virtuals,
                                                const vector<Identifier> &old_capture_names,
                                                optional_ptr<QueryNode> &out_capture_base) {
	auto outer = make_uniq<SelectNode>();
	if (has_returning) {
		outer->select_list.push_back(make_uniq<StarExpression>());
	} else {
		outer->select_list.push_back(
		    make_uniq<FunctionExpression>("count_star", vector<unique_ptr<ParsedExpression>>()));
	}
	auto from_ref = make_uniq<BaseTableRef>();
	from_ref->SetTable(base_cte_name);
	from_ref->alias = GetTableAlias(node, table.name);
	outer->from_table = std::move(from_ref);

	// BEFORE bodies (no transition tables — REFERENCING is rejected at CREATE time).
	for (idx_t i = 0; i < before_triggers.size(); i++) {
		Identifier body_cte_name(string(TRIGGER_BEFORE_BODY_CTE_PREFIX) + to_string(i + 1) + "_" + uuid_suffix);
		auto trig_cte = make_uniq<CommonTableExpressionInfo>();
		trig_cte->query_node = before_triggers[i].get().trigger_action->Copy();
		trig_cte->materialized = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
		trig_cte->is_trigger_generated = true;
		outer->cte_map.map[body_cte_name] = std::move(trig_cte);
	}

	// Base CTE: the DML with RETURNING * (full rows for transition tables), plus referenced
	// virtual columns appended so they are materialised for the RETURNING projection.
	auto base_node = node.Copy();
	auto &base_returning = GetDMLReturningList(*base_node);
	base_returning.push_back(make_uniq<StarExpression>());
	identifier_set_t injected_names;
	for (auto &vc : injected_virtuals) {
		base_returning.push_back(make_uniq<ColumnRefExpression>(vc.second));
		injected_names.insert(vc.second);
	}
	auto base_cte = make_uniq<CommonTableExpressionInfo>();
	base_cte->query_node = std::move(base_node);
	base_cte->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	base_cte->is_trigger_generated = true;
	// Expose the base UPDATE node so ExpandTriggers can flag it for OLD capture in scoped binder state
	// (instead of a transient field on the parsed AST).
	if (!old_capture_names.empty()) {
		out_capture_base = base_cte->query_node.get();
	}
	outer->cte_map.map[base_cte_name] = std::move(base_cte);

	AddAfterTriggerCTEs(*outer, after_triggers, base_cte_name, uuid_suffix, injected_names, table, old_capture_names);
	return outer;
}

BoundStatement Binder::ExpandTriggers(QueryNode &node, TableCatalogEntry &table,
                                      const vector<const_reference<TriggerCatalogEntry>> &before_triggers,
                                      const vector<const_reference<TriggerCatalogEntry>> &after_triggers) {
	D_ASSERT(!before_triggers.empty() || !after_triggers.empty());
	D_ASSERT(node.type == QueryNodeType::INSERT_QUERY_NODE || node.type == QueryNodeType::UPDATE_QUERY_NODE ||
	         node.type == QueryNodeType::DELETE_QUERY_NODE);

	// Drain before copying so the base CTE copy's returning_list starts empty.
	auto user_returning_list = std::move(GetDMLReturningList(node));
	bool has_returning = !user_returning_list.empty();

	auto virtual_columns = ReturningVirtualColumns(node, table);
	auto injected_virtuals = has_returning ? ReferencedVirtualColumns(user_returning_list, virtual_columns, table)
	                                       : vector<pair<column_t, Identifier>>();

	auto uuid_suffix = UUID::ToString(UUID::GenerateRandomUUID());
	Identifier base_cte_name(TRIGGER_BASE_CTE_PREFIX + uuid_suffix);

	// Capture the pre-update (OLD) image when this is an UPDATE and at least one AFTER trigger references OLD.
	// The reserved OLD-capture column names are computed once here and threaded to every consumer.
	vector<Identifier> old_capture_names;
	if (node.type == QueryNodeType::UPDATE_QUERY_NODE &&
	    std::any_of(after_triggers.begin(), after_triggers.end(),
	                [](const_reference<TriggerCatalogEntry> t) { return !t.get().referencing_old_table.empty(); })) {
		old_capture_names = OldCaptureColumnNames(table);
	}

	optional_ptr<QueryNode> capture_base;
	auto outer = BuildTriggerChain(node, table, before_triggers, after_triggers, uuid_suffix, base_cte_name,
	                               has_returning, injected_virtuals, old_capture_names, capture_base);
	// Request OLD capture on the generated base UPDATE in scoped binder state, active only while binding this
	// chain. BindNode(UpdateQueryNode) reads the request by node identity, so it never touches the parsed AST.
	// The reserved CTE presentation names are separate, optional state for the statement-trigger CTE consumer.
	if (capture_base) {
		global_binder_state->trigger_old_capture.insert(*capture_base);
		global_binder_state->trigger_old_capture_cte_names[*capture_base] = old_capture_names;
	}
	auto chain = Bind(*outer);
	if (capture_base) {
		global_binder_state->trigger_old_capture.erase(*capture_base);
		global_binder_state->trigger_old_capture_cte_names.erase(*capture_base);
	}

	if (!has_returning) {
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::CHANGED_ROWS;
		return chain;
	}

	// SELECT * over the base CTE yields a projection with contiguous bindings (index, 0..n-1), so we
	// bind the RETURNING list against that index directly as an identity base-table binding.
	auto child_bindings = chain.plan->GetColumnBindings();
	D_ASSERT(!child_bindings.empty() && child_bindings.size() == chain.types.size());
	auto base_index = child_bindings[0].table_index;

	// Bind RETURNING with ReturningBinder over a base-table binding of the real table — same rules as
	// the no-trigger path (rejects aggregates/subqueries/windows, resolves qualified columns, virtual
	// vs real rowid).  bound_columns is the identity over all logical columns (generated included)
	// because our RETURNING * child materialises every logical column in order.
	auto returning_binder_owner = Binder::CreateBinder(context);
	vector<Identifier> col_names;
	vector<LogicalType> col_types;
	vector<ColumnIndex> bound_columns;
	idx_t logical_count = 0;
	for (auto &col : table.GetColumns().Logical()) {
		col_names.push_back(col.Name());
		col_types.push_back(col.Type());
		bound_columns.emplace_back(logical_count);
		logical_count++;
	}
	returning_binder_owner->bind_context.AddBaseTable(base_index, GetReturningAlias(node), col_names, col_types,
	                                                  bound_columns, table, std::move(virtual_columns));
	ReturningBinder returning_binder(*returning_binder_owner, context);

	vector<unique_ptr<ParsedExpression>> expanded_returning;
	returning_binder_owner->ExpandStarExpressions(user_returning_list, expanded_returning);
	if (expanded_returning.empty()) {
		throw BinderException("RETURNING list is empty!");
	}
	BoundStatement result;
	vector<unique_ptr<Expression>> proj_exprs;
	for (auto &returning_expr : expanded_returning) {
		VerifyNotExcluded(*returning_expr);
		LogicalType result_type;
		auto bound_expr = returning_binder.Bind(returning_expr, &result_type);
		result.names.push_back(bound_expr->GetName());
		result.types.push_back(result_type);
		proj_exprs.push_back(std::move(bound_expr));
	}

	// Virtual columns bind to a sentinel id, but the chain materialised them as trailing columns at
	// logical_count + k.  Remap those bindings to their real positions.
	if (!injected_virtuals.empty()) {
		unordered_map<column_t, idx_t> remap;
		for (idx_t k = 0; k < injected_virtuals.size(); k++) {
			remap[injected_virtuals[k].first] = logical_count + k;
		}
		for (auto &expr : proj_exprs) {
			ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
			    expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &) {
				    auto &binding = colref.BindingMutable();
				    if (binding.table_index != base_index) {
					    return;
				    }
				    auto it = remap.find(binding.column_index.GetIndexUnsafe());
				    if (it != remap.end()) {
					    binding.column_index = ProjectionIndex(it->second);
				    }
			    });
		}
	}

	auto returning_projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(proj_exprs));
	returning_projection->AddChild(std::move(chain.plan));
	result.plan = std::move(returning_projection);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

static bool BoundBodyTargetsTable(const LogicalOperator &op, const TableCatalogEntry &table) {
	if (op.type == LogicalOperatorType::LOGICAL_INSERT) {
		return &op.Cast<LogicalInsert>().table == &table;
	}
	if (op.type == LogicalOperatorType::LOGICAL_DELETE) {
		return &op.Cast<LogicalDelete>().table == &table;
	}
	if (op.type == LogicalOperatorType::LOGICAL_UPDATE) {
		return &op.Cast<LogicalUpdate>().table == &table;
	}
	for (auto &child : op.children) {
		if (child && BoundBodyTargetsTable(*child, table)) {
			return true;
		}
	}
	return false;
}

// Detects a nested LogicalTrigger in a bound trigger body, which means the body targets a table that itself has a
// FOR EACH ROW trigger (a cascade). Cascades are rejected for now for two reasons:
//  1. Over-firing: the inner trigger's affected-row set gets cross-joined with the outer rows during decorrelation,
//     so it fires once per (outer row x inner row) instead of once per inner row.
//  2. all firings run as one set-based batch against a single snapshot, so a downstream trigger cannot
//     see rows an upstream trigger wrote earlier in the same statement. Correct semantics need per-row execution.
bool BoundBodyContainsTrigger(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_TRIGGER) {
		return true;
	}
	for (auto &child : op.children) {
		if (child && BoundBodyContainsTrigger(*child)) {
			return true;
		}
	}
	return false;
}

unique_ptr<BoundStatement> Binder::TryExpandRowTriggers(QueryNode &node,
                                                        vector<unique_ptr<ParsedExpression>> &returning_list,
                                                        TableCatalogEntry &table, TriggerEventType event_type) {
	auto &expanded_tables = global_binder_state->trigger_expanded_tables;
	if (expanded_tables.find(table) != expanded_tables.end()) {
		return nullptr;
	}
	auto triggers = table.GetTriggersForEvent(table.ParentCatalog().GetCatalogTransaction(context),
	                                          TriggerTiming::AFTER, event_type, TriggerForEach::ROW);
	if (triggers.empty()) {
		return nullptr;
	}
	if (node.type == QueryNodeType::INSERT_QUERY_NODE && node.Cast<InsertQueryNode>().on_conflict_info) {
		// Updated rows via ON CONFLICT currently appear in the INSERT affected-row set. This would fire INSERT triggers
		// for those rows. Therefore, the combination is currently rejected.
		throw NotImplementedException("ON CONFLICT is not yet supported on tables with FOR EACH ROW triggers");
	}
	if (!returning_list.empty()) {
		throw NotImplementedException("RETURNING is not yet supported on tables with FOR EACH ROW triggers");
	}
	expanded_tables.insert(table);
	auto bound = ExpandRowTriggers(node, returning_list, table, triggers, event_type);
	expanded_tables.erase(table);
	return make_uniq<BoundStatement>(std::move(bound));
}

string Binder::RowScopeName(TriggerEventType event_type) {
	switch (event_type) {
	case TriggerEventType::DELETE_EVENT:
		return "old";
	default:
		return "new";
	}
}

unique_ptr<ExpressionBinder> Binder::SetupRowScope(TableIndex table_index, const vector<Identifier> &col_names,
                                                   const vector<LogicalType> &col_types, const string &scope_name) {
	bind_context.AddGenericBinding(table_index, Identifier(scope_name), col_names, col_types);
	auto scope_binder = make_uniq<ExpressionBinder>(*this, context);
	PushScope(*scope_binder);
	return scope_binder;
}

BoundStatement Binder::ExpandRowTriggers(QueryNode &node, vector<unique_ptr<ParsedExpression>> &returning_list,
                                         const TableCatalogEntry &table,
                                         const vector<const_reference<TriggerCatalogEntry>> &triggers,
                                         TriggerEventType event_type) {
	D_ASSERT(!triggers.empty());
	D_ASSERT(returning_list.empty());
	returning_list.push_back(make_uniq<StarExpression>());

	auto uuid_suffix = UUID::ToString(UUID::GenerateRandomUUID());
	Identifier base_cte_name(TRIGGER_BASE_CTE_PREFIX + uuid_suffix);

	auto base_cte = make_uniq<CommonTableExpressionInfo>();
	base_cte->query_node = node.Copy();
	base_cte->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	base_cte->is_trigger_generated = true;

	auto outer = make_uniq<SelectNode>();
	outer->select_list.push_back(make_uniq<FunctionExpression>("count_star", vector<unique_ptr<ParsedExpression>>()));
	auto from_ref = make_uniq<BaseTableRef>();
	from_ref->SetTable(base_cte_name);
	outer->from_table = std::move(from_ref);
	outer->cte_map.map[base_cte_name] = std::move(base_cte);

	auto bound = Bind(*outer);
	auto &base_mat_cte = bound.plan->Cast<LogicalMaterializedCTE>();
	auto cte_table_idx = base_mat_cte.table_index;

	// proj_idx is the binding source for NEW.col (INSERT) / OLD.col (DELETE) refs in trigger bodies.
	vector<Identifier> col_names;
	vector<LogicalType> col_types;
	for (auto &col : table.GetColumns().Physical()) {
		col_names.push_back(col.GetName());
		col_types.push_back(col.GetType());
	}

	auto cte_ref_idx = GenerateTableIndex();
	auto cte_ref = make_uniq<LogicalCTERef>(cte_ref_idx, cte_table_idx, col_types, col_names, false);
	cte_ref->ResolveOperatorTypes();

	auto proj_idx = GenerateTableIndex(); // the table_index for NEW/OLD bindings
	vector<unique_ptr<Expression>> proj_exprs;
	for (idx_t i = 0; i < col_types.size(); i++) {
		proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(col_names[i], col_types[i],
		                                                         ColumnBinding(cte_ref_idx, ProjectionIndex(i))));
	}
	auto new_rows_proj = make_uniq<LogicalProjection>(proj_idx, std::move(proj_exprs));
	new_rows_proj->children.push_back(std::move(cte_ref));
	new_rows_proj->ResolveOperatorTypes();

	auto row_scope_name = RowScopeName(event_type);
	auto new_scope_binder = SetupRowScope(proj_idx, col_names, col_types, row_scope_name);

	unique_ptr<LogicalOperator> trigger_plan = std::move(new_rows_proj);
	for (idx_t i = 0; i < triggers.size(); i++) {
		auto &trigger = triggers[i].get();

		auto child_binder = Binder::CreateBinder(context, this);
		auto body_copy = trigger.trigger_action->Copy();
		auto bound_body = child_binder->Bind(*body_copy);

		CorrelatedColumns corr_cols = std::move(child_binder->correlated_columns);
		if (corr_cols.empty()) {
			throw BinderException("FOR EACH ROW trigger %s on table %s must reference at least one NEW or OLD "
			                      "column in the trigger body (use FOR EACH STATEMENT if row data is not needed)",
			                      trigger.name, table.name);
		}

		if (BoundBodyTargetsTable(*bound_body.plan, table)) {
			throw BinderException("FOR EACH ROW trigger %s on table %s writes to the trigger table "
			                      "(self-referential triggers are not supported)",
			                      trigger.name, table.name);
		}

		if (BoundBodyContainsTrigger(*bound_body.plan)) {
			throw NotImplementedException("FOR EACH ROW trigger %s on table %s writes to a table that has its "
			                              "own FOR EACH ROW trigger (cascading row triggers are not yet supported)",
			                              trigger.name, table.name);
		}

		auto logi_trig = make_uniq<LogicalTrigger>(trigger.name.GetIdentifierName(), trigger.timing, trigger.event_type,
		                                           std::move(corr_cols));
		logi_trig->children.push_back(std::move(trigger_plan));
		logi_trig->children.push_back(std::move(bound_body.plan));
		logi_trig->ResolveOperatorTypes();
		trigger_plan = std::move(logi_trig);
	}
	// remove row_scope_binder
	PopScope();

	Identifier trigger_cte_name(string(TRIGGER_BODY_CTE_PREFIX) + "row_" + uuid_suffix);
	auto trigger_cte_idx = GenerateTableIndex();
	auto outer_query = std::move(bound.plan->children[1]);

	auto trigger_mat_cte =
	    make_uniq<LogicalMaterializedCTE>(trigger_cte_name, trigger_cte_idx, col_types.size(), std::move(trigger_plan),
	                                      std::move(outer_query), CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	trigger_mat_cte->ResolveOperatorTypes();

	bound.plan->children[1] = std::move(trigger_mat_cte);
	bound.plan->ResolveOperatorTypes();

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::CHANGED_ROWS;
	return bound;
}

} // namespace duckdb
