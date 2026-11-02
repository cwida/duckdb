#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/storage/table/variant_column_data.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/struct_column_data.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

VariantColumnData::VariantColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                     LogicalType type_p, ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent) {
	D_ASSERT(type.InternalType() == PhysicalType::STRUCT);

	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
		// the sub column index, starting at 1 (0 is the validity mask)
		idx_t sub_column_index = 1;
		auto unshredded_type = VariantShredding::GetUnshreddedType();
		sub_columns.push_back(
		    ColumnData::CreateColumn(block_manager, info, sub_column_index++, unshredded_type, data_type, this));
	} else {
		// leave empty, gets populated by 'SetChildData'
		(void)validity;
		(void)sub_columns;
	}
}

bool FindShreddedColumnInternal(const ColumnData &shredded, reference<const BaseStatistics> &stats,
                                optional_ptr<const StorageIndex> path_iter, ColumnIndex &out, LogicalType &root_type) {
	if (!path_iter) {
		// end of the line
		if (!VariantShreddedStats::IsFullyShredded(stats.get())) {
			//! Child isn't fully shredded, can't use it
			return false;
		}
		//! We're done, we've found the field referenced by the path!
		if (shredded.type.id() == LogicalTypeId::STRUCT) {
			// if this is a struct - refer to the .typed_value field
			out.AddChildIndex(ColumnIndex(VariantColumnData::TYPED_VALUE_INDEX));
			stats = StructStats::GetChildStats(stats.get(), VariantColumnData::TYPED_VALUE_INDEX);
		}
		return true;
	}
	auto &path = *path_iter;
	D_ASSERT(!path.HasPrimaryIndex());
	auto &field_name = path.GetFieldName();
	if (shredded.type.id() != LogicalTypeId::STRUCT) {
		// we're looking for a sub-field but this is a primitive type - not a match
		return false;
	}

	D_ASSERT(shredded.type.id() == LogicalTypeId::STRUCT);
	auto &struct_col = shredded.Cast<StructColumnData>();
	auto &typed_value = struct_col.GetChildColumn(VariantColumnData::TYPED_VALUE_INDEX);
	auto &parent_stats = stats.get();

	stats = StructStats::GetChildStats(parent_stats, VariantColumnData::TYPED_VALUE_INDEX);
	if (typed_value.type.id() != LogicalTypeId::STRUCT) {
		//! Not shredded on an OBJECT, but we're looking for a specific OBJECT field
		return false;
	}
	if (!VariantShreddedStats::IsFullyShredded(parent_stats)) {
		//! Can't push down to the shredded data, stats are inconsistent
		return false;
	}
	//! shredded.typed_value
	out.AddChildIndex(ColumnIndex(VariantColumnData::TYPED_VALUE_INDEX));
	auto &typed_value_index = out.GetChildIndex(0);

	auto &object_children = StructType::GetChildTypes(typed_value.type);
	optional_idx opt_index;
	for (idx_t i = 0; i < object_children.size(); i++) {
		if (field_name == object_children[i].first.GetIdentifierName()) {
			opt_index = i;
			break;
		}
	}
	if (!opt_index.IsValid()) {
		//! OBJECT doesn't contain a field with this name
		return false;
	}

	auto child_index = opt_index.GetIndex();
	auto &typed_value_struct = typed_value.Cast<StructColumnData>();
	auto &object_field = typed_value_struct.GetChildColumn(child_index);
	stats = StructStats::GetChildStats(stats.get(), child_index);

	//! typed_value.<child_name>
	typed_value_index.AddChildIndex(ColumnIndex(child_index));
	auto &child_column = typed_value_index.GetChildIndex(0);

	// recurse
	optional_ptr<const StorageIndex> next_path;
	if (path.HasChildren()) {
		next_path = path.GetChildIndex(0);
	}
	if (!FindShreddedColumnInternal(object_field, stats, next_path, child_column, root_type)) {
		return false;
	}
	if (!next_path && path.HasType()) {
		root_type = path.GetType();
	}
	return true;
}

bool VariantColumnData::PushdownShreddedFieldExtract(const StorageIndex &variant_extract,
                                                     StorageIndex &out_struct_extract) const {
	D_ASSERT(IsShredded());
	auto &shredded = *sub_columns[1];
	auto &variant_stats = GetStatisticsRef();

	if (!VariantStats::IsShredded(variant_stats)) {
		//! FIXME: this happens when we Checkpoint but don't restart, the stats of the ColumnData aren't updated by
		//! Checkpoint The variant is shredded, but there are no stats / the stats are cluttered (?)
		return false;
	}

	//! shredded.typed_value
	ColumnIndex column_index(0);

	reference<const BaseStatistics> shredded_stats(VariantStats::GetShreddedStats(variant_stats));
	LogicalType root_type;
	if (!FindShreddedColumnInternal(shredded, shredded_stats, variant_extract, column_index, root_type)) {
		return false;
	}
	if (shredded_stats.get().GetType().IsNested()) {
		//! Can't push down an extract if the leaf we're extracting is not a primitive
		//! (Since the shredded representation for OBJECT/ARRAY is interleaved with 'untyped_value_index' fields)
		return false;
	}
	if (root_type.id() != LogicalTypeId::INVALID) {
		if (!VariantUtils::ShreddedCastMatchesVariantCast(shredded_stats.get().GetType(), root_type)) {
			//! The cast has to be applied to the VARIANT
			return false;
		}
		column_index.SetPushdownExtractType(shredded.type, root_type);
	}

	out_struct_extract = StorageIndex::FromColumnIndex(column_index);
	return true;
}

void VariantColumnData::CreateScanStates(ColumnScanState &state) {
	//! Re-initialize the scan state, since VARIANT can have a different shape for every RowGroup
	state.child_states.clear();

	state.child_states.emplace_back(state.parent);
	state.child_states[0].scan_options = state.scan_options;

	auto unshredded_type = VariantShredding::GetUnshreddedType();
	state.child_states.emplace_back(state.parent);
	state.child_states[1].Initialize(state.context, unshredded_type, state.scan_options);

	if (IsShredded()) {
		auto &shredded_column = sub_columns[1];
		state.child_states.emplace_back(state.parent);
		if (state.storage_index.IsPushdownExtract()) {
			StorageIndex struct_extract;
			if (PushdownShreddedFieldExtract(state.storage_index.GetChildIndex(0), struct_extract)) {
				//! Shredded field exists and is fully shredded,
				//! add the storage index to create a pushed-down 'struct_extract' to get the leaf
				state.child_states[2].Initialize(state.context, shredded_column->type, struct_extract,
				                                 state.scan_options);
				return;
			}
		}
		state.child_states[2].Initialize(state.context, shredded_column->type, state.scan_options);
	}
}

idx_t VariantColumnData::GetMaxEntry() {
	return sub_columns[0]->GetMaxEntry();
}

void VariantColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
	//! FIXME: does this also need CreateScanStates ??
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], rows);
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		sub_columns[i]->InitializePrefetch(prefetch_state, scan_state.child_states[i + 1], rows);
	}
}

void VariantColumnData::InitializeScan(ColumnScanState &state) {
	CreateScanStates(state);
	state.current = nullptr;

	// initialize the validity segment
	validity->InitializeScan(state.child_states[0]);

	// initialize the sub-columns
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		sub_columns[i]->InitializeScan(state.child_states[i + 1]);
	}
}

void VariantColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	CreateScanStates(state);
	state.current = nullptr;

	// initialize the validity segment
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);

	// initialize the sub-columns
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		sub_columns[i]->InitializeScanWithOffset(state.child_states[i + 1], row_idx);
	}
}

Vector VariantColumnData::CreateUnshreddingIntermediate(idx_t count) const {
	D_ASSERT(IsShredded());
	D_ASSERT(sub_columns.size() == 2);

	child_list_t<LogicalType> child_types;
	child_types.emplace_back("unshredded", sub_columns[0]->type);
	child_types.emplace_back("shredded", sub_columns[1]->type);
	auto intermediate_type = LogicalType::STRUCT(child_types);
	Vector intermediate(intermediate_type, count);
	return intermediate;
}

idx_t VariantColumnData::ScanWithCallback(
    ColumnScanState &state, Vector &result, idx_t target_count,
    const std::function<idx_t(ColumnData &column, ColumnScanState &child_state, Vector &target_vector, idx_t count)>
        &callback) const {
	if (state.storage_index.IsPushdownExtract()) {
		if (IsShredded() && state.child_states[2].storage_index.IsPushdownExtract()) {
			//! Then we can do the unshredding on the extracted data, rather than falling back to unshredding+extracting
			//! This invariant is ensured by CreateScanStates
			//! In the initialize we have verified that the field exists and the data is fully shredded (for this
			//! rowgroup) We have created a scan state that performs a 'struct_extract' in the shredded data, to extract
			//! the requested field.s
			auto res = callback(*sub_columns[1], state.child_states[2], result, target_count);
			if (result.GetType().id() == LogicalTypeId::LIST) {
				//! Shredded ARRAY Variant looks like:
				//! LIST(STRUCT(typed_value <child_type>, untyped_value_index UINTEGER))
				//! We need to transform this to:
				//! LIST(<child_type>)
				// FIXME: this is never called right now
				throw InternalException("this is never called right");
			}
			return res;
		}
		//! Fall back to unshredding
		Vector intermediate(LogicalType::VARIANT(), target_count);
		idx_t scan_count;
		if (IsShredded()) {
			auto unshredding_intermediate = CreateUnshreddingIntermediate(target_count);
			auto &child_vectors = StructVector::GetEntries(unshredding_intermediate);

			callback(*sub_columns[0], state.child_states[1], child_vectors[0], target_count);
			callback(*sub_columns[1], state.child_states[2], child_vectors[1], target_count);
			scan_count = callback(*validity, state.child_states[0], unshredding_intermediate, target_count);

			FlatVector::SetSize(unshredding_intermediate, count_t(scan_count));
			intermediate.Shred(unshredding_intermediate, scan_count);
		} else {
			scan_count = callback(*validity, state.child_states[0], intermediate, target_count);
			callback(*sub_columns[0], state.child_states[1], intermediate, target_count);
		}

		Vector extract_intermediate(LogicalType::VARIANT(), target_count);
		vector<VariantPathComponent> components;
		reference<const StorageIndex> path_iter(state.storage_index.GetChildIndex(0));

		while (true) {
			auto &current = path_iter.get();
			auto &field_name = current.GetFieldName();
			components.emplace_back(field_name);
			if (!current.HasChildren()) {
				break;
			}
			path_iter = current.GetChildIndex(0);
		}
		VariantUtils::VariantExtract(intermediate, components, extract_intermediate, target_count);

		if (state.expression_state) {
			auto &expression_state = *state.expression_state;
			auto &executor = expression_state.executor;

			auto &input = expression_state.input;
			auto &target = expression_state.target;
			input.Reset();
			target.Reset();
			input.data[0].Reference(extract_intermediate);
			executor.Execute(input, target);
			result.Reference(target.data[0]);
		} else {
			result.Reference(extract_intermediate);
		}
		return scan_count;
	} else {
		if (IsShredded()) {
			auto intermediate = CreateUnshreddingIntermediate(target_count);
			auto &child_vectors = StructVector::GetEntries(intermediate);

			callback(*sub_columns[0], state.child_states[1], child_vectors[0], target_count);
			callback(*sub_columns[1], state.child_states[2], child_vectors[1], target_count);
			auto scan_count = callback(*validity, state.child_states[0], intermediate, target_count);

			FlatVector::SetSize(intermediate, count_t(scan_count));
			result.Shred(intermediate, scan_count);
			return scan_count;
		}
		auto scan_count = callback(*validity, state.child_states[0], result, target_count);
		callback(*sub_columns[0], state.child_states[1], result, target_count);
		return scan_count;
	}
}

idx_t VariantColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                              idx_t target_count) {
	return ScanWithCallback(state, result, target_count,
	                        [&](ColumnData &col, ColumnScanState &child_state, Vector &target_vector, idx_t count) {
		                        return col.Scan(transaction, vector_index, child_state, target_vector, count);
	                        });
}

idx_t VariantColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	if (result_offset > 0) {
		throw InternalException("VariantColumnData::ScanCount not supported with result_offset > 0");
	}
	auto result_count = ScanWithCallback(
	    state, result, count, [&](ColumnData &col, ColumnScanState &child_state, Vector &target_vector, idx_t count) {
		    return col.ScanCount(child_state, target_vector, count, result_offset);
	    });
	result.Flatten();
	return result_count;
}

void VariantColumnData::Skip(ColumnScanState &state, idx_t count) {
	validity->Skip(state.child_states[0], count);

	// skip inside the sub-columns
	for (idx_t child_idx = 0; child_idx < sub_columns.size(); child_idx++) {
		sub_columns[child_idx]->Skip(state.child_states[child_idx + 1], count);
	}
}

void VariantColumnData::InitializeAppend(ColumnAppendState &state) {
	ColumnAppendState validity_append;
	validity->InitializeAppend(validity_append);
	state.child_appends.push_back(std::move(validity_append));

	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto &sub_column = sub_columns[i];
		ColumnAppendState child_append;
		sub_column->InitializeAppend(child_append);
		state.child_appends.push_back(std::move(child_append));
	}
}

namespace {

struct VariantShreddedAppendInput {
	ColumnData &unshredded;
	ColumnData &shredded;
	ColumnAppendState &unshredded_append_state;
	ColumnAppendState &shredded_append_state;
	BaseStatistics &unshredded_stats;
	BaseStatistics &shredded_stats;
};

} // namespace

void VariantColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
		Vector append_vector(Vector::Ref(vector));
		append_vector.Flatten();
		Append(state, append_vector, count);
		return;
	}

	// append the null values
	validity->Append(state.child_appends[0], vector, count);

	if (IsShredded()) {
		throw InternalException("Can't append to a shredded VariantColumnData");
	} else {
		for (idx_t i = 0; i < sub_columns.size(); i++) {
			sub_columns[i]->Append(state.child_appends[i + 1], vector, count);
		}
	}
	this->count += count;
}

void VariantColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);

	for (idx_t i = 0; i < sub_columns.size(); i++) {
		ColumnDataFinalizeAppendState child_finalize_state(finalize_state, LogicalTypeId::VARIANT);
		sub_columns[i]->FinalizeAppend(child_finalize_state, state.child_appends[i + 1]);
	}
	if (!IsShredded()) {
		for (auto &stats_ref : finalize_state.global_stats) {
			VariantStats::MarkAsNotShredded(stats_ref.get());
		}
	}
}

void VariantColumnData::RevertAppend(row_t new_count) {
	validity->RevertAppend(new_count);
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto &sub_column = sub_columns[i];
		sub_column->RevertAppend(new_count);
	}
	this->count = UnsafeNumericCast<idx_t>(new_count);
}

idx_t VariantColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	throw NotImplementedException("VARIANT Fetch");
}

void VariantColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                               Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw NotImplementedException("VARIANT Update is not supported.");
}

void VariantColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                     const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                     idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw NotImplementedException("VARIANT Update Column is not supported");
}

unique_ptr<BaseStatistics> VariantColumnData::GetUpdateStatistics() {
	return nullptr;
}

void VariantColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state,
                                  const StorageIndex &storage_index, const idx_t *offsets, const SelectionVector &sel,
                                  idx_t fetch_count, Vector &result, idx_t result_offset) {
	if (storage_index.IsPushdownExtract() && IsShredded()) {
		StorageIndex struct_extract;
		if (PushdownShreddedFieldExtract(storage_index.GetChildIndex(0), struct_extract)) {
			//! Shredded field exists and is fully shredded,
			//! add the storage index to create a pushed-down 'struct_extract' to get the leaf
			sub_columns[1]->FetchRows(transaction, state, struct_extract, offsets, sel, fetch_count, result,
			                          result_offset);
			return;
		}
	}

	for (idx_t idx = 0; idx < fetch_count; idx++) {
		const idx_t offset = offsets[sel.get_index(idx)];
		const idx_t result_idx = result_offset + idx;
		Vector variant_vec(LogicalType::VARIANT(), 1);
		validity->FetchRowsAtSegmentLevel(transaction, state, &offset, *FlatVector::IncrementalSelectionVector(),
		                                  /*count=*/1, variant_vec, 0);
		if (IsShredded()) {
			auto intermediate = CreateUnshreddingIntermediate(1);
			auto &child_vectors = StructVector::GetEntries(intermediate);
			// fetch the validity state
			// fetch the sub-column states
			StorageIndex empty(0);
			for (idx_t i = 0; i < sub_columns.size(); i++) {
				sub_columns[i]->FetchRows(transaction, state, empty, &offset, *FlatVector::IncrementalSelectionVector(),
				                          /*count=*/1, child_vectors[i], 0);
			}

			//! FIXME: adjust UnshredVariantData so we can write the value in place directly.
			Vector unshredded(variant_vec.GetType(), 1);
			VariantUtils::UnshredVariantData(intermediate, unshredded, 1);
			variant_vec.SetValue(0, unshredded.GetValue(0));
		} else {
			StorageIndex full_read(0);
			sub_columns[0]->FetchRows(transaction, state, full_read, &offset, *FlatVector::IncrementalSelectionVector(),
			                          /*count=*/1, variant_vec, 0);
		}

		if (!storage_index.IsPushdownExtract()) {
			//! No extract required
			D_ASSERT(result.GetType().id() == LogicalTypeId::VARIANT);
			result.SetValue(result_idx, variant_vec.GetValue(0));
			continue;
		}

		Vector extracted_variant(variant_vec.GetType(), 1);
		vector<VariantPathComponent> components;
		reference<const StorageIndex> path_iter(storage_index.GetChildIndex(0));

		while (true) {
			auto &current = path_iter.get();
			auto &field_name = current.GetFieldName();
			components.emplace_back(field_name);
			if (!current.HasChildren()) {
				break;
			}
			path_iter = current.GetChildIndex(0);
		}
		VariantUtils::VariantExtract(variant_vec, components, extracted_variant, 1);

		if (result.GetType().id() == LogicalTypeId::VARIANT) {
			//! No cast required
			result.SetValue(result_idx, extracted_variant.GetValue(0));
			continue;
		}

		//! Need to perform the cast here as well
		auto context = transaction.transaction->context.lock();
		auto fetched_row = extracted_variant.GetValue(0).CastAs(*context, result.GetType());
		result.SetValue(result_idx, fetched_row);
	}
}

void VariantColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	validity->VisitBlockIds(visitor);
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto &sub_column = sub_columns[i];
		sub_column->VisitBlockIds(visitor);
	}
}

void VariantColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("VariantColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

void VariantColumnData::SetChildData(vector<shared_ptr<ColumnData>> child_data) {
	if (!sub_columns.empty()) {
		throw InternalException("VariantColumnData::SetChildData cannot be used to overwrite existing data");
	}
	for (auto &col : child_data) {
		col->SetParent(this);
	}
	this->sub_columns = std::move(child_data);
}

struct VariantColumnCheckpointState : public ColumnCheckpointState {
	VariantColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                             PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
		global_stats = VariantStats::CreateEmpty(column_data.type).ToUnique();
	}

	vector<shared_ptr<ColumnData>> shredded_data;

	unique_ptr<ColumnCheckpointState> validity_state;
	vector<unique_ptr<ColumnCheckpointState>> child_states;

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<VariantColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                          original_column.column_index, original_column.type,
		                                          ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (!result_column) {
			result_column = CreateEmptyColumnData();
		}
		auto &column_data = result_column->Cast<VariantColumnData>();
		auto validity_child = validity_state->GetFinalResult();
		column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
		vector<shared_ptr<ColumnData>> child_data;
		for (idx_t i = 0; i < child_states.size(); i++) {
			child_data.push_back(child_states[i]->GetFinalResult());
		}
		column_data.SetChildData(std::move(child_data));
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		D_ASSERT(global_stats);
		global_stats->Merge(*validity_state->GetStatistics());
		VariantStats::SetUnshreddedStats(*global_stats, child_states[0]->GetStatistics());
		if (child_states.size() == 2) {
			VariantStats::SetShreddedStats(*global_stats, child_states[1]->GetStatistics());
		}
		return std::move(global_stats);
	}

	PersistentColumnData ToPersistentData() override {
		PersistentColumnData data(original_column.type);
		if (child_states.size() == 2) {
			//! Use the type of the column data we used to create the Checkpoint
			//! This will either be a pointer to shredded_data[1] if we decided to shred
			//! Or to the existing shredded column data if we didn't decide to reshred
			auto &shredded_state = child_states[1];
			data.extra_data = make_uniq<VariantPersistentColumnData>(shredded_state->original_column.type);
		}
		data.child_columns.push_back(validity_state->ToPersistentData());
		for (auto &child_state : child_states) {
			data.child_columns.push_back(child_state->ToPersistentData());
		}
		return data;
	}
};

unique_ptr<ColumnCheckpointState> VariantColumnData::CreateCheckpointState(const RowGroup &row_group,
                                                                           PartialBlockManager &partial_block_manager) {
	return make_uniq<VariantColumnCheckpointState>(row_group, *this, partial_block_manager);
}

static void AppendShredded(Vector &input, Vector &append_vector, idx_t count, VariantShreddedAppendInput &append_data) {
	D_ASSERT(append_vector.GetType().id() == LogicalTypeId::STRUCT);
	auto &child_vectors = StructVector::GetEntries(append_vector);
	D_ASSERT(child_vectors.size() == 2);

	//! Create the new column data for the shredded data
	VariantColumnData::ShredVariantData(input, append_vector, count);
	auto &unshredded_vector = child_vectors[0];
	auto &shredded_vector = child_vectors[1];

	auto &unshredded = append_data.unshredded;
	auto &shredded = append_data.shredded;

	auto &unshredded_append_state = append_data.unshredded_append_state;
	auto &shredded_append_state = append_data.shredded_append_state;

	unshredded.Append(unshredded_append_state, unshredded_vector, count);
	shredded.Append(shredded_append_state, shredded_vector, count);
}

static void FinalizeShreddedAppend(VariantShreddedAppendInput &append_data) {
	auto &unshredded_stats = append_data.unshredded_stats;
	auto &shredded_stats = append_data.shredded_stats;

	auto &unshredded = append_data.unshredded;
	auto &shredded = append_data.shredded;

	auto &unshredded_append_state = append_data.unshredded_append_state;
	auto &shredded_append_state = append_data.shredded_append_state;

	ColumnDataFinalizeAppendState unshredded_finalize_state(unshredded_stats);
	unshredded.FinalizeAppend(unshredded_finalize_state, unshredded_append_state);
	ColumnDataFinalizeAppendState shredded_finalize_state(shredded_stats);
	shredded.FinalizeAppend(shredded_finalize_state, shredded_append_state);
}

vector<shared_ptr<ColumnData>> VariantColumnData::WriteShreddedData(const RowGroup &row_group,
                                                                    const LogicalType &shredded_type,
                                                                    BaseStatistics &stats) {
	//! scan_chunk
	DataChunk scan_chunk;
	scan_chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARIANT()}, STANDARD_VECTOR_SIZE);
	auto &scan_vector = scan_chunk.data[0];

	//! append_chunk
	auto &child_types = StructType::GetChildTypes(shredded_type);

	DataChunk append_chunk;
	append_chunk.Initialize(Allocator::DefaultAllocator(), {shredded_type}, STANDARD_VECTOR_SIZE);
	auto &append_vector = append_chunk.data[0];

	//! Create the new column data for the shredded data
	D_ASSERT(child_types.size() == 2);
	auto &unshredded_type = child_types[0].second;
	auto &typed_value_type = child_types[1].second;

	vector<shared_ptr<ColumnData>> ret(2);
	ret[0] = ColumnData::CreateColumn(block_manager, info, 1, unshredded_type, GetDataType(), this);
	ret[1] = ColumnData::CreateColumn(block_manager, info, 2, typed_value_type, GetDataType(), this);
	auto &unshredded = ret[0];
	auto &shredded = ret[1];

	ColumnAppendState unshredded_append_state;
	unshredded->InitializeAppend(unshredded_append_state);

	ColumnAppendState shredded_append_state;
	shredded->InitializeAppend(shredded_append_state);

	ColumnScanState scan_state(nullptr);

	InitializeScan(scan_state);
	//! Scan + transform + append
	idx_t total_count = count.load();

	auto transformed_stats = VariantStats::CreateShredded(typed_value_type);
	auto &unshredded_stats = VariantStats::GetUnshreddedStats(transformed_stats);
	auto &shredded_stats = VariantStats::GetShreddedStats(transformed_stats);

	VariantShreddedAppendInput append_data {*unshredded,           *shredded,        unshredded_append_state,
	                                        shredded_append_state, unshredded_stats, shredded_stats};
	idx_t vector_index = 0;
	for (idx_t scanned = 0; scanned < total_count; scanned += STANDARD_VECTOR_SIZE) {
		scan_chunk.Reset();
		auto to_scan = MinValue(total_count - scanned, static_cast<idx_t>(STANDARD_VECTOR_SIZE));
		Scan(TransactionData::Unversioned(), vector_index++, scan_state, scan_vector, to_scan);
		append_chunk.Reset();

		AppendShredded(scan_vector, append_vector, to_scan, append_data);
	}
	FinalizeShreddedAppend(append_data);

	stats = std::move(transformed_stats);
	return ret;
}

LogicalType VariantColumnData::GetShreddedType() {
	VariantShreddingStats variant_stats;

	//! scan_chunk
	DataChunk scan_chunk;
	scan_chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARIANT()}, STANDARD_VECTOR_SIZE);
	auto &scan_vector = scan_chunk.data[0];

	ColumnScanState scan_state(nullptr);
	InitializeScan(scan_state);
	idx_t total_count = count.load();
	idx_t vector_index = 0;
	for (idx_t scanned = 0; scanned < total_count; scanned += STANDARD_VECTOR_SIZE) {
		scan_chunk.Reset();
		auto to_scan = MinValue(total_count - scanned, static_cast<idx_t>(STANDARD_VECTOR_SIZE));
		Scan(TransactionData::Unversioned(), vector_index++, scan_state, scan_vector, to_scan);
		variant_stats.Update(scan_vector, to_scan);
	}

	return variant_stats.GetShreddedType();
}

static bool EnableShredding(int64_t minimum_size, idx_t current_size) {
	if (minimum_size == -1) {
		//! Shredding is entirely disabled
		return false;
	}
	return current_size >= static_cast<idx_t>(minimum_size);
}

unique_ptr<ColumnCheckpointState> VariantColumnData::Checkpoint(const RowGroup &row_group,
                                                                ColumnCheckpointInfo &checkpoint_info,
                                                                const BaseStatistics &old_stats) {
	auto &partial_block_manager = checkpoint_info.GetPartialBlockManager();
	auto checkpoint_state = make_uniq<VariantColumnCheckpointState>(row_group, *this, partial_block_manager);
	checkpoint_state->validity_state = validity->Checkpoint(row_group, checkpoint_info, old_stats);

	auto &table_info = row_group.GetTableInfo();
	auto &db = table_info.GetDB();
	auto &config = DBConfig::Get(db);

	bool should_shred = true;
	if (!HasAnyChanges()) {
		should_shred = false;
	}
	if (!EnableShredding(Settings::Get<VariantMinimumShreddingSizeSetting>(config), count.load())) {
		should_shred = false;
	}

	LogicalType shredded_type;
	if (should_shred) {
		if (config.options.force_variant_shredding.id() != LogicalTypeId::INVALID) {
			shredded_type = config.options.force_variant_shredding;
		} else {
			shredded_type = GetShreddedType();
		}
		D_ASSERT(shredded_type.id() == LogicalTypeId::STRUCT);
		auto &type_entries = StructType::GetChildTypes(shredded_type);
		if (type_entries.size() != 2) {
			//! We couldn't determine a shredding type from the data
			should_shred = false;
		}
	}

	if (!should_shred) {
		checkpoint_state->child_states.push_back(
		    sub_columns[0]->Checkpoint(row_group, checkpoint_info, VariantStats::GetUnshreddedStats(old_stats)));
		if (sub_columns.size() > 1) {
			checkpoint_state->child_states.push_back(
			    sub_columns[1]->Checkpoint(row_group, checkpoint_info, VariantStats::GetShreddedStats(old_stats)));
		}
		return std::move(checkpoint_state);
	}

	//! STRUCT(unshredded VARIANT, shredded <...>)
	BaseStatistics column_stats = BaseStatistics::CreateEmpty(shredded_type);
	checkpoint_state->shredded_data = WriteShreddedData(row_group, shredded_type, column_stats);
	D_ASSERT(checkpoint_state->shredded_data.size() == 2);
	auto &unshredded = checkpoint_state->shredded_data[0];
	auto &shredded = checkpoint_state->shredded_data[1];

	//! Now checkpoint the shredded data
	checkpoint_state->child_states.push_back(
	    unshredded->Checkpoint(row_group, checkpoint_info, VariantStats::GetUnshreddedStats(column_stats)));
	checkpoint_state->child_states.push_back(
	    shredded->Checkpoint(row_group, checkpoint_info, VariantStats::GetShreddedStats(column_stats)));

	return std::move(checkpoint_state);
}

bool VariantColumnData::IsPersistent() {
	if (!validity->IsPersistent()) {
		return false;
	}
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto &sub_column = sub_columns[i];
		if (!sub_column->IsPersistent()) {
			return false;
		}
	}
	return true;
}

bool VariantColumnData::HasAnyChanges() const {
	if (validity->HasAnyChanges()) {
		return true;
	}
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto &sub_column = sub_columns[i];
		if (sub_column->HasAnyChanges()) {
			return true;
		}
	}
	return false;
}

PersistentColumnData VariantColumnData::Serialize() {
	PersistentColumnData persistent_data(type);
	if (IsShredded()) {
		// Set the extra data to indicate that this is shredded data
		persistent_data.extra_data = make_uniq<VariantPersistentColumnData>(sub_columns[1]->type);
	}
	persistent_data.child_columns.push_back(validity->Serialize());
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto &sub_column = sub_columns[i];
		persistent_data.child_columns.push_back(sub_column->Serialize());
	}
	return persistent_data;
}

void VariantColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	validity->InitializeColumn(column_data.child_columns[0], target_stats);

	if (column_data.child_columns.size() == 3) {
		//! This means the VARIANT is shredded
		auto &unshredded_stats = VariantStats::GetUnshreddedStats(target_stats);
		sub_columns[0]->InitializeColumn(column_data.child_columns[1], unshredded_stats);

		// TODO:
		D_ASSERT(column_data.extra_data);
		auto &variant_extra_data = column_data.extra_data->Cast<VariantPersistentColumnData>();
		auto &shredded_type = variant_extra_data.logical_type;
		if (!IsShredded()) {
			VariantStats::SetShreddedStats(target_stats, BaseStatistics::CreateEmpty(shredded_type));
			sub_columns.push_back(ColumnData::CreateColumn(block_manager, info, 2, shredded_type, GetDataType(), this));
		}
		auto &shredded_stats = VariantStats::GetShreddedStats(target_stats);
		sub_columns[1]->InitializeColumn(column_data.child_columns[2], shredded_stats);
	} else {
		auto &unshredded_stats = VariantStats::GetUnshreddedStats(target_stats);
		sub_columns[0]->InitializeColumn(column_data.child_columns[1], unshredded_stats);
	}
	this->count = validity->count.load();
}

void VariantColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                             vector<ColumnSegmentInfo> &result,
                                             const ColumnSegmentInfoScanOptions &options) {
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		col_path.back() = i + 1;
		sub_columns[i]->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	}
}

void VariantColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	ColumnData::Verify(parent);
	validity->Verify(parent);
	for (idx_t i = 0; i < sub_columns.size(); i++) {
		auto &sub_column = sub_columns[i];
		sub_column->Verify(parent);
	}
#endif
}

} // namespace duckdb
