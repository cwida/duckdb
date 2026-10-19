#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"

#include "duckdb/main/settings.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/types/type_map.hpp"
#include "duckdb/function/cast_rules.hpp"
#include "duckdb/planner/collation_binding.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

BindCastInput::BindCastInput(CastFunctionSet &function_set, optional_ptr<BindCastInfo> info,
                             optional_ptr<ClientContext> context)
    : function_set(function_set), info(info), context(context) {
}

//! Orders the stabilities from the most to the least stable
static idx_t StabilityRank(FunctionStability stability) {
	switch (stability) {
	case FunctionStability::CONSISTENT:
		return 0;
	case FunctionStability::CONSISTENT_WITHIN_QUERY:
		return 1;
	case FunctionStability::VOLATILE:
		return 2;
	}
	throw InternalException("Unsupported FunctionStability");
}

//! A cast that applies other casts is as stable as the least stable of them
static FunctionStability LeastStable(FunctionStability left, FunctionStability right) {
	return StabilityRank(left) >= StabilityRank(right) ? left : right;
}

BoundCastInfo BindCastInput::GetCastFunction(const LogicalType &source, const LogicalType &target) {
	GetCastFunctionInput input(context);
	input.query_location = query_location;
	auto result = function_set.GetCastFunction(source, target, input);
	stability = LeastStable(stability, result.GetStability());
	return result;
}

BindCastFunction::BindCastFunction(bind_cast_function_t function_p, unique_ptr<BindCastInfo> info_p)
    : function(function_p), info(std::move(info_p)) {
}

CastFunctionSet::CastFunctionSet()
    : default_bind_function(DefaultCasts::GetDefaultCastFunction), combine_rules(DefaultCombineTypesRules()),
      map_info(nullptr) {
}

CastFunctionSet::CastFunctionSet(DBConfig &config_p) : CastFunctionSet() {
	this->config = &config_p;
}

CastFunctionSet &CastFunctionSet::Get(ClientContext &context) {
	return DBConfig::GetConfig(context).GetCastFunctions();
}

const CastFunctionSet &CastFunctionSet::Get(const ClientContext &context) {
	return DBConfig::GetConfig(context).GetCastFunctions();
}

CollationBinding &CollationBinding::Get(ClientContext &context) {
	return DBConfig::GetConfig(context).GetCollationBinding();
}

CastFunctionSet &CastFunctionSet::Get(DatabaseInstance &db) {
	return DBConfig::GetConfig(db).GetCastFunctions();
}

CollationBinding &CollationBinding::Get(DatabaseInstance &db) {
	return DBConfig::GetConfig(db).GetCollationBinding();
}

BoundCastInfo CastFunctionSet::GetCastFunction(const LogicalType &source, const LogicalType &target,
                                               GetCastFunctionInput &get_input) {
	if (source == target) {
		BoundCastInfo result(DefaultCasts::NopCast);
		result.SetStatisticsCallback(CastStatistics::Propagate);
		return result;
	}
	if (registration_probe) {
		// VARIANT selects additional casts from runtime values.
		registered_cast_found |=
		    source.id() == LogicalTypeId::VARIANT || registration_probe->HasRegisteredCast(source, target);
	}
	auto bind_cast = [&](BindCastFunction &bind_function) {
		BindCastInput input(*this, bind_function.info.get(), get_input.context);
		input.query_location = get_input.query_location;
		auto result = bind_function.function(input, source, target);
		result.SetStability(LeastStable(result.GetStability(), input.stability));
		return result;
	};

	// Registered casts can have different semantics, so only their own statistics callbacks are valid.
	for (auto bind_function = registered_bind_functions.rbegin(); bind_function != registered_bind_functions.rend();
	     bind_function++) {
		auto result = bind_cast(*bind_function);
		if (result.HasFunction()) {
			return result;
		}
	}

	// Built-in casts inherit generic propagation unless they provide a more specific callback.
	auto result = bind_cast(default_bind_function);
	if (result.HasFunction()) {
		if (!result.HasStatisticsCallback()) {
			result.SetStatisticsCallback(CastStatistics::Propagate);
		}
		return result;
	}
	// no cast found: return the default null cast
	return DefaultCasts::TryVectorNullCast;
}

struct MapCastNode {
	MapCastNode(BoundCastInfo info, int64_t implicit_cast_cost)
	    : cast_info(std::move(info)), bind_function(nullptr), implicit_cast_cost(implicit_cast_cost) {
	}
	MapCastNode(bind_cast_function_t func, int64_t implicit_cast_cost)
	    : cast_info(nullptr), bind_function(func), implicit_cast_cost(implicit_cast_cost) {
	}

	BoundCastInfo cast_info;
	bind_cast_function_t bind_function;
	int64_t implicit_cast_cost;
};

template <class MAP>
static auto RelaxedTypeMatch(MAP &map, const LogicalType &type) -> decltype(map.find(type)) {
	D_ASSERT(map.find(type) == map.end()); // we shouldn't be here
	switch (type.id()) {
	case LogicalTypeId::LIST:
		return map.find(LogicalType::LIST(LogicalType::ANY));
	case LogicalTypeId::STRUCT:
		return map.find(LogicalType::STRUCT({{"any", LogicalType::ANY}}));
	case LogicalTypeId::TUPLE:
		return map.find(LogicalType::TUPLE({LogicalType::ANY}));
	case LogicalTypeId::MAP:
		for (auto it = map.begin(); it != map.end(); it++) {
			const auto &entry_type = it->first;
			if (entry_type.id() != LogicalTypeId::MAP) {
				continue;
			}
			auto &entry_key_type = MapType::KeyType(entry_type);
			auto &entry_val_type = MapType::ValueType(entry_type);
			if ((entry_key_type == LogicalType::ANY || entry_key_type == MapType::KeyType(type)) &&
			    (entry_val_type == LogicalType::ANY || entry_val_type == MapType::ValueType(type))) {
				return it;
			}
		}
		return map.end();
	case LogicalTypeId::UNION:
		return map.find(LogicalType::UNION({{"any", LogicalType::ANY}}));
	case LogicalTypeId::ARRAY:
		return map.find(LogicalType::ARRAY(LogicalType::ANY, optional_idx()));
	case LogicalTypeId::DECIMAL:
		return map.find(LogicalTypeId::DECIMAL);
	case LogicalTypeId::ENUM:
		return map.find(LogicalTypeId::ENUM);
	case LogicalTypeId::GEOMETRY: {
		// a GEOMETRY with a CRS matches the casts of the GEOMETRY without a CRS
		auto entry = map.find(LogicalType::GEOMETRY());
		if (entry != map.end()) {
			return entry;
		}
		return map.find(LogicalType::ANY);
	}
	default:
		return map.find(LogicalType::ANY);
	}
}

struct MapCastInfo : public BindCastInfo {
public:
	optional_ptr<const MapCastNode> GetEntry(const LogicalType &source, const LogicalType &target) const {
		auto source_type_id_entry = casts.find(source.id());
		if (source_type_id_entry == casts.end()) {
			source_type_id_entry = casts.find(LogicalTypeId::ANY);
			if (source_type_id_entry == casts.end()) {
				return nullptr;
			}
		}

		auto &source_type_entries = source_type_id_entry->second;
		auto source_type_entry = source_type_entries.find(source);
		if (source_type_entry == source_type_entries.end()) {
			source_type_entry = RelaxedTypeMatch(source_type_entries, source);
			if (source_type_entry == source_type_entries.end()) {
				return nullptr;
			}
		}

		auto &target_type_id_entries = source_type_entry->second;
		auto target_type_id_entry = target_type_id_entries.find(target.id());
		if (target_type_id_entry == target_type_id_entries.end()) {
			target_type_id_entry = target_type_id_entries.find(LogicalTypeId::ANY);
			if (target_type_id_entry == target_type_id_entries.end()) {
				return nullptr;
			}
		}

		auto &target_type_entries = target_type_id_entry->second;
		auto target_type_entry = target_type_entries.find(target);
		if (target_type_entry == target_type_entries.end()) {
			target_type_entry = RelaxedTypeMatch(target_type_entries, target);
			if (target_type_entry == target_type_entries.end()) {
				return nullptr;
			}
		}

		return &target_type_entry->second;
	}

	void AddEntry(const LogicalType &source, const LogicalType &target, MapCastNode node) {
		casts[source.id()][source][target.id()].insert(make_pair(target, std::move(node)));
	}

private:
	type_id_map_t<type_map_t<type_id_map_t<type_map_t<MapCastNode>>>> casts;
};

bool CastFunctionSet::HasRegisteredCast(const LogicalType &source, const LogicalType &target) const {
	return map_info && map_info->GetEntry(source, target);
}

bool CastFunctionSet::CanOverrideDefaultCast(const LogicalType &source, const LogicalType &target) const {
	if (!map_info || source == target) {
		return false;
	}
	if (HasRegisteredCast(source, target)) {
		return true;
	}
	CastFunctionSet probe;
	probe.registration_probe = this;
	GetCastFunctionInput input;
	try {
		probe.GetCastFunction(source, target, input);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERNAL || Exception::InvalidatesDatabase(error.Type())) {
			throw;
		}
		// the context-free probe can fail where the real resolution succeeds (e.g. union member
		// selection through registered implicit casts) - an unresolved probe cannot rule out overrides
		return true;
	}
	return probe.registered_cast_found;
}

int64_t CastFunctionSet::ImplicitCastCost(optional_ptr<ClientContext> context, const LogicalType &source,
                                          const LogicalType &target) {
	if (registration_probe && registration_probe->HasRegisteredCast(source, target)) {
		registered_cast_found = true;
	}
	// check if a cast has been registered
	if (map_info) {
		auto entry = map_info->GetEntry(source, target);
		if (entry) {
			return entry->implicit_cast_cost;
		}
	}
	// if not, fallback to the default implicit cast rules
	auto score = CastRules::ImplicitCast(source, target);
	if (score < 0 && source.id() != LogicalTypeId::BLOB && target.id() == LogicalTypeId::VARCHAR) {
		bool old_implicit_casting = false;
		if (context) {
			old_implicit_casting = Settings::Get<OldImplicitCastingSetting>(*context);
		} else if (config) {
			old_implicit_casting = Settings::Get<OldImplicitCastingSetting>(*config);
		}
		if (old_implicit_casting) {
			// very high cost to avoid choosing this cast if any other option is available
			// (it should be more costly than casting to TEMPLATE if that is available)
			score = 10000000000;
		}
	}
	return score;
}

int64_t CastFunctionSet::ImplicitCastCost(ClientContext &context, const LogicalType &source,
                                          const LogicalType &target) {
	return CastFunctionSet::Get(context).ImplicitCastCost(&context, source, target);
}

int64_t CastFunctionSet::ImplicitCastCost(DatabaseInstance &db, const LogicalType &source, const LogicalType &target) {
	return CastFunctionSet::Get(db).ImplicitCastCost(nullptr, source, target);
}

static BoundCastInfo MapCastFunction(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	D_ASSERT(input.info);
	auto &map_info = input.info->Cast<MapCastInfo>();
	auto entry = map_info.GetEntry(source, target);
	if (entry) {
		if (entry->bind_function) {
			return entry->bind_function(input, source, target);
		}
		return entry->cast_info.Copy();
	}
	return nullptr;
}

void CastFunctionSet::RegisterCastFunction(const LogicalType &source, const LogicalType &target, BoundCastInfo function,
                                           int64_t implicit_cast_cost) {
	RegisterCastFunction(source, target, MapCastNode(std::move(function), implicit_cast_cost));
}

void CastFunctionSet::RegisterCastFunction(const LogicalType &source, const LogicalType &target,
                                           bind_cast_function_t bind_function, int64_t implicit_cast_cost) {
	RegisterCastFunction(source, target, MapCastNode(bind_function, implicit_cast_cost));
}

void CastFunctionSet::RegisterCastFunction(const LogicalType &source, const LogicalType &target, MapCastNode node) {
	if (!map_info) {
		// create the cast map and the cast map function
		auto info = make_uniq<MapCastInfo>();
		map_info = info.get();
		registered_bind_functions.emplace_back(MapCastFunction, std::move(info));
	}
	map_info->AddEntry(source, target, std::move(node));
}

void CastFunctionSet::RegisterCombineTypesRule(CombineTypesRule rule) {
	combine_rules.insert(combine_rules.begin(), rule); // newest extension first, ahead of built-ins
}

bool CastFunctionSet::TryCombineTypes(const vector<CombineTypesRule> &rules, LogicalTypeResolver &resolver,
                                      const LogicalType &left, const LogicalType &right, LogicalType &result,
                                      bool &success) {
	// first matching rule wins
	for (auto &rule : rules) {
		if (rule.matches(left, right)) {
			success = rule.function(resolver, left, right, result);
			return true;
		}
	}
	return false;
}

bool CastFunctionSet::TryCombineTypes(LogicalTypeResolver &resolver, const LogicalType &left, const LogicalType &right,
                                      LogicalType &result, bool &success) {
	return TryCombineTypes(combine_rules, resolver, left, right, result, success);
}

} // namespace duckdb
