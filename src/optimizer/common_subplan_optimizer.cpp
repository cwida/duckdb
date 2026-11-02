#include "duckdb/optimizer/common_subplan_optimizer.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/cte_inlining.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/arena_containers/arena_unordered_map.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/column_binding_map.hpp"

#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Subplan Signature/Info
//===--------------------------------------------------------------------===//
static vector<ColumnBinding> GetExpandedColumnBindings(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return GetExpandedColumnBindings(*op.children[0]);
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_UNNEST: {
		auto bindings = GetExpandedColumnBindings(*op.children[0]);
		const auto table_index = op.GetTableIndex()[0];
		for (auto col_idx : ProjectionIndex::GetIndexes(op.expressions.size())) {
			bindings.emplace_back(table_index, col_idx);
		}
		return bindings;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalJoin>();
		auto left_bindings = GetExpandedColumnBindings(*op.children[0]);
		if (join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI) {
			return left_bindings;
		}
		if (join.join_type == JoinType::MARK) {
			left_bindings.emplace_back(join.mark_index, ProjectionIndex(0));
			return left_bindings;
		}
		auto right_bindings = GetExpandedColumnBindings(*op.children[1]);
		if (join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI) {
			return right_bindings;
		}
		left_bindings.insert(left_bindings.end(), right_bindings.begin(), right_bindings.end());
		return left_bindings;
	}
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN: {
		auto left_bindings = GetExpandedColumnBindings(*op.children[0]);
		auto right_bindings = GetExpandedColumnBindings(*op.children[1]);
		left_bindings.insert(left_bindings.end(), right_bindings.begin(), right_bindings.end());
		return left_bindings;
	}
	default:
		return op.GetColumnBindings();
	}
}

enum class ConversionType {
	TO_CANONICAL,
	RESTORE_ORIGINAL,
};

class PlanSignatureColumnIndexMap {
public:
	explicit PlanSignatureColumnIndexMap(ArenaAllocator &allocator)
	    : to_canonical(allocator), restore_original(allocator) {
	}

public:
	template <ConversionType TYPE>
	bool Empty() const {
		return GetMap<TYPE>().empty();
	}

	void Insert(const ProjectionIndex original, const ProjectionIndex canonical) {
		D_ASSERT(to_canonical.find(original) == to_canonical.end());
		D_ASSERT(restore_original.find(canonical) == restore_original.end());
		to_canonical.emplace(make_pair(original, canonical));
		restore_original.emplace(make_pair(canonical, original));
	}

	template <ConversionType TYPE>
	ProjectionIndex Get(const ProjectionIndex index) const {
		D_ASSERT(!Empty<TYPE>());
		return GetMap<TYPE>().at(index);
	}

private:
	template <ConversionType TYPE>
	const arena_unordered_map<ProjectionIndex, ProjectionIndex> &GetMap() const {
		return TYPE == ConversionType::TO_CANONICAL ? to_canonical : restore_original;
	}

private:
	//! Map from original column index to canonical column index (and reverse)
	arena_unordered_map<ProjectionIndex, ProjectionIndex> to_canonical;
	arena_unordered_map<ProjectionIndex, ProjectionIndex> restore_original;
};

class PlanSignatureTableIndexMap {
public:
	explicit PlanSignatureTableIndexMap(ArenaAllocator &allocator_p)
	    : allocator(allocator_p), table_index_map(allocator), to_canonical_table_index(allocator),
	      restore_original_table_index(allocator), restore_original_table_filter_index(allocator) {
	}

public:
	const arena_unordered_map<TableIndex, PlanSignatureColumnIndexMap> &GetMap() const {
		return table_index_map;
	}

	template <ConversionType TYPE>
	bool Convert(LogicalOperator &op) {
		Initialize<TYPE>(op);
		ConvertTableIndices<TYPE>(op);
		ConvertProjectionMaps<TYPE>(op);
		auto can_materialize = ConvertColumnIndices<TYPE>(op);
		ConvertChildren<TYPE>(op);
		return ConvertExpressions<TYPE>(op) && can_materialize;
	}

private:
	template <ConversionType TYPE>
	void Initialize(LogicalOperator &op) {
		if (TYPE == ConversionType::TO_CANONICAL) {
			// Clear temporary data structures
			to_canonical_table_index.clear();
			restore_original_table_index.clear();
			restore_original_table_filter_index.clear();
			column_ids.clear();
			chunk_column_ids.clear();
			projection_ids.clear();
			table_indices.clear();
			projection_maps.clear();
			expression_info.clear();

			// Store temporary mapping
			for (const auto &child_op : op.children) {
				for (const auto &child_cb : GetExpandedColumnBindings(*child_op)) {
					const auto &original = child_cb.table_index;
					auto it = to_canonical_table_index.find(original);
					if (it != to_canonical_table_index.end()) {
						continue; // We've seen this table index before
					}
					TableIndex canonical(CANONICAL_TABLE_INDEX_OFFSET + to_canonical_table_index.size());
					D_ASSERT(to_canonical_table_index.find(original) == to_canonical_table_index.end());
					D_ASSERT(restore_original_table_index.find(canonical) == restore_original_table_index.end());
					to_canonical_table_index.emplace(make_pair(original, canonical));
					restore_original_table_index.emplace(make_pair(canonical, original));
				}
			}
		}
	}

	template <ConversionType TYPE>
	void ConvertTableIndices(LogicalOperator &op) {
#ifdef D_ASSERT_IS_ENABLED
		const auto table_indices_verification = op.GetTableIndex();
#endif
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_GET:
			ConvertTableIndex<TYPE>(op.Cast<LogicalGet>().table_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_CHUNK_GET:
			ConvertTableIndex<TYPE>(op.Cast<LogicalColumnDataGet>().table_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
			ConvertTableIndex<TYPE>(op.Cast<LogicalExpressionGet>().table_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
			ConvertTableIndex<TYPE>(op.Cast<LogicalDummyScan>().table_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_CTE_REF:
			ConvertTableIndex<TYPE>(op.Cast<LogicalCTERef>().table_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_PROJECTION:
			ConvertTableIndex<TYPE>(op.Cast<LogicalProjection>().table_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_PIVOT:
			ConvertTableIndex<TYPE>(op.Cast<LogicalPivot>().pivot_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_UNNEST:
			ConvertTableIndex<TYPE>(op.Cast<LogicalUnnest>().unnest_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_WINDOW:
			ConvertTableIndex<TYPE>(op.Cast<LogicalWindow>().window_index, 0);
			break;
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			auto &aggr = op.Cast<LogicalAggregate>();
			ConvertTableIndex<TYPE>(aggr.group_index, 0);
			ConvertTableIndex<TYPE>(aggr.aggregate_index, 1);
			if (aggr.groupings_index.IsValid()) {
				ConvertTableIndex<TYPE>(aggr.groupings_index, 2);
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_ANY_JOIN:
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
			auto &join = op.Cast<LogicalJoin>();
			if (join.join_type == JoinType::MARK) {
				ConvertTableIndex<TYPE>(join.mark_index, 0);
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_UNION:
		case LogicalOperatorType::LOGICAL_EXCEPT:
		case LogicalOperatorType::LOGICAL_INTERSECT:
			ConvertTableIndex<TYPE>(op.Cast<LogicalSetOperation>().table_index, 0);
			break;
		default:
			break;
		}
#ifdef D_ASSERT_IS_ENABLED
		if (TYPE == ConversionType::TO_CANONICAL) {
			D_ASSERT(table_indices == table_indices_verification);
		}
#endif
	}

	template <ConversionType TYPE>
	void ConvertProjectionMaps(LogicalOperator &op) {
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_ANY_JOIN:
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
			auto &join = op.Cast<LogicalJoin>();
			if (TYPE == ConversionType::TO_CANONICAL) {
				D_ASSERT(projection_maps.empty());
				projection_maps.push_back(std::move(join.left_projection_map));
				projection_maps.push_back(std::move(join.right_projection_map));
			} else {
				D_ASSERT(TYPE == ConversionType::RESTORE_ORIGINAL);
				D_ASSERT(!projection_maps.empty());
				join.left_projection_map = std::move(projection_maps[0]);
				join.right_projection_map = std::move(projection_maps[1]);
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER: {
			auto &filter = op.Cast<LogicalFilter>();
			if (TYPE == ConversionType::TO_CANONICAL) {
				D_ASSERT(projection_maps.empty());
				projection_maps.push_back(std::move(filter.projection_map));
			} else {
				D_ASSERT(TYPE == ConversionType::RESTORE_ORIGINAL);
				D_ASSERT(!projection_maps.empty());
				filter.projection_map = std::move(projection_maps[0]);
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_ORDER_BY: {
			auto &order = op.Cast<LogicalOrder>();
			if (TYPE == ConversionType::TO_CANONICAL) {
				D_ASSERT(projection_maps.empty());
				projection_maps.push_back(std::move(order.projection_map));
			} else {
				D_ASSERT(TYPE == ConversionType::RESTORE_ORIGINAL);
				D_ASSERT(!projection_maps.empty());
				order.projection_map = std::move(projection_maps[0]);
			}
			break;
		}
		default:
			break;
		}
	}

	template <ConversionType TYPE>
	void ConvertTableIndex(TableIndex &table_index, const idx_t i) {
		switch (TYPE) {
		case ConversionType::TO_CANONICAL:
			D_ASSERT(table_indices.size() == i);
			D_ASSERT(table_index_map.find(table_index) == table_index_map.end());
			table_index_map.emplace(table_index, PlanSignatureColumnIndexMap(allocator));
			table_indices.emplace_back(table_index);
			table_index = TableIndex(CANONICAL_TABLE_INDEX_OFFSET + to_canonical_table_index.size() + i);
			break;
		case ConversionType::RESTORE_ORIGINAL:
			table_index = table_indices[i];
			break;
		}
	}

	template <ConversionType TYPE>
	bool ConvertColumnIndices(LogicalOperator &op) {
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			auto &get = op.Cast<LogicalGet>();
			switch (TYPE) {
			case ConversionType::TO_CANONICAL: {
				// Source ordinality is represented by the bound operators.
				source_ordinality = get.source_ordinality;
				get.source_ordinality = OrdinalityType::WITHOUT_ORDINALITY;
				D_ASSERT(column_ids.empty());
				// Grab selected GET columns and populate with all possible columns
				column_ids = std::move(get.GetMutableColumnIds());
				for (idx_t col_idx = 0; col_idx < get.names.size(); col_idx++) {
					get.GetMutableColumnIds().push_back(ColumnIndex(col_idx));
				}
				for (const auto &vc : get.virtual_columns) {
					get.GetMutableColumnIds().push_back(ColumnIndex(vc.first));
				}

				// Also temporarily don't project any columns out
				projection_ids = std::move(get.projection_ids);

				// FIXME: can probably work for nested columns in the future
				for (const auto &col_id : column_ids) {
					if (col_id.IsVirtualColumn() || col_id.HasChildren()) {
						return false;
					}
				}

				auto register_filter_index = [&](ProjectionIndex filter_index) {
					D_ASSERT(filter_index.GetIndex() < column_ids.size());
					const auto canonical_index = ProjectionIndex(column_ids[filter_index.GetIndex()].GetPrimaryIndex());
					auto entry = restore_original_table_filter_index.emplace(canonical_index, filter_index);
					return entry.second || entry.first->second == filter_index;
				};
				for (auto &entry : get.table_filters) {
					if (!register_filter_index(entry.GetIndex())) {
						restore_original_table_filter_index.clear();
						return false;
					}
				}
				for (const auto &filter : get.table_filters.GetMultiColumnFilters()) {
					const auto &expression_filter =
					    ExpressionFilter::GetExpressionFilter(*filter, "CommonSubplanOptimizer::ConvertTableIndex");
					for (const auto &column_index : expression_filter.column_indexes) {
						if (!register_filter_index(column_index)) {
							restore_original_table_filter_index.clear();
							return false;
						}
					}
				}
				if (!restore_original_table_filter_index.empty()) {
					TableFilterSet remapped_filters;
					for (auto &entry : get.table_filters) {
						const auto canonical_index =
						    ProjectionIndex(column_ids[entry.GetIndex().GetIndex()].GetPrimaryIndex());
						remapped_filters.PushFilter(canonical_index, entry.TakeFilter());
					}
					for (const auto &filter : get.table_filters.GetMultiColumnFilters()) {
						auto remapped_filter =
						    ExpressionFilter::GetExpressionFilter(*filter, "CommonSubplanOptimizer::ConvertTableIndex")
						        .Copy();
						for (auto &column_index : remapped_filter->column_indexes) {
							column_index = ProjectionIndex(column_ids[column_index.GetIndex()].GetPrimaryIndex());
						}
						remapped_filters.PushMultiColumnFilter(std::move(remapped_filter));
					}
					get.table_filters = std::move(remapped_filters);
				}

				// Store mapping for base tables
				auto &column_index_map = table_index_map.at(table_indices[0]);
				if (projection_ids.empty()) {
					for (idx_t col_idx = 0; col_idx < column_ids.size(); col_idx++) {
						ProjectionIndex primary_index(column_ids[col_idx].GetPrimaryIndex());
						column_index_map.Insert(ProjectionIndex(col_idx), primary_index);
					}
				} else {
					for (const auto &proj_id : projection_ids) {
						ProjectionIndex primary_index(column_ids[proj_id].GetPrimaryIndex());
						column_index_map.Insert(proj_id, primary_index);
					}
				}
				break;
			}
			case ConversionType::RESTORE_ORIGINAL:
				get.source_ordinality = source_ordinality;
				D_ASSERT(!column_ids.empty());
				get.GetMutableColumnIds() = std::move(column_ids);
				D_ASSERT(get.projection_ids.empty());
				get.projection_ids = std::move(projection_ids);
				if (!restore_original_table_filter_index.empty()) {
					TableFilterSet remapped_filters;
					for (auto &entry : get.table_filters) {
						remapped_filters.PushFilter(restore_original_table_filter_index.at(entry.GetIndex()),
						                            entry.TakeFilter());
					}
					for (const auto &filter : get.table_filters.GetMultiColumnFilters()) {
						auto remapped_filter =
						    ExpressionFilter::GetExpressionFilter(*filter, "CommonSubplanOptimizer::ConvertTableIndex")
						        .Copy();
						for (auto &column_index : remapped_filter->column_indexes) {
							column_index = restore_original_table_filter_index.at(column_index);
						}
						remapped_filters.PushMultiColumnFilter(std::move(remapped_filter));
					}
					get.table_filters = std::move(remapped_filters);
				}
				break;
			}
		}
		if (op.type == LogicalOperatorType::LOGICAL_CHUNK_GET) {
			auto &get = op.Cast<LogicalColumnDataGet>();
			switch (TYPE) {
			case ConversionType::TO_CANONICAL: {
				D_ASSERT(chunk_column_ids.empty());
				chunk_column_ids = get.GetColumnIds();

				unordered_set<column_t> selected_columns;
				for (auto column_id : chunk_column_ids) {
					if (!selected_columns.insert(column_id).second) {
						return false;
					}
				}

				// Expose all collection columns so pruning does not change the scan signature.
				vector<column_t> all_column_ids;
				all_column_ids.reserve(get.chunk_types.size());
				for (idx_t column_id = 0; column_id < get.chunk_types.size(); column_id++) {
					all_column_ids.push_back(column_id);
				}
				get.SetColumnIds(std::move(all_column_ids));

				auto &column_index_map = table_index_map.at(table_indices[0]);
				for (idx_t column_idx = 0; column_idx < chunk_column_ids.size(); column_idx++) {
					column_index_map.Insert(ProjectionIndex(column_idx), ProjectionIndex(chunk_column_ids[column_idx]));
				}
				break;
			}
			case ConversionType::RESTORE_ORIGINAL:
				get.SetColumnIds(std::move(chunk_column_ids));
				break;
			}
		}
		return true;
	}

	template <ConversionType TYPE>
	void ConvertChildren(LogicalOperator &op) {
		switch (TYPE) {
		case ConversionType::TO_CANONICAL:
			D_ASSERT(children.empty());
			children = std::move(op.children);
			break;
		case ConversionType::RESTORE_ORIGINAL:
			D_ASSERT(op.children.empty());
			op.children = std::move(children);
			break;
		}
	}

	template <ConversionType TYPE>
	bool ConvertExpressions(LogicalOperator &op) {
		idx_t info_idx = 0;
		bool can_materialize = true;
		LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
			ExpressionIterator::EnumerateExpression(*expr, [&](unique_ptr<Expression> &child) {
				ConvertExpression<TYPE>(*child, info_idx, can_materialize);
			});
		});
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			auto &get = op.Cast<LogicalGet>();
			for (auto &entry : get.table_filters) {
				auto &expression_filter =
				    ExpressionFilter::GetExpressionFilter(entry.Filter(), "CommonSubplanOptimizer::ConvertExpressions");
				ConvertExpression<TYPE>(*expression_filter.expr, info_idx, can_materialize);
			}
		}
		return can_materialize;
	}

	template <ConversionType TYPE>
	void ConvertExpression(Expression &expr, idx_t &info_idx, bool &can_materialize) {
		const auto &table_index_mapping =
		    TYPE == ConversionType::TO_CANONICAL ? to_canonical_table_index : restore_original_table_index;

		// Replace column binding
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &col_ref = expr.Cast<BoundColumnRefExpression>();
			const auto lookup_idx = TYPE == ConversionType::TO_CANONICAL
			                            ? col_ref.Binding().table_index
			                            : restore_original_table_index.at(col_ref.Binding().table_index);
			auto &table_map = table_index_map.at(lookup_idx);
			if (!table_map.Empty<TYPE>()) {
				// Replace column index
				col_ref.BindingMutable().column_index = table_map.Get<TYPE>(col_ref.Binding().column_index);
			}
			// Replace table index
			col_ref.BindingMutable().table_index = table_index_mapping.at(col_ref.Binding().table_index);
		}

		// Replace default fields
		switch (TYPE) {
		case ConversionType::TO_CANONICAL:
			expression_info.emplace_back(expr.GetAlias(), expr.GetQueryLocation());
			expr.ClearAlias();
			expr.SetQueryLocation(optional_idx());
			break;
		case ConversionType::RESTORE_ORIGINAL:
			auto &info = expression_info[info_idx++];
			expr.SetAlias(Identifier(std::move(info.first)));
			expr.SetQueryLocation(info.second);
			break;
		}
		if (expr.IsVolatile()) {
			can_materialize = false;
		}
	}

private:
	//! Offset for table index conversion
	static constexpr idx_t CANONICAL_TABLE_INDEX_OFFSET = 10000000000000;

	//! For batching allocations
	ArenaAllocator &allocator;

	//! Map from original table index to column index map
	arena_unordered_map<TableIndex, PlanSignatureColumnIndexMap> table_index_map;

	//! Temporary map from original table index to canonical table index (and reverse)
	arena_unordered_map<TableIndex, TableIndex> to_canonical_table_index;
	arena_unordered_map<TableIndex, TableIndex> restore_original_table_index;
	//! Temporary map from canonical table filter index to original table filter index
	arena_unordered_map<ProjectionIndex, ProjectionIndex> restore_original_table_filter_index;
	//! Temporary vector to store table indices
	vector<TableIndex> table_indices;
	//! Temporary vector to store projection maps
	vector<vector<ProjectionIndex>> projection_maps;

	//! Utility to temporarily store column ids, projection_ids, table indices, expression info and children
	OrdinalityType source_ordinality = OrdinalityType::WITHOUT_ORDINALITY;
	vector<ColumnIndex> column_ids;
	vector<column_t> chunk_column_ids;
	vector<ProjectionIndex> projection_ids;
	vector<pair<string, optional_idx>> expression_info;
	vector<unique_ptr<LogicalOperator>> children;
};

class PlanSignatureCreateState {
public:
	explicit PlanSignatureCreateState(ClientContext &context)
	    : stream(DEFAULT_BLOCK_ALLOC_SIZE), serializer(stream), allocator(BufferAllocator::Get(context)),
	      table_index_map(allocator) {
	}

public:
public:
	//! For serializing operators to a binary string
	MemoryStream stream;
	BinarySerializer serializer;

	//! For batching allocations together
	ArenaAllocator allocator;

	//! Manages mappings
	PlanSignatureTableIndexMap table_index_map;
};

class PlanSignature {
public:
	PlanSignature(const MemoryStream &stream_p, idx_t offset_p, idx_t length_p,
	              vector<reference<PlanSignature>> &&child_signatures_p, idx_t operator_count_p)
	    : stream(stream_p), offset(offset_p), length(length_p),
	      signature_hash(Hash(stream_p.GetData() + offset, length)), child_signatures(std::move(child_signatures_p)),
	      operator_count(operator_count_p) {
	}

public:
	static arena_ptr<PlanSignature> Create(PlanSignatureCreateState &state, LogicalOperator &op,
	                                       vector<reference<PlanSignature>> &&child_signatures) {
		if (!OperatorIsSupported(op)) {
			return nullptr;
		}

		auto can_materialize = state.table_index_map.Convert<ConversionType::TO_CANONICAL>(op);

		// Serialize canonical representation of operator
		const auto offset = state.stream.GetPosition();
		state.serializer.Begin();
		try { // Operators will throw if they cannot serialize, so we need to try/catch here
			op.Serialize(state.serializer);
		} catch (std::exception &) {
			can_materialize = false;
		}
		state.serializer.End();
		const auto length = state.stream.GetPosition() - offset;

		// Convert back from canonical
		state.table_index_map.Convert<ConversionType::RESTORE_ORIGINAL>(op);

		if (!can_materialize) {
			return nullptr; // Cannot materialize, no point in adding it
		}

		// Collect some statistics so we can select a good candidate later
		idx_t operator_count = 1;
		for (auto &child_signature : child_signatures) {
			operator_count += child_signature.get().OperatorCount();
		}
		return state.allocator.MakePtr<PlanSignature>(state.stream, offset, length, std::move(child_signatures),
		                                              operator_count);
	}

	idx_t OperatorCount() const {
		return operator_count;
	}

	hash_t HashSignature() const {
		auto res = signature_hash;
		for (auto &child : child_signatures) {
			res = CombineHash(res, child.get().HashSignature());
		}
		return res;
	}

	bool Equals(const PlanSignature &other) const {
		if (this->GetSignature() != other.GetSignature()) {
			return false;
		}
		if (this->child_signatures.size() != other.child_signatures.size()) {
			return false;
		}
		for (idx_t child_idx = 0; child_idx < this->child_signatures.size(); ++child_idx) {
			if (!this->child_signatures[child_idx].get().Equals(other.child_signatures[child_idx].get())) {
				return false;
			}
		}
		return true;
	}

private:
	String GetSignature() const {
		return String(char_ptr_cast(stream.GetData() + offset), NumericCast<uint32_t>(length));
	}

	static bool OperatorIsSupported(const LogicalOperator &op) {
		if (!op.SupportSerialization()) {
			return false;
		}
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		case LogicalOperatorType::LOGICAL_WINDOW:
		case LogicalOperatorType::LOGICAL_UNNEST:
		case LogicalOperatorType::LOGICAL_LIMIT:
		case LogicalOperatorType::LOGICAL_ORDER_BY:
		case LogicalOperatorType::LOGICAL_TOP_N:
		case LogicalOperatorType::LOGICAL_DISTINCT:
		case LogicalOperatorType::LOGICAL_PIVOT:
		case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
		case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		case LogicalOperatorType::LOGICAL_ANY_JOIN:
		case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
		case LogicalOperatorType::LOGICAL_ASOF_JOIN:
		case LogicalOperatorType::LOGICAL_UNION:
		case LogicalOperatorType::LOGICAL_EXCEPT:
		case LogicalOperatorType::LOGICAL_INTERSECT:
			return true;
		case LogicalOperatorType::LOGICAL_GET: {
			auto &get = op.Cast<LogicalGet>();
			if (get.bind_data && !get.function.HasSerializationCallbacks() && get.parameters.empty()) {
				// Without serialization callbacks, the serialized form carries only the call parameters
				// (see LogicalGet::Serialize). A scan without positional parameters - e.g., one created through
				// an attached catalog - keeps its identity solely in the bind data, and its options do not
				// identify what it reads, so equal serialized bytes cannot prove that two scans read the same
				// table.
				return false;
			}
			return true;
		}
		case LogicalOperatorType::LOGICAL_CHUNK_GET:
			// Avoid serializing massive amounts of data (this is here because of the "Test TPCH arrow roundtrip" test)
			return op.Cast<LogicalColumnDataGet>().collection->Count() < 1000;
		default:
			// Unsupported:
			// - case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
			// - case LogicalOperatorType::LOGICAL_SAMPLE:
			// - case LogicalOperatorType::LOGICAL_COPY_DATABASE:
			// - case LogicalOperatorType::LOGICAL_DELIM_GET:
			// - case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
			// - case LogicalOperatorType::LOGICAL_CTE_REF:
			// - case LogicalOperatorType::LOGICAL_JOIN:
			// - case LogicalOperatorType::LOGICAL_DELIM_JOIN:
			// - case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
			// - case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
			// - case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
			// - case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR
			return false;
		}
	}

private:
	const MemoryStream &stream;
	const idx_t offset;
	const idx_t length;

	const hash_t signature_hash;

	const vector<reference<PlanSignature>> child_signatures;
	const idx_t operator_count;
};

struct PlanSignatureHash {
	std::size_t operator()(const PlanSignature &k) const {
		return k.HashSignature();
	}
};

struct PlanSignatureEquality {
	bool operator()(const PlanSignature &a, const PlanSignature &b) const {
		return a.Equals(b);
	}
};

struct Subplan {
	reference<unique_ptr<LogicalOperator>> op;
	arena_vector<ColumnBinding> canonical_bindings;
};

struct SubplanInfo {
	SubplanInfo(ArenaAllocator &allocator, unique_ptr<LogicalOperator> &op,
	            arena_vector<idx_t> &&canonical_table_indices_p, arena_vector<ColumnBinding> &&canonical_bindings)
	    : canonical_table_indices(std::move(canonical_table_indices_p)), subplans(allocator),
	      lowest_common_ancestor(op) {
		subplans.push_back({op, std::move(canonical_bindings)});
	}
	arena_vector<idx_t> canonical_table_indices;
	arena_vector<Subplan> subplans;
	reference<unique_ptr<LogicalOperator>> lowest_common_ancestor;
};

using subplan_map_t = unordered_map<reference<PlanSignature>, SubplanInfo, PlanSignatureHash, PlanSignatureEquality>;

//===--------------------------------------------------------------------===//
// CommonSubplanFinder
//===--------------------------------------------------------------------===//
class CommonSubplanFinder {
public:
	explicit CommonSubplanFinder(ClientContext &context) : state(context) {
	}

private:
	struct OperatorInfo {
		OperatorInfo(unique_ptr<LogicalOperator> &parent_p, const idx_t &depth_p) : parent(parent_p), depth(depth_p) {
		}

		unique_ptr<LogicalOperator> &parent;
		const idx_t depth;
		arena_ptr<PlanSignature> signature;
	};

	struct StackNode {
		explicit StackNode(unique_ptr<LogicalOperator> &op_p) : op(op_p), child_index(0) {
		}

		bool HasMoreChildren() const {
			return child_index < op->children.size();
		}

		unique_ptr<LogicalOperator> &GetNextChild() {
			D_ASSERT(child_index < op->children.size());
			return op->children[child_index++];
		};

		unique_ptr<LogicalOperator> &op;
		idx_t child_index;
	};

	struct SubplanStats {
		idx_t base_table_count;
		idx_t max_base_table_cardinality;

		void Combine(const SubplanStats &other) {
			this->base_table_count += other.base_table_count;
			this->max_base_table_cardinality =
			    MaxValue(this->max_base_table_cardinality, other.max_base_table_cardinality);
		}
	};

public:
	void FindCommonSubplans(reference<unique_ptr<LogicalOperator>> root) {
		// Find first operator with more than 1 child
		while (root.get()->children.size() == 1) {
			root = root.get()->children[0];
		}

		// Recurse through query plan using stack-based recursion
		arena_vector<StackNode> stack(state.allocator);
		stack.emplace_back(root);
		operator_infos.emplace(root, OperatorInfo(root, 0));

		while (!stack.empty()) {
			auto &current = stack.back();

			// Depth-first
			if (current.HasMoreChildren()) {
				auto &child = current.GetNextChild();
				operator_infos.emplace(child, OperatorInfo(current.op, stack.size()));
				stack.emplace_back(child);
				continue;
			}

			if (!RefersToSameObject(current.op, root.get())) {
				// We have all child information for this operator now, compute signature
				D_ASSERT(operator_infos.find(current.op) != operator_infos.end());
				auto &signature = operator_infos.find(current.op)->second.signature;
				signature = CreatePlanSignature(current.op);

				// Add to subplans (if we got actually got a signature)
				if (signature) {
					const auto current_op_table_index = current.op->GetTableIndex();
					auto it = subplans.find(*signature);
					if (it == subplans.end()) {
						// New subplan, map table indices
						arena_vector<idx_t> canonical_table_indices(state.allocator);
						for (auto &table_index : current_op_table_index) {
							const auto next_canonical_table_index = to_canonical_table_index.size();
							canonical_table_indices.push_back(next_canonical_table_index);
							D_ASSERT(to_canonical_table_index.find(table_index) == to_canonical_table_index.end());
							to_canonical_table_index.emplace(table_index, next_canonical_table_index);
						}

						// Add new subplan
						SubplanInfo subplan_info(state.allocator, current.op, std::move(canonical_table_indices),
						                         GetCanonicalBindings(*current.op));
						subplans.emplace(*signature, std::move(subplan_info));
					} else {
						auto &info = it->second;
						// Matches existing subplan, map already existing table indices
						D_ASSERT(current_op_table_index.size() == info.canonical_table_indices.size());
						for (idx_t i = 0; i < current_op_table_index.size(); i++) {
							to_canonical_table_index.emplace(current_op_table_index[i],
							                                 info.canonical_table_indices[i]);
						}

						// Add subplan to existing
						info.subplans.push_back({current.op, GetCanonicalBindings(*current.op)});
						info.lowest_common_ancestor = LowestCommonAncestor(info.lowest_common_ancestor, current.op);
					}
				}
			}

			// Done with current
			stack.pop_back();
		}
	}

	void FilterSubplans() {
		// Filter out redundant or ineligible subplans before returning
		vector<subplan_map_t::key_type> to_remove;
		for (auto &entry : subplans) {
			auto &signature = entry.first.get();
			auto &subplan_info = entry.second;
			if (signature.OperatorCount() == 1) {
				to_remove.push_back(signature); // Just one operator in this subplan
				continue;
			}

			if (subplan_info.subplans.size() == 1) {
				to_remove.push_back(signature); // No other identical subplan
				continue;
			}

			D_ASSERT(operator_infos.find(subplan_info.subplans[0].op) != operator_infos.end());
			auto &parent_op = operator_infos.find(subplan_info.subplans[0].op)->second.parent;
			D_ASSERT(operator_infos.find(parent_op) != operator_infos.end());
			auto &parent_signature = operator_infos.find(parent_op)->second.signature;
			if (parent_signature) {
				auto parent_it = subplans.find(*parent_signature);
				if (parent_it != subplans.end()) {
					const auto subplan_count = subplan_info.subplans.size() * signature.OperatorCount();
					const auto parent_subplan_count = parent_it->second.subplans.size();
					const auto parent_count = parent_subplan_count * parent_signature->OperatorCount();
					if (parent_subplan_count != 1 && parent_count >= subplan_count) {
						to_remove.push_back(signature); // Parent is better, this one is redundant
						continue;
					}
				}
			}

			// We can bail on a subplan for various reasons (some of which could potentially be fixed)
			bool bail = false;

			for (auto &subplan : subplan_info.subplans) {
				column_binding_set_t subplan_bindings;
				for (auto &cb : subplan.canonical_bindings) {
					if (subplan_bindings.find(cb) != subplan_bindings.end()) {
						bail =
						    true; // Subplan contains duplicate column bindings, i.e., another nested duplicate subplan
						break;
					}
					subplan_bindings.insert(cb);
				}
				if (bail) {
					break;
				}
			}

			if (bail || !SelectPrimarySubplan(subplan_info)) {
				to_remove.push_back(signature);
				continue;
			}
		}

		// Only remove them all at the end so the logic above doesn't get affected
		for (auto &signature : to_remove) {
			subplans.erase(signature);
		}
	}

	void ConvertSubplansToCTEs(Optimizer &optimizer, unique_ptr<LogicalOperator> &op) {
		const auto sorted_subplans = GetSortedSubplans();
		idx_t index = 1;
		bool converted_subplans = false;
		for (auto &entry : sorted_subplans) {
			auto &subplan_info = entry.get().second;
			if (!ShouldMaterialize(subplan_info)) {
				continue; // No longer worth materializing due to other materializations
			}
			if (!SelectPrimarySubplan(subplan_info)) {
				continue; // Nested subplans were replaced by CTE refs that no longer expose the required bindings
			}

			const auto cte_index = optimizer.binder.GenerateTableIndex();
			const auto cte_name = StringUtil::Format("__common_subplan_%llu", index++);
			if (!min_cte_idx.IsValid()) {
				min_cte_idx = cte_index;
			}

			// Resolve types to be used for creating the materialized CTE and refs
			op->ResolveOperatorTypes();

			const auto &primary_subplan = subplan_info.subplans[0];

			vector<vector<ColumnBinding>> old_bindings(subplan_info.subplans.size());
			vector<vector<LogicalType>> old_types(subplan_info.subplans.size());
			arena_vector<ColumnBinding> required_canonical_bindings(state.allocator);
			column_binding_set_t required_canonical_binding_set;
			for (idx_t subplan_idx = 0; subplan_idx < subplan_info.subplans.size(); subplan_idx++) {
				auto &subplan = subplan_info.subplans[subplan_idx];
				old_bindings[subplan_idx] = subplan.op.get()->GetColumnBindings();
				old_types[subplan_idx] = subplan.op.get()->types;
				for (auto &cb : subplan.canonical_bindings) {
					if (required_canonical_binding_set.find(cb) != required_canonical_binding_set.end()) {
						continue;
					}
					required_canonical_binding_set.insert(cb);
					required_canonical_bindings.push_back(cb);
				}
			}

			if (required_canonical_bindings.size() != primary_subplan.canonical_bindings.size()) {
				// The signature ignores projection maps. If duplicate subplans expose different
				// subsets of the same work, widen the materialized producer just enough to make
				// every referenced output available, then project each reader back to its original
				// schema below.
				ClearProjectionMaps(*primary_subplan.op.get());
				primary_subplan.op.get()->ResolveOperatorTypes();
			}

			const auto materialized_bindings = primary_subplan.op.get()->GetColumnBindings();
			const auto materialized_canonical_bindings = GetCanonicalBindings(*primary_subplan.op.get());
			column_binding_map_t<idx_t> materialized_binding_index;
			for (idx_t i = 0; i < materialized_canonical_bindings.size(); i++) {
				materialized_binding_index.emplace(materialized_canonical_bindings[i], i);
			}

			vector<LogicalType> types;
			vector<ColumnBinding> materialized_output_bindings;
			types.reserve(required_canonical_bindings.size());
			materialized_output_bindings.reserve(required_canonical_bindings.size());
			column_binding_map_t<idx_t> cte_binding_index;
			for (idx_t i = 0; i < required_canonical_bindings.size(); i++) {
				auto &cb = required_canonical_bindings[i];
				const auto entry = materialized_binding_index.find(cb);
				D_ASSERT(entry != materialized_binding_index.end()); // guaranteed by FilterSubplans
				const auto materialized_col_idx = entry->second;
				cte_binding_index.emplace(cb, i);
				types.push_back(primary_subplan.op.get()->types[materialized_col_idx]);
				materialized_output_bindings.push_back(materialized_bindings[materialized_col_idx]);
			}

			// Get names
			vector<Identifier> col_names;
			for (idx_t i = 0; i < types.size(); i++) {
				col_names.emplace_back(StringUtil::Format("%s_col_%llu", cte_name, i + 1));
			}
			vector<vector<idx_t>> cte_column_indexes(subplan_info.subplans.size());
			vector<bool> needs_projection(subplan_info.subplans.size(), false);
			for (idx_t subplan_idx = 0; subplan_idx < subplan_info.subplans.size(); subplan_idx++) {
				const auto &subplan = subplan_info.subplans[subplan_idx];
				const auto &canonical_bindings = subplan.canonical_bindings;
				cte_column_indexes[subplan_idx].reserve(canonical_bindings.size());
				needs_projection[subplan_idx] = canonical_bindings.size() != types.size();
				for (idx_t i = 0; i < canonical_bindings.size(); i++) {
					const auto &cb = canonical_bindings[i];
					const auto entry = cte_binding_index.find(cb);
					D_ASSERT(entry != cte_binding_index.end()); // guaranteed by FilterSubplans
					const auto cte_col_idx = entry->second;
					// Types must match: same canonical binding = same base column = same type
					D_ASSERT(old_types[subplan_idx][i] == types[cte_col_idx]);
					cte_column_indexes[subplan_idx].push_back(cte_col_idx);
					needs_projection[subplan_idx] = needs_projection[subplan_idx] || cte_col_idx != i;
				}
			}

			// Create CTE refs and figure out column binding replacements
			vector<unique_ptr<LogicalOperator>> cte_refs;
			ColumnBindingReplacer replacer;
			for (idx_t subplan_idx = 0; subplan_idx < subplan_info.subplans.size(); subplan_idx++) {
				const auto &subplan = subplan_info.subplans[subplan_idx];
				const auto cte_ref_index = optimizer.binder.GenerateTableIndex();
				cte_refs.emplace_back(make_uniq<LogicalCTERef>(cte_ref_index, cte_index, types, col_names));
				if (subplan.op.get()->has_estimated_cardinality) {
					cte_refs.back()->SetEstimatedCardinality(subplan.op.get()->estimated_cardinality);
				}
				auto new_bindings = cte_refs.back()->GetColumnBindings();
				if (needs_projection[subplan_idx]) {
					// Preserve each subplan's original output order when it differs from the
					// materialized CTE.
					vector<unique_ptr<Expression>> select_list;
					for (auto cte_col_idx : cte_column_indexes[subplan_idx]) {
						select_list.emplace_back(make_uniq<BoundColumnRefExpression>(
						    types[cte_col_idx], ColumnBinding(cte_ref_index, ProjectionIndex(cte_col_idx))));
					}

					// Place the projection on top
					auto proj =
					    make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(select_list));
					proj->children.emplace_back(std::move(cte_refs.back()));
					cte_refs.back() = std::move(proj);
					new_bindings = cte_refs.back()->GetColumnBindings();
				}
				D_ASSERT(old_bindings[subplan_idx].size() == new_bindings.size());
				D_ASSERT(subplan.canonical_bindings.size() == new_bindings.size());
				for (idx_t i = 0; i < old_bindings[subplan_idx].size(); i++) {
					replacer.replacement_bindings.emplace_back(old_bindings[subplan_idx][i], new_bindings[i]);
#ifdef D_ASSERT_IS_ENABLED
					const auto inserted = generated_binding_map.emplace(new_bindings[i], subplan.canonical_bindings[i]);
					D_ASSERT(inserted.second);
#else
					generated_binding_map.emplace(new_bindings[i], subplan.canonical_bindings[i]);
#endif
				}
			}

			// Create the materialized CTE and replace the common subplans with references to it
			auto &lowest_common_ancestor = subplan_info.lowest_common_ancestor.get();
			const auto materialized_column_count = types.size();
			auto materialized_subplan = std::move(primary_subplan.op.get());
			auto remainder = std::move(lowest_common_ancestor);
			vector<unique_ptr<Expression>> materialized_select_list;
			for (idx_t i = 0; i < materialized_output_bindings.size(); i++) {
				materialized_select_list.emplace_back(
				    make_uniq<BoundColumnRefExpression>(types[i], materialized_output_bindings[i]));
			}
			auto materialized_projection = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(),
			                                                            std::move(materialized_select_list));
			materialized_projection->children.emplace_back(std::move(materialized_subplan));
			auto cte = make_uniq<LogicalMaterializedCTE>(Identifier(cte_name), cte_index, materialized_column_count,
			                                             std::move(materialized_projection), std::move(remainder),
			                                             CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
			for (idx_t subplan_idx = 0; subplan_idx < subplan_info.subplans.size(); subplan_idx++) {
				const auto &subplan = subplan_info.subplans[subplan_idx];
				subplan.op.get() = std::move(cte_refs[subplan_idx]);
			}
			lowest_common_ancestor = std::move(cte);

			// Replace bindings of subplans with those of the CTE refs
			replacer.stop_operator = lowest_common_ancestor.get();
			replacer.VisitOperator(*op);                                  // Replace from the root until CTE
			replacer.VisitOperator(*lowest_common_ancestor->children[1]); // Replace in CTE child

			// We have to be careful with the order in which we place the CTEs created by this optimizer
			// Pipeline dependencies cannot be set up if CTEs are in the wrong order
			// This performs "CTE pushdown" until the CTEs are in the correct order
			reference<unique_ptr<LogicalOperator>> current_op = lowest_common_ancestor;
			while (true) {
				auto &rhs_child = current_op.get()->children[1];
				if (rhs_child->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
					const auto child_table_index = rhs_child->Cast<LogicalMaterializedCTE>().table_index;
					if (child_table_index >= min_cte_idx && child_table_index < cte_index) {
						auto tmp = std::move(rhs_child->children[1]);
						rhs_child->children[1] = std::move(current_op.get());
						current_op.get() = std::move(rhs_child);
						rhs_child = std::move(tmp);
						current_op = current_op.get()->children[1];
						continue;
					}
				}
				break;
			}
			converted_subplans = true;
		}
		if (converted_subplans) {
			// Subplan replacement changes child output bindings under existing positional projection maps.
			// Invalidate them here; column lifetime runs again later and rebuilds the maps.
			ClearProjectionMaps(*op);
		}
	}

private:
	arena_ptr<PlanSignature> CreatePlanSignature(const unique_ptr<LogicalOperator> &op) {
		vector<reference<PlanSignature>> child_signatures;
		for (auto &child : op->children) {
			auto it = operator_infos.find(child);
			D_ASSERT(it != operator_infos.end());
			if (!it->second.signature) {
				return nullptr; // Failed to create signature from one of the children
			}
			child_signatures.emplace_back(*it->second.signature);
		}
		return PlanSignature::Create(state, *op, std::move(child_signatures));
	}

	arena_vector<ColumnBinding> GetCanonicalBindings(LogicalOperator &op) {
		// Compute the canonical column bindings coming out of this operator for convenience later
		const auto original_bindings = op.GetColumnBindings();
		return GetCanonicalBindings(original_bindings);
	}

	unique_ptr<LogicalOperator> &LowestCommonAncestor(reference<unique_ptr<LogicalOperator>> a,
	                                                  reference<unique_ptr<LogicalOperator>> b) {
		auto a_it = operator_infos.find(a);
		auto b_it = operator_infos.find(b);
		D_ASSERT(a_it != operator_infos.end() && b_it != operator_infos.end());

		// Get parents of a and b until they're at the same depth
		while (a_it->second.depth > b_it->second.depth) {
			a = a_it->second.parent;
			a_it = operator_infos.find(a);
			D_ASSERT(a_it != operator_infos.end());
		}
		while (b_it->second.depth > a_it->second.depth) {
			b = b_it->second.parent;
			b_it = operator_infos.find(b);
			D_ASSERT(b_it != operator_infos.end());
		}

		// Move up one level at a time for both until ancestor is the same
		while (!RefersToSameObject(a, b)) {
			a_it = operator_infos.find(a);
			b_it = operator_infos.find(b);
			D_ASSERT(a_it != operator_infos.end() && b_it != operator_infos.end());
			a = a_it->second.parent;
			b = b_it->second.parent;
		}

		return a.get();
	}

	arena_vector<ColumnBinding> GetCanonicalBindings(const vector<ColumnBinding> &original_bindings) {
		const auto &table_index_map = state.table_index_map.GetMap();
		arena_vector<ColumnBinding> canonical_bindings(state.allocator);
		for (auto &cb : original_bindings) {
			auto generated_entry = generated_binding_map.find(cb);
			if (generated_entry != generated_binding_map.end()) {
				canonical_bindings.push_back(generated_entry->second);
				continue;
			}
			const auto canonical_table_index = to_canonical_table_index.at(cb.table_index);
			auto &table_map = table_index_map.at(cb.table_index);
			if (table_map.Empty<ConversionType::TO_CANONICAL>()) {
				canonical_bindings.emplace_back(canonical_table_index, cb.column_index);
			} else {
				const auto canonical_col_idx = table_map.Get<ConversionType::TO_CANONICAL>(cb.column_index);
				canonical_bindings.emplace_back(canonical_table_index, canonical_col_idx);
			}
		}
		return canonical_bindings;
	}

	arena_vector<ColumnBinding> GetExpandedCanonicalBindings(LogicalOperator &op) {
		return GetCanonicalBindings(GetExpandedColumnBindings(op));
	}

	//! Moves a subplan that can expose every binding required by the duplicate occurrences to the front
	bool SelectPrimarySubplan(SubplanInfo &subplan_info) {
		column_binding_set_t required_bindings;
		for (auto &subplan : subplan_info.subplans) {
			required_bindings.insert(subplan.canonical_bindings.begin(), subplan.canonical_bindings.end());
		}

		optional_idx primary_subplan_idx;
		idx_t primary_subplan_binding_count = 0;
		for (idx_t subplan_idx = 0; subplan_idx < subplan_info.subplans.size(); subplan_idx++) {
			auto &subplan = subplan_info.subplans[subplan_idx];
			const auto expanded_bindings = GetExpandedCanonicalBindings(*subplan.op.get());
			bool contains_required_bindings = true;
			for (auto &cb : required_bindings) {
				if (std::find(expanded_bindings.begin(), expanded_bindings.end(), cb) == expanded_bindings.end()) {
					contains_required_bindings = false;
					break;
				}
			}
			if (!contains_required_bindings) {
				continue;
			}
			if (!primary_subplan_idx.IsValid() || subplan.canonical_bindings.size() > primary_subplan_binding_count) {
				primary_subplan_idx = subplan_idx;
				primary_subplan_binding_count = subplan.canonical_bindings.size();
			}
		}
		if (!primary_subplan_idx.IsValid()) {
			return false;
		}
		std::swap(subplan_info.subplans[0], subplan_info.subplans[primary_subplan_idx.GetIndex()]);
		return true;
	}

	static void ClearProjectionMaps(LogicalOperator &op) {
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_ANY_JOIN:
		case LogicalOperatorType::LOGICAL_ASOF_JOIN:
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
			auto &join = op.Cast<LogicalJoin>();
			join.left_projection_map.clear();
			join.right_projection_map.clear();
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER:
			op.Cast<LogicalFilter>().projection_map.clear();
			break;
		case LogicalOperatorType::LOGICAL_ORDER_BY:
			op.Cast<LogicalOrder>().projection_map.clear();
			break;
		default:
			break;
		}
		for (auto &child : op.children) {
			ClearProjectionMaps(*child);
		}
	}

	bool ShouldMaterialize(const SubplanInfo &subplan_info) const {
		auto &subplan = subplan_info.subplans[0].op.get();
		return CTEInlining::EndsInAggregateOrDistinct(*subplan) || IsSelectiveMultiTablePlan(subplan);
	}

	static SubplanStats GetSubplanStats(const LogicalOperator &op) {
		SubplanStats subplan_stats;
		if (op.children.empty()) {
			subplan_stats.base_table_count = 1;
			subplan_stats.max_base_table_cardinality = op.has_estimated_cardinality ? op.estimated_cardinality : 0;
		} else {
			subplan_stats = GetSubplanStats(*op.children[0]);
			for (idx_t i = 1; i < op.children.size(); i++) {
				subplan_stats.Combine(GetSubplanStats(*op.children[i]));
			}
		}
		return subplan_stats;
	}

	static bool IsSelectiveMultiTablePlan(unique_ptr<LogicalOperator> &op) {
		static constexpr idx_t CARDINALITY_THRESHOLD = 2048;
		static constexpr idx_t CARDINALITY_RATIO = 2;

		// Must have an estimated cardinality
		if (!op->has_estimated_cardinality) {
			return false;
		}

		// Must select more than 1 base table
		const auto subplan_stats = GetSubplanStats(*op);
		if (subplan_stats.base_table_count < 2) {
			return false;
		}

		// If it has this cardinality or less, just materialize
		if (op->estimated_cardinality <= CARDINALITY_THRESHOLD) {
			return true;
		}

		// Otherwise, materialize if it is selective enough
		return op->estimated_cardinality < subplan_stats.max_base_table_cardinality / CARDINALITY_RATIO;
	}

	vector<reference<subplan_map_t::value_type>> GetSortedSubplans() {
		// Grab entries from map, and sort by smallest plans first
		vector<reference<subplan_map_t::value_type>> subplan_infos;
		for (auto &entry : subplans) {
			subplan_infos.push_back(entry);
		}
		std::sort(subplan_infos.begin(), subplan_infos.end(),
		          [](reference<subplan_map_t::value_type> lhs, reference<subplan_map_t::value_type> rhs) {
			          return lhs.get().first.get().OperatorCount() < rhs.get().first.get().OperatorCount();
		          });
		return subplan_infos;
	}

private:
	//! State for creating PlanSignature with reusable data structures
	PlanSignatureCreateState state;
	//! Mapping from operator to info
	reference_map_t<unique_ptr<LogicalOperator>, OperatorInfo> operator_infos;
	//! Mapping from subplan signature to subplan information
	subplan_map_t subplans;
	//! Mapping from original table index to canonical table index
	unordered_map<TableIndex, TableIndex> to_canonical_table_index;
	//! Mapping from generated CTE bindings to canonical bindings
	column_binding_map_t<ColumnBinding> generated_binding_map;
	//! Minimum CTE index created by this optimizer
	TableIndex min_cte_idx;
};

//===--------------------------------------------------------------------===//
// CommonSubplanOptimizer
//===--------------------------------------------------------------------===//
CommonSubplanOptimizer::CommonSubplanOptimizer(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

unique_ptr<LogicalOperator> CommonSubplanOptimizer::Optimize(unique_ptr<LogicalOperator> op) {
	// Bottom-up identification of identical subplans
	CommonSubplanFinder finder(optimizer.context);
	finder.FindCommonSubplans(op);
	finder.FilterSubplans();
	finder.ConvertSubplansToCTEs(optimizer, op);
	return op;
}

} // namespace duckdb
