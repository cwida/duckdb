//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/vector_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/string_heap.hpp"

#include <tuple>
#include <utility>

namespace duckdb {

//! Returns StructVector::GetEntries(vector) (mutable) without requiring struct_vector.hpp
//! to be complete in this header. Defined in struct_vector.cpp.
DUCKDB_API vector<Vector> &VectorWriterGetStructEntries(Vector &vector);

//! List-vector helpers that avoid a circular include with list_vector.hpp.
//! Defined in list_vector.cpp.
DUCKDB_API Vector &VectorWriterGetListChild(Vector &vector);
DUCKDB_API idx_t VectorWriterGetListSize(const Vector &vector);
DUCKDB_API void VectorWriterReserveList(Vector &vector, idx_t required_capacity);
DUCKDB_API void VectorWriterSetListSize(Vector &vector, idx_t size);

template <class T>
struct VectorWriter {
	VectorWriter(Vector &vector, idx_t count, idx_t offset)
	    : data(FlatVector::GetDataMutable<T>(vector)), validity(FlatVector::ValidityMutable(vector)),
	      count(offset + count), current_idx(offset) {
	}
	VectorWriter(VectorWriter &&other) noexcept
	    : data(other.data), validity(other.validity), count(other.count), current_idx(other.current_idx) {
		other.count = other.current_idx;
	}
	~VectorWriter() {
		// ensure that all values we said we were going to write have been written
		D_ASSERT(Exception::UncaughtException() || current_idx == count);
	}

	void WriteValue(const T &value) {
		D_ASSERT(current_idx < count);
		data[current_idx] = value;
		current_idx++;
	}

	void WriteNull() {
		D_ASSERT(current_idx < count);
		validity.SetInvalid(current_idx);
		current_idx++;
	}

	void WriteNull(const T &value) {
		D_ASSERT(current_idx < count);
		data[current_idx] = value;
		validity.SetInvalid(current_idx);
		current_idx++;
	}

	//! Cap the writer's count to the values written so far. Use this when
	//! releasing a writer that hasn't been fully written (e.g., when a
	//! dynamic-length list is closed early); subsequent destruction passes
	//! the count==current_idx assertion.
	void Truncate() noexcept {
		count = current_idx;
	}

private:
	T *data;
	ValidityMask &validity;
	idx_t count;
	idx_t current_idx;
};

template <>
struct VectorWriter<string_t> {
	VectorWriter(Vector &vector, idx_t count, idx_t offset);
	VectorWriter(VectorWriter &&other) noexcept
	    : vector(other.vector), data(other.data), validity(other.validity), heap(other.heap), count(other.count),
	      current_idx(other.current_idx) {
		other.count = other.current_idx;
	}
	~VectorWriter() {
		D_ASSERT(Exception::UncaughtException() || current_idx == count);
	}

	inline const string_t &WriteValue(string_t val) {
		D_ASSERT(current_idx < count);
		AssignString(current_idx, val);
		current_idx++;
		return data[current_idx - 1];
	}

	inline void WriteStringRef(string_t val) {
		D_ASSERT(current_idx < count);
		data[current_idx] = val;
		current_idx++;
	}

	inline void WriteNull() {
		D_ASSERT(current_idx < count);
		validity.SetInvalid(current_idx);
		current_idx++;
	}

	inline string_t &WriteEmptyString(idx_t length) {
		if (length <= string_t::INLINE_LENGTH) {
			data[current_idx] = string_t(UnsafeNumericCast<uint32_t>(length));
		} else {
			data[current_idx] = GetHeap().CreateEmptyStringInHeap(length);
		}
		auto &res = data[current_idx];
		current_idx++;
		return res;
	}

	void Truncate() noexcept {
		count = current_idx;
	}

	inline StringHeap &GetHeap() {
		if (!heap) {
			InitializeHeap();
		}
		return *heap;
	}

private:
	void InitializeHeap();

	inline void AssignString(idx_t idx, string_t val) {
		if (val.IsInlined()) {
			data[idx] = val;
		} else {
			auto &string_heap = GetHeap();
			data[idx] = string_heap.AddBlobToHeap(val.GetData(), val.GetSize());
		}
	}

private:
	Vector &vector;
	string_t *data;
	ValidityMask &validity;
	optional_ptr<StringHeap> heap;
	idx_t count;
	idx_t current_idx;
};

//! Specialization of VectorWriter for VectorStructType<Args...>.
//! Writes rows to a struct vector with NULL propagation to all children.
//! Supports recursive nesting: a child writer may itself be a struct writer.
template <class... Args>
struct VectorWriter<VectorStructType<Args...>> {
private:
	static_assert(sizeof...(Args) > 0, "VectorStructType must have at least one child type");

	using ChildWriters = std::tuple<VectorWriter<Args>...>;

public:
	VectorWriter(Vector &vector, idx_t count, idx_t offset)
	    : validity(FlatVector::ValidityMutable(vector)), count(offset + count), current_idx(offset),
	      children(MakeChildren(vector, count, offset, std::index_sequence_for<Args...> {})) {
	}
	~VectorWriter() {
		D_ASSERT(Exception::UncaughtException() || current_idx == count);
	}

	//! Write a NULL for this struct row. Also writes NULL to every child so all
	//! per-child row counters stay in sync with the top-level counter.
	void WriteNull() {
		D_ASSERT(current_idx < count);
		validity.SetInvalid(current_idx);
		WriteNullToChildren(std::index_sequence_for<Args...> {});
		current_idx++;
	}

	//! Write a non-NULL row by calling fun(child0, child1, ...) with each child
	//! writer as a separate argument. fun is responsible for writing one value to
	//! each child. Advances the top-level row counter automatically.
	template <class FUN>
	void WriteValue(FUN &&fun) {
		D_ASSERT(current_idx < count);
		std::apply(std::forward<FUN>(fun), children);
		current_idx++;
	}

	//! Call fun(child_writer) for each child in declaration order, then advance
	//! the top-level row counter. Useful when all children share the same type
	//! and the same operation is applied to each.
	template <class FUN>
	void ForEach(FUN &&fun) {
		D_ASSERT(current_idx < count);
		ForEachImpl(std::forward<FUN>(fun), std::index_sequence_for<Args...> {});
		current_idx++;
	}

	void Truncate() noexcept {
		count = current_idx;
		TruncateChildren(std::index_sequence_for<Args...> {});
	}

private:
	template <std::size_t... Is>
	void TruncateChildren(std::index_sequence<Is...>) {
		(std::get<Is>(children).Truncate(), ...);
	}

	template <std::size_t... Is>
	void WriteNullToChildren(std::index_sequence<Is...>) {
		(std::get<Is>(children).WriteNull(), ...);
	}

	template <class FUN, std::size_t... Is>
	void ForEachImpl(FUN &&fun, std::index_sequence<Is...>) {
		(fun(std::get<Is>(children)), ...);
	}

	template <std::size_t... Is>
	static ChildWriters MakeChildren(Vector &vector, idx_t count, idx_t offset, std::index_sequence<Is...>) {
		auto &entries = VectorWriterGetStructEntries(vector);
		D_ASSERT(entries.size() >= sizeof...(Is));
		return ChildWriters(VectorWriter<Args>(entries[Is], count, offset)...);
	}

private:
	ValidityMask &validity;
	idx_t count;
	idx_t current_idx;
	ChildWriters children;
};

//! Forward declaration for the dynamic list-of-T writer (defined below).
template <class T>
class DynamicListWriter;

//! Forward declaration for the type-erased dynamic list appender (defined below).
class DynamicListAppender;

//! Specialization of VectorWriter for VectorListType<T>.
//! Writes rows to a list vector. Non-null rows are opened with WriteList(n),
//! which returns a range that iterates over n child writers with their in-list
//! index. Null rows are written with WriteNull(). Supports recursive nesting:
//! the child writer T may itself be a list or struct writer.
template <class T>
struct VectorWriter<VectorListType<T>> {
public:
	//! Range returned by WriteList(n). Holds a VectorWriter<T> scoped to the
	//! n reserved child slots. Destroying the range asserts all n were written.
	//! C++17 guaranteed copy elision ensures it is always constructed in place.
	struct WriteRange {
		class RangeIterator {
		public:
			RangeIterator(VectorWriter<T> &writer, idx_t pos) : writer(writer), pos(pos) {
			}
			VectorWriter<T> &operator*() {
				return writer;
			}
			RangeIterator &operator++() { // NOLINT: match stl API
				++pos;
				return *this;
			}
			bool operator!=(const RangeIterator &other) const {
				return pos != other.pos;
			}

		private:
			VectorWriter<T> &writer;
			idx_t pos;
		};

		WriteRange(Vector &child_vec, idx_t n, idx_t offset) : child_writer(child_vec, n, offset), length(n) {
		}

		RangeIterator begin() { // NOLINT: match stl API
			return RangeIterator(child_writer, 0);
		}
		RangeIterator end() { // NOLINT: match stl API
			return RangeIterator(child_writer, length);
		}

		VectorWriter<T> child_writer;
		idx_t length;
	};

public:
	VectorWriter(Vector &vector, idx_t count, idx_t offset)
	    : list_data(FlatVector::GetDataMutable<list_entry_t>(vector)), validity(FlatVector::ValidityMutable(vector)),
	      list_vec(vector), child_vec(VectorWriterGetListChild(vector)), count(offset + count), current_idx(offset),
	      child_offset(VectorWriterGetListSize(vector)) {
	}
	VectorWriter(VectorWriter &&other) noexcept
	    : list_data(other.list_data), validity(other.validity), list_vec(other.list_vec), child_vec(other.child_vec),
	      count(other.count), current_idx(other.current_idx), child_offset(other.child_offset) {
		other.count = other.current_idx;
	}
	~VectorWriter() {
		D_ASSERT(Exception::UncaughtException() || current_idx == count);
	}

	void WriteNull() {
		D_ASSERT(current_idx < count);
		validity.SetInvalid(current_idx);
		current_idx++;
	}

	void Truncate() noexcept {
		count = current_idx;
	}

	//! Reserve n child slots, record the list entry for this row, and return a
	//! WriteRange whose iterator yields child writers for each slot.
	//! Destroying the range asserts that all n child slots were written.
	WriteRange WriteList(idx_t n) {
		D_ASSERT(current_idx < count);
		VectorWriterReserveList(list_vec, child_offset + n);
		VectorWriterSetListSize(list_vec, child_offset + n);
		list_data[current_idx] = {child_offset, n};
		current_idx++;
		const auto old_offset = child_offset;
		child_offset += n;
		return WriteRange(child_vec, n, old_offset);
	}

	//! Open a list whose final length is unknown. Returns a DynamicListWriter<T>
	//! that pre-reserves a small capacity in the child vector and grows on demand.
	//! Each call to WriteElement() returns a writer for the next slot; the list
	//! length is recorded when the returned writer goes out of scope.
	DynamicListWriter<T> WriteDynamicList();

private:
	friend class DynamicListWriter<T>;

	list_entry_t *list_data;
	ValidityMask &validity;
	Vector &list_vec;
	Vector &child_vec;
	idx_t count;
	idx_t current_idx;
	idx_t child_offset;
};

//! Writer for a single list whose final length is unknown. Holds a single
//! VectorWriter<T> that is reused across all elements. When the underlying
//! child vector needs to grow (because the user wrote past the reserved
//! capacity), the stored writer is destroyed and reconstructed in place with
//! a refreshed data pointer. Each WriteElement() call returns a reference to
//! that writer, advanced to the next slot. The actual list_entry length is
//! recorded when the DynamicListWriter goes out of scope.
//!
//! Typical usage:
//!     auto list = list_writer.WriteDynamicList();
//!     while (...) {
//!         auto &child_writer = list.WriteElement();
//!         child_writer.WriteValue(value);
//!     }
template <class T>
class DynamicListWriter {
public:
	DynamicListWriter(VectorWriter<VectorListType<T>> &parent, idx_t row_idx, idx_t base_offset)
	    : parent(parent), row_idx(row_idx), base_offset(base_offset), capacity(INITIAL_CAPACITY), current_idx(0) {
		VectorWriterReserveList(parent.list_vec, base_offset + capacity);
		VectorWriterSetListSize(parent.list_vec, base_offset + capacity);
		new (&child_writer) VectorWriter<T>(parent.child_vec, capacity, base_offset);
	}
	DynamicListWriter(const DynamicListWriter &) = delete;
	DynamicListWriter(DynamicListWriter &&) = delete;
	~DynamicListWriter() {
		parent.list_data[row_idx] = {base_offset, current_idx};
		parent.child_offset = base_offset + current_idx;
		VectorWriterSetListSize(parent.list_vec, base_offset + current_idx);
		// suppress the writer's count-mismatch assertion (the reserved capacity
		// is typically larger than what was actually written) and destroy it.
		child_writer.Truncate();
		child_writer.~VectorWriter<T>();
	}

	//! Returns a reference to the stored child writer, advanced to the next
	//! slot. The caller must write exactly one value via the returned writer
	//! before calling WriteElement() again.
	VectorWriter<T> &WriteElement() {
		if (current_idx == capacity) {
			Grow();
		}
		current_idx++;
		return child_writer;
	}

private:
	void Grow() {
		// the stored writer was filled to capacity, so its assertion passes
		child_writer.~VectorWriter<T>();
		capacity = capacity * 2;
		VectorWriterReserveList(parent.list_vec, base_offset + capacity);
		VectorWriterSetListSize(parent.list_vec, base_offset + capacity);
		new (&child_writer) VectorWriter<T>(parent.child_vec, capacity - current_idx, base_offset + current_idx);
	}

	static constexpr idx_t INITIAL_CAPACITY = 8;

	VectorWriter<VectorListType<T>> &parent;
	idx_t row_idx;
	idx_t base_offset;
	idx_t capacity;
	idx_t current_idx;
	// stored via union so the writer's lifetime is managed manually -- it must
	// be destroyed and reconstructed when the underlying buffer is reallocated.
	union {
		VectorWriter<T> child_writer;
	};
};

template <class T>
inline DynamicListWriter<T> VectorWriter<VectorListType<T>>::WriteDynamicList() {
	D_ASSERT(current_idx < count);
	const auto old_offset = child_offset;
	const auto row_idx = current_idx;
	current_idx++;
	return DynamicListWriter<T>(*this, row_idx, old_offset);
}

//! Specialization of VectorWriter for list_entry_t. Writes per-row list entries
//! to a list vector without a templated child type. Supports the primitive-style
//! WriteValue(list_entry_t) / WriteNull() API for callers that compute list
//! entries themselves, plus WriteDynamicList() which returns a DynamicListAppender
//! that grows the child vector via Vector::Copy from another vector.
template <>
struct VectorWriter<list_entry_t> {
	VectorWriter(Vector &vector, idx_t count, idx_t offset)
	    : list_data(FlatVector::GetDataMutable<list_entry_t>(vector)), validity(FlatVector::ValidityMutable(vector)),
	      list_vec(vector), count(offset + count), current_idx(offset), child_offset(VectorWriterGetListSize(vector)) {
	}
	VectorWriter(VectorWriter &&other) noexcept
	    : list_data(other.list_data), validity(other.validity), list_vec(other.list_vec), count(other.count),
	      current_idx(other.current_idx), child_offset(other.child_offset) {
		other.count = other.current_idx;
	}
	~VectorWriter() {
		D_ASSERT(Exception::UncaughtException() || current_idx == count);
	}

	void WriteValue(const list_entry_t &value) {
		D_ASSERT(current_idx < count);
		list_data[current_idx] = value;
		current_idx++;
	}

	void WriteNull() {
		D_ASSERT(current_idx < count);
		validity.SetInvalid(current_idx);
		current_idx++;
	}

	void WriteNull(const list_entry_t &value) {
		D_ASSERT(current_idx < count);
		list_data[current_idx] = value;
		validity.SetInvalid(current_idx);
		current_idx++;
	}

	void Truncate() noexcept {
		count = current_idx;
	}

	//! Open a list whose final length is unknown. Returns a DynamicListAppender
	//! that copies entries from other vectors into the child via Vector::Copy.
	//! The list_entry for this row is finalized when the appender goes out of scope.
	DynamicListAppender WriteDynamicList();

private:
	friend class DynamicListAppender;

	list_entry_t *list_data;
	ValidityMask &validity;
	Vector &list_vec;
	idx_t count;
	idx_t current_idx;
	idx_t child_offset;
};

//! Type-erased counterpart to DynamicListWriter<T> for callers that don't know
//! the child element type at compile time (e.g., list_concat). Instead of
//! offering a per-element writer, it appends ranges from another vector via
//! Vector::Copy. The list_entry length is recorded when the appender goes out
//! of scope.
//!
//! Typical usage:
//!     auto list = list_writer.WriteDynamicList();
//!     for (auto &input : inputs) {
//!         list.Append(input.child_vec, *FlatVector::IncrementalSelectionVector(),
//!                     entry.offset + entry.length, entry.offset, entry.length);
//!     }
class DynamicListAppender {
public:
	DynamicListAppender(VectorWriter<list_entry_t> &parent, idx_t row_idx, idx_t base_offset)
	    : parent(parent), row_idx(row_idx), base_offset(base_offset), current_length(0) {
	}
	DynamicListAppender(const DynamicListAppender &) = delete;
	DynamicListAppender(DynamicListAppender &&) = delete;
	~DynamicListAppender() { // NOLINT(bugprone-exception-escape): writer invariants make these setters non-throwing.
		parent.list_data[row_idx] = {base_offset, current_length};
		parent.child_offset = base_offset + current_length;
		VectorWriterSetListSize(parent.list_vec, base_offset + current_length);
	}

	//! Copy `copy_count` rows out of `source` (selected via source_sel/source_offset/source_count)
	//! into the list under construction. Grows the parent's child vector as needed.
	void Append(const Vector &source, const SelectionVector &source_sel, idx_t source_count, idx_t source_offset,
	            idx_t copy_count) {
		const idx_t target_offset = base_offset + current_length;
		VectorWriterReserveList(parent.list_vec, target_offset + copy_count);
		VectorWriterSetListSize(parent.list_vec, target_offset + copy_count);
		Vector &child_vec = VectorWriterGetListChild(parent.list_vec);
		child_vec.Copy(source, source_sel, source_count, source_offset, target_offset, copy_count);
		current_length += copy_count;
	}

	//! Append `count` NULL elements to the list under construction. Used to pad a
	//! list to a target length when no source value is available. Goes through
	//! FlatVector::SetNull so NULL propagates correctly to struct/array children.
	void AppendNulls(idx_t count) {
		const idx_t target_offset = base_offset + current_length;
		VectorWriterReserveList(parent.list_vec, target_offset + count);
		VectorWriterSetListSize(parent.list_vec, target_offset + count);
		Vector &child_vec = VectorWriterGetListChild(parent.list_vec);
		for (idx_t i = 0; i < count; i++) {
			FlatVector::SetNull(child_vec, target_offset + i, true);
		}
		current_length += count;
	}

private:
	VectorWriter<list_entry_t> &parent;
	idx_t row_idx;
	idx_t base_offset;
	idx_t current_length;
};

inline DynamicListAppender VectorWriter<list_entry_t>::WriteDynamicList() {
	D_ASSERT(current_idx < count);
	const auto old_offset = child_offset;
	const auto row_idx = current_idx;
	current_idx++;
	return DynamicListAppender(*this, row_idx, old_offset);
}

template <class T>
struct VectorScatterWriter {
	explicit VectorScatterWriter(Vector &vector)
	    : data(FlatVector::GetDataMutable<T>(vector)), validity(FlatVector::ValidityMutable(vector)) {
	}

	void SetInvalid(idx_t idx) {
		validity.SetInvalid(idx);
	}

	T &operator[](idx_t idx) {
		return data[idx];
	}

private:
	T *data;
	ValidityMask &validity;
};

template <>
struct VectorScatterWriter<string_t> {
	struct StringElement {
		StringElement(VectorScatterWriter<string_t> &writer, string_t *data, idx_t idx)
		    : writer(writer), data(data), idx(idx) {
		}

		//! Constructs an empty string of a given length and returns it
		//! Note: the empty string must be filled and .Finalize() must be called on it
		inline string_t &EmptyString(idx_t length) {
			if (length <= string_t::INLINE_LENGTH) {
				data[idx] = string_t(UnsafeNumericCast<uint32_t>(length));
			} else {
				auto &heap = writer.GetHeap();
				data[idx] = heap.CreateEmptyStringInHeap(length);
			}
			return data[idx];
		}
		inline string_t &operator=(string_t val) {
			if (val.IsInlined()) {
				data[idx] = val;
			} else {
				auto &heap = writer.GetHeap();
				data[idx] = heap.AddBlobToHeap(val.GetData(), val.GetSize());
			}
			return data[idx];
		}
		inline char *GetDataWriteable() {
			return data[idx].GetDataWriteable();
		}
		inline void Finalize() {
			data[idx].Finalize();
		}
		inline string GetString() {
			return data[idx].GetString();
		}

		operator string_t() const { // NOLINT: allow implicit conversion
			return data[idx];
		}

	private:
		VectorScatterWriter<string_t> &writer;
		string_t *data;
		idx_t idx;
	};

	explicit VectorScatterWriter(Vector &vector);

	inline void SetInvalid(idx_t idx) {
		validity.SetInvalid(idx);
	}

	inline StringElement operator[](idx_t idx) {
		return StringElement(*this, data, idx);
	}

	inline StringHeap &GetHeap() {
		if (!heap) {
			InitializeHeap();
		}
		return *heap;
	}

private:
	void InitializeHeap();

private:
	Vector &vector;
	string_t *data;
	ValidityMask &validity;
	optional_ptr<StringHeap> heap;
};

} // namespace duckdb
