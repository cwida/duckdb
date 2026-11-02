#include "duckdb/common/types/variant.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/function/scalar/variant_path_function.hpp"
#include "yyjson.hpp"
#include "yyjson_memory.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

class VariantStringWriter {
public:
	explicit VariantStringWriter(ConvertedJSONHolder &json_holder, string &error) : error(error), holder(json_holder) {
	}

	void operator()(const optional<VariantNode> &node, VectorWriter<string_t> &string_writer) const {
		if (!node || node->IsNull() || node->GetTypeId() == VariantLogicalType::VARIANT_NULL) {
			string_writer.WriteNull();
			return;
		}

		const auto type_id = node->GetTypeId();
		if (type_id == VariantLogicalType::VARCHAR) {
			string_writer.WriteValue(node->GetString());
			return;
		}

		const auto json_value = VariantCasts::ConvertVariantToJSON(holder.GetDocument(), *node);
		if (!json_value) {
			throw SerializationException("Failed to convert VARIANT value to JSON object");
		}

		if (yyjson_mut_is_str(json_value)) {
			const auto str = yyjson_mut_get_str(json_value);
			const auto str_len = NumericCast<uint32_t>(yyjson_mut_get_len(json_value));
			string_writer.WriteValue(string_t(str, str_len));
			holder.Reset();
			return;
		}

		const auto serialized = holder.Serialize(json_value, error);
		if (!serialized) {
			throw SerializationException(error.c_str());
		}

		string_writer.WriteValue(*serialized);
		holder.Reset();
	}

private:
	string &error;
	ConvertedJSONHolder &holder;
};

static void VariantExtractStringFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	string error;
	ConvertedJSONHolder holder(BufferAllocator::Get(state.GetContext()));
	VariantPathFunction::Execute<string_t>(input, state, result, VariantStringWriter(holder, error));
}

ScalarFunctionSet VariantExtractStringFun::GetFunctions() {
	auto functions = VariantPathFunction::CreateFunctionSet("variant_extract_string", VariantExtractStringFunction,
	                                                        LogicalType::VARCHAR, false);
	functions.SetFallible();
	return functions;
}

} // namespace duckdb
