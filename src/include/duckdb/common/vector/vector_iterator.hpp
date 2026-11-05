//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/vector_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

#include <tuple>
#include <utility>

namespace duckdb {

//! Returns StructVector::GetEntries(vector) without requiring StructVector to
//! be complete in this header (avoids a circular include via flat_vector.hpp).
//! Defined in struct_vector.cpp.
DUCKDB_API const vector<Vector> &VectorIteratorGetStructEntries(const Vector &vector);

//! Returns ListVector::GetChild(vector) / ListVector::GetListSize(vector) without
//! requiring list_vector.hpp to be complete in this header.
//! Defined in list_vector.cpp.
DUCKDB_API const Vector &VectorIteratorGetListChild(const Vector &vector);
DUCKDB_API idx_t VectorIteratorGetListSize(const Vector &vector);

DUCKDB_API const Vector &VectorIteratorGetArrayChild(const Vector &vector);

class VectorValidityIterator {
public:
	explicit VectorValidityIterator(const Vector &vector) : count(vector.size()) {
		vector.ToUnifiedFormat(format);
	}

	bool IsValid(idx_t i) const {
		return format.validity.RowIsValid(format.sel->get_index(i));
	}
	bool CanHaveNull() const {
		return format.validity.CanHaveNull();
	}
	bool CannotHaveNull() const {
		return format.validity.CannotHaveNull();
	}
	idx_t size() const {
		return count;
	}

private:
	UnifiedVectorFormat format;
	idx_t count;
};

template <class T>
class VectorIterator {
public:
	explicit VectorIterator(const Vector &vector) : count(vector.size()) {
		vector.ToUnifiedFormat(format);
		data = UnifiedVectorFormat::GetData<T>(format);
		contiguous_data = format.sel->IsSet() ? nullptr : data;
	}

public:
	struct ValueEntry {
		ValueEntry(const UnifiedVectorFormat &format, const T *data, idx_t index)
		    : format(format), data(data), index(index) {
			sel_index = format.sel->get_index(index);
		}

		//! Return the data - the value must be valid
		const T &GetValue() const {
			D_ASSERT(IsValid());
			return GetValueUnsafe();
		}
		//! Return the underlying data. If the data is not valid then uninitialized memory is returned.
		const T &GetValueUnsafe() const {
			return data[sel_index];
		}
		bool IsValid() const {
			return format.validity.RowIsValid(sel_index);
		}
		idx_t GetIndex() const {
			return index;
		}

	private:
		const UnifiedVectorFormat &format;
		const T *data;
		idx_t sel_index;
		idx_t index;
	};

private:
	class Iterator {
	public:
		explicit Iterator(UnifiedVectorFormat &format, const T *data, idx_t index)
		    : format(format), data(data), index(index) {
		}

	public:
		Iterator &operator++() { // NOLINT: match stl API
			++index;
			return *this;
		}
		Iterator operator++(int) { // NOLINT: match stl API
			auto tmp = *this;
			++index;
			return tmp;
		}
		Iterator &operator--() { // NOLINT: match stl API
			--index;
			return *this;
		}
		Iterator &operator+=(idx_t n) {
			index += n;
			return *this;
		}
		Iterator &operator-=(idx_t n) {
			index -= n;
			return *this;
		}
		Iterator operator+(idx_t n) const {
			return Iterator(format, data, index + n);
		}
		Iterator operator-(idx_t n) const {
			return Iterator(format, data, index - n);
		}
		int64_t operator-(const Iterator &other) const {
			return static_cast<int64_t>(index) - static_cast<int64_t>(other.index);
		}
		bool operator==(const Iterator &other) const {
			return index == other.index;
		}
		bool operator!=(const Iterator &other) const {
			return index != other.index;
		}
		bool operator<(const Iterator &other) const {
			return index < other.index;
		}
		bool operator<=(const Iterator &other) const {
			return index <= other.index;
		}
		bool operator>(const Iterator &other) const {
			return index > other.index;
		}
		bool operator>=(const Iterator &other) const {
			return index >= other.index;
		}
		ValueEntry operator*() const {
			return GetEntry(index);
		}
		ValueEntry operator[](idx_t n) const {
			return GetEntry(index + n);
		}

	private:
		ValueEntry GetEntry(idx_t i) const {
			return ValueEntry(format, data, i);
		}

	private:
		UnifiedVectorFormat &format;
		const T *data;
		idx_t index;
	};

public:
	Iterator begin() { // NOLINT: match stl API
		return Iterator(format, data, 0);
	}
	Iterator end() { // NOLINT: match stl API
		return Iterator(format, data, count);
	}
	idx_t size() const {
		return count;
	}
	ValueEntry operator[](idx_t i) const {
		return ValueEntry(format, data, i);
	}
	//! Returns the value at the specified location without checking the NULL mask
	T GetValueUnsafe(idx_t i) const {
		return data[format.sel->get_index(i)];
	}
	bool CanHaveNull() const {
		return format.validity.CanHaveNull();
	}
	//! Returns the values in [offset, offset + length) if they are contiguous and all valid, or nullptr otherwise
	const T *TryGetContiguousData(idx_t offset, idx_t length) const {
		if (!contiguous_data || !format.validity.CheckAllValid(offset + length, offset)) {
			return nullptr;
		}
		return contiguous_data + offset;
	}

private:
	UnifiedVectorFormat format;
	const T *data;
	//! The data if row i is stored at data[i], nullptr otherwise
	const T *contiguous_data;
	idx_t count;
};

//! Specialization of VectorIterator for FlatStruct<Args...>.
//! Iterates over a struct vector, exposing per-row top-level NULL information
//! and per-child VectorIterator<T>::ValueEntry access via the compile-time
//! GetValue<I>() accessor. Supports heterogeneous child types.
template <class... Args>
class VectorIterator<VectorStructType<Args...>> {
private:
	static_assert(sizeof...(Args) > 0, "FlatStruct must have at least one child type");

	template <idx_t I>
	using ChildTypeAt = typename std::tuple_element<I, std::tuple<Args...>>::type;
	template <idx_t I>
	using ChildIteratorAt = VectorIterator<ChildTypeAt<I>>;
	template <idx_t I>
	using ChildEntryAt = typename ChildIteratorAt<I>::ValueEntry;

	using ChildIterators = std::tuple<VectorIterator<Args>...>;

public:
	static constexpr idx_t WIDTH = sizeof...(Args);

	explicit VectorIterator(const Vector &vector)
	    : children(MakeChildren(vector, std::index_sequence_for<Args...> {})), count(vector.size()) {
		vector.ToUnifiedFormat(format);
	}

public:
	struct ValueEntry {
		ValueEntry(const UnifiedVectorFormat &format, const ChildIterators &children, idx_t index)
		    : format(format), children(children), index(index) {
			sel_index = format.sel->get_index(index);
		}

		//! Returns true if the top-level struct entry is not NULL
		bool IsValid() const {
			return format.validity.RowIsValid(sel_index);
		}
		idx_t GetIndex() const {
			return index;
		}
		//! Returns the ValueEntry for the I-th child at this row
		template <idx_t I>
		ChildEntryAt<I> GetChildValue() const {
			static_assert(I < sizeof...(Args), "FlatStruct child index out of range");
			return std::get<I>(children)[index];
		}
		//! Invokes fun(child_entry) for each child in declaration order.
		//! For homogeneous structs the lambda's argument can be a concrete
		//! VectorIterator<T>::ValueEntry; for heterogeneous structs use auto.
		template <class FUN>
		void ForEach(FUN &&fun) const {
			ForEachImpl(std::forward<FUN>(fun), std::index_sequence_for<Args...> {});
		}

	private:
		template <class FUN, std::size_t... Is>
		void ForEachImpl(FUN &&fun, std::index_sequence<Is...>) const {
			(fun(GetChildValue<Is>()), ...);
		}

	private:
		const UnifiedVectorFormat &format;
		const ChildIterators &children;
		idx_t sel_index;
		idx_t index;
	};

private:
	template <std::size_t... Is>
	static ChildIterators MakeChildren(const Vector &vector, std::index_sequence<Is...>) {
		auto &entries = VectorIteratorGetStructEntries(vector);
		D_ASSERT(entries.size() >= sizeof...(Is));
		return ChildIterators(VectorIterator<Args>(entries[Is])...);
	}

	class Iterator {
	public:
		Iterator(const UnifiedVectorFormat &format, const ChildIterators &children, idx_t index)
		    : format(format), children(children), index(index) {
		}

	public:
		Iterator &operator++() { // NOLINT: match stl API
			++index;
			return *this;
		}
		Iterator operator++(int) { // NOLINT: match stl API
			auto tmp = *this;
			++index;
			return tmp;
		}
		bool operator==(const Iterator &other) const {
			return index == other.index;
		}
		bool operator!=(const Iterator &other) const {
			return index != other.index;
		}
		ValueEntry operator*() const {
			return ValueEntry(format, children, index);
		}
		ValueEntry operator[](idx_t n) const {
			return ValueEntry(format, children, index + n);
		}

	private:
		const UnifiedVectorFormat &format;
		const ChildIterators &children;
		idx_t index;
	};

public:
	Iterator begin() { // NOLINT: match stl API
		return Iterator(format, children, 0);
	}
	Iterator end() { // NOLINT: match stl API
		return Iterator(format, children, count);
	}
	idx_t size() const {
		return count;
	}
	ValueEntry operator[](idx_t i) const {
		return ValueEntry(format, children, i);
	}
	bool CanHaveNull() const {
		return format.validity.CanHaveNull();
	}

private:
	UnifiedVectorFormat format;
	ChildIterators children;
	idx_t count;
};

//! Range over the child entries [offset, offset + length) of a list or array entry, for range-based for loops.
template <class CHILD_ITERATOR>
struct VectorChildRange {
private:
	using ChildEntryType = typename CHILD_ITERATOR::ValueEntry;

	struct RangeIterator {
	public:
		RangeIterator(const CHILD_ITERATOR &child_iter, idx_t pos) : child_iter(child_iter), pos(pos) {
		}
		RangeIterator &operator++() { // NOLINT: match stl API
			++pos;
			return *this;
		}
		RangeIterator operator++(int) { // NOLINT: match stl API
			auto tmp = *this;
			++pos;
			return tmp;
		}
		bool operator==(const RangeIterator &other) const {
			return pos == other.pos;
		}
		bool operator!=(const RangeIterator &other) const {
			return pos != other.pos;
		}
		ChildEntryType operator*() const {
			return child_iter[pos];
		}

	private:
		const CHILD_ITERATOR &child_iter;
		idx_t pos;
	};

public:
	VectorChildRange(const CHILD_ITERATOR &child_iter, idx_t offset, idx_t length)
	    : child_iter(child_iter), offset(offset), length(length) {
	}
	RangeIterator begin() const { // NOLINT: match stl API
		return RangeIterator(child_iter, offset);
	}
	RangeIterator end() const { // NOLINT: match stl API
		return RangeIterator(child_iter, offset + length);
	}
	idx_t size() const {
		return length;
	}

private:
	const CHILD_ITERATOR &child_iter;
	idx_t offset;
	idx_t length;
};

//! Specialization of VectorIterator for VectorListType<T>.
//! Iterates over a list vector, exposing per-row NULL information and access to
//! the child entries via GetChildValue(idx) / GetChildValues().  Supports
//! recursive nesting: VectorListType<VectorListType<T>>, etc.
//!
//! The source vector must not be a DICTIONARY_VECTOR; call Flatten() first if needed.
template <class T>
class VectorIterator<VectorListType<T>> {
private:
	using ChildIteratorType = VectorIterator<T>;
	using ChildEntryType = typename ChildIteratorType::ValueEntry;

public:
	explicit VectorIterator(const Vector &vector)
	    : child_iter(VectorIteratorGetListChild(vector)), count(vector.size()) {
		vector.ToUnifiedFormat(format);
		list_data = UnifiedVectorFormat::GetData<list_entry_t>(format);
	}

public:
	//! Range object returned by ValueEntry::GetChildValues() for range-based for loops.
	using ChildRange = VectorChildRange<ChildIteratorType>;

	struct ValueEntry {
		ValueEntry(const UnifiedVectorFormat &format, const list_entry_t *list_data,
		           const ChildIteratorType &child_iter, idx_t index)
		    : format(format), list_data(list_data), child_iter(child_iter), index(index) {
			sel_index = format.sel->get_index(index);
		}

		bool IsValid() const {
			return format.validity.RowIsValid(sel_index);
		}
		idx_t GetIndex() const {
			return index;
		}
		//! Returns the list_entry_t (offset + length). The entry must be valid.
		const list_entry_t &GetValue() const {
			D_ASSERT(IsValid());
			return list_data[sel_index];
		}
		//! Returns the number of child elements in this list entry.
		idx_t GetListLength() const {
			return GetValue().length;
		}
		//! Returns the child entry at position idx within this list.
		ChildEntryType GetChildValue(idx_t idx) const {
			return child_iter[GetValue().offset + idx];
		}
		//! Returns an iterable range over all child entries in this list.
		ChildRange GetChildValues() const {
			const auto &entry = GetValue();
			return ChildRange(child_iter, entry.offset, entry.length);
		}

	private:
		const UnifiedVectorFormat &format;
		const list_entry_t *list_data;
		const ChildIteratorType &child_iter;
		idx_t sel_index;
		idx_t index;
	};

private:
	class Iterator {
	public:
		Iterator(const UnifiedVectorFormat &format, const list_entry_t *list_data, const ChildIteratorType &child_iter,
		         idx_t index)
		    : format(format), list_data(list_data), child_iter(child_iter), index(index) {
		}

	public:
		Iterator &operator++() { // NOLINT: match stl API
			++index;
			return *this;
		}
		Iterator operator++(int) { // NOLINT: match stl API
			auto tmp = *this;
			++index;
			return tmp;
		}
		bool operator==(const Iterator &other) const {
			return index == other.index;
		}
		bool operator!=(const Iterator &other) const {
			return index != other.index;
		}
		ValueEntry operator*() const {
			return ValueEntry(format, list_data, child_iter, index);
		}
		ValueEntry operator[](idx_t n) const {
			return ValueEntry(format, list_data, child_iter, index + n);
		}

	private:
		const UnifiedVectorFormat &format;
		const list_entry_t *list_data;
		const ChildIteratorType &child_iter;
		idx_t index;
	};

public:
	Iterator begin() { // NOLINT: match stl API
		return Iterator(format, list_data, child_iter, 0);
	}
	Iterator end() { // NOLINT: match stl API
		return Iterator(format, list_data, child_iter, count);
	}
	idx_t size() const {
		return count;
	}
	ValueEntry operator[](idx_t i) const {
		return ValueEntry(format, list_data, child_iter, i);
	}
	bool CanHaveNull() const {
		return format.validity.CanHaveNull();
	}

private:
	UnifiedVectorFormat format;
	const list_entry_t *list_data;
	ChildIteratorType child_iter;
	idx_t count;
};

//! Specialization of VectorIterator for VectorArrayType<T>.
//! Iterates over an array vector, exposing per-row NULL information and access to
//! the child entries via GetChildValue(idx) / GetChildValues(). TryGetData() reads
//! the elements of an array in place when they are contiguous and not NULL.
template <class T>
class VectorIterator<VectorArrayType<T>> {
private:
	using ChildIteratorType = VectorIterator<T>;
	using ChildEntryType = typename ChildIteratorType::ValueEntry;

public:
	explicit VectorIterator(const Vector &vector)
	    : child_iter(VectorIteratorGetArrayChild(vector)), array_size(ArrayType::GetSize(vector.GetType())),
	      count(vector.size()) {
		vector.ToUnifiedFormat(format);
	}

public:
	using ChildRange = VectorChildRange<ChildIteratorType>;

	//! Small enough to be passed in registers
	struct ValueEntry {
		ValueEntry(const VectorIterator &parent, idx_t index) : parent(parent), index(index) {
		}

		bool IsValid() const {
			return parent.format.validity.RowIsValid(GetSelIndex());
		}
		idx_t GetIndex() const {
			return index;
		}
		idx_t GetArraySize() const {
			return parent.array_size;
		}
		//! Returns the child entry at position idx within this array.
		ChildEntryType GetChildValue(idx_t idx) const {
			return parent.child_iter[GetOffset() + idx];
		}
		//! Returns an iterable range over all child entries in this array.
		ChildRange GetChildValues() const {
			return ChildRange(parent.child_iter, GetOffset(), parent.array_size);
		}
		//! Returns the elements of this array if they are contiguous and none are NULL, or nullptr otherwise.
		const T *TryGetData() const {
			return parent.child_iter.TryGetContiguousData(GetOffset(), parent.array_size);
		}

	private:
		idx_t GetSelIndex() const {
			return parent.format.sel->get_index(index);
		}
		idx_t GetOffset() const {
			return GetSelIndex() * parent.array_size;
		}

	private:
		const VectorIterator &parent;
		idx_t index;
	};

private:
	class Iterator {
	public:
		Iterator(const VectorIterator &parent, idx_t index) : parent(parent), index(index) {
		}

	public:
		Iterator &operator++() { // NOLINT: match stl API
			++index;
			return *this;
		}
		Iterator operator++(int) { // NOLINT: match stl API
			auto tmp = *this;
			++index;
			return tmp;
		}
		bool operator==(const Iterator &other) const {
			return index == other.index;
		}
		bool operator!=(const Iterator &other) const {
			return index != other.index;
		}
		ValueEntry operator*() const {
			return ValueEntry(parent, index);
		}
		ValueEntry operator[](idx_t n) const {
			return ValueEntry(parent, index + n);
		}

	private:
		const VectorIterator &parent;
		idx_t index;
	};

public:
	Iterator begin() { // NOLINT: match stl API
		return Iterator(*this, 0);
	}
	Iterator end() { // NOLINT: match stl API
		return Iterator(*this, count);
	}
	idx_t size() const {
		return count;
	}
	ValueEntry operator[](idx_t i) const {
		return ValueEntry(*this, i);
	}
	bool CanHaveNull() const {
		return format.validity.CanHaveNull();
	}

private:
	UnifiedVectorFormat format;
	ChildIteratorType child_iter;
	idx_t array_size;
	idx_t count;
};

template <class T>
class VectorValidValueIterator {
public:
	explicit VectorValidValueIterator(const Vector &vector) : count(vector.size()) {
		vector.ToUnifiedFormat(format);
		data = UnifiedVectorFormat::GetData<T>(format);
	}

private:
	class VectorScanIterator;

public:
	struct VectorValueEntry {
		const T &GetValue() const {
			return value;
		}
		idx_t GetIndex() const {
			return index;
		}

	private:
		idx_t index;
		T value;
		friend class VectorScanIterator;
	};

private:
	class VectorScanIterator {
	public:
		explicit VectorScanIterator(UnifiedVectorFormat &format, const T *data, idx_t index, idx_t count)
		    : format(format), data(data), count(count), can_have_null(format.validity.CanHaveNull()) {
			r.index = index;
			AdvanceToValid();
		}

	public:
		VectorScanIterator &operator++() {
			++r.index;
			AdvanceToValid();
			return *this;
		}
		VectorScanIterator operator++(int) {
			auto tmp = *this;
			++(*this);
			return tmp;
		}
		bool operator!=(const VectorScanIterator &other) const {
			return r.index != other.r.index;
		}
		const VectorValueEntry &operator*() const {
			return r;
		}

	private:
		void AdvanceToValid() {
			if (!can_have_null) {
				if (r.index < count) {
					// we know this value is valid
					r.value = data[format.sel->get_index(r.index)];
				}
				return;
			}
			for (; r.index < count; r.index++) {
				auto idx = format.sel->get_index(r.index);
				if (format.validity.RowIsValid(idx)) {
					// found a valid value - stop
					r.value = data[idx];
					break;
				}
			}
		}

	private:
		UnifiedVectorFormat &format;
		const T *data;
		VectorValueEntry r;
		idx_t count;
		bool can_have_null;
	};

public:
	VectorScanIterator begin() { // NOLINT: match stl API
		return VectorScanIterator(format, data, 0, count);
	}
	VectorScanIterator end() { // NOLINT: match stl API
		return VectorScanIterator(format, data, count, count);
	}
	idx_t size() const {
		return count;
	}

private:
	UnifiedVectorFormat format;
	const T *data;
	idx_t count;
};

template <class T>
inline VectorIterator<T> Vector::Values() const {
	return VectorIterator<T>(*this);
}

template <class T>
inline VectorValidValueIterator<T> Vector::ValidValues() const {
	return VectorValidValueIterator<T>(*this);
}

inline VectorValidityIterator Vector::Validity() const {
	return VectorValidityIterator(*this);
}

} // namespace duckdb
