//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/quantile_sort_tree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "core_functions/aggregate/quantile_helpers.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/interpolate.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/function/window/window_index_tree.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdlib.h>
#include <utility>

namespace duckdb {

// Paged access
template <typename INPUT_TYPE>
struct QuantileCursor {
	explicit QuantileCursor(const WindowPartitionInput &partition) : inputs(*partition.inputs) {
		// the quantile parameter is folded into the bind data by the bind, but stays part of the expression tree -
		// only the leading input column is paged in
		D_ASSERT(!partition.column_ids.empty());
		vector<column_t> input_ids {partition.column_ids[0]};
		inputs.InitializeScan(scan, input_ids);
		inputs.InitializeScanChunk(scan, page);

		D_ASSERT(!partition.all_valid.empty());
		cannot_have_null = partition.all_valid[0];
	}

	inline sel_t RowOffset(idx_t row_idx) const {
		D_ASSERT(RowIsVisible(row_idx));
		return UnsafeNumericCast<sel_t>(row_idx - scan.current_row_index);
	}

	inline bool RowIsVisible(idx_t row_idx) const {
		return (row_idx < scan.next_row_index && scan.current_row_index <= row_idx);
	}

	inline idx_t Seek(idx_t row_idx) {
		if (!RowIsVisible(row_idx)) {
			inputs.Seek(row_idx, scan, page);
			values = make_uniq<VectorIterator<INPUT_TYPE>>(page.data[0]);
		}
		return RowOffset(row_idx);
	}

	inline const INPUT_TYPE &operator[](idx_t row_idx) {
		const auto offset = Seek(row_idx);
		return (*values)[offset].GetValueUnsafe();
	}

	inline bool RowIsValid(idx_t row_idx) {
		const auto offset = Seek(row_idx);
		return (*values)[offset].IsValid();
	}

	inline bool CannotHaveNull() {
		return cannot_have_null;
	}

	//! Windowed paging
	const ColumnDataCollection &inputs;
	//! The state used for reading the collection on this thread
	ColumnDataScanState scan;
	//! The data chunk paged into into
	DataChunk page;
	//! Cell access into the page, rebuilt per page load - scans can return constant and dictionary vectors
	unique_ptr<VectorIterator<INPUT_TYPE>> values;
	//! Paged chunks do not track this but it is really necessary for performance
	bool cannot_have_null;
};

// Direct access
template <typename T>
struct QuantileDirect {
	using INPUT_TYPE = T;
	using RESULT_TYPE = T;

	inline const INPUT_TYPE &operator()(const INPUT_TYPE &x) const {
		return x; // NOLINT
	}
};

// Indirect access
template <typename T>
struct QuantileIndirect {
	using INPUT_TYPE = idx_t;
	using RESULT_TYPE = T;
	using CURSOR = QuantileCursor<RESULT_TYPE>;
	CURSOR &data;

	explicit QuantileIndirect(CURSOR &data_p) : data(data_p) {
	}

	inline RESULT_TYPE operator()(const INPUT_TYPE &input) const {
		return data[input];
	}
};

// Composed access
template <typename OUTER, typename INNER>
struct QuantileComposed {
	using INPUT_TYPE = typename INNER::INPUT_TYPE;
	using RESULT_TYPE = typename OUTER::RESULT_TYPE;

	const OUTER &outer;
	const INNER &inner;

	explicit QuantileComposed(const OUTER &outer_p, const INNER &inner_p) : outer(outer_p), inner(inner_p) {
	}

	inline RESULT_TYPE operator()(const idx_t &input) const {
		return outer(inner(input));
	}
};

// Accessed comparison
template <typename ACCESSOR>
struct QuantileCompare {
	using INPUT_TYPE = typename ACCESSOR::INPUT_TYPE;
	const ACCESSOR &accessor_l;
	const ACCESSOR &accessor_r;
	const bool desc;

	// Single cursor for linear operations
	explicit QuantileCompare(const ACCESSOR &accessor, bool desc_p)
	    : accessor_l(accessor), accessor_r(accessor), desc(desc_p) {
	}

	// Independent cursors for sorting
	explicit QuantileCompare(const ACCESSOR &accessor_l, const ACCESSOR &accessor_r, bool desc_p)
	    : accessor_l(accessor_l), accessor_r(accessor_r), desc(desc_p) {
	}

	inline bool operator()(const INPUT_TYPE &lhs, const INPUT_TYPE &rhs) const {
		const auto lval = accessor_l(lhs);
		const auto rval = accessor_r(rhs);

		return desc ? LessThan::Operation(rval, lval) : LessThan::Operation(lval, rval);
	}
};

struct QuantileCast {
	template <class INPUT_TYPE, class TARGET_TYPE>
	static inline TARGET_TYPE Operation(const INPUT_TYPE &src, Vector &result) {
		return Cast::Operation<INPUT_TYPE, TARGET_TYPE>(src);
	}
};

template <>
interval_t QuantileCast::Operation(const dtime_t &src, Vector &result);
template <>
string_t QuantileCast::Operation(const string_t &src, Vector &result);

// Continuous interpolation
template <bool DISCRETE>
struct QuantileInterpolator {
	QuantileInterpolator(const QuantileValue &q, const idx_t n_p, const bool desc_p)
	    : desc(desc_p), RN((double)(n_p - 1) * q.dbl), FRN(ExactNumericCast<idx_t>(floor(RN))),
	      CRN(ExactNumericCast<idx_t>(ceil(RN))), begin(0), end(n_p) {
	}

	template <class INPUT_TYPE, class TARGET_TYPE, typename ACCESSOR = QuantileDirect<INPUT_TYPE>>
	TARGET_TYPE Interpolate(INPUT_TYPE lidx, INPUT_TYPE hidx, Vector &result, const ACCESSOR &accessor) const {
		using ACCESS_TYPE = typename ACCESSOR::RESULT_TYPE;
		if (lidx == hidx) {
			return QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(accessor(lidx), result);
		} else {
			auto lo = QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(accessor(lidx), result);
			auto hi = QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(accessor(hidx), result);
			return InterpolateOperator::Operation<TARGET_TYPE>(lo, RN - FRN, hi);
		}
	}

	template <class INPUT_TYPE, class TARGET_TYPE, typename ACCESSOR = QuantileDirect<INPUT_TYPE>>
	TARGET_TYPE Operation(INPUT_TYPE *v_t, Vector &result, const ACCESSOR &accessor = ACCESSOR()) const {
		using ACCESS_TYPE = typename ACCESSOR::RESULT_TYPE;
		QuantileCompare<ACCESSOR> comp(accessor, desc);
		if (CRN == FRN) {
			std::nth_element(v_t + begin, v_t + FRN, v_t + end, comp);
			return QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(accessor(v_t[FRN]), result);
		} else {
			std::nth_element(v_t + begin, v_t + FRN, v_t + end, comp);
			std::nth_element(v_t + FRN, v_t + CRN, v_t + end, comp);
			auto lo = QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(accessor(v_t[FRN]), result);
			auto hi = QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(accessor(v_t[CRN]), result);
			return InterpolateOperator::Operation<TARGET_TYPE>(lo, RN - FRN, hi);
		}
	}

	template <class INPUT_TYPE, class TARGET_TYPE>
	inline TARGET_TYPE Extract(const INPUT_TYPE *dest, Vector &result) const {
		if (CRN == FRN) {
			return QuantileCast::Operation<INPUT_TYPE, TARGET_TYPE>(dest[0], result);
		} else {
			auto lo = QuantileCast::Operation<INPUT_TYPE, TARGET_TYPE>(dest[0], result);
			auto hi = QuantileCast::Operation<INPUT_TYPE, TARGET_TYPE>(dest[1], result);
			return InterpolateOperator::Operation<TARGET_TYPE>(lo, RN - FRN, hi);
		}
	}

	const bool desc;
	const double RN;
	const idx_t FRN;
	const idx_t CRN;

	idx_t begin;
	idx_t end;
};

// Discrete "interpolation"
template <>
struct QuantileInterpolator<true> {
	static inline idx_t Index(const QuantileValue &q, const idx_t n) {
		idx_t floored;
		switch (q.val.type().id()) {
		case LogicalTypeId::DECIMAL: {
			//	Integer arithmetic for accuracy
			const auto integral = q.integral;
			const auto scaling = q.scaling;
			const auto scaled_q =
			    DecimalMultiplyOverflowCheck::Operation<hugeint_t, hugeint_t, hugeint_t>(Hugeint::Convert(n), integral);
			const auto scaled_n =
			    DecimalMultiplyOverflowCheck::Operation<hugeint_t, hugeint_t, hugeint_t>(Hugeint::Convert(n), scaling);
			floored = Cast::Operation<hugeint_t, idx_t>((scaled_n - scaled_q) / scaling);
			break;
		}
		default: {
			//	The target is ceil(n * q) evaluated as if exactly, matching the DECIMAL path.
			//	A binary double for a decimal fraction is slightly off, so when n * q is mathematically an
			//	integer (n = 10, q = 0.9) the product lands a few ulps away from it and ceil() would
			//	pick the next element. Worse, the outcome of floor(n - n * q) depended on whether the
			//	compiler fused the multiply and subtract into an FMA, so different call sites of this
			//	function disagreed. Snap to the nearest integer when the product is within a few ulps
			//	of it; the result is then independent of contraction and of the rounding of q.
			const auto scaled_q = double(n) * q.dbl;
			const auto nearest = std::round(scaled_q);
			const auto tolerance = double(n) * 4 * std::numeric_limits<double>::epsilon();
			const auto ceiled = (std::fabs(scaled_q - nearest) <= tolerance) ? nearest : std::ceil(scaled_q);
			//	0 <= q <= 1, so 0 <= ceiled <= n
			floored = n - LossyNumericCast<idx_t>(ceiled);
			break;
		}
		}

		return MaxValue<idx_t>(1, n - floored) - 1;
	}

	QuantileInterpolator(const QuantileValue &q, const idx_t n_p, bool desc_p)
	    : desc(desc_p), FRN(Index(q, n_p)), CRN(FRN), begin(0), end(n_p) {
	}

	template <class INPUT_TYPE, class TARGET_TYPE, typename ACCESSOR = QuantileDirect<INPUT_TYPE>>
	TARGET_TYPE Interpolate(INPUT_TYPE lidx, INPUT_TYPE hidx, Vector &result, const ACCESSOR &accessor) const {
		using ACCESS_TYPE = typename ACCESSOR::RESULT_TYPE;
		return QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(accessor(lidx), result);
	}

	template <class INPUT_TYPE, typename ACCESSOR = QuantileDirect<INPUT_TYPE>>
	typename ACCESSOR::RESULT_TYPE InterpolateInternal(INPUT_TYPE *v_t, const ACCESSOR &accessor = ACCESSOR()) const {
		QuantileCompare<ACCESSOR> comp(accessor, desc);
		std::nth_element(v_t + begin, v_t + FRN, v_t + end, comp);
		return accessor(v_t[FRN]);
	}

	template <class INPUT_TYPE, class TARGET_TYPE, typename ACCESSOR = QuantileDirect<INPUT_TYPE>>
	TARGET_TYPE Operation(INPUT_TYPE *v_t, Vector &result, const ACCESSOR &accessor = ACCESSOR()) const {
		using ACCESS_TYPE = typename ACCESSOR::RESULT_TYPE;
		return QuantileCast::Operation<ACCESS_TYPE, TARGET_TYPE>(InterpolateInternal(v_t, accessor), result);
	}

	template <class INPUT_TYPE, class TARGET_TYPE>
	TARGET_TYPE Extract(const INPUT_TYPE *dest, Vector &result) const {
		return QuantileCast::Operation<INPUT_TYPE, TARGET_TYPE>(dest[0], result);
	}

	const bool desc;
	const idx_t FRN;
	const idx_t CRN;

	idx_t begin;
	idx_t end;
};

template <typename INPUT_TYPE>
struct QuantileIncluded {
	using CURSOR_TYPE = QuantileCursor<INPUT_TYPE>;

	inline explicit QuantileIncluded(const ValidityMask &fmask_p, CURSOR_TYPE &dmask_p)
	    : fmask(fmask_p), dmask(dmask_p) {
	}

	inline bool operator()(const idx_t &idx) {
		return fmask.RowIsValid(idx) && dmask.RowIsValid(idx);
	}

	inline bool CannotHaveNull() {
		return fmask.CannotHaveNull() && dmask.CannotHaveNull();
	}
	inline bool AllValid() {
		return CannotHaveNull();
	}

	const ValidityMask &fmask;
	CURSOR_TYPE &dmask;
};

struct QuantileSortTree {
	unique_ptr<WindowIndexTree> index_tree;

	QuantileSortTree(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition) {
		// TODO: Two pass parallel sorting using Build
		auto &inputs = *partition.inputs;
		auto &interrupt = partition.interrupt_state;
		ColumnDataScanState scan;
		DataChunk sort;
		// only the leading input column is sorted on - see QuantileCursor
		D_ASSERT(!partition.column_ids.empty());
		vector<column_t> input_ids {partition.column_ids[0]};
		inputs.InitializeScan(scan, input_ids);
		inputs.InitializeScanChunk(scan, sort);

		// Sort on the single argument
		auto &bind_data = aggr_input_data.bind_data->Cast<QuantileBindData>();
		auto order_expr = make_uniq<BoundConstantExpression>(Value(sort.GetTypes()[0]));
		auto order_type = bind_data.desc ? OrderType::DESCENDING : OrderType::ASCENDING;
		BoundOrderModifier order_bys;
		order_bys.orders.emplace_back(BoundOrderByNode(order_type, OrderByNullType::NULLS_LAST, std::move(order_expr)));
		vector<column_t> sort_idx(1, 0);
		const auto count = partition.count;

		index_tree = make_uniq<WindowIndexTree>(partition.context.client, order_bys, sort_idx, count);
		auto index_state = index_tree->GetLocalState(partition.context);
		auto &local_state = index_state->Cast<WindowIndexTreeLocalState>();

		//	Build the indirection array by scanning the valid indices
		const auto &filter_mask = partition.filter_mask;
		SelectionVector filter_sel(STANDARD_VECTOR_SIZE);
		while (inputs.Scan(scan, sort)) {
			const auto row_idx = scan.current_row_index;
			if (filter_mask.CanHaveNull() || !partition.all_valid[0]) {
				// scans can return constant and dictionary vectors - read their validity in place
				auto validity = sort.data[0].Validity();
				idx_t filtered = 0;
				for (sel_t i = 0; i < sort.size(); ++i) {
					if (filter_mask.RowIsValid(i + row_idx) && validity.IsValid(i)) {
						filter_sel[filtered++] = i;
					}
				}
				local_state.Sink(partition.context, sort, row_idx, filter_sel, filtered, interrupt);
			} else {
				local_state.Sink(partition.context, sort, row_idx, nullptr, 0, interrupt);
			}
		}
		local_state.Finalize(partition.context, interrupt);
	}

	inline idx_t SelectNth(const SubFrames &frames, size_t n) const {
		return index_tree->SelectNth(frames, n).first;
	}

	template <typename INPUT_TYPE, typename RESULT_TYPE, bool DISCRETE>
	RESULT_TYPE WindowScalar(ClientContext &context, QuantileCursor<INPUT_TYPE> &data, const SubFrames &frames,
	                         const idx_t n, Vector &result, const QuantileValue &q) {
		D_ASSERT(n > 0);

		//	Thread safe and idempotent.
		index_tree->Build(context);

		//	Find the interpolated indices within the frame
		QuantileInterpolator<DISCRETE> interp(q, n, false);
		const auto lo_data = SelectNth(frames, interp.FRN);
		auto hi_data = lo_data;
		if (interp.CRN != interp.FRN) {
			hi_data = SelectNth(frames, interp.CRN);
		}

		//	Interpolate indirectly
		using ID = QuantileIndirect<INPUT_TYPE>;
		ID indirect(data);
		return interp.template Interpolate<idx_t, RESULT_TYPE, ID>(lo_data, hi_data, result, indirect);
	}

	template <typename INPUT_TYPE, typename CHILD_TYPE, bool DISCRETE>
	void WindowList(ClientContext &context, QuantileCursor<INPUT_TYPE> &data, const SubFrames &frames, const idx_t n,
	                Vector &list, const idx_t lidx, const QuantileBindData &bind_data) {
		D_ASSERT(n > 0);

		//	Thread safe and idempotent.
		index_tree->Build(context);

		// Result is a constant LIST<CHILD_TYPE> with a fixed length
		auto ldata = FlatVector::GetDataMutable<list_entry_t>(list);
		auto &lentry = ldata[lidx];
		lentry.offset = ListVector::GetListSize(list);
		lentry.length = bind_data.quantiles.size();

		ListVector::Reserve(list, lentry.offset + lentry.length);
		ListVector::SetListSize(list, lentry.offset + lentry.length);
		auto &result = ListVector::GetChildMutable(list);
		auto rdata = FlatVector::GetDataMutable<CHILD_TYPE>(result);

		using ID = QuantileIndirect<INPUT_TYPE>;
		ID indirect(data);
		for (const auto &q : bind_data.order) {
			const auto &quantile = bind_data.quantiles[q];
			QuantileInterpolator<DISCRETE> interp(quantile, n, false);

			const auto lo_data = SelectNth(frames, interp.FRN);
			auto hi_data = lo_data;
			if (interp.CRN != interp.FRN) {
				hi_data = SelectNth(frames, interp.CRN);
			}

			//	Interpolate indirectly
			rdata[lentry.offset + q] =
			    interp.template Interpolate<idx_t, CHILD_TYPE, ID>(lo_data, hi_data, result, indirect);
		}
	}
};

} // namespace duckdb
