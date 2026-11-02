//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/column_binding_replacer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"

#include <functional>

namespace duckdb {

class BoundColumnRefExpression;
class BoundSubqueryExpression;
class ColumnBindingReplacer;

struct ReplacementBinding {
public:
	ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding);
	ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding, LogicalType new_type);

public:
	ColumnBinding old_binding;
	ColumnBinding new_binding;

	bool replace_type;
	LogicalType new_type;
};

//! A validated graph of direct binding replacement edges.
//! Resolution follows these edges transitively and is independent of insertion order.
class BindingReplacementGraph {
public:
	ColumnBinding Resolve(ColumnBinding binding) const;
	//! Find the direct replacement edge for a binding, if any
	optional_ptr<const ReplacementBinding> Find(ColumnBinding old_binding) const;
	bool TryAdd(const ReplacementBinding &replacement);
	void Add(ColumnBinding old_binding, ColumnBinding new_binding);
	void Add(const ReplacementBinding &replacement);
	void Merge(const BindingReplacementGraph &replacements);
	void AddTo(ColumnBindingReplacer &replacer) const;
	//! Collapse the graph to single edges that resolve into the given output boundary, dropping edges that do not.
	//! Resolution at any enclosing boundary is unaffected because operators above cannot see bindings dropped here.
	BindingReplacementGraph ScopedToBoundary(const vector<ColumnBinding> &boundary) const;

	bool Empty() const {
		return replacement_bindings.empty();
	}
	vector<ReplacementBinding>::const_iterator begin() const {
		return replacement_bindings.begin();
	}
	vector<ReplacementBinding>::const_iterator end() const {
		return replacement_bindings.end();
	}

private:
	ReplacementBinding ResolveReplacement(ColumnBinding binding) const;
	vector<ReplacementBinding> replacement_bindings;
	//! Index into replacement_bindings by old_binding (unique by construction)
	column_binding_map_t<idx_t> binding_slots;
};

//! The ColumnBindingReplacer updates column bindings (e.g., after changing the operator plan), utility for optimizers
class ColumnBindingReplacer : public LogicalOperatorVisitor {
public:
	ColumnBindingReplacer();

public:
	//! Update each operator of the plan, recursing directly into children without resolving projection maps
	void VisitOperator(LogicalOperator &op) override;
	//! Update bindings owned by this operator without visiting its children
	virtual void VisitOperatorBindings(LogicalOperator &op);
	//! Add binding replacements by position
	void AddReplacements(const vector<ColumnBinding> &old_bindings, const vector<ColumnBinding> &new_bindings);
	//! Index replacement_bindings by old_binding for O(1) lookup. Only takes effect for replacement lists that
	//! sequential application cannot chain (no new_binding occurs as an old_binding); otherwise lookups stay linear.
	//! Call after the replacement list is complete; later list changes invalidate the index.
	void BuildIndex();
	//! Apply the replacement list to a single binding (and optionally its type)
	void Replace(ColumnBinding &binding) const;
	void Replace(ColumnBinding &binding, LogicalType &type) const;

protected:
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;
	//! Find the applicable replacement, using the index if one was built
	optional_ptr<const ReplacementBinding> FindReplacement(ColumnBinding binding) const;

public:
	//! Do not recurse further than this operator (optional)
	optional_ptr<LogicalOperator> stop_operator;

	//! Contains all bindings that need to be updated
	vector<ReplacementBinding> replacement_bindings;

private:
	column_binding_map_t<idx_t> replacement_index;
	bool use_index = false;
};

//! Like ColumnBindingReplacer, but also updates correlated-column metadata and nested subquery plans.
class CorrelatedColumnBindingReplacer : public ColumnBindingReplacer {
public:
	void VisitOperatorBindings(LogicalOperator &op) override;

protected:
	using ColumnBindingReplacer::VisitReplace;
	unique_ptr<Expression> VisitReplace(BoundSubqueryExpression &expr, unique_ptr<Expression> *expr_ptr) override;
};

//! Applies binding replacements together with their projection-layout invariants.
class ColumnBindingRewrite {
public:
	//! Rewrite a child, preserving the parent's explicit and implicit projection selections and updating its bindings.
	//! The callback may replace or wrap the child and must record its binding replacements in the supplied graph.
	//! Callers must still rewrite affected ancestors and resolve operator types after the complete transformation.
	static void
	RewriteChild(unique_ptr<LogicalOperator> &op, idx_t child_index,
	             const std::function<void(unique_ptr<LogicalOperator> &, BindingReplacementGraph &)> &rewrite);
	//! Apply the output-boundary view of a complete replacement graph to one parent-child edge.
	static void ApplyToChild(unique_ptr<LogicalOperator> &op, idx_t child_index,
	                         vector<ColumnBinding> old_child_bindings, const BindingReplacementGraph &replacements);
	static void ApplyToOperatorBindings(LogicalOperator &op, const BindingReplacementGraph &replacements);
	//! Verify that every previous public output binding is still reachable at the rewritten output boundary.
	static void ValidateOutput(const vector<ColumnBinding> &old_output, const vector<ColumnBinding> &new_output,
	                           const BindingReplacementGraph &replacements);

private:
	static void RemapProjectionMapStrict(vector<ProjectionIndex> &projection_map,
	                                     const vector<ColumnBinding> &child_bindings_before,
	                                     const vector<ColumnBinding> &child_bindings_after);
};

} // namespace duckdb
