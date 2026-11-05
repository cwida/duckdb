//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/compressed_materialization.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

class Optimizer;
class ClientContext;
class ColumnBindingReplacer;
class LogicalOperator;
class Expression;
class BoundColumnRefExpression;
class BoundFunctionExpression;
struct JoinCondition;

enum class CompressedMaterializationType : uint8_t {
	INVALID = 0,
	//! Use the opaque internal (de-)compress functions
	FUNCTION = 1,
	//! Use down- and upcasts that are transparent to subsequent optimizers
	CAST = 2
};

struct CMChildInfo {
public:
	CMChildInfo(LogicalOperator &op, const column_binding_set_t &referenced_bindings);

public:
	//! Bindings and types before compressing
	vector<ColumnBinding> bindings_before;
	vector<LogicalType> &types;
	//! Whether the input binding is eligible for compression
	vector<bool> can_compress;

	//! Bindings after compressing (projection on top)
	vector<ColumnBinding> bindings_after;
};

struct CMBindingInfo {
public:
	explicit CMBindingInfo(ColumnBinding binding, const LogicalType &type);

public:
	ColumnBinding binding;

	//! Type before compressing
	LogicalType type;
	CompressedMaterializationType materialization_type;
	unique_ptr<BaseStatistics> stats;
};

struct CompressedMaterializationInfo {
public:
	CompressedMaterializationInfo(LogicalOperator &op, vector<idx_t> &&child_idxs,
	                              const column_binding_set_t &referenced_bindings);

public:
	//! Mapping from incoming bindings to outgoing bindings
	column_binding_map_t<CMBindingInfo> binding_map;

	//! Operator child info
	vector<idx_t> child_idxs;
	vector<CMChildInfo> child_info;
};

struct CompressExpression {
public:
	CompressExpression(unique_ptr<Expression> expression, unique_ptr<BaseStatistics> stats,
	                   CompressedMaterializationType materialization_type);

public:
	unique_ptr<Expression> expression;
	unique_ptr<BaseStatistics> stats;
	CompressedMaterializationType materialization_type;
};

typedef column_binding_map_t<unique_ptr<BaseStatistics>> statistics_map_t;

//! The CompressedMaterialization optimizer compressed columns using projections, based on available statistics,
//! but only if the data enters a materializing operator
class CompressedMaterialization {
private:
	struct VariantJoinKeyInfo {
		optional_ptr<const Expression> child;
		LogicalType shredded_type;
		unique_ptr<BaseStatistics> typed_stats;
	};

	//! Somewhat defensive constants that try to limit when compressed materialization is triggered for joins
	//! We only consider compressed materialization for joins when the build cardinality is greater than this
	static constexpr idx_t JOIN_BUILD_CARDINALITY_THRESHOLD = 1048576;
	//! If the cardinality > threshold, we always perform compressed materialization if there are this many columns
	static constexpr idx_t JOIN_BUILD_COLUMN_COUNT_THRESHOLD = 20;
	//! If not, we perform compressed materialization if join result cardinality > build cardinality * this threshold
	static constexpr double JOIN_CARDINALITY_RATIO_THRESHOLD = 8;

public:
	CompressedMaterialization(Optimizer &optimizer, LogicalOperator &root,
	                          const vector<reference<unique_ptr<LogicalOperator>>> &ancestors,
	                          statistics_map_t &statistics_map);

	void Compress(unique_ptr<LogicalOperator> &op);

private:
	//! Compress materializing operators
	void CompressAggregate(unique_ptr<LogicalOperator> &op);
	void CompressComparisonJoin(unique_ptr<LogicalOperator> &op);
	void CompressDistinct(unique_ptr<LogicalOperator> &op);
	void CompressOrder(unique_ptr<LogicalOperator> &op);

	//! Update statistics after compressing
	void UpdateAggregateStats(unique_ptr<LogicalOperator> &op);
	void UpdateComparisonJoinStats(unique_ptr<LogicalOperator> &op);
	void UpdateOrderStats(unique_ptr<LogicalOperator> &op);

	//! Adds bindings referenced in expression to referenced_bindings
	static void GetReferencedBindings(const Expression &expression, column_binding_set_t &referenced_bindings);
	static bool IsVariantWrapperFunction(const BoundFunctionExpression &expr);
	static optional_ptr<const BoundColumnRefExpression> TryGetVariantWrapperColumnRef(const Expression &expr);
	//! Updates CMBindingInfo in the binding_map in info
	void UpdateBindingInfo(CompressedMaterializationInfo &info, const ColumnBinding &binding,
	                       CompressedMaterializationType materialization_type);
	optional_ptr<BaseStatistics> GetVariantWrapperStats(const Expression &expr);

	//! Create (de)compress projections around the operator
	void CreateProjections(unique_ptr<LogicalOperator> &op, CompressedMaterializationInfo &info);
	bool TryCompressChild(CompressedMaterializationInfo &info, const CMChildInfo &child_info,
	                      vector<unique_ptr<CompressExpression>> &compress_expressions);
	void CreateCompressProjection(unique_ptr<LogicalOperator> &child_op,
	                              vector<unique_ptr<CompressExpression>> compress_exprs,
	                              CompressedMaterializationInfo &info, CMChildInfo &child_info);
	void CreateDecompressProjection(unique_ptr<LogicalOperator> &op, CompressedMaterializationInfo &info);
	//! Rebind references to compressed bindings, which can only occur in the ancestors of the compressed operator
	void RebindAncestors(ColumnBindingReplacer &replacer);
	unique_ptr<Expression> CreateRestoreExpression(unique_ptr<Expression> input, const CMBindingInfo &binding_info,
	                                               const BaseStatistics &stats);

	//! Create expressions that apply a scalar compression function
	unique_ptr<CompressExpression> GetCompressExpression(const ColumnBinding &binding, const LogicalType &type,
	                                                     const bool &can_compress);
	unique_ptr<CompressExpression> GetCompressExpression(unique_ptr<Expression> input, const BaseStatistics &stats);
	unique_ptr<CompressExpression> GetIntegralCompress(unique_ptr<Expression> input, const BaseStatistics &stats);
	unique_ptr<CompressExpression> GetStringCompress(unique_ptr<Expression> input, const BaseStatistics &stats);
	unique_ptr<CompressExpression> GetGeometryCompress(unique_ptr<Expression> input, const BaseStatistics &stats);
	unique_ptr<CompressExpression> GetVariantCompress(unique_ptr<Expression> input, const BaseStatistics &stats);
	bool TryGetVariantJoinKeyInfo(const Expression &expr, VariantJoinKeyInfo &result);
	static unique_ptr<BaseStatistics> CastVariantJoinKeyStats(const VariantJoinKeyInfo &key_info,
	                                                          const LogicalType &target_type);
	unique_ptr<Expression> CreateVariantJoinKeyCast(unique_ptr<Expression> input, const LogicalType &shredded_type,
	                                                const LogicalType &target_type);
	bool TryCompressVariantComparisonJoinKey(JoinCondition &condition);

	//! Create an expression that applies a scalar decompression function
	unique_ptr<Expression> GetDecompressExpression(unique_ptr<Expression> input, const LogicalType &result_type,
	                                               const BaseStatistics &stats);
	unique_ptr<Expression> GetIntegralDecompress(unique_ptr<Expression> input, const LogicalType &result_type,
	                                             const BaseStatistics &stats);
	unique_ptr<Expression> GetStringDecompress(unique_ptr<Expression> input, const LogicalType &result_type,
	                                           const BaseStatistics &stats);
	unique_ptr<Expression> GetGeometryDecompress(unique_ptr<Expression> input, const LogicalType &result_type,
	                                             const BaseStatistics &stats);
	unique_ptr<Expression> GetVariantDecompress(unique_ptr<Expression> input, const LogicalType &result_type,
	                                            const BaseStatistics &stats);

private:
	Optimizer &optimizer;
	ClientContext &context;
	//! The root of the query plan
	optional_ptr<LogicalOperator> root;
	//! The chain of operators from the root down to the operator being compressed
	const vector<reference<unique_ptr<LogicalOperator>>> &ancestors;
	//! The map of ColumnBinding -> statistics for the various nodes
	statistics_map_t &statistics_map;
};

} // namespace duckdb
