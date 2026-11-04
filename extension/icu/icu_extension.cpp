#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "include/icu-current.hpp"
#include "include/icu-dateadd.hpp"
#include "include/icu-datepart.hpp"
#include "include/icu-datesub.hpp"
#include "include/icu-datetrunc.hpp"
#include "include/icu-list-range.hpp"
#include "include/icu-makedate.hpp"
#include "include/icu-strptime.hpp"
#include "include/icu-table-range.hpp"
#include "include/icu-timebucket.hpp"
#include "include/icu-timezone.hpp"
#include "include/icu_extension.hpp"
#include "icu-helpers.hpp"
#include "collation_collator.hpp"
#include "calendar.hpp"
#include "timezone.hpp"

#include <cassert>

namespace duckdb {

//! The collation settings of the tagged collations that are registered by the extension
static collation::CollationSettings SettingsFromTag(const string &tag) {
	collation::CollationSettings settings;
	// "und-u-ks-level1-kc-true": compare at the primary level, but keep case differences
	if (tag == "und-u-ks-level1-kc-true") {
		settings.strength = collation::CollationStrength::PRIMARY;
		settings.case_level = true;
		return settings;
	}
	throw InvalidInputException("Unknown collation tag %s", tag);
}

struct IcuBindData : public FunctionData {
	collation::Collator collator;
	string language;
	string country;
	string tag;

	IcuBindData(string language_p, string country_p)
	    : collator(country_p.empty() ? language_p : language_p + "_" + country_p), language(std::move(language_p)),
	      country(std::move(country_p)) {
	}

	explicit IcuBindData(string tag_p) : collator(SettingsFromTag(tag_p)), tag(std::move(tag_p)) {
	}

	static duckdb::unique_ptr<FunctionData> CreateInstance(const string &language, const string &country,
	                                                       const string &tag) {
		//! give priority to tagged collation
		if (!tag.empty()) {
			return make_uniq<IcuBindData>(tag);
		} else {
			return make_uniq<IcuBindData>(language, country);
		}
	}

	duckdb::unique_ptr<FunctionData> Copy() const override {
		return CreateInstance(language, country, tag);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<IcuBindData>();
		return language == other.language && country == other.country && tag == other.tag;
	}

	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
	                      const BoundScalarFunction &function) {
		auto &bind_data = bind_data_p->Cast<IcuBindData>();
		serializer.WriteProperty(100, "language", bind_data.language);
		serializer.WriteProperty(101, "country", bind_data.country);
		serializer.WritePropertyWithDefault<string>(102, "tag", bind_data.tag);
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &function) {
		string language;
		string country;
		string tag;
		deserializer.ReadProperty(100, "language", language);
		deserializer.ReadProperty(101, "country", country);
		deserializer.ReadPropertyWithDefault<string>(102, "tag", tag);
		return CreateInstance(language, country, tag);
	}

	//! The prefix of the collation functions, and of the ones that are kept for backwards
	//! compatibility, which return the sort key in hexadecimal instead of as a blob
	static const string FUNCTION_PREFIX;
	static const string HEX_FUNCTION_PREFIX;

	static string EncodeFunctionName(const string &collation) {
		return FUNCTION_PREFIX + collation;
	}
	static string EncodeHexFunctionName(const string &collation) {
		return HEX_FUNCTION_PREFIX + collation;
	}
	static string DecodeFunctionName(const Identifier &fname) {
		auto &name = fname.GetIdentifierName();
		auto prefix = StringUtil::StartsWith(name, HEX_FUNCTION_PREFIX) ? HEX_FUNCTION_PREFIX : FUNCTION_PREFIX;
		return name.substr(prefix.size());
	}
};

const string IcuBindData::FUNCTION_PREFIX = "collate_";
const string IcuBindData::HEX_FUNCTION_PREFIX = "icu_collate_";

//! The two hexadecimal characters of every byte value
static const uint16_t &HexDigits(uint8_t byte) {
	static const auto HEX_PAIRS = []() {
		const char digits[] = "0123456789ABCDEF";
		array<uint16_t, 256> pairs {};
		for (idx_t value = 0; value < 256; value++) {
			auto low = static_cast<uint16_t>(digits[value % 16]);
			auto high = static_cast<uint16_t>(digits[value / 16]);
			// the pair is stored in the order it is written to memory
			pairs[value] = static_cast<uint16_t>(high | (low << 8));
		}
		return pairs;
	}();
	return HEX_PAIRS[byte];
}

//! Keeps the buffers the collator works in alive across the chunks of a scan, so that
//! generating sort keys does not allocate
struct CollatorLocalState : public FunctionLocalState {
	collation::CollationBuffer buffer;

	static unique_ptr<FunctionLocalState> Init(ExpressionState &, const BoundFunctionExpression &, FunctionData *) {
		return make_uniq<CollatorLocalState>();
	}
};

//! Writes the sort key of every string, either as a blob or in hexadecimal. The hexadecimal
//! form is only used by the functions that are kept for backwards compatibility.
template <bool HEX>
static void ICUCollateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<IcuBindData>();
	// the collator is immutable, it is shared between the threads that run the function
	auto &collator = info.collator;

	auto &buffer = ExecuteFunctionState::GetFunctionState(state)->Cast<CollatorLocalState>().buffer;
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t input) {
		// create a sort key from the string, the trailing null byte is not part of the result
		collator.GetSortKey(input.GetData(), input.GetSize(), buffer);
		auto &key = buffer.key;
		auto key_size = key.size() - 1;
		if (!HEX) {
			return StringVector::AddStringOrBlob(result, const_char_ptr_cast(key.data()), key_size);
		}
		auto str_result = StringVector::EmptyString(result, key_size * 2);
		auto str_data = str_result.GetDataWriteable();
		for (idx_t i = 0; i < key_size; i++) {
			D_ASSERT(key[i] != 0);
			auto digits = HexDigits(key[i]);
			memcpy(str_data + i * 2, &digits, sizeof(digits));
		}
		str_result.Finalize();
		return str_result;
	});
}

static duckdb::unique_ptr<FunctionData> ICUCollateBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();

	//! Return a tagged collator
	if (!bound_function.GetExtraInfo().empty()) {
		return make_uniq<IcuBindData>(bound_function.GetExtraInfo());
	}

	const auto collation = IcuBindData::DecodeFunctionName(bound_function.GetName());
	auto splits = StringUtil::Split(collation, "_");
	if (splits.size() == 1) {
		return make_uniq<IcuBindData>(splits[0], "");
	} else if (splits.size() == 2) {
		return make_uniq<IcuBindData>(splits[0], splits[1]);
	} else {
		throw InvalidInputException("Expected one or two splits");
	}
}

static duckdb::unique_ptr<FunctionData> ICUSortKeyBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();

	auto val = input.GetNonNullConstant(1).CastAs(context, LogicalType::VARCHAR);
	//! Verify tagged collation
	if (!bound_function.GetExtraInfo().empty()) {
		return make_uniq<IcuBindData>(bound_function.GetExtraInfo());
	}
	auto splits = StringUtil::Split(StringValue::Get(val), "_");
	if (splits.size() == 1) {
		return make_uniq<IcuBindData>(splits[0], "");
	} else if (splits.size() == 2) {
		return make_uniq<IcuBindData>(splits[0], splits[1]);
	} else {
		throw InvalidInputException("Expected one or two splits");
	}
}

//! The function a collation pushes into a query, it writes the sort key as a blob
static ScalarFunction GetCollateFunction(const string &collation, const string &tag) {
	string fname = IcuBindData::EncodeFunctionName(collation);
	ScalarFunction result(Identifier(fname), {}, LogicalType::BLOB, ICUCollateFunction<false>, ICUCollateBind);
	result.GetSignature().AddParameter("str", LogicalType::VARCHAR);
	//! collation tag is added into the Function extra info
	result.extra_info = tag;
	result.SetInitStateCallback(CollatorLocalState::Init);
	result.SetSerializeCallback(IcuBindData::Serialize);
	result.SetDeserializeCallback(IcuBindData::Deserialize);
	return result;
}

//! The function collations used before they wrote blobs, it is kept registered so that
//! queries and plans that call it directly keep working
static ScalarFunction GetICUCollateFunction(const string &collation, const string &tag) {
	string fname = IcuBindData::EncodeHexFunctionName(collation);
	ScalarFunction result(Identifier(fname), {}, LogicalType::VARCHAR, ICUCollateFunction<true>, ICUCollateBind);
	result.GetSignature().AddParameter("str", LogicalType::VARCHAR);
	result.extra_info = tag;
	result.SetInitStateCallback(CollatorLocalState::Init);
	result.SetSerializeCallback(IcuBindData::Serialize);
	result.SetDeserializeCallback(IcuBindData::Deserialize);
	return result;
}

unique_ptr<TimeZone> GetKnownTimeZone(const string &tz_str) {
	return TimeZone::TryCreate(tz_str);
}

unique_ptr<TimeZone> GetNormalizedTimeZone(string &tz_str) {
	auto tz = GetKnownTimeZone(tz_str);
	if (tz) {
		return tz;
	}

	//	Map UTC±NN00 to Etc/UTC±N
	do {
		if (tz_str.size() <= 4) {
			break;
		}
		if (tz_str.compare(0, 3, "UTC")) {
			break;
		}

		idx_t pos = 3;
		const auto utc = tz_str[pos++];
		// Invert the sign (UTC and Etc use opposite sign conventions)
		// https://en.wikipedia.org/wiki/Tz_database#Area
		auto sign = utc;
		if (utc == '+') {
			sign = '-';
		} else if (utc == '-') {
			sign = '+';
		} else {
			break;
		}

		// Collect remaining characters (digits and colons)
		string remainder;
		for (; pos < tz_str.size(); ++pos) {
			const auto ch = tz_str[pos];
			if (ch != ':' && !StringUtil::CharacterIsDigit(ch)) {
				break;
			}
			remainder += ch;
		}
		if (pos < tz_str.size()) {
			break;
		}

		// Step 1: Strip leading zeros
		idx_t start = 0;
		while (start < remainder.size() && remainder[start] == '0') {
			++start;
		}
		remainder = remainder.substr(start);

		// Step 2: Parse hours based on whether colon is present
		string hours_str;
		auto colon_idx = remainder.find(':');
		if (colon_idx != string::npos) {
			// Has colon: split by colon, part before colon is hours
			hours_str = remainder.substr(0, colon_idx);
		} else if (remainder.size() <= 2) {
			// 1-2 digits: entire string is hours
			hours_str = remainder;
		} else {
			// No colon, 3+ digits: HHMM format, last 2 are minutes, rest are hours
			hours_str = remainder.substr(0, remainder.size() - 2);
		}

		// Build the mapped timezone string
		string mapped = "Etc/GMT";
		if (hours_str.empty()) {
			// Zero offset
			mapped += "+0";
		} else {
			mapped += sign;
			mapped += hours_str;
		}
		// Final sanity check
		tz = GetKnownTimeZone(mapped);
		if (tz) {
			tz_str = mapped;
			return tz;
		}
	} while (false);

	return nullptr;
}

unique_ptr<TimeZone> GetTimeZoneInternal(string &tz_str, vector<string> &candidates) {
	auto tz = GetNormalizedTimeZone(tz_str);
	if (tz) {
		return tz;
	}

	// Try to be friendlier
	// Go through all the zone names and look for a case insensitive match
	// If we don't find one, make a suggestion
	for (const auto &candidate : TimeZone::GetAvailableIds()) {
		if (StringUtil::CIEquals(candidate, tz_str)) {
			// case insensitive match - return this timezone instead
			tz_str = candidate;
			return TimeZone::TryCreate(tz_str);
		}
		candidates.emplace_back(candidate);
	}
	return nullptr;
}

unique_ptr<TimeZone> ICUHelpers::GetTimeZone(string &tz_str, string *error_message) {
	vector<string> candidates;
	auto tz = GetTimeZoneInternal(tz_str, candidates);
	if (tz) {
		return tz;
	}
	string candidate_str =
	    StringUtil::CandidatesMessage(StringUtil::TopNJaroWinkler(candidates, tz_str), "Candidate time zones");
	if (error_message) {
		duckdb::stringstream ss;
		ss << "Unknown TimeZone '" << tz_str << "'!\n" << candidate_str;
		*error_message = ss.str();
		return nullptr;
	}
	throw NotImplementedException("Unknown TimeZone '%s'!\n%s", tz_str, candidate_str);
}

static void SetICUTimeZone(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("TimeZone setting cannot be NULL");
	}
	auto tz_str = StringValue::Get(parameter);
	ICUHelpers::GetTimeZone(tz_str);
	parameter = Value(tz_str);
}

struct ICUCalendarData : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static duckdb::unique_ptr<FunctionData> ICUCalendarBind(ClientContext &context, TableFunctionBindInput &input,
                                                        vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

static duckdb::unique_ptr<GlobalTableFunctionState> ICUCalendarInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<ICUCalendarData>();
}

static void ICUCalendarFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<ICUCalendarData>();
	const auto &types = Calendar::GetAvailableTypes();

	// name, VARCHAR
	auto &name_col = output.data[0];

	idx_t index = 0;
	while (index < STANDARD_VECTOR_SIZE && data.offset < types.size()) {
		name_col.Append(Value(types[data.offset++]));
		++index;
	}
}

static void SetICUCalendar(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("Calendar setting cannot be NULL");
	}
	const auto name = parameter.Value::GetValueUnsafe<string>();
	//	Try to be friendlier: look for a case insensitive match, and if we don't find one,
	//	make a suggestion
	for (const auto &candidate : Calendar::GetAvailableTypes()) {
		if (StringUtil::CIEquals(candidate, name)) {
			parameter = Value(candidate);
			return;
		}
	}

	string candidate_str = StringUtil::CandidatesMessage(
	    StringUtil::TopNJaroWinkler(Calendar::GetAvailableTypes(), name), "Candidate calendars");

	throw NotImplementedException("Unknown Calendar '%s'!\n%s", name, candidate_str);
}

static void LoadInternal(ExtensionLoader &loader) {
	// iterate over all the collations
	for (auto &collation : collation::Collator::GetCollations()) {
		CreateCollationInfo info(Identifier(collation), GetCollateFunction(collation, ""), false, false);
		loader.RegisterCollation(info);
		loader.RegisterFunction(GetICUCollateFunction(collation, ""));
	}

	/**
	 * This collation function is inpired on the Postgres "ignore_accents":
	 * See: https://www.postgresql.org/docs/current/collation.html
	 * CREATE COLLATION ignore_accents (provider = icu, locale = 'und-u-ks-level1-kc-true', deterministic = false);
	 *
	 * Also, according with the source file: postgres/src/backend/utils/adt/pg_locale.c.
	 * "und-u-kc-ks-level1" is converted to the equivalent ICU format locale ID,
	 * e.g. "und@colcaselevel=yes;colstrength=primary"
	 *
	 */
	CreateCollationInfo info("icu_noaccent", GetCollateFunction("noaccent", "und-u-ks-level1-kc-true"), false, false);
	loader.RegisterCollation(info);
	loader.RegisterFunction(GetICUCollateFunction("noaccent", "und-u-ks-level1-kc-true"));

	ScalarFunction sort_key("icu_sort_key", {}, LogicalType::VARCHAR, ICUCollateFunction<true>, ICUSortKeyBind);
	sort_key.GetSignature().AddParameter("str", LogicalType::VARCHAR).AddParameter("collator", LogicalType::VARCHAR);
	sort_key.SetInitStateCallback(CollatorLocalState::Init);
	loader.RegisterFunction(sort_key);

	// Time Zones
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	auto tz = TimeZone::TryCreateDefault();
	// If the host time zone is unknown, fall back to UTC
	string tz_string = tz ? tz->GetId() : "UTC";
	config.AddExtensionOption("TimeZone", "The current time zone", LogicalType::VARCHAR, Value(tz_string),
	                          SetICUTimeZone);

	RegisterICUCurrentFunctions(loader);
	RegisterICUDateAddFunctions(loader);
	RegisterICUDatePartFunctions(loader);
	RegisterICUDateSubFunctions(loader);
	RegisterICUDateTruncFunctions(loader);
	RegisterICUMakeDateFunctions(loader);
	RegisterICUTableRangeFunctions(loader);
	RegisterICUListRangeFunctions(loader);
	RegisterICUStrptimeFunctions(loader);
	RegisterICUTimeBucketFunctions(loader);
	RegisterICUTimeZoneFunctions(loader);

	// Calendars
	config.AddExtensionOption("Calendar", "The current calendar", LogicalType::VARCHAR, Value("gregorian"),
	                          SetICUCalendar);

	TableFunction cal_names("icu_calendar_names", {}, ICUCalendarFunction, ICUCalendarBind, ICUCalendarInit);
	loader.RegisterFunction(cal_names);
}

// LCOV_EXCL_START
void IcuExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string IcuExtension::Name() {
	return "icu";
}

std::string IcuExtension::Version() const {
#ifdef EXT_VERSION_ICU
	return EXT_VERSION_ICU;
#else
	return "";
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(icu, loader) { // NOLINT
	duckdb::LoadInternal(loader);
}
}
