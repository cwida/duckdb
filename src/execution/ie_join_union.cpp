#include "duckdb/execution/ie_join_union.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

idx_t IEJoinUnion::AppendKey(ExecutionContext &context, InterruptState &interrupt, SortedTable &table,
                             ExpressionExecutor &executor, SortedTable &marked, int64_t increment, int64_t rid,
                             const ChunkRange &chunk_range) {
	const auto chunk_begin = chunk_range.first;
	const auto chunk_end = chunk_range.second;

	if (chunk_begin == chunk_end) {
		return 0;
	}

	// Reading
	const auto valid = table.count - table.has_null;
	auto &source = *table.sorted->payload_data;
	TupleDataScanState scanner;
	source.InitializeScan(scanner);

	DataChunk scanned;
	source.InitializeScanChunk(scanner, scanned);
	idx_t table_idx = source.Seek(scanner, chunk_begin);

	// Writing
	auto &sort = *marked.sort;
	auto local_sort_state = sort.GetLocalSinkState(context);
	vector<LogicalType> types;
	for (const auto &expr : executor.expressions) {
		types.emplace_back(expr->GetReturnType());
	}
	const idx_t rid_idx = types.size();
	types.emplace_back(LogicalType::BIGINT);

	DataChunk keys;
	DataChunk payload;
	keys.Initialize(Allocator::DefaultAllocator(), types);

	OperatorSinkInput sink {*marked.global_sink, *local_sort_state, interrupt};
	idx_t inserted = 0;
	for (auto chunk_idx = chunk_begin; chunk_idx < chunk_end; ++chunk_idx) {
		source.Scan(scanner, scanned);

		// NULLs are at the end, so stop when we reach them
		auto scan_count = scanned.size();
		if (table_idx + scan_count > valid) {
			if (table_idx >= valid) {
				scan_count = 0;
				;
			} else {
				scan_count = valid - table_idx;
				scanned.SetChildCardinality(scan_count);
			}
		}
		if (scan_count == 0) {
			break;
		}
		table_idx += scan_count;

		// Compute the input columns from the payload
		keys.Reset();
		keys.Split(payload, rid_idx);
		executor.Execute(scanned, keys);

		// Mark the rid column
		payload.data[0].Sequence(rid, increment, scan_count);
		keys.Fuse(payload);
		rid += increment * UnsafeNumericCast<int64_t>(scan_count);

		// Sort on the sort columns (which will no longer be needed)
		sort.Sink(context, keys, sink);
		inserted += scan_count;
	}
	OperatorSinkCombineInput combine {*marked.global_sink, *local_sort_state, interrupt};
	sort.Combine(context, combine);
	marked.count += inserted;

	return inserted;
}

IEJoinUnion::IEJoinUnion(SortedTable &l2, ColumnDataCollection &li, ColumnDataCollection &p,
                         const vector<JoinCondition> &conditions, const ChunkRange &chunks)
    : n(0), i(0), li(li), p(p) {
	// 7. initialize bit-array B (|B| = n), and set all bits to 0
	n_j = l2.count.load();
	bit_array.resize(ValidityMask::EntryCount(n_j), 0);
	bit_mask.Initialize(bit_array.data(), n_j);

	// Bloom filter
	bloom_count = (n_j + (BLOOM_CHUNK_BITS - 1)) / BLOOM_CHUNK_BITS;
	bloom_array.resize(ValidityMask::EntryCount(bloom_count), 0);
	bloom_filter.Initialize(bloom_array.data(), bloom_count);

	// 11. for(i←1 to n) do
	const auto strict2 = IsStrictComparison(conditions[1].GetComparisonType());
	op2 = make_uniq<UnionIterator>(l2, strict2);
	off2 = make_uniq<UnionIterator>(l2, strict2);
	n = l2.BlockStart(chunks.second);
	i = l2.BlockStart(chunks.first);
	j = i;
	anti_i = i;

	const auto sort_key_type = l2.GetSortKeyType();
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		next_row_func = &IEJoinUnion::NextRow<SortKeyType::SORT_KEY_TYPE>;                                             \
		break;
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("IEJoinUnion for %s", EnumUtil::ToString(sort_key_type));
	}

	(this->*next_row_func)();
}

template <SortKeyType SORT_KEY_TYPE>
bool IEJoinUnion::NextRow() {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCKS_ITERATOR = block_iterator_t<ExternalBlockIteratorState, SORT_KEY>;

	BLOCKS_ITERATOR off2_itr(*off2->state);
	BLOCKS_ITERATOR op2_itr(*op2->state);
	const auto strict = off2->strict;

	auto pinned_idx = off2->GetChunkIndex();
	for (; i < n; ++i) {
		// 12. pos ← P[i]
		auto pos = p[i];
		lrid = li[pos];
		if (lrid < 0) {
			continue;
		}

		// 16. B[pos] ← 1
		op2->SetIndex(i);
		for (; off2->GetIndex() < n_j; ++(*off2)) {
			//	Prevent buildup of pinned blocks
			if (off2->GetChunkIndex() != pinned_idx) {
				off2->Repin();
				pinned_idx = off2->GetChunkIndex();
			}
			if (!Compare(off2_itr[off2->GetIndex()], op2_itr[op2->GetIndex()], strict)) {
				break;
			}
			const auto p2 = p[off2->GetIndex()];
			if (li[p2] < 0) {
				// Only mark rhs matches.
				bit_mask.SetValidUnsafe(p2);
				bloom_filter.SetValidUnsafe(p2 / BLOOM_CHUNK_BITS);
			}
		}

		// 9.  if (op1 ∈ {≤,≥} and op2 ∈ {≤,≥}) eqOff = 0
		// 10. else eqOff = 1
		// No, because there could be more than one equal value.
		// Find the leftmost off1 where L1[pos] op1 L1[off1..n]
		// These are the rows that satisfy the op1 condition
		// and that is where we should start scanning B from
		j = pos;

		return true;
	}
	return false;
}

static idx_t NextValid(const ValidityMask &bits, idx_t j, const idx_t n) {
	if (j >= n) {
		return n;
	}

	// We can do a first approximation by checking entries one at a time
	// which gives 64:1.
	idx_t entry_idx, idx_in_entry;
	bits.GetEntryIndex(j, entry_idx, idx_in_entry);

	// Copy first entry to local and trim the bits before the start position
	auto first_entry = bits.GetValidityEntryUnsafe(entry_idx++);
	first_entry &= (ValidityMask::ValidityBuffer::MAX_ENTRY << idx_in_entry);

	// If the first entry has a valid bit, we can return immediately
	if (first_entry) {
		return j + CountZeros<validity_t>::Trailing(first_entry) - idx_in_entry;
	}

	// The first entry did not have a valid bit
	j += ValidityMask::BITS_PER_VALUE - idx_in_entry;

	// Loop over non-ragged entries
	const auto entry_count_minus_one = bits.EntryCount(n) - 1;
	const auto entry_idx_before = entry_idx;

	// The compiler has a hard time optimizing this loop for some reason
	// Creating a static inner loop like this improves performance by almost 2x
	static constexpr idx_t NEXT_VALID_UNROLL = 8;
	for (; entry_idx + NEXT_VALID_UNROLL < entry_count_minus_one; entry_idx += NEXT_VALID_UNROLL) {
		for (idx_t unroll_idx = 0; unroll_idx < NEXT_VALID_UNROLL; unroll_idx++) {
			const auto unroll_entry_idx = entry_idx + unroll_idx;
			const auto &entry = bits.GetValidityEntryUnsafe(unroll_entry_idx);
			if (entry) {
				return j + (unroll_entry_idx - entry_idx_before) * ValidityMask::BITS_PER_VALUE +
				       CountZeros<validity_t>::Trailing(entry);
			}
		}
	}

	for (; entry_idx < entry_count_minus_one; ++entry_idx) {
		const auto &entry = bits.GetValidityEntryUnsafe(entry_idx);
		if (entry) {
			return j + (entry_idx - entry_idx_before) * ValidityMask::BITS_PER_VALUE +
			       CountZeros<validity_t>::Trailing(entry);
		}
	}

	// Update j once after the loop so we don't have to update it in each iteration
	j += (entry_idx - entry_idx_before) * ValidityMask::BITS_PER_VALUE;

	// Check the final entry
	return j >= n ? n : j + CountZeros<validity_t>::Trailing(bits.GetValidityEntryUnsafe(entry_idx));
}

idx_t IEJoinUnion::JoinBlocks(unsafe_vector<idx_t> &lsel, unsafe_vector<idx_t> &rsel) {
	// Release pinned blocks
	op2->Repin();
	off2->Repin();

	// 8. initialize join result as an empty list for tuple pairs
	idx_t result_count = 0;

	lsel.resize(STANDARD_VECTOR_SIZE);
	rsel.resize(STANDARD_VECTOR_SIZE);

	// 11. for(i←1 to n) do
	while (i < n) {
		// 13. for (j ← pos+eqOff to n) do
		for (;;) {
			// 14. if B[j] = 1 then

			//	Use the Bloom filter to find candidate blocks
			while (j < n_j) {
				auto bloom_begin = NextValid(bloom_filter, j / BLOOM_CHUNK_BITS, bloom_count) * BLOOM_CHUNK_BITS;
				auto bloom_end = MinValue<idx_t>(n_j, bloom_begin + BLOOM_CHUNK_BITS);

				j = MaxValue<idx_t>(j, bloom_begin);
				j = NextValid(bit_mask, j, bloom_end);
				if (j < bloom_end) {
					break;
				}
			}

			if (j >= n_j) {
				break;
			}

			// Filter out tuples with the same sign (they come from the same table)
			const auto rrid = li[j];
			++j;

			D_ASSERT(lrid > 0 && rrid < 0);
			// 15. add tuples w.r.t. (L1[j], L1[i]) to join result
			lsel[result_count] = static_cast<idx_t>(+lrid - 1);
			rsel[result_count] = static_cast<idx_t>(-rrid - 1);
			++result_count;
			if (result_count == STANDARD_VECTOR_SIZE) {
				// out of space!
				return result_count;
			}
		}

		if (!FinishRow()) {
			break;
		}
	}

	lsel.resize(result_count);
	rsel.resize(result_count);

	return result_count;
}

void IEJoinUnion::InitializeTables(ClientContext &client, const PhysicalComparisonJoin &op,
                                   const vector<JoinCondition> &conditions, unique_ptr<SortedTable> &l1,
                                   unique_ptr<SortedTable> &l2) {
	// input : query Q with 2 join predicates t1.X op1 t2.X' and t1.Y op2 t2.Y', tables T, T' of sizes m and n resp.
	// output: a list of tuple pairs (ti , tj)
	// Note that T/T' are already sorted on X/X' and contain the payload data
	// We only join the two block numbers and use the sizes of the blocks as the counts

	// 1. let L1 (resp. L2) be the array of column X (resp. Y )
	const auto first = conditions[0].GetComparisonType();
	const auto second = conditions[1].GetComparisonType();
	const auto first_order =
	    first == ExpressionType::COMPARE_LESSTHAN || first == ExpressionType::COMPARE_LESSTHANOREQUALTO
	        ? OrderType::ASCENDING
	        : OrderType::DESCENDING;
	const auto second_order =
	    second == ExpressionType::COMPARE_LESSTHAN || second == ExpressionType::COMPARE_LESSTHANOREQUALTO
	        ? OrderType::DESCENDING
	        : OrderType::ASCENDING;
	BoundOrderByNode order1(first_order, OrderByNullType::NULLS_LAST,
	                        make_uniq<BoundReferenceExpression>(conditions[0].GetLHS().GetReturnType(), storage_t(0)));
	BoundOrderByNode order2(second_order, OrderByNullType::NULLS_LAST,
	                        make_uniq<BoundReferenceExpression>(conditions[1].GetLHS().GetReturnType(), storage_t(0)));

	// 2. if (op1 ∈ {>, ≥}) sort L1 in descending order
	// 3. else if (op1 ∈ {<, ≤}) sort L1 in ascending order

	// For the union algorithm, we make a unified table with the keys and the rids as the payload:
	//		X/X', Y/Y', R/R'/Li
	// The first position is the sort key.
	vector<LogicalType> types;
	types.emplace_back(order2.expression->GetReturnType());
	types.emplace_back(LogicalType::BIGINT);

	// Sort on the first expression
	auto ref = make_uniq<BoundReferenceExpression>(order1.expression->GetReturnType(), 0U);
	vector<BoundOrderByNode> orders;
	orders.emplace_back(order1.type, order1.null_order, std::move(ref));
	// The goal is to make i (from the left table) < j (from the right table),
	// if value[i] and value[j] match the condition 1.
	// Add a column from_left to solve the problem when there exist multiple equal values in l1.
	// If the operator is loose inequality, make t1.from_left (== true) sort BEFORE t2.from_left (== false).
	// Otherwise, make t1.from_left sort (== true) sort AFTER t2.from_left (== false).
	// For example, if t1.time <= t2.time
	// | value     | 1     | 1     | 1     | 1     |
	// | --------- | ----- | ----- | ----- | ----- |
	// | from_left | T(l2) | T(l2) | F(r1) | F(r2) |
	// if t1.time < t2.time
	// | value     | 1     | 1     | 1     | 1     |
	// | --------- | ----- | ----- | ----- | ----- |
	// | from_left | F(r2) | F(r1) | T(l2) | T(l1) |
	// Using this OrderType, if i < j then value[i] (from left table) and value[j] (from right table) match
	// the condition (t1.time <= t2.time or t1.time < t2.time), then from_left will force them into the correct order.
	auto from_left = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
	const auto strict1 = IEJoinUnion::IsStrictComparison(conditions[0].GetComparisonType());
	orders.emplace_back(!strict1 ? OrderType::DESCENDING : OrderType::ASCENDING, OrderByNullType::ORDER_DEFAULT,
	                    std::move(from_left));

	l1 = make_uniq<SortedTable>(client, orders, types, op);

	// 4. if (op2 ∈ {>, ≥}) sort L2 in ascending order
	// 5. else if (op2 ∈ {<, ≤}) sort L2 in descending order

	// We sort on Y/Y' to obtain the sort keys and the permutation array.
	// For this we just need a two-column table of Y, P
	types.clear();
	types.emplace_back(LogicalType::BIGINT);

	// Sort on the first expression
	orders.clear();
	ref = make_uniq<BoundReferenceExpression>(order2.expression->GetReturnType(), 0U);
	orders.emplace_back(order2.type, order2.null_order, std::move(ref));

	l2 = make_uniq<SortedTable>(client, orders, types, op);
}

} // namespace duckdb
