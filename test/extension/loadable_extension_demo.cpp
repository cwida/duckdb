#include "duckdb.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

using namespace duckdb; // NOLINT

//===--------------------------------------------------------------------===//
// Scalar function
//===--------------------------------------------------------------------===//
static inline void TestAliasHello(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value("Hello Alias!"), count_t(args.size()));
}

static inline void AddPointFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &left_vector = args.data[0];
	auto &right_vector = args.data[1];
	const auto count = args.size();
	auto left_vector_type = left_vector.GetVectorType();
	auto right_vector_type = right_vector.GetVectorType();

	args.Flatten();

	UnifiedVectorFormat lhs_data;
	UnifiedVectorFormat rhs_data;
	left_vector.ToUnifiedFormat(lhs_data);
	right_vector.ToUnifiedFormat(rhs_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &child_entries = StructVector::GetEntries(result);
	auto &left_child_entries = StructVector::GetEntries(left_vector);
	auto &right_child_entries = StructVector::GetEntries(right_vector);
	for (idx_t base_idx = 0; base_idx < count; base_idx++) {
		auto lhs_list_index = lhs_data.sel->get_index(base_idx);
		auto rhs_list_index = rhs_data.sel->get_index(base_idx);
		if (!lhs_data.validity.RowIsValid(lhs_list_index) || !rhs_data.validity.RowIsValid(rhs_list_index)) {
			FlatVector::SetNull(result, base_idx, true);
			continue;
		}
		for (size_t col = 0; col < child_entries.size(); ++col) {
			auto &child_entry = child_entries[col];
			auto &left_child_entry = left_child_entries[col];
			auto &right_child_entry = right_child_entries[col];
			auto pdata = FlatVector::GetDataMutable<int32_t>(child_entry);
			auto left_pdata = FlatVector::GetData<int32_t>(left_child_entry);
			auto right_pdata = FlatVector::GetData<int32_t>(right_child_entry);
			pdata[base_idx] = left_pdata[lhs_list_index] + right_pdata[rhs_list_index];
		}
	}
	if (left_vector_type == VectorType::CONSTANT_VECTOR && right_vector_type == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
	result.Verify();
}

static inline void SubPointFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &left_vector = args.data[0];
	auto &right_vector = args.data[1];
	const auto count = args.size();
	auto left_vector_type = left_vector.GetVectorType();
	auto right_vector_type = right_vector.GetVectorType();

	args.Flatten();
	UnifiedVectorFormat lhs_data;
	UnifiedVectorFormat rhs_data;
	left_vector.ToUnifiedFormat(lhs_data);
	right_vector.ToUnifiedFormat(rhs_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &child_entries = StructVector::GetEntries(result);
	auto &left_child_entries = StructVector::GetEntries(left_vector);
	auto &right_child_entries = StructVector::GetEntries(right_vector);
	for (idx_t base_idx = 0; base_idx < count; base_idx++) {
		auto lhs_list_index = lhs_data.sel->get_index(base_idx);
		auto rhs_list_index = rhs_data.sel->get_index(base_idx);
		if (!lhs_data.validity.RowIsValid(lhs_list_index) || !rhs_data.validity.RowIsValid(rhs_list_index)) {
			FlatVector::SetNull(result, base_idx, true);
			continue;
		}
		for (size_t col = 0; col < child_entries.size(); ++col) {
			auto &child_entry = child_entries[col];
			auto &left_child_entry = left_child_entries[col];
			auto &right_child_entry = right_child_entries[col];
			auto pdata = FlatVector::GetDataMutable<int32_t>(child_entry);
			auto left_pdata = FlatVector::GetData<int32_t>(left_child_entry);
			auto right_pdata = FlatVector::GetData<int32_t>(right_child_entry);
			pdata[base_idx] = left_pdata[lhs_list_index] - right_pdata[rhs_list_index];
		}
	}
	if (left_vector_type == VectorType::CONSTANT_VECTOR && right_vector_type == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
	result.Verify();
}

//===--------------------------------------------------------------------===//
// Quack Table Function
//===--------------------------------------------------------------------===//
class QuackFunction : public TableFunction {
public:
	QuackFunction() {
		name = "quack";
		GetSignature().AddParameter(LogicalType::BIGINT);
		bind = QuackBind;
		init_global = QuackInit;
		function = QuackFunc;
		get_bind_info = QuackGetBindInfo;
	}

	struct QuackBindData : public TableFunctionData {
		explicit QuackBindData(idx_t number_of_quacks) : number_of_quacks(number_of_quacks) {
		}

		idx_t number_of_quacks;
		//! The definitions of the returned columns, exposed through get_bind_info
		ColumnList columns;

		duckdb::unique_ptr<FunctionData> Copy() const override {
			auto result = make_uniq<QuackBindData>(number_of_quacks);
			result->columns = columns.Copy();
			return std::move(result);
		}
		bool Equals(const FunctionData &other) const override {
			return number_of_quacks == other.Cast<QuackBindData>().number_of_quacks;
		}
	};

	struct QuackGlobalData : public GlobalTableFunctionState {
		QuackGlobalData() : offset(0) {
		}

		idx_t offset;
	};

	static duckdb::unique_ptr<FunctionData> QuackBind(ClientContext &context, TableFunctionBindInput &input,
	                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
		names.emplace_back("quack");
		return_types.emplace_back(LogicalType::VARCHAR);
		auto result = make_uniq<QuackBindData>(BigIntValue::Get(input.inputs[0]));
		ColumnDefinition quack_column("quack", LogicalType::VARCHAR);
		quack_column.SetComment(Value("A single duck utterance"));
		InsertionOrderPreservingMap<string> tags;
		tags["ext:name"] = "loadable_extension_demo";
		tags["ext:column_type"] = "sound";
		quack_column.SetTags(std::move(tags));
		result->columns.AddColumn(std::move(quack_column));
		return std::move(result);
	}

	static BindInfo QuackGetBindInfo(TableFunctionGetBindInfoInput &input) {
		BindInfo info(ScanType::EXTERNAL);
		info.columns = &input.bind_data->Cast<QuackBindData>().columns;
		return info;
	}

	static duckdb::unique_ptr<GlobalTableFunctionState> QuackInit(ClientContext &context,
	                                                              TableFunctionInitInput &input) {
		return make_uniq<QuackGlobalData>();
	}

	static void QuackFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
		auto &bind_data = data_p.bind_data->Cast<QuackBindData>();
		auto &data = data_p.global_state->Cast<QuackGlobalData>();
		if (bind_data.number_of_quacks == 999) {
			throw InvalidInputException("quack scan must not execute");
		}
		if (data.offset >= bind_data.number_of_quacks) {
			// finished returning values
			return;
		}
		// start returning values
		// either fill up the chunk or return all the remaining columns
		idx_t count = 0;
		auto &quack_col = output.data[0];
		while (data.offset < bind_data.number_of_quacks && count < STANDARD_VECTOR_SIZE) {
			quack_col.Append(Value("QUACK"));
			data.offset++;
			count++;
		}
		output.SetChildCardinality(count);
	}
};

//===--------------------------------------------------------------------===//
// DuckWeed Window Function
//===--------------------------------------------------------------------===//
// Simple "fill down" function.
class DuckWeedFunction : public WindowFunction {
public:
	using CollectionPtr = optional_ptr<WindowCollection>;

	using GlobalState = WindowExecutorGlobalState;

	class LocalState : public WindowExecutorLocalState {
	public:
		LocalState(ExecutionContext &context, const WindowExecutorGlobalState &gstate)
		    : WindowExecutorLocalState(context, gstate) {
		}

		//! The valid values in the collection
		optional_ptr<ValidityMask> validity;
		//! The state used for reading the collection
		unique_ptr<WindowCursor> cursor;
		//! The current partition
		idx_t partition_idx = DConstants::INVALID_INDEX;
		//! The current filler row
		idx_t filler_row = DConstants::INVALID_INDEX;
	};

	DuckWeedFunction()
	    : WindowFunction("duckweed", {LogicalType::ANY}, LogicalType::ANY, ExpressionType::WINDOW_FUNCTION, Bind,
	                     GetBounds, GetSharing, GetGlobal, GetLocal, nullptr, Finalizer, GetData) {
		//	Not implemented
		SetCanOrderBy(false);
		//	We are filling in NULLs
		SetCanIgnoreNulls(false);

		SetCanStreamCallback(CanStream);
		SetStreamingStateCallback(GetStreamingState);
		SetStreamingDataCallback(StreamData);
	}

	//! Binding APIs
	static unique_ptr<FunctionData> Bind(BindWindowFunctionInput &input) {
		auto &function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		function.SetReturnType(arguments[0]->GetReturnType());
		return nullptr;
	}

	//! Blocking APIs
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
		//	Fill in from the start of the partition
		required.insert(PARTITION_BEGIN);
	}
	static void GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared) {
		//	Build the argument into a shared collection, including the NULLs (so we can find them quickly.)
		const auto &wexpr = executor.wexpr;
		auto &child_idx = executor.child_idx;
		child_idx.emplace_back(shared.RegisterCollection(wexpr.GetChildren()[0], true));
	}

	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask) {
		return make_uniq<GlobalState>(client, executor, payload_count, partition_mask, order_mask);
	}
	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
		return make_uniq<LocalState>(context, gstate.Cast<GlobalState>());
	}
	static void Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink) {
		auto &gvstate = sink.global_state.Cast<GlobalState>();
		auto &executor = gvstate.executor;
		const auto value_idx = executor.child_idx[0];

		auto &lvstate = sink.local_state.Cast<LocalState>();
		lvstate.validity = &collection->validities[value_idx];
		lvstate.cursor = make_uniq<WindowCursor>(*collection, value_idx);
	}
	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink) {
		auto &lvstate = sink.local_state.Cast<LocalState>();
		auto &cursor = *lvstate.cursor;
		const auto count = eval_chunk.size();
		auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
		for (idx_t i = 0; i < count; ++i, ++row_idx) {
			if (partition_begin[i] != lvstate.partition_idx) {
				lvstate.partition_idx = partition_begin[i];
				lvstate.filler_row = row_idx;
				// Jumped to a  different partition, so scan back to realign
				if (row_idx != lvstate.partition_idx) {
					idx_t delta = 1;
					lvstate.filler_row =
					    WindowBoundariesState::FindPrevStart(*lvstate.validity, lvstate.partition_idx, row_idx, delta);
				}
			}
			if (!cursor.CellIsNull(0, row_idx)) {
				lvstate.filler_row = row_idx;
			}
			cursor.CopyCell(0, lvstate.filler_row, result, i);
		}
	}

	//! Streaming APIs
	class StreamingState : public WindowExecutorStreamingState {
	public:
		StreamingState(ClientContext &client, DataChunk &input, const BoundWindowExpression &wexpr)
		    : wexpr(wexpr), filler(Value(wexpr.GetChildren()[0]->GetReturnType()), count_t(STANDARD_VECTOR_SIZE)),
		      executor(client), arg(wexpr.GetChildren()[0]->GetReturnType()) {
			executor.AddExpression(*wexpr.GetChildren()[0]);
		}
		//! The window expression
		const BoundWindowExpression &wexpr;
		//! A constant vector holding the repeated value. Starts NULL.
		Vector filler;
		//! A reusable executor for evaluating the argument
		ExpressionExecutor executor;
		//! A reusable output for the argument
		Vector arg;
	};

	static bool CanStream(ClientContext &client, const BoundWindowExpression &wexpr, idx_t max_delta) {
		//	We use the default framing.
		return true;
	}
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<StreamingState>(client, input, wexpr);
	}
	static void StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	                       Vector &result, WindowExecutorStreamingState &state) {
		auto &sstate = state.Cast<StreamingState>();
		auto &filler = sstate.filler;
		auto &arg = sstate.arg;
		const auto count = input.size();

		//	Evaluate the argument
		sstate.executor.ExecuteExpression(input, arg);
		UnifiedVectorFormat unified;
		arg.ToUnifiedFormat(unified);
		const auto &validity = unified.validity;
		for (idx_t i = 0; i < count; ++i) {
			const auto idx = unified.sel->get_index(i);
			if (validity.RowIsValid(idx)) {
				filler.Reference(arg.GetValue(i), count_t(count));
			}
			result.SetValue(i, filler.GetValue(0));
		}
	}
};

//===--------------------------------------------------------------------===//
// Parser extension
//===--------------------------------------------------------------------===//
struct QuackExtensionData : public ParserExtensionParseData {
	explicit QuackExtensionData(idx_t number_of_quacks) : number_of_quacks(number_of_quacks) {
	}

	idx_t number_of_quacks;

	duckdb::unique_ptr<ParserExtensionParseData> Copy() const override {
		return make_uniq<QuackExtensionData>(number_of_quacks);
	}

	string ToString() const override {
		vector<string> quacks;
		for (idx_t i = 0; i < number_of_quacks; i++) {
			quacks.push_back("QUACK");
		}
		return StringUtil::Join(quacks, " ");
	}
};

class QuackExtension : public ParserExtension {
public:
	QuackExtension() {
		parse_function = QuackParseFunction;
		plan_function = QuackPlanFunction;
		parser_override = QuackParser;
	}

	static ParserExtensionParseResult QuackParseFunction(ParserExtensionInfo *info, const vector<SimpleToken> &tokens) {
		// Claim a maximal run of consecutive "quack" identifier tokens (case-insensitive) at the
		// start of the view. PEG happily parses one or two identifiers (as `SELECT x AS y`), but
		// three or more in a row without separators is the syntactic hole that invokes this
		// parse_function; we then greedily consume every leading "quack".
		idx_t quacks = 0;
		while (quacks < tokens.size() && StringUtil::CIEquals(tokens[quacks].text, "quack")) {
			quacks++;
		}
		if (quacks == 0) {
			// Not our input — let the next extension or the original PEG error surface.
			return ParserExtensionParseResult();
		}
		// To be a proper TopLevelStatement (`Statement? (';'+ / EndOfInput)`), the quack run must
		// be followed by ';' or end-of-input. The tokenizer always appends an END_OF_INPUT
		// sentinel, so tokens[quacks] is valid here. If a real token (e.g. SELECT) follows without
		// a separator, decline so the original PEG syntax error surfaces.
		const auto next_type = tokens[quacks].type;
		if (next_type != TokenType::TERMINATOR && next_type != TokenType::END_OF_INPUT &&
		    next_type != TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			return ParserExtensionParseResult();
		}
		// The QUACK row count is the number of quack words.
		auto result = ParserExtensionParseResult(make_uniq<QuackExtensionData>(quacks));
		// Consume the terminator token too — whether it's ';' or the end-of-input sentinel — so the
		// QUACK statement owns its terminator, just like a real TopLevelStatement.
		result.consumed_tokens = NumericCast<int64_t>(quacks + 1);
		return result;
	}

	static ParserExtensionPlanResult QuackPlanFunction(ParserExtensionInfo *info, ClientContext &context,
	                                                   duckdb::unique_ptr<ParserExtensionParseData> parse_data) {
		auto &quack_data = parse_data->Cast<QuackExtensionData>();

		ParserExtensionPlanResult result;
		result.function = QuackFunction();
		result.parameters.push_back(Value::BIGINT(UnsafeNumericCast<int64_t>(quack_data.number_of_quacks)));
		result.requires_valid_transaction = false;
		result.return_type = StatementReturnType::QUERY_RESULT;
		return result;
	}

	static ParserOverrideResult QuackParser(ParserExtensionInfo *info, const string &query, ParserOptions &options) {
		vector<string> queries = StringUtil::Split(query, ";");
		vector<unique_ptr<SQLStatement>> statements;
		for (const auto &query_input : queries) {
			if (StringUtil::CIEquals(query_input, "override")) {
				auto select_node = make_uniq<SelectNode>();
				select_node->select_list.push_back(ConstantExpression::String("The DuckDB parser has been overridden"));
				select_node->from_table = make_uniq<EmptyTableRef>();
				auto select_statement = make_uniq<SelectStatement>();
				select_statement->node = std::move(select_node);
				statements.push_back(std::move(select_statement));
			}
			if (StringUtil::CIEquals(query_input, "overri")) {
				auto exception = ParserException("Parser overridden, query equaled \"overri\" but not \"override\"");
				return ParserOverrideResult(exception);
			}
		}
		if (statements.empty()) {
			// Return DISPLAY_ORIGINAL_ERROR so postgres parser + parse_function extensions can handle the query.
			return ParserOverrideResult();
		}
		return ParserOverrideResult(std::move(statements));
	}
};

//===--------------------------------------------------------------------===//
// Planner extension - adds an extra constant column to every query
//===--------------------------------------------------------------------===//
class AddColumnExtension : public PlannerExtension {
public:
	AddColumnExtension() {
		post_bind_function = AddColumnPostBind;
	}

	static void AddColumnPostBind(PlannerExtensionInput &input, BoundStatement &statement) {
		// Check if extension is enabled
		Value enabled;
		if (!input.context.TryGetCurrentSetting("add_column_enabled", enabled) || !enabled.GetValue<bool>()) {
			return;
		}

		// Only modify statements that return query results (SELECT, INSERT/UPDATE/DELETE RETURNING, etc.)
		auto &properties = input.binder.GetStatementProperties();
		if (properties.return_type != StatementReturnType::QUERY_RESULT) {
			return;
		}

		// Get the column bindings from the existing plan
		auto column_bindings = statement.plan->GetColumnBindings();

		// Get a new table index for the projection
		auto table_index = input.binder.GenerateTableIndex();

		// Create references to all existing columns using BoundColumnRefExpression
		vector<unique_ptr<Expression>> projections;
		for (idx_t i = 0; i < column_bindings.size(); i++) {
			projections.push_back(make_uniq<BoundColumnRefExpression>(statement.types[i], column_bindings[i]));
		}

		// Add a constant column
		projections.push_back(make_uniq<BoundConstantExpression>(Value("quack")));

		// Create a projection operator wrapping the existing plan
		auto projection = make_uniq<LogicalProjection>(table_index, std::move(projections));
		projection->children.push_back(std::move(statement.plan));
		projection->ResolveOperatorTypes();

		// Update the statement with the new plan, names, and types
		statement.plan = std::move(projection);
		statement.names.push_back("extra_column");
		statement.types.push_back(LogicalType::VARCHAR);
	}
};

static set<string> test_loaded_extension_list; // NOLINT

class QuackLoadExtension : public ExtensionCallback {
	void OnExtensionLoaded(DatabaseInstance &db, const string &name) override {
		test_loaded_extension_list.insert(name);
	}
};

static inline void LoadedExtensionsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	string result_str;
	for (auto &ext : test_loaded_extension_list) {
		if (!result_str.empty()) {
			result_str += ", ";
		}
		result_str += ext;
	}
	result.Reference(Value(result_str), count_t(args.size()));
}
//===--------------------------------------------------------------------===//
// Bounded type
//===--------------------------------------------------------------------===//

struct BoundedType {
	static LogicalType Bind(BindLogicalTypeInput &input) {
		return Get(input.modifiers[0].GetValue().GetValue<int32_t>());
	}

	static TypeConstructorSet Constructors() {
		auto signature = TypeConstructor::Signature();
		signature.AddParameter("bound", LogicalType::INTEGER);

		TypeConstructorSet result;
		result.AddFunction(TypeConstructor(std::move(signature), Bind));
		return result;
	}

	static LogicalType Get(int32_t max_val) {
		auto info = make_uniq<ExtensionTypeInfo>();
		info->modifiers.emplace_back(Value::INTEGER(max_val));
		return LogicalType(LogicalTypeId::INTEGER).WithAlias("BOUNDED").WithExtensionInfo(std::move(info));
	}

	static LogicalType GetDefault() {
		return LogicalType(LogicalTypeId::INTEGER).WithAlias("BOUNDED");
	}

	static int32_t GetMaxValue(const LogicalType &type) {
		if (!type.HasExtensionInfo()) {
			throw InvalidInputException("BOUNDED type must have a max value");
		}
		auto &mods = type.GetExtensionInfo()->modifiers;
		if (mods[0].value.IsNull()) {
			throw InvalidInputException("BOUNDED type must have a max value");
		}
		return mods[0].value.GetValue<int32_t>();
	}
};

static void BoundedMaxFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value::INTEGER(BoundedType::GetMaxValue(args.data[0].GetType())), count_t(args.size()));
}

static unique_ptr<FunctionData> BoundedMaxBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments[0]->GetReturnType() == BoundedType::GetDefault()) {
		bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
	} else {
		throw BinderException("bounded_max expects a BOUNDED type");
	}
	return nullptr;
}

static void BoundedAddFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &left_vector = args.data[0];
	auto &right_vector = args.data[1];
	BinaryExecutor::Execute<int32_t, int32_t, int32_t>(left_vector, right_vector, result,
	                                                   [&](int32_t left, int32_t right) { return left + right; });
}

static unique_ptr<FunctionData> BoundedAddBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (BoundedType::GetDefault() == arguments[0]->GetReturnType() &&
	    BoundedType::GetDefault() == arguments[1]->GetReturnType()) {
		auto left_max_val = BoundedType::GetMaxValue(arguments[0]->GetReturnType());
		auto right_max_val = BoundedType::GetMaxValue(arguments[1]->GetReturnType());

		auto new_max_val = left_max_val + right_max_val;
		bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
		bound_function.GetArguments()[1] = arguments[1]->GetReturnType();
		bound_function.SetReturnType(BoundedType::Get(new_max_val));
	} else {
		throw BinderException("bounded_add expects two BOUNDED types");
	}
	return nullptr;
}

struct BoundedFunctionData : public FunctionData {
	int32_t max_val;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<BoundedFunctionData>();
		copy->max_val = max_val;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<BoundedFunctionData>();
		return max_val == other.max_val;
	}
};

static unique_ptr<FunctionData> BoundedInvertBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments[0]->GetReturnType() == BoundedType::GetDefault()) {
		bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
		bound_function.SetReturnType(arguments[0]->GetReturnType());
	} else {
		throw BinderException("bounded_invert expects a BOUNDED type");
	}
	auto result = make_uniq<BoundedFunctionData>();
	result->max_val = BoundedType::GetMaxValue(bound_function.GetReturnType());
	return std::move(result);
}

static void BoundedInvertFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &source_vector = args.data[0];
	auto result_type = result.GetType();
	auto output_max_val = BoundedType::GetMaxValue(result_type);

	UnaryExecutor::Execute<int32_t, int32_t>(source_vector, result,
	                                         [&](int32_t input) { return std::min(-input, output_max_val); });
}

static void BoundedEvenFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &source_vector = args.data[0];
	UnaryExecutor::Execute<int32_t, bool>(source_vector, result, [&](int32_t input) { return input % 2 == 0; });
}

static void BoundedToAsciiFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &source_vector = args.data[0];
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<int32_t, string_t>(source_vector, result, [&](int32_t input) {
		if (input < 0) {
			throw NotImplementedException("Negative values not supported");
		}
		string s;
		s.push_back(static_cast<char>(input));
		return heap.AddString(s);
	});
}

static bool BoundedToBoundedCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto input_max_val = BoundedType::GetMaxValue(source.GetType());
	auto output_max_val = BoundedType::GetMaxValue(result.GetType());

	if (input_max_val <= output_max_val) {
		result.Reinterpret(source);
		return true;
	} else {
		throw ConversionException(source.GetType(), result.GetType());
	}
}

static bool IntToBoundedCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &ty = result.GetType();
	auto output_max_val = BoundedType::GetMaxValue(ty);
	UnaryExecutor::Execute<int32_t, int32_t>(source, result, count, [&](int32_t input) {
		if (input > output_max_val) {
			throw ConversionException(StringUtil::Format("Value %s exceeds max value of bounded type (%s)",
			                                             to_string(input), to_string(output_max_val)));
		}
		return input;
	});
	return true;
}

//===--------------------------------------------------------------------===//
//  MINMAX type
//===--------------------------------------------------------------------===//
// This is like the BOUNDED type, except it has a custom bind_modifiers function
// to verify that the range is valid

struct MinMaxType {
	static LogicalType Bind(BindLogicalTypeInput &input) {
		const auto min_val = input.modifiers[0].GetValue().GetValue<int32_t>();
		const auto max_val = input.modifiers[1].GetValue().GetValue<int32_t>();

		if (min_val >= max_val) {
			throw BinderException("MINMAX type min value must be less than max value");
		}

		auto info = make_uniq<ExtensionTypeInfo>();
		info->modifiers.emplace_back(Value::INTEGER(min_val));
		info->modifiers.emplace_back(Value::INTEGER(max_val));
		return LogicalType(LogicalTypeId::INTEGER).WithAlias("MINMAX").WithExtensionInfo(std::move(info));
	}

	static TypeConstructorSet Constructors() {
		auto signature = TypeConstructor::Signature();
		signature.AddParameter("min", LogicalType::INTEGER);
		signature.AddParameter("max", LogicalType::INTEGER);

		TypeConstructorSet result;
		result.AddFunction(TypeConstructor(std::move(signature), Bind));
		return result;
	}

	static int32_t GetMinValue(const LogicalType &type) {
		D_ASSERT(type.HasExtensionInfo());
		auto &mods = type.GetExtensionInfo()->modifiers;
		return mods[0].value.GetValue<int32_t>();
	}

	static int32_t GetMaxValue(const LogicalType &type) {
		D_ASSERT(type.HasExtensionInfo());
		auto &mods = type.GetExtensionInfo()->modifiers;
		return mods[1].value.GetValue<int32_t>();
	}

	static LogicalType Get(int32_t min_val, int32_t max_val) {
		auto info = make_uniq<ExtensionTypeInfo>();
		info->modifiers.emplace_back(Value::INTEGER(min_val));
		info->modifiers.emplace_back(Value::INTEGER(max_val));
		return LogicalType(LogicalTypeId::INTEGER).WithAlias("MINMAX").WithExtensionInfo(std::move(info));
	}

	static LogicalType GetDefault() {
		return LogicalType(LogicalTypeId::INTEGER).WithAlias("MINMAX");
	}
};

static bool IntToMinMaxCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &ty = result.GetType();
	auto min_val = MinMaxType::GetMinValue(ty);
	auto max_val = MinMaxType::GetMaxValue(ty);
	UnaryExecutor::Execute<int32_t, int32_t>(source, result, count, [&](int32_t input) {
		if (input < min_val || input > max_val) {
			throw ConversionException(StringUtil::Format("Value %s is outside of range [%s,%s]", to_string(input),
			                                             to_string(min_val), to_string(max_val)));
		}
		return input;
	});
	return true;
}

static void MinMaxRangeFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &ty = args.data[0].GetType();
	auto min_val = MinMaxType::GetMinValue(ty);
	auto max_val = MinMaxType::GetMaxValue(ty);
	result.Reference(Value::INTEGER(max_val - min_val), count_t(args.size()));
}

//===--------------------------------------------------------------------===//
// Row ID Filter — extensible table filter demo
//
// This demo shows how an optimizer extension can inject an ExpressionFilter
// into a table scan's filter set. The filter wraps a ScalarFunction that
// checks each row's ROW_ID against a sorted allow-list.
//
//
// Three callbacks are involved:
//   1. RowIdFilterFunction  — execution: stateful linear scan over allow-list
//   2. RowIdFilterPropagate — row-group pruning via min/max statistics
//   3. RowIdFilterInit      — per-thread state initialization
//===--------------------------------------------------------------------===//

// The bind callback is unused for most extensible table filters
static unique_ptr<FunctionData> RowIdFilterBind(BindScalarFunctionInput &input) {
	throw InternalException("rowid_filter: bind should never be called");
}

struct RowIdFilterBindData : public FunctionData {
	vector<int64_t> allowed_ids;
	unordered_set<int64_t> allowed_set;

	explicit RowIdFilterBindData(vector<int64_t> ids) : allowed_ids(std::move(ids)) {
		allowed_set.insert(allowed_ids.begin(), allowed_ids.end());
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RowIdFilterBindData>(allowed_ids);
	}
	bool Equals(const FunctionData &other_p) const override {
		return allowed_ids == other_p.Cast<RowIdFilterBindData>().allowed_ids;
	}
};

// Per-thread local state for the filter function.
// In this demo we use a simple hash-set lookup, so no per-thread state is needed.
// In practice, if row IDs arrive in non-decreasing order (e.g. sequential scan),
// a cursor-based linear scan over a sorted allowed list would be more efficient — O(n) total.
struct RowIdFilterState : public FunctionLocalState {};

static unique_ptr<FunctionLocalState> RowIdFilterInit(ExpressionState &, const BoundFunctionExpression &,
                                                      FunctionData *) {
	return make_uniq<RowIdFilterState>();
}

static void RowIdFilterFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<RowIdFilterBindData>();
	auto &allowed = bind_data.allowed_set;

	auto &input_vec = args.data[0];
	idx_t count = args.size();

	UnifiedVectorFormat vdata;
	input_vec.ToUnifiedFormat(vdata);
	auto row_ids = UnifiedVectorFormat::GetData<int64_t>(vdata);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetDataMutable<bool>(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			out[i] = false;
			continue;
		}
		out[i] = allowed.count(row_ids[idx]) > 0;
	}
}

static FilterPropagateResult RowIdFilterPropagate(const FunctionStatisticsPruneInput &input) {
	auto &allowed = input.bind_data->Cast<RowIdFilterBindData>().allowed_ids;
	auto column_stats = input.ChildStats(0);
	if (!column_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &stats = *column_stats;

	if (!NumericStats::HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto min_val = NumericStats::GetMin<int64_t>(stats);
	auto max_val = NumericStats::GetMax<int64_t>(stats);

	auto it = std::lower_bound(allowed.begin(), allowed.end(), min_val);
	if (it != allowed.end() && *it <= max_val) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

class RowIdOptimizerExtension : public OptimizerExtension {
public:
	RowIdOptimizerExtension() {
		optimize_function = RowIdOptimizeFunction;
	}

	static void RowIdOptimizeFunction(OptimizerExtensionInput &input, duckdb::unique_ptr<LogicalOperator> &plan) {
		for (auto &child : plan->children) {
			RowIdOptimizeFunction(input, child);
		}
		if (plan->type != LogicalOperatorType::LOGICAL_GET) {
			return;
		}
		auto &get = plan->Cast<LogicalGet>();
		auto table = get.GetTable();
		if (!table || table->name != "rowid_test_table") {
			return;
		}

		// Build the scalar function
		ScalarFunction func("rowid_filter", {LogicalType::BIGINT}, LogicalType::BOOLEAN, RowIdFilterFunction,
		                    RowIdFilterBind);
		func.SetInitStateCallback(RowIdFilterInit);
		func.SetFilterPruneCallback(RowIdFilterPropagate);

		// Construct the bound expression (column index 0: the filter chunk contains only the filtered column)
		vector<unique_ptr<Expression>> children;
		children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0));

		BoundScalarFunction bound_func(func);
		auto expr = make_uniq<BoundFunctionExpression>(std::move(bound_func), std::move(children),
		                                               make_uniq<RowIdFilterBindData>(vector<int64_t> {3, 4, 5, 7, 9}));

		// Ensure ROW_ID is in the scan's column list
		auto row_id_index = get.TryGetProjectionIndex(COLUMN_IDENTIFIER_ROW_ID);
		if (!row_id_index.IsValid()) {
			if (get.projection_ids.empty()) {
				for (idx_t i = 0; i < get.GetColumnIds().size(); i++) {
					get.projection_ids.emplace_back(i);
				}
			}
			row_id_index = get.AddColumnId(COLUMN_IDENTIFIER_ROW_ID);
		}

		// Push the filter on the ROW_ID column
		get.table_filters.PushFilter(row_id_index, make_uniq<ExpressionFilter>(std::move(expr)));
	}
};

//===--------------------------------------------------------------------===//
// Test extension function with named arguments, overloads and default values
//===--------------------------------------------------------------------===//
template <int OVERLOAD_IDX>
static void TestFunctionArgs(DataChunk &args, ExpressionState &state, Vector &result) {
	args.Flatten();
	for (idx_t row = 0; row < args.size(); row++) {
		string str = StringUtil::Format("<%d>|", OVERLOAD_IDX);
		for (auto &vec : args.data) {
			if (FlatVector::IsNull(vec, row)) {
				str += "NULL|";
			} else {
				str += vec.GetValue(row).ToSQLString() + "|";
			}
		}
		FlatVector::GetDataMutable<string_t>(result)[row] = StringVector::AddString(result, str);
	}
}

// Bind data of test_args_kwargs_bind: where each argument ended up, as seen by the bind callback
struct ArgsKwargsBindData : public FunctionData {
	vector<string> labels;
	idx_t varargs_count;
	idx_t kwargs_offset;
	idx_t kwargs_count;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ArgsKwargsBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ArgsKwargsBindData>();
		return labels == other.labels && varargs_count == other.varargs_count && kwargs_offset == other.kwargs_offset &&
		       kwargs_count == other.kwargs_count;
	}
};

static unique_ptr<FunctionData> ArgsKwargsBind(BindScalarFunctionInput &input) {
	auto result = make_uniq<ArgsKwargsBindData>();
	auto &bound_function = input.GetBoundFunction();
	const auto argument_count = input.GetArguments().size();
	for (idx_t i = 0; i < argument_count; i++) {
		switch (bound_function.GetArgumentParameterKind(i)) {
		case FunctionParameterKind::VAR_POSITIONAL:
			result->labels.push_back("*");
			break;
		case FunctionParameterKind::POSITIONAL_ONLY:
			result->labels.push_back("/" + input.GetArgumentName(i).GetIdentifierName());
			break;
		case FunctionParameterKind::VAR_KEYWORD:
			result->labels.push_back("**" + input.GetArgumentName(i).GetIdentifierName());
			break;
		default:
			result->labels.push_back(input.GetArgumentName(i).GetIdentifierName());
			break;
		}
	}
	result->varargs_count = bound_function.GetVarArgsCount();
	result->kwargs_count = bound_function.GetKwargsCount();
	result->kwargs_offset = argument_count - result->kwargs_count;
	return std::move(result);
}

// Prints "<22>|args=N|kwargs=N@offset|name=value|...", with "*" as the name of the "*args" arguments and a "**"
// prefix for the "**kwargs" arguments
static void ArgsKwargsBindFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<ArgsKwargsBindData>();
	args.Flatten();
	for (idx_t row = 0; row < args.size(); row++) {
		string str = StringUtil::Format("<22>|args=%llu|kwargs=%llu@%llu|", bind_data.varargs_count,
		                                bind_data.kwargs_count, bind_data.kwargs_offset);
		for (idx_t col = 0; col < args.ColumnCount(); col++) {
			str += bind_data.labels[col] + "=" + args.data[col].GetValue(row).ToSQLString() + "|";
		}
		FlatVector::GetDataMutable<string_t>(result)[row] = StringVector::AddString(result, str);
	}
}

// Aggregate counterpart of the scalar inspection function. The state captures the argument values
// from the first row it sees; on finalize it emits "<7>|a|b|c|" so a test can observe how named
// arguments / default values were bound into positional order.
struct InspectAggState {
	int32_t vals[3];
	bool valid[3];
	idx_t arg_count;
	bool initialized;
};

static atomic<idx_t> volatile_aggregate_calls {0};

struct VolatileAggregateState {
	bool initialized;
};

struct VolatileAggregateOp {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.initialized = true;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &, STATE &, AggregateInputData &) {
	}

	template <class T, class STATE>
	static void Finalize(STATE &, T &target, AggregateFinalizeData &) {
		target = NumericCast<int64_t>(volatile_aggregate_calls.fetch_add(1) + 1);
	}

	static bool IgnoreNull() {
		return true;
	}
};

static void VolatileAggregateUpdate(Vector[], AggregateInputData &, idx_t, Vector &, idx_t) {
}

static void ResetVolatileAggregate(DataChunk &args, ExpressionState &, Vector &result) {
	volatile_aggregate_calls.store(0);
	result.Reference(Value::BIGINT(0), count_t(args.size()));
}

static void GetVolatileAggregateCalls(DataChunk &args, ExpressionState &, Vector &result) {
	result.Reference(Value::BIGINT(NumericCast<int64_t>(volatile_aggregate_calls.load())), count_t(args.size()));
}

struct InspectAggOp {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.arg_count = 0;
		state.initialized = false;
		for (idx_t i = 0; i < 3; i++) {
			state.valid[i] = false;
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (source.initialized && !target.initialized) {
			target = source;
		}
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		string str = "<7>|";
		for (idx_t i = 0; i < state.arg_count; i++) {
			str += (state.valid[i] ? to_string(state.vals[i]) : string("NULL")) + "|";
		}
		target = StringVector::AddString(finalize_data.result, str);
	}

	static bool IgnoreNull() {
		return false;
	}
};

static void InspectAggUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &state_vector,
                             idx_t count) {
	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(sdata);
	auto states = UnifiedVectorFormat::GetData<InspectAggState *>(sdata);

	vector<UnifiedVectorFormat> idata(input_count);
	for (idx_t c = 0; c < input_count; c++) {
		inputs[c].ToUnifiedFormat(idata[c]);
	}

	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[sdata.sel->get_index(i)];
		if (state.initialized) {
			continue;
		}
		state.arg_count = MinValue<idx_t>(input_count, 3);
		for (idx_t c = 0; c < state.arg_count; c++) {
			const auto idx = idata[c].sel->get_index(i);
			if (idata[c].validity.RowIsValid(idx)) {
				state.vals[c] = UnifiedVectorFormat::GetData<int32_t>(idata[c])[idx];
				state.valid[c] = true;
			} else {
				state.valid[c] = false;
			}
		}
		state.initialized = true;
	}
}

// The inspection functions below return VARCHAR and print "<idx>|arg|arg|..." (see
// TestFunctionArgs) so that a single query result reveals both which overload was chosen
// and the final positional argument list (after reordering/defaults/varargs). SPECIAL_HANDLING
// is used so that NULL arguments still reach the body and we can observe their final position.
static void RegisterNamedArgumentFunction(ExtensionLoader &loader) {
	using NH = FunctionNullHandling;

	// test_named_inspect(a INTEGER, b INTEGER = 100, c INTEGER = 200) -> VARCHAR
	// Single overload, used to test reordering + defaults.
	{
		FunctionSignature sig;
		sig.AddParameter("a", LogicalType::INTEGER);
		sig.AddParameter("b", LogicalType::INTEGER, Value::INTEGER(100));
		sig.AddParameter("c", LogicalType::INTEGER, Value::INTEGER(200));
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_named_inspect", std::move(sig), TestFunctionArgs<1>);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));
	}

	// test_named_varargs(a INTEGER, b INTEGER = 100, ... INTEGER) -> VARCHAR
	// Varargs are appended trailing; named varargs have their names discarded but keep order.
	{
		FunctionSignature sig;
		sig.AddParameter("a", LogicalType::INTEGER);
		sig.AddParameter("b", LogicalType::INTEGER, Value::INTEGER(100));
		sig.AddArgs("args", LogicalType::INTEGER).AddKwargs("kwargs", LogicalType::INTEGER);
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_named_varargs", std::move(sig), TestFunctionArgs<2>);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));
	}

	// test_named_overload: overload resolution driven by argument types/arity, including when
	// the call uses named arguments (which must be reordered before cost is computed).
	//   <10> (a INTEGER, b INTEGER)
	//   <11> (a INTEGER, b VARCHAR)
	//   <12> (a INTEGER)
	{
		ScalarFunctionSet set("test_named_overload");
		{
			FunctionSignature sig;
			sig.AddParameter("a", LogicalType::INTEGER);
			sig.AddParameter("b", LogicalType::INTEGER);
			sig.SetReturnType(LogicalType::VARCHAR);
			ScalarFunction fn("", std::move(sig), TestFunctionArgs<10>);
			fn.SetNullHandling(NH::SPECIAL_HANDLING);
			set.AddFunction(std::move(fn));
		}
		{
			FunctionSignature sig;
			sig.AddParameter("a", LogicalType::INTEGER);
			sig.AddParameter("b", LogicalType::VARCHAR);
			sig.SetReturnType(LogicalType::VARCHAR);
			ScalarFunction fn("", std::move(sig), TestFunctionArgs<11>);
			fn.SetNullHandling(NH::SPECIAL_HANDLING);
			set.AddFunction(std::move(fn));
		}
		{
			FunctionSignature sig;
			sig.AddParameter("a", LogicalType::INTEGER);
			sig.SetReturnType(LogicalType::VARCHAR);
			ScalarFunction fn("", std::move(sig), TestFunctionArgs<12>);
			fn.SetNullHandling(NH::SPECIAL_HANDLING);
			set.AddFunction(std::move(fn));
		}
		loader.RegisterFunction(set);
	}

	// test_named_ambig: two symmetric overloads that tie in cost for an (INTEGER, INTEGER)
	// call, to verify the ambiguity error is raised even when arguments are named/reordered.
	//   <13> (x INTEGER, y BIGINT)
	//   <14> (x BIGINT,  y INTEGER)
	{
		ScalarFunctionSet set("test_named_ambig");
		{
			FunctionSignature sig;
			sig.AddParameter("x", LogicalType::INTEGER);
			sig.AddParameter("y", LogicalType::BIGINT);
			sig.SetReturnType(LogicalType::VARCHAR);
			ScalarFunction fn("", std::move(sig), TestFunctionArgs<13>);
			fn.SetNullHandling(NH::SPECIAL_HANDLING);
			set.AddFunction(std::move(fn));
		}
		{
			FunctionSignature sig;
			sig.AddParameter("x", LogicalType::BIGINT);
			sig.AddParameter("y", LogicalType::INTEGER);
			sig.SetReturnType(LogicalType::VARCHAR);
			ScalarFunction fn("", std::move(sig), TestFunctionArgs<14>);
			fn.SetNullHandling(NH::SPECIAL_HANDLING);
			set.AddFunction(std::move(fn));
		}
		loader.RegisterFunction(set);
	}

	// test_named_nullshort(a INTEGER, b INTEGER = 100) -> VARCHAR
	// DEFAULT_NULL_HANDLING (the default): a NULL argument should short-circuit the whole call
	// to NULL, even when arguments are named/reordered.
	{
		FunctionSignature sig;
		sig.AddParameter("a", LogicalType::INTEGER);
		sig.AddParameter("b", LogicalType::INTEGER, Value::INTEGER(100));
		sig.SetReturnType(LogicalType::VARCHAR);
		loader.RegisterFunction(ScalarFunction("test_named_nullshort", std::move(sig), TestFunctionArgs<6>));
	}

	// test_args_kwargs(a INTEGER, *args INTEGER, kw INTEGER, kw2 INTEGER := 7, **kwargs ANY) -> VARCHAR
	// The arguments are laid out in the order of the signature; "kw" and "kw2" can only be passed by name.
	{
		FunctionSignature sig;
		sig.AddParameter("a", LogicalType::INTEGER);
		sig.AddArgs("args", LogicalType::INTEGER);
		sig.AddKeywordOnly("kw", LogicalType::INTEGER);
		sig.AddKeywordOnly("kw2", LogicalType::INTEGER, Value::INTEGER(7));
		sig.AddKwargs("kwargs", LogicalType::ANY);
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_args_kwargs", sig, TestFunctionArgs<20>);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));

		// Same signature, but reports what the bind callback saw
		ScalarFunction bind_fn("test_args_kwargs_bind", sig, ArgsKwargsBindFunction);
		bind_fn.SetBindCallback(ArgsKwargsBind);
		bind_fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(bind_fn));
	}

	// test_positional_only(a INTEGER, /, b INTEGER, **kwargs ANY) -> VARCHAR
	// "a" can only be passed by position, so "a := ..." does not match it and is received by "**kwargs" instead.
	{
		FunctionSignature sig;
		sig.AddPositionalOnly("a", LogicalType::INTEGER);
		sig.AddParameter("b", LogicalType::INTEGER, Value::INTEGER(9));
		sig.AddKwargs("kwargs", LogicalType::ANY);
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_positional_only", std::move(sig), ArgsKwargsBindFunction);
		fn.SetBindCallback(ArgsKwargsBind);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));
	}

	// test_varargs_bind(a INTEGER, ... INTEGER) -> VARCHAR
	// Declared with an "*args" and a "**kwargs" of the same type: named trailing arguments are received by an unnamed
	// "**kwargs".
	{
		FunctionSignature sig;
		sig.AddParameter("a", LogicalType::INTEGER);
		sig.AddArgs("args", LogicalType::INTEGER).AddKwargs("kwargs", LogicalType::INTEGER);
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_varargs_bind", std::move(sig), ArgsKwargsBindFunction);
		fn.SetBindCallback(ArgsKwargsBind);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));
	}

	// test_varargs_bind(a INTEGER, ... INTEGER) -> VARCHAR
	// Declared with an "*args" and a "**kwargs" of the same type: named trailing arguments are received by "**kwargs".
	{
		FunctionSignature sig;
		sig.AddParameter("a", LogicalType::INTEGER);
		sig.AddArgs("args", LogicalType::INTEGER).AddKwargs("kwargs", LogicalType::INTEGER);
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_varargs_bind", std::move(sig), ArgsKwargsBindFunction);
		fn.SetBindCallback(ArgsKwargsBind);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));
	}

	// test_args_only(a INTEGER, *args INTEGER) -> VARCHAR
	// Without "**kwargs", a named argument that does not match a parameter is rejected.
	{
		FunctionSignature sig;
		sig.AddParameter("a", LogicalType::INTEGER);
		sig.AddArgs("args", LogicalType::INTEGER);
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_args_only", std::move(sig), TestFunctionArgs<21>);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));
	}

	// test_args_template(*args T, **kwargs T) -> VARCHAR
	// All variadic arguments are unified to a single type.
	{
		FunctionSignature sig;
		sig.AddArgs("args", LogicalType::TEMPLATE("T"));
		sig.AddKwargs("kwargs", LogicalType::TEMPLATE("T"));
		sig.SetReturnType(LogicalType::VARCHAR);
		ScalarFunction fn("test_args_template", std::move(sig), TestFunctionArgs<23>);
		fn.SetNullHandling(NH::SPECIAL_HANDLING);
		loader.RegisterFunction(std::move(fn));
	}

	// test_named_agg_inspect(a INTEGER, b INTEGER = 100, c INTEGER = 200) -> VARCHAR
	// Aggregate counterpart of test_named_inspect. AggregateFunction has no FunctionSignature
	// constructor, and the parameters its type list declares are positional-only, so the signature
	// is replaced by one declaring the names and defaults. Exercises named-argument binding for
	// aggregates (shared resolution path).
	{
		AggregateFunction agg(
		    "test_named_agg_inspect", {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER},
		    LogicalType::VARCHAR, AggregateFunction::StateSize<InspectAggState>,
		    AggregateFunction::StateInitialize<InspectAggState, InspectAggOp>, InspectAggUpdate,
		    AggregateFunction::StateCombine<InspectAggState, InspectAggOp>,
		    AggregateFunction::StateFinalize<InspectAggState, string_t, InspectAggOp>, NH::DEFAULT_NULL_HANDLING);
		auto signature = FunctionSignature()
		                     .AddParameter("a", LogicalType::INTEGER)
		                     .AddParameter("b", LogicalType::INTEGER, Value::INTEGER(100))
		                     .AddParameter("c", LogicalType::INTEGER, Value::INTEGER(200));
		signature.SetReturnType(LogicalType::VARCHAR);
		agg.GetSignature() = std::move(signature);
		loader.RegisterFunction(std::move(agg));
	}

	// test_args_agg(a INTEGER, *args INTEGER, kw INTEGER := 9) -> VARCHAR
	{
		AggregateFunction agg("test_args_agg", {LogicalType::INTEGER}, LogicalType::VARCHAR,
		                      AggregateFunction::StateSize<InspectAggState>,
		                      AggregateFunction::StateInitialize<InspectAggState, InspectAggOp>, InspectAggUpdate,
		                      AggregateFunction::StateCombine<InspectAggState, InspectAggOp>,
		                      AggregateFunction::StateFinalize<InspectAggState, string_t, InspectAggOp>,
		                      NH::DEFAULT_NULL_HANDLING);
		auto &sig = agg.GetSignature();
		sig.GetParameter(0).SetName("a");
		sig.AddArgs("args", LogicalType::INTEGER);
		sig.AddKeywordOnly("kw", LogicalType::INTEGER, Value::INTEGER(9));
		loader.RegisterFunction(std::move(agg));
	}
}

//===--------------------------------------------------------------------===//
// Extension load + setup
//===--------------------------------------------------------------------===//
extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(loadable_extension_demo, loader) {
	CreateScalarFunctionInfo hello_alias_info(
	    ScalarFunction("test_alias_hello", {}, LogicalType::VARCHAR, TestAliasHello));

	RegisterNamedArgumentFunction(loader);
	AggregateFunction volatile_aggregate(
	    "test_volatile_aggregate", {LogicalType::INTEGER}, LogicalType::BIGINT,
	    AggregateFunction::StateSize<VolatileAggregateState>,
	    AggregateFunction::StateInitialize<VolatileAggregateState, VolatileAggregateOp>, VolatileAggregateUpdate,
	    AggregateFunction::StateCombine<VolatileAggregateState, VolatileAggregateOp>,
	    AggregateFunction::StateFinalize<VolatileAggregateState, int64_t, VolatileAggregateOp>,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING);
	volatile_aggregate.SetVolatile();
	loader.RegisterFunction(std::move(volatile_aggregate));
	auto reset_volatile_aggregate =
	    ScalarFunction("test_reset_volatile_aggregate", {}, LogicalType::BIGINT, ResetVolatileAggregate);
	reset_volatile_aggregate.SetVolatile();
	loader.RegisterFunction(std::move(reset_volatile_aggregate));
	auto get_volatile_aggregate_calls =
	    ScalarFunction("test_volatile_aggregate_calls", {}, LogicalType::BIGINT, GetVolatileAggregateCalls);
	get_volatile_aggregate_calls.SetVolatile();
	loader.RegisterFunction(std::move(get_volatile_aggregate_calls));

	auto &db = loader.GetDatabaseInstance();
	// create a scalar function
	Connection con(db);
	auto &client_context = *con.context;
	auto &catalog = Catalog::GetSystemCatalog(client_context);
	con.BeginTransaction();
	catalog.CreateFunction(client_context, hello_alias_info);

	// Add alias POINT type
	string alias_name = "POINT";
	child_list_t<LogicalType> child_types;
	child_types.emplace_back(make_pair("x", LogicalType::INTEGER));
	child_types.emplace_back(make_pair("y", LogicalType::INTEGER));
	auto alias_info = make_uniq<CreateTypeInfo>();
	alias_info->internal = true;
	alias_info->SetTypeName(Identifier(alias_name));
	LogicalType target_type = LogicalType::STRUCT(child_types).WithAlias(alias_name);
	alias_info->type = target_type;

	auto type_entry = catalog.CreateType(client_context, *alias_info);
	type_entry->tags["ext:name"] = "loadable_extension_demo";
	type_entry->tags["ext:author"] = "DuckDB Labs";

	// Function add point
	ScalarFunction add_point_func("add_point", {target_type, target_type}, target_type, AddPointFunction);
	CreateScalarFunctionInfo add_point_info(add_point_func);
	auto add_point_entry = catalog.CreateFunction(client_context, add_point_info);
	add_point_entry->tags["ext:name"] = "loadable_extension_demo";
	add_point_entry->tags["ext:author"] = "DuckDB Labs";

	// Function sub point
	ScalarFunction sub_point_func("sub_point", {target_type, target_type}, target_type, SubPointFunction);
	CreateScalarFunctionInfo sub_point_info(sub_point_func);
	auto sub_point_entry = catalog.CreateFunction(client_context, sub_point_info);
	sub_point_entry->tags["ext:name"] = "loadable_extension_demo";
	sub_point_entry->tags["ext:author"] = "DuckDB Labs";

	// Function sub point
	ScalarFunction loaded_extensions("loaded_extensions", {}, LogicalType::VARCHAR, LoadedExtensionsFunction);
	CreateScalarFunctionInfo loaded_extensions_info(loaded_extensions);
	catalog.CreateFunction(client_context, loaded_extensions_info);

	// Quack function
	QuackFunction quack_function;
	CreateTableFunctionInfo quack_info(quack_function);
	catalog.CreateTableFunction(client_context, quack_info);

	con.Commit();

	// Table with tagged columns
	{
		auto tagged_table_info = make_uniq<CreateTableInfo>();
		tagged_table_info->SetQualifiedName(
		    QualifiedName(INVALID_CATALOG, Identifier::DefaultSchema(), "tagged_table"));
		tagged_table_info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		tagged_table_info->temporary = false;
		tagged_table_info->internal = true;

		ColumnDefinition col_a("a", LogicalType::INTEGER);
		col_a.SetComment(Value("Primary tagged column"));
		InsertionOrderPreservingMap<string> col_a_tags;
		col_a_tags["ext:name"] = "loadable_extension_demo";
		col_a_tags["ext:column_type"] = "primary";
		col_a.SetTags(std::move(col_a_tags));
		tagged_table_info->columns.AddColumn(std::move(col_a));

		ColumnDefinition col_b("b", LogicalType::VARCHAR);
		col_b.SetComment(Value("Dimension tagged column"));
		InsertionOrderPreservingMap<string> col_b_tags;
		col_b_tags["ext:name"] = "loadable_extension_demo";
		col_b_tags["ext:column_type"] = "dimension";
		col_b.SetTags(std::move(col_b_tags));
		tagged_table_info->columns.AddColumn(std::move(col_b));

		ColumnDefinition col_c("c", LogicalType::DOUBLE);
		InsertionOrderPreservingMap<string> col_c_tags;
		col_c_tags["ext:name"] = "loadable_extension_demo";
		col_c_tags["ext:column_type"] = "measure";
		col_c.SetTags(std::move(col_c_tags));
		tagged_table_info->columns.AddColumn(std::move(col_c));

		con.BeginTransaction();
		auto default_db_name = DatabaseManager::GetDefaultDatabase(client_context);
		auto &default_catalog = Catalog::GetCatalog(client_context, default_db_name);
		MetaTransaction::Get(client_context).ModifyDatabase(default_catalog.GetAttached(), DatabaseModificationType());
		default_catalog.CreateTable(client_context, std::move(tagged_table_info));
		con.Commit();
	}

	// add a parser extension
	auto &config = DBConfig::GetConfig(db);
	ParserExtension::Register(config, QuackExtension());
	ExtensionCallback::Register(config, make_shared_ptr<QuackLoadExtension>());

	// add a planner extension that adds an extra column to queries
	PlannerExtension::Register(config, AddColumnExtension());
	config.AddExtensionOption("add_column_enabled", "enable adding extra column to queries", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false));

	// Global-default extension option used to exercise RESET on GLOBAL-scoped
	// extension options across multiple connections.
	config.AddExtensionOption("demo_global_setting", "demo GLOBAL-default extension option", LogicalType::VARCHAR,
	                          Value("default"), nullptr, SetScope::GLOBAL);

	// Bounded type
	auto bounded_type = BoundedType::GetDefault();
	loader.RegisterType("BOUNDED", bounded_type, BoundedType::Constructors());

	// Example of function inspecting the type property
	ScalarFunction bounded_max("bounded_max", {bounded_type}, LogicalType::INTEGER, BoundedMaxFunc, BoundedMaxBind);
	loader.RegisterFunction(bounded_max);

	// Example of function inspecting the type property and returning the same type
	ScalarFunction bounded_invert("bounded_invert", {bounded_type}, bounded_type, BoundedInvertFunc, BoundedInvertBind);
	// bounded_invert.serialize = BoundedReturnSerialize;
	// bounded_invert.deserialize = BoundedReturnDeserialize;
	loader.RegisterFunction(bounded_invert);

	// Example of function inspecting the type property of both arguments and returning a new type
	ScalarFunction bounded_add("bounded_add", {bounded_type, bounded_type}, bounded_type, BoundedAddFunc,
	                           BoundedAddBind);
	loader.RegisterFunction(bounded_add);

	// Example of function that is generic over the type property (the bound is not important)
	ScalarFunction bounded_even("bounded_even", {bounded_type}, LogicalType::BOOLEAN, BoundedEvenFunc);
	loader.RegisterFunction(bounded_even);

	// Example of function that is specialized over type property
	auto bounded_specialized_type = BoundedType::Get(0xFF);
	ScalarFunction bounded_to_ascii("bounded_ascii", {bounded_specialized_type}, LogicalType::VARCHAR,
	                                BoundedToAsciiFunc);
	loader.RegisterFunction(bounded_to_ascii);

	// Example of a window function
	DuckWeedFunction duckweed;
	loader.RegisterFunction(duckweed);

	// Enable explicit casting to our specialized type
	loader.RegisterCastFunction(bounded_type, bounded_specialized_type, BoundCastInfo(BoundedToBoundedCast), 0);
	// Casts
	loader.RegisterCastFunction(LogicalType::INTEGER, bounded_type, BoundCastInfo(IntToBoundedCast), 0);

	// MinMax Type
	auto minmax_type = MinMaxType::GetDefault();
	loader.RegisterType("MINMAX", minmax_type, MinMaxType::Constructors());
	loader.RegisterCastFunction(LogicalType::INTEGER, minmax_type, BoundCastInfo(IntToMinMaxCast), 0);
	loader.RegisterFunction(ScalarFunction("minmax_range", {minmax_type}, LogicalType::INTEGER, MinMaxRangeFunc));

	// Register the RowId optimizer extension (extensible table filter demo)
	config.GetCallbackManager().Register(RowIdOptimizerExtension());
}
}
