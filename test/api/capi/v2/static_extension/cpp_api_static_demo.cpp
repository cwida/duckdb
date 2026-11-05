// A V2 C API extension linked straight into the test binary. Statically linked extensions bind DuckDB's symbols at
// link time, so there is no vtable and get_api is never called -- but the entrypoint still receives a context, and it
// is DuckDB that has to supply one, since a static extension loads before any client connection exists.

#include "duckdb_cpp_extension.hpp"

namespace {

// The three data slots of a scalar function, one struct per slot: user data
// (set at registration), bind data (planted by bind), init data (planted by
// init). Exec reads all three.
struct Factor {
	int value;
};

// Bind data must be equality-comparable: the engine compares it when it compares expressions.
struct Offset {
	int value;
	bool operator==(const Offset &other) const {
		return value == other.value;
	}
};

void MaddBind(duckdb::cxx::ScalarFunction::BindInput &input) {
	const auto &factor = input.GetUserData<Factor>();
	input.SetBindData<Offset>(Offset {factor.value + 7});
}

void MaddInit(duckdb::cxx::ScalarFunction::InitInput &input) {
	input.SetInitData<int>(input.GetBindData<Offset>().value);
}

// out[i] = a[i] * factor + b[i] + offset
void MaddExec(duckdb::cxx::ScalarFunction::ExecInput &input) {
	const auto factor = input.GetUserData<Factor>().value;
	const auto offset = input.GetInitData<int>();
	const auto a = input.GetArg(0).GetView();
	const auto b = input.GetArg(1).GetView();
	auto result = input.GetResult();
	auto *out = static_cast<int32_t *>(result.GetDataMutable());
	const auto count = input.GetRowCount();
	for (duckdb::cxx::idx_t i = 0; i < count; i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] * factor + b.Data<int32_t>()[b.SelAt(i)] + offset;
	}
}

} // namespace

DUCKDB_CPP_EXTENSION_ENTRYPOINT(duckdb::cxx::Extension &extension, duckdb::cxx::Context &context) {
	const auto type = context.GetFactory().ParseType("DECIMAL(18, 3)");
	context.Log(duckdb::cxx::LogLevel::LOG_INFO, "cpp_api_static_demo loaded, parsed " + type.ToText(),
	            "CppApiStaticDemo");

	// Register a scalar function through the C++ wrapper, exercising every data slot.
	const auto integer = context.GetFactory().ParseType("INTEGER");
	auto function = duckdb::cxx::ScalarFunction::Create(context.GetFactory());
	function.SetName("cpp_demo_madd");
	function.GetSignature().AddParameter("a", integer).AddParameter("b", integer).SetReturnType(integer);
	function.SetUserData<Factor>(Factor {3});
	function.SetBindCallback(MaddBind);
	function.SetInitCallback(MaddInit);
	function.SetExecCallback(MaddExec);
	extension.Register(function);
}
