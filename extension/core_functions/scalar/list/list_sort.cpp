#include "duckdb/common/type_visitor.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/sorting/sort.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {

struct ListSortBindData : public FunctionData {
	ListSortBindData(OrderType order_type_p, OrderByNullType null_order_p, bool is_grade_up,
	                 const LogicalType &return_type_p, const LogicalType &child_type_p, ClientContext &context_p,
	                 unique_ptr<Expression> sort_key_p = nullptr, optional<string> default_collation_p = {});
	ListSortBindData(const ListSortBindData &other);
	~ListSortBindData() override;

	OrderType order_type;
	OrderByNullType null_order;
	LogicalType return_type;
	LogicalType child_type;
	bool is_grade_up;

	vector<LogicalType> types;

	ClientContext &context;
	unique_ptr<Expression> sort_key;
	optional<string> default_collation;
	//! Shared by the copies, building it again needs a catalog lookup in a transaction of the context
	shared_ptr<Sort> sort;

public:
	bool Equals(const FunctionData &other_p) const override;
	unique_ptr<FunctionData> Copy() const override;
	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                      const BoundScalarFunction &);
	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &);
};

ListSortBindData::ListSortBindData(OrderType order_type_p, OrderByNullType null_order_p, bool is_grade_up_p,
                                   const LogicalType &return_type_p, const LogicalType &child_type_p,
                                   ClientContext &context_p, unique_ptr<Expression> sort_key_p,
                                   optional<string> default_collation_p)
    : order_type(order_type_p), null_order(null_order_p), return_type(return_type_p), child_type(child_type_p),
      is_grade_up(is_grade_up_p), context(context_p), default_collation(std::move(default_collation_p)) {
	// get the vector types
	types.emplace_back(LogicalType::USMALLINT);
	types.emplace_back(child_type);
	types.emplace_back(LogicalType::UINTEGER);
	D_ASSERT(types.size() == 3);

	// get the BoundOrderByNode
	auto idx_col_expr = make_uniq_base<Expression, BoundReferenceExpression>(LogicalType::USMALLINT, 0U);
	if (!sort_key_p) {
		default_collation = Settings::Get<DefaultCollationSetting>(context);
		sort_key_p = make_uniq_base<Expression, BoundReferenceExpression>(child_type, 1U);
		// Normalize the sort key without changing the sorted values (#25108): push the
		// type's collation (for INTERVAL this wraps the expression in
		// normalized_interval(...), the same normalization comparison operators, ORDER BY
		// and aggregates apply) onto the key expression BEFORE constructing its
		// BoundOrderByNode. The key then byte-compares like ORDER BY, while the list
		// payload keeps the original (non-normalized) values.
		ExpressionBinder::PushCollation(context, sort_key_p, child_type);
	}
	sort_key = sort_key_p->Copy();
	vector<BoundOrderByNode> orders;
	orders.emplace_back(OrderType::ASCENDING, OrderByNullType::ORDER_DEFAULT, std::move(idx_col_expr));
	orders.emplace_back(order_type, null_order, std::move(sort_key_p));
	if (is_grade_up) {
		// break ties on the original position, so equal elements are graded in a deterministic order
		auto position_expr = make_uniq_base<Expression, BoundReferenceExpression>(LogicalType::UINTEGER, 2U);
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::ORDER_DEFAULT, std::move(position_expr));
	}
	sort =
	    shared_ptr<Sort>(new Sort(context, orders, {LogicalType::USMALLINT, child_type, LogicalType::UINTEGER}, {2}));
}

ListSortBindData::ListSortBindData(const ListSortBindData &other)
    : FunctionData(other), order_type(other.order_type), null_order(other.null_order), return_type(other.return_type),
      child_type(other.child_type), is_grade_up(other.is_grade_up), types(other.types), context(other.context),
      sort_key(other.sort_key->Copy()), default_collation(other.default_collation), sort(other.sort) {
}

unique_ptr<FunctionData> ListSortBindData::Copy() const {
	return make_uniq<ListSortBindData>(*this);
}

bool ListSortBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<ListSortBindData>();
	return order_type == other.order_type && null_order == other.null_order && return_type == other.return_type &&
	       child_type == other.child_type && is_grade_up == other.is_grade_up &&
	       Expression::Equals(sort_key, other.sort_key) && default_collation == other.default_collation;
}

void ListSortBindData::Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                 const BoundScalarFunction &) {
	auto &data = bind_data->Cast<ListSortBindData>();
	serializer.WriteProperty(100, "order_type", data.order_type);
	serializer.WriteProperty(101, "null_order", data.null_order);
	serializer.WriteProperty(102, "return_type", data.return_type);
	serializer.WriteProperty(103, "child_type", data.child_type);
	serializer.WriteProperty(104, "is_grade_up", data.is_grade_up);
	serializer.WriteProperty(105, "sort_key", data.sort_key);
	serializer.WritePropertyWithDefault(106, "default_collation", data.default_collation);
}

unique_ptr<FunctionData> ListSortBindData::Deserialize(Deserializer &deserializer, BoundScalarFunction &) {
	auto order_type = deserializer.ReadProperty<OrderType>(100, "order_type");
	auto null_order = deserializer.ReadProperty<OrderByNullType>(101, "null_order");
	auto return_type = deserializer.ReadProperty<LogicalType>(102, "return_type");
	auto child_type = deserializer.ReadProperty<LogicalType>(103, "child_type");
	auto is_grade_up = deserializer.ReadProperty<bool>(104, "is_grade_up");
	auto sort_key = deserializer.ReadProperty<unique_ptr<Expression>>(105, "sort_key");
	auto default_collation = deserializer.ReadPropertyWithDefault<optional<string>>(106, "default_collation");
	auto &context = deserializer.Get<ClientContext &>();
	return make_uniq<ListSortBindData>(order_type, null_order, is_grade_up, return_type, child_type, context,
	                                   std::move(sort_key), std::move(default_collation));
}

ListSortBindData::~ListSortBindData() {
}

// create the key_chunk and the payload_chunk and sink them into the local_sort_state
static void SinkDataChunk(const Sort &sort, ExecutionContext &context, OperatorSinkInput &sink_input,
                          Vector *child_vector, SelectionVector &sel, idx_t offset_lists_indices,
                          vector<LogicalType> &types, Vector &payload_vector, bool &data_to_sort,
                          Vector &lists_indices) {
	// slice the child vector
	Vector slice(*child_vector, sel, offset_lists_indices);

	// initialize and fill chunk
	DataChunk chunk;
	chunk.InitializeEmpty(types);
	chunk.data[0].Reference(lists_indices);
	chunk.data[1].Reference(slice);
	chunk.data[2].Reference(payload_vector);
	chunk.SetChildCardinality(offset_lists_indices);
	chunk.Verify(context.client.db);

	// sink
	chunk.Flatten();
	sort.Sink(context, chunk, sink_input);
	data_to_sort = true;
}

static void ListSortFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() >= 1 && args.ColumnCount() <= 3);
	auto count = args.size();
	const Vector &input_lists = args.data[0];

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::ValidityMutable(result);

	if (input_lists.GetType().id() == LogicalTypeId::SQLNULL) {
		result_validity.SetInvalid(0);
		return;
	}

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ListSortBindData>();

	// initialize the global and local sorting state
	auto global_sink_state = info.sort->GetGlobalSinkState(info.context);
	ThreadContext thread_context(info.context);
	ExecutionContext execution_context(info.context, thread_context, nullptr);
	auto local_sink_state = info.sort->GetLocalSinkState(execution_context);
	InterruptState interrupt_state;
	OperatorSinkInput sink_input {*global_sink_state, *local_sink_state, interrupt_state};

	Vector sort_result_vec = info.is_grade_up ? Vector(input_lists.GetType()) : Vector::Ref(result);

	// this ensures that we do not change the order of the entries in the input chunk
	VectorOperations::Copy(input_lists, sort_result_vec, count, 0, 0);

	// get the child vector
	auto lists_size = ListVector::GetListSize(sort_result_vec);
	auto &child_vector = ListVector::GetChildMutable(sort_result_vec);

	// get the lists data
	auto list_entries = sort_result_vec.Values<list_entry_t>();

	// create the lists_indices vector, this contains an element for each list's entry,
	// the element corresponds to the list's index, e.g. for [1, 2, 4], [5, 4]
	// lists_indices contains [0, 0, 0, 1, 1]
	Vector lists_indices(LogicalType::USMALLINT);
	auto lists_indices_data = FlatVector::ScatterWriter<uint16_t>(lists_indices);

	// create the payload_vector, this is just a vector containing incrementing integers
	// this will later be used as the 'new' selection vector of the child_vector, after
	// rearranging the payload according to the sorting order
	Vector payload_vector(LogicalType::UINTEGER);
	auto payload_vector_data = FlatVector::ScatterWriter<uint32_t>(payload_vector);

	// selection vector pointing to the data of the child vector,
	// used for slicing the child_vector correctly
	SelectionVector sel(STANDARD_VECTOR_SIZE);

	idx_t offset_lists_indices = 0;
	uint32_t incr_payload_count = 0;
	bool data_to_sort = false;

	for (idx_t i = 0; i < count; i++) {
		auto entry = list_entries[i];

		// nothing to do for this list
		if (!entry.IsValid()) {
			result_validity.SetInvalid(i);
			continue;
		}

		const auto &list_entry = entry.GetValue();

		// empty list, no sorting required
		if (list_entry.length == 0) {
			continue;
		}

		for (idx_t child_idx = 0; child_idx < list_entry.length; child_idx++) {
			// lists_indices vector is full, sink
			if (offset_lists_indices == STANDARD_VECTOR_SIZE) {
				SinkDataChunk(*info.sort, execution_context, sink_input, &child_vector, sel, offset_lists_indices,
				              info.types, payload_vector, data_to_sort, lists_indices);
				offset_lists_indices = 0;
			}

			auto source_idx = list_entry.offset + child_idx;
			sel.set_index(offset_lists_indices, source_idx);
			lists_indices_data[offset_lists_indices] = UnsafeNumericCast<uint16_t>(i);
			payload_vector_data[offset_lists_indices] = NumericCast<uint32_t>(source_idx);
			offset_lists_indices++;
			incr_payload_count++;
		}
	}

	if (offset_lists_indices != 0) {
		SinkDataChunk(*info.sort, execution_context, sink_input, &child_vector, sel, offset_lists_indices, info.types,
		              payload_vector, data_to_sort, lists_indices);
	}

	if (info.is_grade_up) {
		ListVector::Reserve(result, lists_size);
		ListVector::SetListSize(result, lists_size);
		auto result_list_data = FlatVector::Writer<list_entry_t>(result, count);
		for (idx_t i = 0; i < count; i++) {
			result_list_data.WriteValue(list_entries.GetValueUnsafe(i));
		}
	}

	if (data_to_sort) {
		// add local state to global state, which sorts the data
		OperatorSinkCombineInput combine_input {*global_sink_state, *local_sink_state, interrupt_state};
		info.sort->Combine(execution_context, combine_input);

		OperatorSinkFinalizeInput finalize_input {*global_sink_state, interrupt_state};
		info.sort->Finalize(info.context, finalize_input);

		// selection vector that is to be filled with the 'sorted' payload
		SelectionVector sel_sorted(incr_payload_count);
		idx_t sel_sorted_idx = 0;

		// scan the sorted row data
		auto global_source_state = info.sort->GetGlobalSourceState(info.context, *global_sink_state);
		auto local_source_state = info.sort->GetLocalSourceState(execution_context, *global_source_state);
		OperatorSourceInput source_input {*global_source_state, *local_source_state, interrupt_state};
		for (;;) {
			DataChunk result_chunk;
			result_chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::UINTEGER});
			info.sort->GetData(execution_context, result_chunk, source_input);
			if (result_chunk.size() == 0) {
				break;
			}

			// construct the selection vector with the new order from the result vectors
			auto row_count = result_chunk.size();
			auto result_values = result_chunk.data[0].Values<uint32_t>();
			for (idx_t i = 0; i < row_count; i++) {
				auto sorted_idx = result_values[i].GetValue();
				sel_sorted.set_index(sel_sorted_idx, sorted_idx);
				D_ASSERT(sorted_idx < lists_size);
				sel_sorted_idx++;
			}
		}

		D_ASSERT(sel_sorted_idx == incr_payload_count);
		if (info.is_grade_up) {
			auto &result_entry = ListVector::GetChildMutable(result);
			auto result_data = FlatVector::GetData<list_entry_t>(result);
			for (idx_t i = 0; i < count; i++) {
				if (!result_validity.RowIsValid(i)) {
					continue;
				}
				for (idx_t j = result_data[i].offset; j < result_data[i].offset + result_data[i].length; j++) {
					auto b = sel_sorted.get_index(j) - result_data[i].offset;
					result_entry.SetValue(j, Value::BIGINT(UnsafeNumericCast<int64_t>(b + 1)));
				}
			}
		} else {
			child_vector.Slice(sel_sorted, sel_sorted_idx);
			child_vector.Flatten();
		}
	}
}

static unique_ptr<FunctionData> ListSortBind(ClientContext &context, BoundScalarFunction &bound_function,
                                             vector<unique_ptr<Expression>> &arguments, OrderType &order,
                                             OrderByNullType &null_order) {
	LogicalType child_type;
	if (arguments[0]->GetReturnType() == LogicalTypeId::UNKNOWN) {
		bound_function.GetArguments()[0] = LogicalTypeId::UNKNOWN;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		child_type = bound_function.GetReturnType();
		return make_uniq<ListSortBindData>(order, null_order, false, bound_function.GetReturnType(), child_type,
		                                   context);
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::LIST) {
		throw BinderException("list_sort can only operate on LISTs");
	}
	child_type = ListType::GetChildType(arguments[0]->GetReturnType());

	bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
	bound_function.SetReturnType(arguments[0]->GetReturnType());

	return make_uniq<ListSortBindData>(order, null_order, false, bound_function.GetReturnType(), child_type, context);
}

template <class T>
static T GetOrder(const Value &order_value) {
	auto order_name = StringUtil::Upper(order_value.ToString());
	return EnumUtil::FromString<T>(order_name.c_str());
}

static unique_ptr<FunctionData> ListGradeUpBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(!arguments.empty() && arguments.size() <= 3);
	auto order = OrderType::ORDER_DEFAULT;
	auto null_order = OrderByNullType::ORDER_DEFAULT;

	// get the sorting order
	if (arguments.size() >= 2) {
		order = GetOrder<OrderType>(input.GetConstant(1));
	}
	// get the null sorting order
	if (arguments.size() == 3) {
		null_order = GetOrder<OrderByNullType>(input.GetConstant(2));
	}
	auto &config = DBConfig::GetConfig(context);
	order = config.ResolveOrder(context, order);
	null_order = config.ResolveNullOrder(context, order, null_order);

	if (arguments[0]->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		bound_function.GetArguments()[0] = LogicalTypeId::UNKNOWN;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		return make_uniq<ListSortBindData>(order, null_order, true, bound_function.GetReturnType(),
		                                   bound_function.GetReturnType(), context);
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::LIST) {
		throw BinderException("list_grade_up can only operate on LISTs");
	}

	bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
	bound_function.SetReturnType(LogicalType::LIST(LogicalTypeId::BIGINT));
	auto child_type = ListType::GetChildType(arguments[0]->GetReturnType());
	return make_uniq<ListSortBindData>(order, null_order, true, bound_function.GetReturnType(), child_type, context);
}

static unique_ptr<FunctionData> ListNormalSortBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(!arguments.empty() && arguments.size() <= 3);
	auto order = OrderType::ORDER_DEFAULT;
	auto null_order = OrderByNullType::ORDER_DEFAULT;

	// get the sorting order
	if (arguments.size() >= 2) {
		order = GetOrder<OrderType>(input.GetConstant(1));
	}
	// get the null sorting order
	if (arguments.size() == 3) {
		null_order = GetOrder<OrderByNullType>(input.GetConstant(2));
	}
	auto &config = DBConfig::GetConfig(context);
	order = config.ResolveOrder(context, order);
	null_order = config.ResolveNullOrder(context, order, null_order);
	return ListSortBind(context, bound_function, arguments, order, null_order);
}

static unique_ptr<FunctionData> ListReverseSortBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto order = OrderType::ORDER_DEFAULT;
	auto null_order = OrderByNullType::ORDER_DEFAULT;

	if (arguments.size() == 2) {
		null_order = GetOrder<OrderByNullType>(input.GetConstant(1));
	}
	auto &config = DBConfig::GetConfig(context);
	order = config.ResolveOrder(context, order);
	switch (order) {
	case OrderType::ASCENDING:
		order = OrderType::DESCENDING;
		break;
	case OrderType::DESCENDING:
		order = OrderType::ASCENDING;
		break;
	default:
		throw InternalException("Unexpected order type in list reverse sort");
	}
	null_order = config.ResolveNullOrder(context, order, null_order);
	return ListSortBind(context, bound_function, arguments, order, null_order);
}

static unique_ptr<ParsedExpression> ListSortUnbind(FunctionUnbindInput &input) {
	if (input.children.empty() || !input.expression.BindInfo()) {
		return nullptr;
	}
	auto &data = input.expression.BindInfo()->Cast<ListSortBindData>();
	if (!data.default_collation ||
	    TypeVisitor::Contains(data.child_type, [](const LogicalType &type) { return type.HasAlias(); })) {
		return nullptr;
	}
	auto type = TypeVisitor::VisitReplace(data.child_type, [&](const LogicalType &child) {
		if (child.id() == LogicalTypeId::VARCHAR && StringType::GetCollation(child).empty()) {
			return LogicalType::VARCHAR_COLLATION(data.default_collation->empty() ? "binary" : *data.default_collation);
		}
		return child;
	});
	auto witness = BoundExpressionSQLExporter::Export(BoundConstantExpression(Value(LogicalType::LIST(type))), {});
	if (witness.HasError()) {
		return nullptr;
	}
	vector<unique_ptr<ParsedExpression>> cast_children;
	cast_children.push_back(std::move(input.children[0]));
	cast_children.push_back(std::move(witness.GetValue()));
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(
	    make_uniq<FunctionExpression>(QualifiedName("system", "main", "cast_to_type"), std::move(cast_children)));
	children.push_back(ConstantExpression::String(data.order_type == OrderType::ASCENDING ? "ASC" : "DESC"));
	children.push_back(
	    ConstantExpression::String(data.null_order == OrderByNullType::NULLS_FIRST ? "NULLS FIRST" : "NULLS LAST"));
	auto result_type = BoundExpressionSQLExporter::Export(BoundConstantExpression(Value(data.return_type)), {});
	if (result_type.HasError()) {
		return nullptr;
	}
	vector<unique_ptr<ParsedExpression>> result;
	result.push_back(make_uniq<FunctionExpression>(
	    QualifiedName("system", "main", data.is_grade_up ? "list_grade_up" : "list_sort"), std::move(children)));
	result.push_back(std::move(result_type.GetValue()));
	return make_uniq<FunctionExpression>(QualifiedName("system", "main", "cast_to_type"), std::move(result));
}

static void ListSortResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto list_type = input.GetArgumentType(0);
	if (list_type.id() == LogicalTypeId::ARRAY) {
		list_type = LogicalType::LIST(ArrayType::GetChildType(list_type));
	}
	if (list_type.id() == LogicalTypeId::LIST) {
		input.GetBoundFunction().SetReturnType(list_type);
	}
}

static void SetListSortSerialization(ScalarFunction &function) {
	function.SetUnbindCallback(ListSortUnbind);
	function.SetSerializeCallback(ListSortBindData::Serialize);
	function.SetDeserializeCallback(ListSortBindData::Deserialize);
}

ScalarFunctionSet ListSortFun::GetFunctions() {
	// one parameter: list
	ScalarFunction sort({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListNormalSortBind);
	sort.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));

	// two parameters: list, order
	ScalarFunction sort_order({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListNormalSortBind);
	sort_order.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR);

	// three parameters: list, order, null order
	ScalarFunction sort_orders({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListNormalSortBind);
	sort_orders.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR)
	    .AddParameter("null_order", LogicalType::VARCHAR);
	SetListSortSerialization(sort);
	sort.SetResolveTypesCallback(ListSortResolveTypes);
	SetListSortSerialization(sort_order);
	sort_order.SetResolveTypesCallback(ListSortResolveTypes);
	SetListSortSerialization(sort_orders);
	sort_orders.SetResolveTypesCallback(ListSortResolveTypes);

	ScalarFunctionSet list_sort;
	list_sort.AddFunction(sort);
	list_sort.AddFunction(sort_order);
	list_sort.AddFunction(sort_orders);
	return list_sort;
}

ScalarFunctionSet ListGradeUpFun::GetFunctions() {
	// one parameter: list
	ScalarFunction sort({}, LogicalType::LIST(LogicalType::BIGINT), ListSortFunction, ListGradeUpBind);
	sort.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));

	// two parameters: list, order
	ScalarFunction sort_order({}, LogicalType::LIST(LogicalType::BIGINT), ListSortFunction, ListGradeUpBind);
	sort_order.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR);

	// three parameters: list, order, null order
	ScalarFunction sort_orders({}, LogicalType::LIST(LogicalType::BIGINT), ListSortFunction, ListGradeUpBind);
	sort_orders.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR)
	    .AddParameter("null_order", LogicalType::VARCHAR);
	SetListSortSerialization(sort);
	SetListSortSerialization(sort_order);
	SetListSortSerialization(sort_orders);

	ScalarFunctionSet list_grade_up;
	list_grade_up.AddFunction(sort);
	list_grade_up.AddFunction(sort_order);
	list_grade_up.AddFunction(sort_orders);
	return list_grade_up;
}

ScalarFunctionSet ListReverseSortFun::GetFunctions() {
	// one parameter: list
	ScalarFunction sort_reverse({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListReverseSortBind);
	sort_reverse.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));

	// two parameters: list, null order
	ScalarFunction sort_reverse_null_order({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction,
	                                       ListReverseSortBind);
	sort_reverse_null_order.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("null_order", LogicalType::VARCHAR);
	SetListSortSerialization(sort_reverse);
	sort_reverse.SetResolveTypesCallback(ListSortResolveTypes);
	SetListSortSerialization(sort_reverse_null_order);
	sort_reverse_null_order.SetResolveTypesCallback(ListSortResolveTypes);

	ScalarFunctionSet list_reverse_sort;
	list_reverse_sort.AddFunction(sort_reverse);
	list_reverse_sort.AddFunction(sort_reverse_null_order);
	return list_reverse_sort;
}

} // namespace duckdb
