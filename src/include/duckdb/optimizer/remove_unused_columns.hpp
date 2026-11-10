//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/remove_unused_columns.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/column_index_map.hpp"

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"

namespace duckdb {
class Binder;
class BoundColumnRefExpression;
class ClientContext;
class LogicalColumnDataGet;
class LogicalRecursiveCTE;
class LogicalSecureView;
class Optimizer;

struct ReferencedExtractComponent {
public:
	explicit ReferencedExtractComponent(unique_ptr<Expression> &extract) : extract(extract) {
	}

public:
	//! The extract expression in the chain of extracts (i.e: s.my_field.my_nested_field has 2 components)
	unique_ptr<Expression> &extract;
	//! (Optionally) the cast on top of the extract
	optional_ptr<unique_ptr<Expression>> cast;
};

struct ReferencedStructExtract {
public:
	ReferencedStructExtract(vector<ReferencedExtractComponent> components, idx_t bindings_idx, ColumnIndex &&path)
	    : bindings_idx(bindings_idx), components(std::move(components)), extract_path(std::move(path)) {
	}

public:
	//! The index into the 'bindings' of the ReferencedColumn that is the child of this struct_extract
	idx_t bindings_idx;
	//! The struct extract components, in order from root to leaf
	vector<ReferencedExtractComponent> components;
	//! The ColumnIndex with a path that matches this struct extract
	ColumnIndex extract_path;
};

enum class PushdownExtractSupport : uint8_t { UNCHECKED, DISABLED, ENABLED };

class ReferencedColumn {
public:
	void AddPath(const ColumnIndex &path);

public:
	//! The BoundColumnRefExpressions in the operator that reference the same ColumnBinding
	vector<reference<BoundColumnRefExpression>> bindings;
	vector<ReferencedStructExtract> struct_extracts;
	vector<ColumnIndex> child_columns;
	//! Whether we can create a pushdown extract for the children of this column (if any)
	PushdownExtractSupport supports_pushdown_extract = PushdownExtractSupport::UNCHECKED;
	//! Map from extract path to the binding created for it (if pushdown extract)
	column_index_set unique_paths;
};

enum class BaseColumnPrunerMode : uint8_t {
	DEFAULT,
	//! Any child reference disables PUSHDOWN_EXTRACT for the parent column
	DISABLE_PUSHDOWN_EXTRACT
};

struct MaterializedCTEInfo {
public:
	column_binding_map_t<ReferencedColumn> column_references;
	unordered_set<TableIndex> expected_readers;
	unordered_set<TableIndex> seen_readers;
	bool everything_referenced = true;
};

enum class RemoveUnusedColumnsMode : uint8_t { APPLY, ANALYZE };

class BaseColumnPruner : public LogicalOperatorVisitor {
protected:
	void VisitExpression(unique_ptr<Expression> *expression) override;

	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;
	unique_ptr<Expression> VisitReplace(BoundReferenceExpression &expr, unique_ptr<Expression> *expr_ptr) override;

protected:
	//! Add a reference to the column in its entirety
	void AddBinding(BoundColumnRefExpression &col);
	//! Add a reference to a sub-section of the column
	void AddBinding(BoundColumnRefExpression &col, ColumnIndex child_column);
	//! Add a reference to a sub-section of the column used in a struct extract, with the parent expression
	void AddBinding(BoundColumnRefExpression &col, ColumnIndex child_column,
	                const vector<ReferencedExtractComponent> &parent);
	//! Perform a replacement of the ColumnBinding, iterating over all the currently found column references and
	//! replacing the bindings
	//! ret: The amount of bindings created
	idx_t ReplaceBinding(ColumnBinding current_binding, ColumnBinding new_binding);

	bool HandleExtractExpression(unique_ptr<Expression> *expression,
	                             optional_ptr<unique_ptr<Expression>> cast_expression = nullptr);

	bool HandleStructExtract(unique_ptr<Expression> &expr, optional_ptr<BoundColumnRefExpression> &colref,
	                         reference<ColumnIndex> &path_ref, vector<ReferencedExtractComponent> &expressions);
	bool HandleVariantExtract(unique_ptr<Expression> &expr, optional_ptr<BoundColumnRefExpression> &colref,
	                          reference<ColumnIndex> &path_ref, vector<ReferencedExtractComponent> &expressions);
	bool HandleExtractRecursive(unique_ptr<Expression> &expr, optional_ptr<BoundColumnRefExpression> &colref,
	                            reference<ColumnIndex> &path_ref, vector<ReferencedExtractComponent> &expressions);
	void SetMode(BaseColumnPrunerMode mode);
	BaseColumnPrunerMode GetMode() const;

private:
	void MergeChildColumns(vector<ColumnIndex> &current_child_columns, ColumnIndex &new_child_column);

protected:
	//! The map of column references
	column_binding_map_t<ReferencedColumn> column_references;

private:
	//! The current mode of the pruner, enables/disables certain behaviors
	BaseColumnPrunerMode mode = BaseColumnPrunerMode::DEFAULT;
};

//! The RemoveUnusedColumns optimizer traverses the logical operator tree and removes any columns that are not required
class RemoveUnusedColumns : public BaseColumnPruner {
public:
	explicit RemoveUnusedColumns(Optimizer &optimizer);
	RemoveUnusedColumns(RemoveUnusedColumns &parent, bool is_root);

	void VisitOperator(unique_ptr<LogicalOperator> &op) override;

private:
	Optimizer &optimizer;
	Binder &binder;
	ClientContext &context;
	//! Whether or not all the columns are referenced. This happens in the case of the root expression (because the
	//! output implicitly refers all the columns below it)
	bool everything_referenced;
	bool allow_missing_cte_references = false;
	RemoveUnusedColumnsMode mode = RemoveUnusedColumnsMode::APPLY;

	RemoveUnusedColumns &root;
	unique_ptr<unordered_map<TableIndex, MaterializedCTEInfo>> root_cte_map;
	column_binding_map_t<vector<ColumnBinding>> projection_map_replacements;
	struct PrunedViewColumn {
		ColumnBinding binding;
		//! Expression over the previous binding; nullptr for a rename.
		unique_ptr<Expression> expression;
	};
	vector<reference<LogicalSecureView>> active_secure_views;
	column_binding_map_t<vector<PrunedViewColumn>> secure_view_replacements;

private:
	template <class T>
	void ClearUnusedExpressions(vector<T> &list, TableIndex table_idx, bool replace = true);
	void RecordSecureViewReplacement(ColumnBinding old_binding, ColumnBinding new_binding,
	                                 unique_ptr<Expression> expression = nullptr);
	void ApplySecureViewReplacements();
	void RemoveColumnsFromLogicalColumnDataGet(LogicalColumnDataGet &get);
	void RemoveColumnsFromLogicalGet(LogicalGet &get, unique_ptr<LogicalOperator> &op_ref);
	void VisitSecureView(LogicalSecureView &view);
	void CheckPushdownExtract(LogicalOperator &op);
	void RewriteExpressions(LogicalProjection &proj, idx_t expression_count);
	void GatherRecursiveDependencies(unique_ptr<LogicalOperator> &bottom, TableIndex cte_index,
	                                 const unordered_set<ProjectionIndex> &required_columns,
	                                 unordered_set<ProjectionIndex> &recursive_dependencies);
	bool ComputeRecursiveRequiredColumns(LogicalRecursiveCTE &rec, unordered_set<ProjectionIndex> &required_columns);
	void ApplyRecursiveProjections(LogicalRecursiveCTE &rec, const unordered_set<ProjectionIndex> &required_columns);
	vector<ReplacementBinding> RewriteRecursiveCTEReferences(LogicalRecursiveCTE &rec,
	                                                         const unordered_set<ProjectionIndex> &required_columns);
	bool TryPruneRecursiveCTE(LogicalRecursiveCTE &rec);
	void VisitPrunableChildren(LogicalOperator &op);
	void WritePushdownExtractColumns(
	    ReferencedColumn &col,
	    const std::function<ProjectionIndex(const ColumnIndex &new_index, optional_ptr<const LogicalType> cast_type)>
	        &callback);
	unordered_map<TableIndex, MaterializedCTEInfo> &GetCTEMap();
	optional_ptr<unordered_map<TableIndex, MaterializedCTEInfo>> TryGetCTEMap();
};

class CTERefPruner : public LogicalOperatorVisitor {
public:
	CTERefPruner(const TableIndex table_index, const unordered_set<ProjectionIndex> &referenced_columns);

	void VisitOperator(LogicalOperator &op) override;

private:
	const TableIndex cte_index;
	const unordered_set<ProjectionIndex> &referenced_columns;
	column_binding_map_t<vector<ColumnBinding>> projection_map_replacements;

public:
	vector<ReplacementBinding> binding_replacements;
};

} // namespace duckdb
