//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/cast/bound_cast_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {

struct ListBoundCastData : public BoundCastData {
	explicit ListBoundCastData(BoundCastInfo child_cast) : child_cast_info(std::move(child_cast)) {
	}

	BoundCastInfo child_cast_info;
	static unique_ptr<BoundCastData> BindListToListCast(BindCastInput &input, const LogicalType &source,
	                                                    const LogicalType &target);
	static unique_ptr<FunctionLocalState> InitListLocalState(CastLocalStateParameters &parameters);

public:
	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<ListBoundCastData>(child_cast_info.Copy());
	}
};

struct ArrayBoundCastData : public BoundCastData {
	explicit ArrayBoundCastData(BoundCastInfo child_cast) : child_cast_info(std::move(child_cast)) {
	}

	BoundCastInfo child_cast_info;

	static unique_ptr<BoundCastData> BindArrayToArrayCast(BindCastInput &input, const LogicalType &source,
	                                                      const LogicalType &target);
	static unique_ptr<FunctionLocalState> InitArrayLocalState(CastLocalStateParameters &parameters);

public:
	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<ArrayBoundCastData>(child_cast_info.Copy());
	}
};

struct ListCast {
	static bool ListToListCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
};

//! How the fields of a STRUCT -> STRUCT cast are paired - positionally for unnamed structs, by name otherwise
struct StructCastFieldMapping {
	enum class MatchKind : uint8_t { VALID, SIZE_MISMATCH, DUPLICATE_TARGET_NAME, NO_MATCHING_FIELD };

	MatchKind kind = MatchKind::VALID;
	//! The duplicate target field name, set for DUPLICATE_TARGET_NAME
	Identifier duplicate_name;
	//! Matched (source, target) field pairs, in source field order
	vector<idx_t> source_indexes;
	vector<idx_t> target_indexes;
	//! Target fields without a matching source field - the cast sets these to NULL
	vector<idx_t> target_null_indexes;
};

struct StructBoundCastData : public BoundCastData {
	StructBoundCastData(vector<BoundCastInfo> child_casts, LogicalType target_p, vector<idx_t> source_indexes_p,
	                    vector<idx_t> target_indexes_p, vector<idx_t> target_null_indexes_p)
	    : child_cast_info(std::move(child_casts)), target(std::move(target_p)),
	      source_indexes(std::move(source_indexes_p)), target_indexes(std::move(target_indexes_p)),
	      target_null_indexes(std::move(target_null_indexes_p)) {
		D_ASSERT(child_cast_info.size() == source_indexes.size());
		D_ASSERT(source_indexes.size() == target_indexes.size());
	}
	StructBoundCastData(vector<BoundCastInfo> child_casts, LogicalType target_p)
	    : child_cast_info(std::move(child_casts)), target(std::move(target_p)) {
		for (idx_t i = 0; i < child_cast_info.size(); i++) {
			source_indexes.push_back(i);
			target_indexes.push_back(i);
		}
	}

	vector<BoundCastInfo> child_cast_info;
	LogicalType target;
	vector<idx_t> source_indexes;
	vector<idx_t> target_indexes;
	vector<idx_t> target_null_indexes;

	//! Computes the field pairing of a STRUCT -> STRUCT cast without binding the child casts
	static StructCastFieldMapping GetFieldMapping(const LogicalType &source, const LogicalType &target);
	static unique_ptr<BoundCastData> BindStructToStructCast(BindCastInput &input, const LogicalType &source,
	                                                        const LogicalType &target);
	static unique_ptr<FunctionLocalState> InitStructCastLocalState(CastLocalStateParameters &parameters);
	//! The shared (positional/by-name) value cast - used by both the STRUCT and TUPLE cast switches
	static bool StructToStructCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);

public:
	unique_ptr<BoundCastData> Copy() const override {
		vector<BoundCastInfo> copy_info;
		for (auto &info : child_cast_info) {
			copy_info.push_back(info.Copy());
		}
		return make_uniq<StructBoundCastData>(std::move(copy_info), target, source_indexes, target_indexes,
		                                      target_null_indexes);
	}
};

struct StructCastLocalState : public FunctionLocalState {
public:
	vector<unique_ptr<FunctionLocalState>> local_states;
};

struct MapBoundCastData : public BoundCastData {
	MapBoundCastData(BoundCastInfo key_cast, BoundCastInfo value_cast)
	    : key_cast(std::move(key_cast)), value_cast(std::move(value_cast)) {
	}

	BoundCastInfo key_cast;
	BoundCastInfo value_cast;

	static unique_ptr<BoundCastData> BindMapToMapCast(BindCastInput &input, const LogicalType &source,
	                                                  const LogicalType &target);

public:
	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<MapBoundCastData>(key_cast.Copy(), value_cast.Copy());
	}
};

struct MapCastLocalState : public FunctionLocalState {
public:
	unique_ptr<FunctionLocalState> key_state;
	unique_ptr<FunctionLocalState> value_state;
};

struct StructToMapBoundCastData : public BoundCastData {
	StructToMapBoundCastData(BoundCastInfo key_cast, vector<BoundCastInfo> value_casts)
	    : key_cast(std::move(key_cast)), value_casts(std::move(value_casts)) {
	}

	BoundCastInfo key_cast;
	vector<BoundCastInfo> value_casts;

	static unique_ptr<BoundCastData> BindStructToMapCast(BindCastInput &input, const LogicalType &source,
	                                                     const LogicalType &target);
	static unique_ptr<FunctionLocalState> InitStructToMapCastLocalState(CastLocalStateParameters &parameters);

public:
	unique_ptr<BoundCastData> Copy() const override {
		vector<BoundCastInfo> copy_value_casts;
		for (auto &value_cast : value_casts) {
			copy_value_casts.push_back(value_cast.Copy());
		}
		return make_uniq<StructToMapBoundCastData>(key_cast.Copy(), std::move(copy_value_casts));
	}
};

struct StructToMapCastLocalState : public FunctionLocalState {
public:
	unique_ptr<FunctionLocalState> key_state;
	vector<unique_ptr<FunctionLocalState>> value_states;
};

struct StructToUnionCast {
public:
	static bool AllowImplicitCastFromStruct(const LogicalType &source, const LogicalType &target);
	static bool Cast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
	static unique_ptr<BoundCastData> BindData(BindCastInput &input, const LogicalType &source,
	                                          const LogicalType &target);
	static BoundCastInfo Bind(BindCastInput &input, const LogicalType &source, const LogicalType &target);
};

} // namespace duckdb
