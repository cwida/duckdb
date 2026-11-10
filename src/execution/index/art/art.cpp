#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/main/attached_database.hpp"

#include "duckdb/common/types/conflict_manager.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/index/art/art_builder.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/art_merger.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"
#include "duckdb/execution/index/art/art_scanner.hpp"
#include "duckdb/execution/index/art/base_leaf.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/iterator.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/node256.hpp"
#include "duckdb/execution/index/art/node256_leaf.hpp"
#include "duckdb/execution/index/art/node48.hpp"
#include "duckdb/execution/index/art/prefix.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/execution/index/index_lock.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {

struct ARTIndexScanState : public IndexScanState {
	//! The predicates to scan.
	//! A single predicate for point lookups, and two predicates for range scans.
	Value values[2];
	//! The expressions over the scan predicates.
	ExpressionType expressions[2];
	bool checked = false;
	//! All scanned row IDs.
	set<row_t> row_ids;
};

//===--------------------------------------------------------------------===//
// ART
//===--------------------------------------------------------------------===//

static idx_t GetNode256LeafSegmentSize(const IndexStorageInfo &info, StorageVersion storage_version) {
	// Preserve the oversized mask and padding of the persisted legacy layout.
	struct LegacyNode256Leaf {
		uint16_t count;
		validity_t mask[Node256Leaf::CAPACITY / sizeof(validity_t)];
	};
	constexpr idx_t compact_size = sizeof(Node256Leaf);
	constexpr idx_t legacy_size = sizeof(LegacyNode256Leaf);
	const auto allocator_idx = NodePtr::GetAllocatorIdx(NType::NODE_256_LEAF);
	if (info.allocator_infos.size() > allocator_idx) {
		// Preserve the persisted segment size so existing buffers retain their original layout.
		const auto saved_segment_size = info.allocator_infos[allocator_idx].segment_size;
		if (saved_segment_size != compact_size && saved_segment_size != legacy_size) {
			throw SerializationException("Invalid Node256Leaf segment size %llu (expected %llu or %llu)",
			                             saved_segment_size, compact_size, legacy_size);
		}
		return saved_segment_size;
	}
	// New indexes must retain the original layout when targeting older storage versions.
	return storage_version >= StorageVersion::V2_0_0 ? compact_size : legacy_size;
}

ART::ART(const Identifier &name, const IndexConstraintType index_constraint_type, const vector<column_t> &column_ids,
         TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
         AttachedDatabase &db,
         const shared_ptr<array<unsafe_unique_ptr<FixedSizeAllocator>, ALLOCATOR_COUNT>> &allocators_ptr,
         const IndexStorageInfo &info)
    : BoundIndex(name, ART::TYPE_NAME, index_constraint_type, column_ids, table_io_manager, unbound_expressions, db),
      allocators(allocators_ptr), owns_data(false) {
	// FIXME: Use the new byte representation function to support nested types.
	for (idx_t i = 0; i < types.size(); i++) {
		switch (types[i]) {
		case PhysicalType::BOOL:
		case PhysicalType::INT8:
		case PhysicalType::INT16:
		case PhysicalType::INT32:
		case PhysicalType::INT64:
		case PhysicalType::INT128:
		case PhysicalType::UINT8:
		case PhysicalType::UINT16:
		case PhysicalType::UINT32:
		case PhysicalType::UINT64:
		case PhysicalType::UINT128:
		case PhysicalType::FLOAT:
		case PhysicalType::DOUBLE:
		case PhysicalType::VARCHAR:
			break;
		default:
			throw InvalidTypeException(logical_types[i], "Invalid type for index key.");
		}
	}

	// Initialize the allocators.
	SetPrefixCount(info);
	if (!allocators) {
		owns_data = true;
		auto prefix_size = NumericCast<idx_t>(prefix_count) + NumericCast<idx_t>(Prefix::METADATA_SIZE);
		auto &block_manager = table_io_manager.GetIndexBlockManager();
		const auto leaf256_size = GetNode256LeafSegmentSize(info, db.GetStorageManager().GetStorageVersion());

		array<unsafe_unique_ptr<FixedSizeAllocator>, ALLOCATOR_COUNT> allocator_array = {
		    make_unsafe_uniq<FixedSizeAllocator>(prefix_size, block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(sizeof(Leaf), block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(sizeof(Node4), block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(sizeof(Node16), block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(sizeof(Node48), block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(sizeof(Node256), block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(sizeof(Node7Leaf), block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(sizeof(Node15Leaf), block_manager),
		    make_unsafe_uniq<FixedSizeAllocator>(leaf256_size, block_manager),
		};
		allocators =
		    make_shared_ptr<array<unsafe_unique_ptr<FixedSizeAllocator>, ALLOCATOR_COUNT>>(std::move(allocator_array));
	}

	if (!info.IsValid()) {
		// We create a new ART.
		storage_version = db.GetStorageManager().GetStorageVersion();
		return;
	}

	if (info.root_block_ptr.IsValid()) {
		// Backwards compatibility.
		Deserialize(info.root_block_ptr);
		return;
	}

	// Set the root node and initialize the allocators.
	tree.Set(info.root);
	InitAllocators(info);

	// Set the storage version of the ART
	auto it = info.options.find("storage_version");
	if (it != info.options.end()) {
		// If this is an existing index with a saved storage version, use it.
		storage_version = static_cast<StorageVersion>(it->second.GetValue<uint64_t>());
	} else {
		// Otherwise, this must be an existing index without a saved storage version.
		// We started saving the storage version in v1.5.0, so if it is not present,
		// we can not make any general assumptions about the exact storage version.
		storage_version = StorageVersion::INVALID;
	}
}

//===--------------------------------------------------------------------===//
// Initialize Scans
//===--------------------------------------------------------------------===//

static unique_ptr<IndexScanState> InitializeScanSinglePredicate(const Value &value,
                                                                const ExpressionType expression_type) {
	auto result = make_uniq<ARTIndexScanState>();
	result->values[0] = value;
	result->expressions[0] = expression_type;
	return std::move(result);
}

static unique_ptr<IndexScanState> InitializeScanTwoPredicates(const Value &low_value,
                                                              const ExpressionType low_expression_type,
                                                              const Value &high_value,
                                                              const ExpressionType high_expression_type) {
	auto result = make_uniq<ARTIndexScanState>();
	result->values[0] = low_value;
	result->expressions[0] = low_expression_type;
	result->values[1] = high_value;
	result->expressions[1] = high_expression_type;
	return std::move(result);
}

unique_ptr<IndexScanState> ART::TryInitializeScan(const Expression &expr, const Expression &filter_expr) const {
	Value low_value, high_value, equal_value;
	ExpressionType low_comparison_type = ExpressionType::INVALID, high_comparison_type = ExpressionType::INVALID;

	// Try to find a matching index for any of the filter expressions.
	ComparisonExpressionMatcher matcher;

	// Match on a comparison type.
	matcher.expr_type = make_uniq<ComparisonExpressionTypeMatcher>();

	// Match on a constant comparison with the indexed expression.
	matcher.matchers.push_back(make_uniq<ExpressionEqualityMatcher>(expr));
	matcher.matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	matcher.policy = SetMatcher::Policy::UNORDERED;

	vector<reference<Expression>> bindings;
	auto filter_match =
	    matcher.Match(const_cast<Expression &>(filter_expr), bindings); // NOLINT: Match does not alter the expr.
	if (filter_match) {
		// This is a range or equality comparison with a constant value, so we can use the index.
		// 		bindings[0] = the expression
		// 		bindings[1] = the index expression
		// 		bindings[2] = the constant
		auto &comparison = bindings[0].get().Cast<BoundFunctionExpression>();
		auto constant_value = bindings[2].get().Cast<BoundConstantExpression>().GetValue();
		auto comparison_type = comparison.GetExpressionType();

		auto &left = BoundComparisonExpression::Left(comparison);
		if (left.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			// The expression is on the right side, we flip the comparison expression.
			comparison_type = FlipComparisonExpression(comparison_type);
		}

		if (comparison_type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			// Table filters discard NULL and false alike, so IS NOT DISTINCT FROM a
			// non-NULL constant selects the same rows as equality. NULL is not indexed.
			if (constant_value.IsNull()) {
				return nullptr;
			}
			equal_value = constant_value;
		} else if (comparison_type == ExpressionType::COMPARE_EQUAL) {
			// An equality value overrides any other bounds.
			equal_value = constant_value;
		} else if (comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		           comparison_type == ExpressionType::COMPARE_GREATERTHAN) {
			// This is a lower bound.
			low_value = constant_value;
			low_comparison_type = comparison_type;
		} else {
			// This is an upper bound.
			high_value = constant_value;
			high_comparison_type = comparison_type;
		}
	} else if (filter_expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
		auto &between = filter_expr.Cast<BoundFunctionExpression>();
		auto &input = BoundBetweenExpression::Input(between);
		if (!input.Equals(expr)) {
			// The expression does not match the index expression.
			return nullptr;
		}
		auto &lower_bound = BoundBetweenExpression::LowerBound(between);
		auto &upper_bound = BoundBetweenExpression::UpperBound(between);

		if (lower_bound.GetExpressionType() != ExpressionType::VALUE_CONSTANT ||
		    upper_bound.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			// Not a constant expression.
			return nullptr;
		}

		auto lower_inclusive = BoundBetweenExpression::LowerInclusive(between);
		auto upper_inclusive = BoundBetweenExpression::UpperInclusive(between);
		low_value = lower_bound.Cast<BoundConstantExpression>().GetValue();
		low_comparison_type =
		    lower_inclusive ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : ExpressionType::COMPARE_GREATERTHAN;
		high_value = (upper_bound.Cast<BoundConstantExpression>()).GetValue();
		high_comparison_type =
		    upper_inclusive ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN;
	}
	// FIXME: add another if...else... to match rewritten BETWEEN,
	// i.e., WHERE i BETWEEN 50 AND 1502 is rewritten to CONJUNCTION_AND.

	// We cannot use an index scan.
	if (equal_value.IsNull() && low_value.IsNull() && high_value.IsNull()) {
		return nullptr;
	}

	// Initialize the index scan state and return it.
	if (!equal_value.IsNull()) {
		// Equality predicate.
		return InitializeScanSinglePredicate(equal_value, ExpressionType::COMPARE_EQUAL);
	}
	if (!low_value.IsNull() && !high_value.IsNull()) {
		// Two-sided predicate.
		return InitializeScanTwoPredicates(low_value, low_comparison_type, high_value, high_comparison_type);
	}
	if (!low_value.IsNull()) {
		// Less-than predicate.
		return InitializeScanSinglePredicate(low_value, low_comparison_type);
	}
	// Greater-than predicate.
	return InitializeScanSinglePredicate(high_value, high_comparison_type);
}

unique_ptr<IndexScanState> ART::InitializeFullScan() {
	return make_uniq<ARTIndexScanState>();
}
//===--------------------------------------------------------------------===//
// ART Keys
//===--------------------------------------------------------------------===//

template <class T, bool IS_NOT_NULL>
static void TemplatedGenerateKeys(ArenaAllocator &allocator, const Vector &input, unsafe_vector<ARTKey> &keys) {
	const idx_t count = input.size();
	D_ASSERT(keys.size() >= count);

	UnifiedVectorFormat data;
	input.ToUnifiedFormat(data);
	auto input_data = UnifiedVectorFormat::GetData<T>(data);

	for (idx_t i = 0; i < count; i++) {
		auto idx = data.sel->get_index(i);
		if (IS_NOT_NULL || data.validity.RowIsValid(idx)) {
			ARTKey::CreateARTKey<T>(allocator, keys[i], input_data[idx]);
			continue;
		}

		// We need to reset the key value in the reusable keys vector.
		keys[i] = ARTKey();
	}
}

template <class T, bool IS_NOT_NULL>
static void ConcatenateKeys(ArenaAllocator &allocator, const Vector &input, unsafe_vector<ARTKey> &keys) {
	const idx_t count = input.size();
	UnifiedVectorFormat data;
	input.ToUnifiedFormat(data);
	auto input_data = UnifiedVectorFormat::GetData<T>(data);

	for (idx_t i = 0; i < count; i++) {
		auto idx = data.sel->get_index(i);

		if (IS_NOT_NULL) {
			auto other_key = ARTKey::CreateARTKey<T>(allocator, input_data[idx]);
			keys[i].Concat(allocator, other_key);
			continue;
		}

		// A previous column entry was NULL.
		if (keys[i].Empty()) {
			continue;
		}

		// This column entry is NULL, so we set the whole key to NULL.
		if (!data.validity.RowIsValid(idx)) {
			keys[i] = ARTKey();
			continue;
		}

		// Concatenate the keys.
		auto other_key = ARTKey::CreateARTKey<T>(allocator, input_data[idx]);
		keys[i].Concat(allocator, other_key);
	}
}

template <bool IS_NOT_NULL>
void GenerateKeysInternal(ArenaAllocator &allocator, DataChunk &input, unsafe_vector<ARTKey> &keys) {
	switch (input.data[0].GetType().InternalType()) {
	case PhysicalType::BOOL:
		TemplatedGenerateKeys<bool, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::INT8:
		TemplatedGenerateKeys<int8_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::INT16:
		TemplatedGenerateKeys<int16_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::INT32:
		TemplatedGenerateKeys<int32_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::INT64:
		TemplatedGenerateKeys<int64_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::INT128:
		TemplatedGenerateKeys<hugeint_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::UINT8:
		TemplatedGenerateKeys<uint8_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::UINT16:
		TemplatedGenerateKeys<uint16_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::UINT32:
		TemplatedGenerateKeys<uint32_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::UINT64:
		TemplatedGenerateKeys<uint64_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::UINT128:
		TemplatedGenerateKeys<uhugeint_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::FLOAT:
		TemplatedGenerateKeys<float, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::DOUBLE:
		TemplatedGenerateKeys<double, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	case PhysicalType::VARCHAR:
		TemplatedGenerateKeys<string_t, IS_NOT_NULL>(allocator, input.data[0], keys);
		break;
	default:
		throw InternalException("Invalid type for index");
	}

	// We concatenate the keys for each remaining column of a compound key.
	for (idx_t i = 1; i < input.ColumnCount(); i++) {
		switch (input.data[i].GetType().InternalType()) {
		case PhysicalType::BOOL:
			ConcatenateKeys<bool, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::INT8:
			ConcatenateKeys<int8_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::INT16:
			ConcatenateKeys<int16_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::INT32:
			ConcatenateKeys<int32_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::INT64:
			ConcatenateKeys<int64_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::INT128:
			ConcatenateKeys<hugeint_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::UINT8:
			ConcatenateKeys<uint8_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::UINT16:
			ConcatenateKeys<uint16_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::UINT32:
			ConcatenateKeys<uint32_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::UINT64:
			ConcatenateKeys<uint64_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::UINT128:
			ConcatenateKeys<uhugeint_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::FLOAT:
			ConcatenateKeys<float, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::DOUBLE:
			ConcatenateKeys<double, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		case PhysicalType::VARCHAR:
			ConcatenateKeys<string_t, IS_NOT_NULL>(allocator, input.data[i], keys);
			break;
		default:
			throw InternalException("Invalid type for index");
		}
	}
}

template <>
void ART::GenerateKeys<>(ArenaAllocator &allocator, DataChunk &input, unsafe_vector<ARTKey> &keys) const {
	GenerateKeysInternal<false>(allocator, input, keys);
}

template <>
void ART::GenerateKeys<true>(ArenaAllocator &allocator, DataChunk &input, unsafe_vector<ARTKey> &keys) const {
	GenerateKeysInternal<true>(allocator, input, keys);
}

static bool KeyInputNeedConversion(const vector<LogicalType> &types, StorageVersion storage_version) {
	// We only started tracking the storage version of the index in v1.5.0.
	// Old GEOMETRY columns (pre v1.5.0) had a different internal representation.
	if (storage_version == StorageVersion::INVALID || (storage_version < StorageVersion::V1_5_0)) {
		for (auto &type : types) {
			// ART does not support nested types, so we only need to check the top-level type.
			if (type.id() == LogicalTypeId::GEOMETRY) {
				return true;
			}
		}
	}

	return false;
}

static void ConvertKeyInput(DataChunk &input, DataChunk &result) {
	vector<LogicalType> new_types;

	for (auto &type : input.GetTypes()) {
		if (type.id() == LogicalTypeId::GEOMETRY) {
			new_types.push_back(LogicalType::BLOB);
		} else {
			new_types.push_back(type);
		}
	}

	// Initialize the result chunk with the new types
	result.Initialize(Allocator::DefaultAllocator(), new_types, input.size());

	// Reference or convert the input data into the result chunk
	for (idx_t i = 0; i < input.ColumnCount(); i++) {
		if (input.data[i].GetType().id() == LogicalTypeId::GEOMETRY) {
			Geometry::ToSpatialGeometry(input.data[i], result.data[i], input.size());
		} else {
			result.data[i].Reference(input.data[i]);
		}
	}
}

void ART::GenerateKeyVectors(ArenaAllocator &allocator, DataChunk &input, const Vector &row_ids,
                             unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_id_keys) {
	auto key_input = &input;

	DataChunk converted_chunk;
	// Do we need to convert the input first before generating keys?
	if (KeyInputNeedConversion(input.GetTypes(), storage_version)) {
		ConvertKeyInput(input, converted_chunk);
		key_input = &converted_chunk;
	}

	GenerateKeys<>(allocator, *key_input, keys);

	DataChunk row_id_chunk;
	row_id_chunk.Initialize(Allocator::DefaultAllocator(), vector<LogicalType> {LogicalType::ROW_TYPE},
	                        key_input->size());
	row_id_chunk.data[0].Reference(row_ids);
	GenerateKeys<>(allocator, row_id_chunk, row_id_keys);
}

//===--------------------------------------------------------------------===//
// Build from sorted data.
//===--------------------------------------------------------------------===//

ARTConflictType ART::Build(unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_ids, const idx_t row_count) {
	ArenaAllocator arena(BufferAllocator::Get(db));
	ARTBuilder builder(arena, *this, keys, row_ids);
	builder.Init(tree, row_count - 1);

	auto result = builder.Build();
	if (result != ARTConflictType::NO_CONFLICT) {
		return result;
	}

#ifdef DEBUG
	set<row_t> row_ids_debug;
	Iterator it(*this);
	it.FindMinimum(tree);
	ARTKey empty_key = ARTKey();
	RowIdSetOutput output(row_ids_debug, NumericLimits<idx_t>().Maximum());
	it.Scan(empty_key, output, false);
	D_ASSERT(row_count == row_ids_debug.size());
#endif

	return ARTConflictType::NO_CONFLICT;
}

//===--------------------------------------------------------------------===//
// Insert and Constraint Checking
//===--------------------------------------------------------------------===//

ErrorData ART::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	l.AssertHeld(*this);
	IndexAppendInfo info;
	return Insert(l, chunk, row_ids, info);
}

ErrorData ART::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	l.AssertHeld(*this);
	D_ASSERT(row_ids.GetType().InternalType() == ROW_TYPE);
	auto row_count = chunk.size();

	ArenaAllocator arena(BufferAllocator::Get(db));
	unsafe_vector<ARTKey> keys(row_count);
	unsafe_vector<ARTKey> row_id_keys(row_count);
	GenerateKeyVectors(arena, chunk, row_ids, keys, row_id_keys);

	return InsertKeys(arena, keys, row_id_keys, row_count, DeleteIndexInfo(info.delete_indexes), info.append_mode,
	                  &chunk);
}

ErrorData ART::InsertKeys(ArenaAllocator &arena, unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_id_keys,
                          idx_t row_count, const DeleteIndexInfo &delete_info, IndexAppendMode append_mode,
                          optional_ptr<DataChunk> chunk) {
	auto conflict_type = ARTConflictType::NO_CONFLICT;
	optional_idx conflict_idx;
	auto was_empty = !tree.HasMetadata();

	// Insert the entries into the index.
	for (idx_t i = 0; i < row_count; i++) {
		if (keys[i].Empty()) {
			continue;
		}
		conflict_type = ARTOperator::Insert(arena, *this, tree, keys[i], 0, row_id_keys[i], GateStatus::GATE_NOT_SET,
		                                    delete_info, append_mode);
		if (conflict_type != ARTConflictType::NO_CONFLICT) {
			conflict_idx = i;
			break;
		}
	}

	// Remove any previously inserted entries.
	if (conflict_type != ARTConflictType::NO_CONFLICT) {
		D_ASSERT(conflict_idx.IsValid());
		for (idx_t i = 0; i < conflict_idx.GetIndex(); i++) {
			if (keys[i].Empty()) {
				continue;
			}
			D_ASSERT(tree.GetGateStatus() == GateStatus::GATE_NOT_SET);
			ARTOperator::Delete(*this, tree, keys[i], row_id_keys[i]);
		}
	}

	if (was_empty) {
		// All nodes are in-memory.
		VerifyAllocationsInternal();
	}

	if (conflict_type == ARTConflictType::CONSTRAINT) {
		// chunk is only null when called from MergeCheckpointDeltas.
		auto msg = chunk ? AppendRowError(*chunk, conflict_idx.GetIndex()) : string("???");
		return ErrorData(ConstraintException("PRIMARY KEY or UNIQUE constraint violation: duplicate key \"%s\"", msg));
	}

#ifdef DEBUG
	for (idx_t i = 0; i < row_count; i++) {
		if (keys[i].Empty()) {
			continue;
		}
		auto leaf = ARTOperator::Lookup(*this, tree, keys[i], 0);
		D_ASSERT(leaf);
		D_ASSERT(ARTOperator::LookupInLeaf(*this, leaf.Get(), row_id_keys[i]));
	}
#endif
	return ErrorData();
}

ErrorData ART::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	l.AssertHeld(*this);
	IndexAppendInfo info;
	return Append(l, chunk, row_ids, info);
}

ErrorData ART::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	l.AssertHeld(*this);
	// Execute all column expressions before inserting the data chunk.
	DataChunk expr_chunk;
	expr_chunk.Initialize(Allocator::DefaultAllocator(), logical_types);
	try {
		ExecuteExpressions(chunk, expr_chunk);
	} catch (std::exception &ex) {
		// nothing was inserted yet: return the error instead of throwing
		return ErrorData(ex);
	}

	// Now insert the data chunk.
	return Insert(l, expr_chunk, row_ids, info);
}

void ART::VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) {
	if (manager) {
		D_ASSERT(manager->GetVerifyExistenceType() == VerifyExistenceType::APPEND);
		return VerifyConstraint(chunk, info, *manager);
	}
	ConflictManager local_manager(VerifyExistenceType::APPEND, chunk.size());
	VerifyConstraint(chunk, info, local_manager);
}

//===--------------------------------------------------------------------===//
// Drop and Delete
//===--------------------------------------------------------------------===//

void ART::ResetStorage(IndexLock &index_lock) {
	index_lock.AssertHeld(*this);
	for (auto &allocator : *allocators) {
		allocator->Reset();
	}
	tree.Clear();
}

void ART::DisownStorage(IndexLock &index_lock) {
	index_lock.AssertHeld(*this);
	D_ASSERT(owns_data);
	for (auto &allocator : *allocators) {
		allocator->DisownBuffers();
	}
	tree.Clear();
}

idx_t ART::TryDelete(IndexLock &state, DataChunk &entries, Vector &row_ids, optional_ptr<SelectionVector> deleted_sel,
                     optional_ptr<SelectionVector> non_deleted_sel) {
	state.AssertHeld(*this);
	// FIXME: We could pass a row_count in here, as we sometimes don't have to delete all row IDs in the chunk,
	// FIXME: but rather all row IDs up to the conflicting row.
	auto row_count = entries.size();

	DataChunk expr_chunk;
	expr_chunk.Initialize(Allocator::DefaultAllocator(), logical_types);
	ExecuteExpressions(entries, expr_chunk);

	ArenaAllocator allocator(BufferAllocator::Get(db));
	unsafe_vector<ARTKey> keys(row_count);
	unsafe_vector<ARTKey> row_id_keys(row_count);
	GenerateKeyVectors(allocator, expr_chunk, row_ids, keys, row_id_keys);

	return DeleteKeys(keys, row_id_keys, row_count, deleted_sel, non_deleted_sel);
}

idx_t ART::DeleteKeys(unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_id_keys, idx_t row_count,
                      optional_ptr<SelectionVector> deleted_sel, optional_ptr<SelectionVector> non_deleted_sel) {
	idx_t delete_count = 0;
	for (idx_t i = 0; i < row_count; i++) {
		bool deleted = true;
		if (!keys[i].Empty()) {
			D_ASSERT(tree.GetGateStatus() == GateStatus::GATE_NOT_SET);
			deleted = ARTOperator::Delete(*this, tree, keys[i], row_id_keys[i]);
		}
		if (deleted) {
			if (deleted_sel) {
				deleted_sel->set_index(delete_count, i);
			}
			delete_count++;
		} else if (non_deleted_sel) {
			idx_t non_delete_count = i - delete_count;
			non_deleted_sel->set_index(non_delete_count, i);
		}
	}

	if (!tree.HasMetadata()) {
		// No more allocations.
		VerifyAllocationsInternal();
	}

#ifdef DEBUG
	for (idx_t i = 0; i < row_count; i++) {
		if (keys[i].Empty()) {
			continue;
		}
		auto leaf = ARTOperator::Lookup(*this, tree, keys[i], 0);
		if (leaf) {
			auto contains_row_id = ARTOperator::LookupInLeaf(*this, leaf.Get(), row_id_keys[i]);
			D_ASSERT(!contains_row_id);
		}
	}
#endif
	return delete_count;
}

bool ART::HasLegacyGeometryKeys() const {
	// Equivalent to asking whether key generation applies the legacy GEOMETRY conversion for this index.
	return KeyInputNeedConversion(logical_types, storage_version);
}

//===--------------------------------------------------------------------===//
// Point and range lookups
//===--------------------------------------------------------------------===//
bool ART::FullScan(idx_t max_count, set<row_t> &row_ids) const {
	if (!tree.HasMetadata()) {
		return true;
	}
	Iterator it(*this);
	it.FindMinimum(tree);
	const auto empty_key = ARTKey();
	RowIdSetOutput output(row_ids, max_count);
	return it.Scan(empty_key, output, false) == ARTScanResult::COMPLETED;
}

bool ART::SearchEqual(const ARTKey &key, idx_t max_count, set<row_t> &row_ids) const {
	auto leaf = ARTOperator::Lookup(*this, tree, key, 0);
	if (!leaf) {
		return true;
	}

	Iterator it(*this);
	it.FindMinimum(leaf.Get());
	const auto empty_key = ARTKey();
	RowIdSetOutput output(row_ids, max_count);
	return it.Scan(empty_key, output, false) == ARTScanResult::COMPLETED;
}

bool ART::SearchGreater(const ARTKey &key, bool equal, idx_t max_count, set<row_t> &row_ids) const {
	if (!tree.HasMetadata()) {
		return true;
	}

	// Find the lowest value that satisfies the predicate.
	Iterator it(*this);

	// Early-out, if the maximum value in the ART is lower than the lower bound.
	if (!it.LowerBound(tree, key, equal)) {
		return true;
	}

	// We continue the scan. We do not check the bounds as any value following this value is
	// greater and satisfies our predicate.
	RowIdSetOutput output(row_ids, max_count);
	return it.Scan(ARTKey(), output, false) == ARTScanResult::COMPLETED;
}

bool ART::SearchLess(const ARTKey &upper_bound, bool equal, idx_t max_count, set<row_t> &row_ids) const {
	if (!tree.HasMetadata()) {
		return true;
	}

	// Find the minimum value in the ART: we start scanning from this value.
	Iterator it(*this);
	it.FindMinimum(tree);

	// Early-out, if the minimum value is higher than the upper bound.
	if (it.current_key.GreaterThan(upper_bound, equal, it.GetNestedDepth())) {
		return true;
	}

	// Continue the scan until we reach the upper bound.
	RowIdSetOutput output(row_ids, max_count);
	return it.Scan(upper_bound, output, equal) == ARTScanResult::COMPLETED;
}

bool ART::SearchCloseRange(const ARTKey &lower_bound, const ARTKey &upper_bound, bool left_equal, bool right_equal,
                           idx_t max_count, set<row_t> &row_ids) const {
	if (!tree.HasMetadata()) {
		return true;
	}

	// Find the first node that satisfies the left predicate.
	Iterator it(*this);

	// Early-out, if the maximum value in the ART is lower than the lower bound.
	if (!it.LowerBound(tree, lower_bound, left_equal)) {
		return true;
	}

	// Continue the scan until we reach the upper bound.
	RowIdSetOutput output(row_ids, max_count);
	return it.Scan(upper_bound, output, right_equal) == ARTScanResult::COMPLETED;
}

bool ART::Scan(IndexScanState &state, const idx_t max_count, set<row_t> &row_ids) const {
	auto &scan_state = state.Cast<ARTIndexScanState>();
	if (scan_state.values[0].IsNull()) {
		// full scan
		IndexLock l(*this);
		return FullScan(max_count, row_ids);
	}
	D_ASSERT(scan_state.values[0].type().InternalType() == types[0]);
	ArenaAllocator arena_allocator(Allocator::Get(db));

	auto key = ARTKey::CreateKey(arena_allocator, scan_state.values[0], storage_version);

	IndexLock l(*this);
	if (scan_state.values[1].IsNull()) {
		// Single predicate.
		switch (scan_state.expressions[0]) {
		case ExpressionType::COMPARE_EQUAL:
			return SearchEqual(key, max_count, row_ids);
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return SearchGreater(key, true, max_count, row_ids);
		case ExpressionType::COMPARE_GREATERTHAN:
			return SearchGreater(key, false, max_count, row_ids);
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			return SearchLess(key, true, max_count, row_ids);
		case ExpressionType::COMPARE_LESSTHAN:
			return SearchLess(key, false, max_count, row_ids);
		default:
			throw InternalException("Index scan type not implemented");
		}
	}

	// Two predicates.
	D_ASSERT(scan_state.values[1].type().InternalType() == types[0]);

	auto upper_bound = ARTKey::CreateKey(arena_allocator, scan_state.values[1], storage_version);

	bool left_equal = scan_state.expressions[0] == ExpressionType ::COMPARE_GREATERTHANOREQUALTO;
	bool right_equal = scan_state.expressions[1] == ExpressionType ::COMPARE_LESSTHANOREQUALTO;
	return SearchCloseRange(key, upper_bound, left_equal, right_equal, max_count, row_ids);
}

//===--------------------------------------------------------------------===//
// More Constraint Checking
//===--------------------------------------------------------------------===//

string ART::GenerateErrorKeyName(DataChunk &input, idx_t row_idx) const {
	DataChunk expr_chunk;
	expr_chunk.Initialize(Allocator::DefaultAllocator(), logical_types);
	ExecuteExpressions(input, expr_chunk);

	string key_name;
	for (idx_t k = 0; k < expr_chunk.ColumnCount(); k++) {
		if (k > 0) {
			key_name += ", ";
		}
		key_name += unbound_expressions[k]->GetName() + ": " + expr_chunk.data[k].GetValue(row_idx).ToString();
	}
	return key_name;
}

string ART::GenerateConstraintErrorMessage(VerifyExistenceType verify_type, const string &key_name) const {
	switch (verify_type) {
	case VerifyExistenceType::APPEND: {
		// APPEND to PK/UNIQUE table, but node/key already exists in PK/UNIQUE table.
		string type = IsPrimary() ? "primary key" : "unique";
		return StringUtil::Format("Duplicate key \"%s\" violates %s constraint.", key_name, type);
	}
	case VerifyExistenceType::APPEND_FK: {
		// APPEND_FK to FK table, node/key does not exist in PK/UNIQUE table.
		return StringUtil::Format(
		    "Violates foreign key constraint because key \"%s\" does not exist in the referenced table", key_name);
	}
	case VerifyExistenceType::DELETE_FK: {
		// DELETE_FK that still exists in a FK table, i.e., not a valid delete.
		return StringUtil::Format(
		    "Violates foreign key constraint because key \"%s\" is still referenced by a foreign "
		    "key in a different table. If this is an unexpected constraint violation, please refer to our "
		    "foreign key limitations in the documentation",
		    key_name);
	}
	default:
		throw NotImplementedException("Type not implemented for VerifyExistenceType");
	}
}

void ART::VerifyLeaf(const NodePtr &leaf, const ARTKey &key, DeleteIndexInfo delete_index_info,
                     ConflictManager &manager, optional_idx &conflict_idx, idx_t i) const {
	// Get the set of deleted row ids for this value if we have any delete indexes
	vector<row_t> deleted_row_ids;
	if (delete_index_info.delete_indexes) {
		for (auto &index : *delete_index_info.delete_indexes) {
			auto &delete_art = index.get().Cast<ART>();
			auto deleted_leaf = ARTOperator::Lookup(delete_art, delete_art.tree, key, 0);
			if (!deleted_leaf) {
				continue;
			}
			// All leaves in the delete ART are inlined.
			if (deleted_leaf.Get().GetType() != NType::LEAF_INLINED) {
				throw InternalException("Non-inlined leaf?");
			}
			auto deleted_row_id = deleted_leaf.Get().GetRowId();
			deleted_row_ids.push_back(deleted_row_id);
		}
	}

	if (leaf.GetType() == NType::LEAF_INLINED) {
		auto this_row_id = leaf.GetRowId();
		if (!deleted_row_ids.empty()) {
			// The leaf is inlined, and the same key exists in the delete ART.
			// check if the row-id matches - if it does there is no conflict
			for (auto &deleted_row_id : deleted_row_ids) {
				if (deleted_row_id == this_row_id) {
					return;
				}
			}
		}

		if (manager.AddHit(i, this_row_id)) {
			conflict_idx = i;
		}
		return;
	}

	// Fast path for FOREIGN KEY constraints.
	// Up to here, the above code paths work implicitly for FKs, as the leaf is inlined.
	// FIXME: proper foreign key + delete ART support.
	if (index_constraint_type == IndexConstraintType::FOREIGN) {
		D_ASSERT(deleted_row_ids.empty());
		// We don't handle FK conflicts in UPSERT, so the row ID should not matter.
		if (manager.AddHit(i, MAX_ROW_ID)) {
			conflict_idx = i;
		}
		return;
	}

	// Scan the two row IDs in the leaf.
	Iterator it(*this);
	it.FindMinimum(leaf);
	ARTKey empty_key = ARTKey();
	set<row_t> row_ids;
	RowIdSetOutput output(row_ids, 2);
	auto result = it.Scan(empty_key, output, false);
	if (result != ARTScanResult::COMPLETED || row_ids.size() != 2) {
		throw InternalException("VerifyLeaf expects exactly two row IDs to be scanned");
	}

	if (!deleted_row_ids.empty()) {
		for (const auto row_id : row_ids) {
			for (auto deleted_row_id : deleted_row_ids) {
				if (deleted_row_id == row_id) {
					return;
				}
			}
		}
	}

	auto row_id_it = row_ids.begin();
	if (manager.AddHit(i, *row_id_it)) {
		conflict_idx = i;
	}
	row_id_it++;
	manager.AddSecondHit(i, *row_id_it);
}

void ART::VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) {
	// Lock the index during constraint checking.
	IndexLock l(*this);

	DataChunk expr_chunk;
	expr_chunk.Initialize(Allocator::DefaultAllocator(), logical_types);
	ExecuteExpressions(chunk, expr_chunk);

	ArenaAllocator arena_allocator(BufferAllocator::Get(db));
	unsafe_vector<ARTKey> keys(expr_chunk.size());
	GenerateKeys<>(arena_allocator, expr_chunk, keys);

	optional_idx conflict_idx;
	for (idx_t i = 0; !conflict_idx.IsValid() && i < chunk.size(); i++) {
		if (keys[i].Empty()) {
			if (manager.AddNull(i)) {
				conflict_idx = i;
			}
			continue;
		}

		auto leaf = ARTOperator::Lookup(*this, tree, keys[i], 0);
		if (!leaf) {
			continue;
		}
		VerifyLeaf(leaf.Get(), keys[i], DeleteIndexInfo(info.delete_indexes), manager, conflict_idx, i);
	}

	manager.FinishLookup();
	if (!conflict_idx.IsValid()) {
		return;
	}

	auto key_name = GenerateErrorKeyName(chunk, conflict_idx.GetIndex());
	auto exception_msg = GenerateConstraintErrorMessage(manager.GetVerifyExistenceType(), key_name);
	throw ConstraintException(exception_msg);
}

string ART::GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index, DataChunk &input) const {
	IndexLock l(*this);
	auto key_name = GenerateErrorKeyName(input, failed_index);
	auto exception_msg = GenerateConstraintErrorMessage(verify_type, key_name);
	return exception_msg;
}

//===--------------------------------------------------------------------===//
// Storage and Memory
//===--------------------------------------------------------------------===//

void ART::TransformToDeprecated() {
	auto idx = NodePtr::GetAllocatorIdx(NType::PREFIX);
	auto &block_manager = (*allocators)[idx]->block_manager;
	unsafe_unique_ptr<FixedSizeAllocator> deprecated_allocator = nullptr;
	if (prefix_count != Prefix::DEPRECATED_COUNT) {
		auto prefix_size = NumericCast<idx_t>(Prefix::DEPRECATED_COUNT) + NumericCast<idx_t>(Prefix::METADATA_SIZE);
		deprecated_allocator = make_unsafe_uniq<FixedSizeAllocator>(prefix_size, block_manager);
	}

	unique_ptr<TransformToDeprecatedState> state =
	    make_uniq<TransformToDeprecatedState>(std::move(deprecated_allocator));

	// Transform all leaves, and possibly the prefixes.
	if (tree.HasMetadata()) {
		NodePtr::TransformToDeprecated(*this, tree, *state);
	}

	// Replace the prefix allocator with the deprecated allocator.
	if (state->HasAllocator()) {
		deprecated_allocator = state->TakeAllocator();
		prefix_count = Prefix::DEPRECATED_COUNT;

		D_ASSERT((*allocators)[idx]->Empty());
		(*allocators)[idx]->Reset();
		(*allocators)[idx] = std::move(deprecated_allocator);
	}
}

IndexStorageInfo ART::PrepareSerialize(const case_insensitive_map_t<Value> &options, const bool v1_0_0_storage) {
	if (v1_0_0_storage) {
		TransformToDeprecated();
	}

	IndexStorageInfo info(name);
	info.root = tree.Get();
	info.options = options;

	// It never hurts to serialize the storage version, even to older formats
	if (storage_version != StorageVersion::INVALID) {
		info.options["storage_version"] = Value::UBIGINT(static_cast<uint64_t>(storage_version));
	}

	for (auto &allocator : *allocators) {
		allocator->RemoveEmptyBuffers();
	}

#ifdef DEBUG
	if (v1_0_0_storage) {
		D_ASSERT((*allocators)[NodePtr::GetAllocatorIdx(NType::NODE_7_LEAF)]->Empty());
		D_ASSERT((*allocators)[NodePtr::GetAllocatorIdx(NType::NODE_15_LEAF)]->Empty());
		D_ASSERT((*allocators)[NodePtr::GetAllocatorIdx(NType::NODE_256_LEAF)]->Empty());
		D_ASSERT((*allocators)[NodePtr::GetAllocatorIdx(NType::PREFIX)]->GetSegmentSize() ==
		         Prefix::DEPRECATED_COUNT + Prefix::METADATA_SIZE);
	}
#endif

	return info;
}

IndexStorageInfo ART::SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) {
	IndexLock guard(*this);

	// If the storage format uses deprecated leaf storage,
	// then we need to transform all nested leaves before serialization.
	auto v1_0_0_option = options.find("v1_0_0_storage");
	bool v1_0_0_storage = v1_0_0_option == options.end() || v1_0_0_option->second != Value(false);
	auto info = PrepareSerialize(options, v1_0_0_storage);
	auto allocator_count = v1_0_0_storage ? DEPRECATED_ALLOCATOR_COUNT : ALLOCATOR_COUNT;

	// Store the data on disk as partial blocks and set the block ids.
	WritePartialBlocks(context, v1_0_0_storage);

	for (idx_t i = 0; i < allocator_count; i++) {
		info.allocator_infos.push_back((*allocators)[i]->GetInfo());
	}

	return info;
}

IndexStorageInfo ART::SerializeToWAL(const case_insensitive_map_t<Value> &options) {
	// If the storage format uses deprecated leaf storage,
	// then we need to transform all nested leaves before serialization.
	auto v1_0_0_option = options.find("v1_0_0_storage");
	bool v1_0_0_storage = v1_0_0_option == options.end() || v1_0_0_option->second != Value(false);
	auto info = PrepareSerialize(options, v1_0_0_storage);
	auto allocator_count = v1_0_0_storage ? DEPRECATED_ALLOCATOR_COUNT : ALLOCATOR_COUNT;

	// Set the correct allocation sizes and get the map containing all buffers.
	for (idx_t i = 0; i < allocator_count; i++) {
		info.buffers.push_back((*allocators)[i]->InitSerializationToWAL());
	}

	for (idx_t i = 0; i < allocator_count; i++) {
		info.allocator_infos.push_back((*allocators)[i]->GetInfo());
	}

	return info;
}

void ART::WritePartialBlocks(QueryContext context, const bool v1_0_0_storage) {
	auto &block_manager = table_io_manager.GetIndexBlockManager();
	PartialBlockManager partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT);

	idx_t allocator_count = v1_0_0_storage ? DEPRECATED_ALLOCATOR_COUNT : ALLOCATOR_COUNT;
	for (idx_t i = 0; i < allocator_count; i++) {
		(*allocators)[i]->SerializeBuffers(partial_block_manager);
	}
	partial_block_manager.FlushPartialBlocks();
}

void ART::InitAllocators(const IndexStorageInfo &info) {
	for (idx_t i = 0; i < info.allocator_infos.size(); i++) {
		(*allocators)[i]->Init(info.allocator_infos[i]);
	}
}

void ART::Deserialize(const BlockPointer &pointer) {
	D_ASSERT(pointer.IsValid());

	auto &metadata_manager = table_io_manager.GetMetadataManager();
	MetadataReader reader(metadata_manager, pointer);
	tree = reader.Read<NodePtr>();

	for (idx_t i = 0; i < DEPRECATED_ALLOCATOR_COUNT; i++) {
		(*allocators)[i]->Deserialize(metadata_manager, reader.Read<BlockPointer>());
	}
}

void ART::SetPrefixCount(const IndexStorageInfo &info) {
	if (info.IsValid() && info.root_block_ptr.IsValid()) {
		prefix_count = Prefix::DEPRECATED_COUNT;
		return;
	}

	if (info.IsValid()) {
		auto serialized_count = info.allocator_infos[0].segment_size - Prefix::METADATA_SIZE;
		prefix_count = NumericCast<uint8_t>(serialized_count);
		return;
	}

	idx_t compound_size = 0;
	for (const auto &type : types) {
		compound_size += GetTypeIdSize(type);
	}

	// Get the maximum possible prefix size.
	// Minus one to index the prefix count (last byte).
	auto numeric_max = NumericLimits<uint8_t>().Maximum();
	uint8_t max_aligned = AlignValueFloor<uint8_t>(numeric_max - Prefix::METADATA_SIZE) - 1;

	// Ceiling of compound size,
	// minus one to index the prefix count (last byte).
	idx_t key_aligned = AlignValue(compound_size) - 1;

	// Set the prefix size to the maximum of the (compound) key size and the maximum prefix size.
	bool exceeds_max = key_aligned > NumericCast<idx_t>(max_aligned);
	prefix_count = exceeds_max ? max_aligned : NumericCast<uint8_t>(key_aligned);
}

idx_t ART::GetInMemorySize(IndexLock &index_lock) const {
	index_lock.AssertHeld(*this);
	D_ASSERT(owns_data);

	idx_t in_memory_size = 0;
	for (const auto &allocator : *allocators) {
		in_memory_size += allocator->GetInMemorySize();
	}
	return in_memory_size;
}

bool ART::SupportsDeltaIndexes() const {
	return true;
}

unique_ptr<BoundIndex> ART::CreateEmptyCopy(const IndexConstraintType constraint_type) const {
	return make_uniq<ART>(name, constraint_type, GetColumnIds(), table_io_manager, unbound_expressions, db);
}

//===-------------------------------------------------------------------===//
// Vacuum
//===--------------------------------------------------------------------===//

void ART::InitializeVacuum(unordered_set<uint8_t> &indexes) {
	for (idx_t i = 0; i < allocators->size(); i++) {
		if ((*allocators)[i]->InitializeVacuum()) {
			indexes.insert(NumericCast<uint8_t>(i));
		}
	}
}

void ART::FinalizeVacuum(const unordered_set<uint8_t> &indexes) {
	for (const auto &idx : indexes) {
		(*allocators)[idx]->FinalizeVacuum();
	}
}

static void VacuumPointerIfNeeded(ART &art, const unordered_set<uint8_t> &indexes, NodePtr &node) {
	const auto type = node.GetType();
	if (type == NType::LEAF_INLINED) {
		return;
	}
	const auto idx = NodePtr::GetAllocatorIdx(type);
	if (indexes.find(idx) == indexes.end()) {
		return;
	}
	auto &allocator = NodePtr::GetAllocator(art, type);
	if (!allocator.NeedsVacuum(node)) {
		return;
	}
	const auto status = node.GetGateStatus();
	node = allocator.VacuumPointer(node);
	node.SetMetadata(static_cast<uint8_t>(type));
	node.SetGateStatus(status);
}

void ART::Vacuum(IndexLock &state) {
	state.AssertHeld(*this);
	D_ASSERT(owns_data);

	if (!tree.HasMetadata()) {
		for (auto &allocator : *allocators) {
			allocator->Reset();
		}
		return;
	}
	// True, if an allocator needs a vacuum, false otherwise.
	unordered_set<uint8_t> indexes;
	InitializeVacuum(indexes);

	// Skip vacuum, if no allocators require it.
	if (indexes.empty()) {
		return;
	}

	auto &art = *this;
	const auto vacuum_deprecated_leaves = indexes.find(NodePtr::GetAllocatorIdx(NType::LEAF)) != indexes.end();

	auto child_handler = [&](NodePtr &child) -> OptionalNodePtr {
		// Vacuums the pointer if needed and updates in place within the parent.
		VacuumPointerIfNeeded(art, indexes, child);
		if (child.GetType() == NType::LEAF_INLINED) {
			return OptionalNodePtr();
		}
		// Push the updated pointer onto the stack to continue vacuum traversal on the subtree.
		return child;
	};
	auto on_pop = [&](NodePtr current) -> ARTScanNodeResult {
		D_ASSERT(current.HasMetadata());
		if (current.GetType() == NType::LEAF) {
			if (vacuum_deprecated_leaves) {
				// Vacuum the internal pointers in the deprecated leaf chain.
				Leaf::DeprecatedVacuum(art, current);
			}
			return ARTScanNodeResult::SKIP;
		}
		return ARTScanNodeResult::SCAN_CHILDREN;
	};
	ARTScanPreorder(art, tree, child_handler, on_pop);

	// Finalize the vacuum operation.
	FinalizeVacuum(indexes);
}

//===--------------------------------------------------------------------===//
// Merging
//===--------------------------------------------------------------------===//

void ART::InitializeMergeUpperBounds(unsafe_vector<idx_t> &upper_bounds) {
	D_ASSERT(owns_data);
	for (auto &allocator : *allocators) {
		upper_bounds.emplace_back(allocator->GetUpperBoundBufferId());
	}
}

void ART::InitializeMerge(NodePtr &other_tree, unsafe_vector<idx_t> &upper_bounds) {
	D_ASSERT(other_tree.HasMetadata());

	auto child_handler = [&](NodePtr &child) -> OptionalNodePtr {
		D_ASSERT(child.HasMetadata());
		auto type = child.GetType();
		// no-op
		if (type == NType::LEAF_INLINED) {
			return OptionalNodePtr();
		}
		// FIXME: Implement merging for deprecated leaves.
		if (type == NType::LEAF) {
			throw InternalException("deprecated ART storage in InitializeMerge");
		}
		auto original = child;
		// remap BufferId in-place within the parent.
		auto idx = NodePtr::GetAllocatorIdx(type);
		child.IncreaseBufferId(upper_bounds[idx]);

		switch (type) {
		case NType::NODE_7_LEAF:
		case NType::NODE_15_LEAF:
		case NType::NODE_256_LEAF:
			// no-op
			return OptionalNodePtr();
		case NType::PREFIX:
		case NType::NODE_4:
		case NType::NODE_16:
		case NType::NODE_48:
		case NType::NODE_256:
			// Original pointer is pushed onto the stack.
			return original;
		default:
			throw InternalException("invalid node type for InitializeMerge: %d", type);
		}
	};

	auto on_pop = [](NodePtr node) -> ARTScanNodeResult {
		D_ASSERT(node.HasMetadata());
		return ARTScanNodeResult::SCAN_CHILDREN;
	};

	ARTScanPreorder(*this, other_tree, child_handler, on_pop);
}

bool ART::MergeIndexes(IndexLock &state, BoundIndex &source_index) {
	state.AssertHeld(*this);
	auto &other_art = source_index.Cast<ART>();
	if (!other_art.tree.HasMetadata()) {
		return true;
	}

	if (other_art.owns_data) {
		if (prefix_count != other_art.prefix_count) {
			throw InternalException("Failed to merge ARTs - prefix count does not match");
		}
		const auto target_size = NodePtr::GetAllocator(*this, NType::NODE_256_LEAF).GetSegmentSize();
		const auto source_size = NodePtr::GetAllocator(other_art, NType::NODE_256_LEAF).GetSegmentSize();
		if (target_size != source_size) {
			throw InternalException(
			    "Failed to merge ARTs - Node256Leaf segment sizes do not match (target %llu, source %llu)", target_size,
			    source_size);
		}
		if (tree.HasMetadata()) {
			// Fully deserialize other_index, and traverse it to increment its buffer IDs.
			unsafe_vector<idx_t> upper_bounds;
			InitializeMergeUpperBounds(upper_bounds);
			other_art.InitializeMerge(other_art.tree, upper_bounds);
		}

		// Merge the node storage.
		for (idx_t i = 0; i < allocators->size(); i++) {
			(*allocators)[i]->Merge(*(*other_art.allocators)[i]);
		}
	}

	// Merge the ARTs.
	D_ASSERT(tree.GetGateStatus() == other_art.tree.GetGateStatus());
	if (tree.HasMetadata()) {
		ArenaAllocator arena(Allocator::Get(db));
		ARTMerger merger(arena, *this);
		merger.Init(tree, other_art.tree);
		return merger.Merge() == ARTConflictType::NO_CONFLICT;
	}

	tree = other_art.tree;
	other_art.tree.Clear();
	return true;
}

// FIXME : Make this a more efficient structural tree removal merge
//		   Right now this is only used in MergeCheckpointDeltas to avoid having to do a table scan.
void ART::RemovalMerge(IndexLock &state, BoundIndex &source_index) {
	state.AssertHeld(*this);
	auto &source = source_index.Cast<ART>();
	if (!source.tree.HasMetadata()) {
		return;
	}

	ArenaAllocator arena(BufferAllocator::Get(db));
	idx_t scan_count = 0;
	idx_t delete_count = 0;

	Iterator it(source);
	it.FindMinimum(source.tree);

	unsafe_vector<ARTKey> keys(STANDARD_VECTOR_SIZE);
	unsafe_vector<ARTKey> row_id_keys(STANDARD_VECTOR_SIZE);
	ARTKey empty_key = ARTKey();

	KeyRowIdOutput output(arena, keys, row_id_keys, STANDARD_VECTOR_SIZE);
	ARTScanResult result;
	do {
		output.Reset();
		result = it.Scan(empty_key, output, false);
		if (output.Count() > 0) {
			scan_count += output.Count();
			delete_count += DeleteKeys(keys, row_id_keys, output.Count());
		}
	} while (result == ARTScanResult::PAUSED);

	if (delete_count != scan_count) {
		throw InternalException("Failed to remove all rows while merging checkpoint deltas - "
		                        "this signifies a bug or broken index");
	}
}

void ART::RemovalMerge(BoundIndex &source_index) {
	IndexLock state(*this);
	RemovalMerge(state, source_index);
}

// FIXME: We already have a structural tree merge, this only exists right now since the structural merge doesn't
// handle deprecated leaves. This is being used in merging checkpoint deltas, to avoid a more inefficient table scan.
// Once the structural merge adds support for deprecated leaves, we can replace the calls of this function with that.
ErrorData ART::InsertMerge(IndexLock &state, BoundIndex &source_index, IndexAppendMode append_mode) {
	state.AssertHeld(*this);
	auto &source = source_index.Cast<ART>();
	if (!source.tree.HasMetadata()) {
		return ErrorData();
	}

	ArenaAllocator arena(BufferAllocator::Get(db));

	Iterator it(source);
	it.FindMinimum(source.tree);

	unsafe_vector<ARTKey> keys(STANDARD_VECTOR_SIZE);
	unsafe_vector<ARTKey> row_id_keys(STANDARD_VECTOR_SIZE);
	ARTKey empty_key = ARTKey();

	KeyRowIdOutput output(arena, keys, row_id_keys, STANDARD_VECTOR_SIZE);
	ARTScanResult result;
	do {
		output.Reset();
		result = it.Scan(empty_key, output, false);
		if (output.Count() > 0) {
			auto error = InsertKeys(arena, keys, row_id_keys, output.Count(), DeleteIndexInfo(), append_mode);
			if (error.HasError()) {
				return error;
			}
		}
	} while (result == ARTScanResult::PAUSED);

	return ErrorData();
}

ErrorData ART::InsertMerge(BoundIndex &source_index, IndexAppendMode append_mode) {
	IndexLock state(*this);
	return InsertMerge(state, source_index, append_mode);
}

ErrorData ART::MergeCheckpointDelta(const IndexDeltaType type, BoundIndex &delta_index) {
	switch (type) {
	case IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT:
		RemovalMerge(delta_index);
		return ErrorData();
	case IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT:
		// Inserts happen before deletes during commit, so duplicates must be accepted while merging the added rows.
		return InsertMerge(delta_index, IndexAppendMode::INSERT_DUPLICATES);
	default:
		throw InternalException("Unsupported ART checkpoint delta type");
	}
}

//===--------------------------------------------------------------------===//
// Verification
//===--------------------------------------------------------------------===//

string ART::ToString(IndexLock &l, bool display_ascii) {
	l.AssertHeld(*this);
	return ToStringInternal(display_ascii);
}

string ART::ToStringInternal(bool display_ascii) {
	if (tree.HasMetadata()) {
		return "\nART: \n" + tree.ToString(*this, ToStringOptions(false, display_ascii, nullptr, 0, 0, true, false));
	}
	return "[empty]";
}

void ART::Verify(IndexLock &l) {
	l.AssertHeld(*this);
	VerifyInternal();
}

void ART::VerifyInternal() {
	if (tree.HasMetadata()) {
		tree.Verify(*this);
	}
}

void ART::VerifyAllocations(IndexLock &l) {
	l.AssertHeld(*this);
	return VerifyAllocationsInternal();
}

void ART::VerifyAllocationsInternal() {
#ifdef DEBUG
	unordered_map<uint8_t, idx_t> node_counts;
	for (idx_t i = 0; i < allocators->size(); i++) {
		node_counts[NumericCast<uint8_t>(i)] = 0;
	}

	if (tree.HasMetadata()) {
		tree.VerifyAllocations(*this, node_counts);
	}

	for (idx_t i = 0; i < allocators->size(); i++) {
		auto segment_count = (*allocators)[i]->GetSegmentCount();
		D_ASSERT(segment_count == node_counts[NumericCast<uint8_t>(i)]);
	}
#endif
}

void ART::VerifyBuffers(IndexLock &l) {
	l.AssertHeld(*this);
	for (auto &allocator : *allocators) {
		allocator->VerifyBuffers();
	}
}

constexpr const char *ART::TYPE_NAME;

} // namespace duckdb
