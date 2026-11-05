// A minimal extension built against the V2 C API through the stable C++ API. It exists to exercise the V2 loader
// end-to-end: the entrypoint, the vtable handed out by get_api, and the context DuckDB lends for the duration of the
// load. It reads through the context, logs, and registers macro functions, which only an extension can register.

#include "duckdb_cpp_extension.hpp"

static constexpr const char *LOG_TYPE = "CppApiDemo";

using namespace duckdb::cxx;

// Registers a macro that must be refused, logging why.
template <class F>
static void RegisterRefusedMacro(Extension &extension, Context &context, const std::string &name, bool table,
                                 F &&configure) {
	auto &factory = context.GetFactory();
	auto macro = table ? MacroFunction::CreateTable(factory) : MacroFunction::CreateScalar(factory);
	macro.SetName(name);
	configure(macro);
	try {
		extension.Register(macro);
		context.Log(LogLevel::LOG_INFO, name + " registered", LOG_TYPE);
	} catch (const Exception &ex) {
		context.Log(LogLevel::LOG_INFO, name + " refused: " + ex.what(), LOG_TYPE);
	}
}

DUCKDB_CPP_EXTENSION_ENTRYPOINT(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();

	// Binding a type proves the context is live and has a transaction: this reaches into the catalog.
	const auto type = factory.ParseType("STRUCT(a INTEGER, b VARCHAR)");

	context.Log(LogLevel::LOG_INFO, "cpp_api_demo loaded, parsed " + type.ToText(), LOG_TYPE);

	// A scalar macro with a typed parameter, an untyped one, and one with a default value.
	auto scale = MacroFunction::CreateScalar(factory);
	scale.SetName("cpp_demo_scale")
	    .SetBody("(x + shift) * factor")
	    .WithSignature([&](FunctionSignature &sig) {
		    sig.AddParameter("x", factory.ParseType("INTEGER"));
		    sig.AddParameter("shift", factory.CreateType(LogicalTypeId::ANY));
		    sig.AddParameter("factor", factory.ParseType("INTEGER"), Value::Create(factory, int32_t {2}));
	    })
	    .WithDocs([](FunctionDocs &docs) { docs.SetDescription("Shifts x by shift, then scales it by factor"); });
	extension.Register(scale);

	// A table macro whose body is a query over its parameters.
	auto numbers = MacroFunction::CreateTable(factory);
	numbers.SetName("cpp_demo_numbers").SetBody("SELECT i * step AS n FROM range(count) t(i)");
	numbers.GetSignature()
	    .AddParameter("count", factory.ParseType("BIGINT"))
	    .AddParameter("step", factory.ParseType("BIGINT"), Value::Create(factory, int64_t {1}));
	extension.Register(numbers);

	// What a macro cannot be, each refused at registration.
	RegisterRefusedMacro(extension, context, "cpp_demo_variadic", false, [&](MacroFunction &macro) {
		macro.SetBody("1").GetSignature().AddArgs("rest", factory.CreateType(LogicalTypeId::ANY));
	});
	RegisterRefusedMacro(extension, context, "cpp_demo_named_only", false, [&](MacroFunction &macro) {
		macro.SetBody("x").GetSignature().AddParameter("x", factory.CreateType(LogicalTypeId::ANY),
		                                               FunctionParameterKind::NAMED_ONLY);
	});
	RegisterRefusedMacro(extension, context, "cpp_demo_typed_result", false, [&](MacroFunction &macro) {
		macro.SetBody("1").GetSignature().SetReturnType(factory.ParseType("INTEGER"));
	});
	RegisterRefusedMacro(extension, context, "cpp_demo_bad_body", false,
	                     [&](MacroFunction &macro) { macro.SetBody("1 +"); });
	RegisterRefusedMacro(extension, context, "cpp_demo_two_expressions", false,
	                     [&](MacroFunction &macro) { macro.SetBody("1, 2"); });
	RegisterRefusedMacro(extension, context, "cpp_demo_not_a_query", true,
	                     [&](MacroFunction &macro) { macro.SetBody("CREATE TABLE t (i INTEGER)"); });
	RegisterRefusedMacro(extension, context, "cpp_demo_no_body", false, [&](MacroFunction &) {});
	RegisterRefusedMacro(extension, context, "cpp_demo_scale", false,
	                     [&](MacroFunction &macro) { macro.SetBody("1"); });
}
