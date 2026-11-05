#include "duckdb/function/scalar/sequence_functions.hpp"
#include "duckdb/function/scalar/sequence_utils.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

namespace {

struct CurrentSequenceValueOperator {
	static int64_t Operation(DuckTransaction &, SequenceCatalogEntry &seq, const int64_t, const bool) {
		return seq.CurrentValue();
	}
};

struct NextSequenceValueOperator {
	static int64_t Operation(DuckTransaction &transaction, SequenceCatalogEntry &seq, const int64_t, const bool) {
		return seq.NextValue(transaction);
	}
};

struct SetValValueOperator {
	static int64_t Operation(DuckTransaction &transaction, SequenceCatalogEntry &seq, const int64_t value,
	                         const bool is_called) {
		return seq.SetValue(transaction, value, is_called);
	}
};

SequenceCatalogEntry &BindSequence(Binder &binder, const QualifiedName &name) {
	// resolve the (possibly nested) catalog/schema qualification and fetch the sequence from the catalog
	EntryLookupInfo sequence_lookup(CatalogType::SEQUENCE_ENTRY, Binder::BindTableName(binder.EntryRetriever(), name));
	return binder.EntryRetriever().GetEntry(sequence_lookup)->Cast<SequenceCatalogEntry>();
}

SequenceCatalogEntry &BindSequenceFromContext(ClientContext &context, const QualifiedName &name) {
	CatalogEntryRetriever retriever(context);
	return Catalog::GetEntry<SequenceCatalogEntry>(context, Binder::BindTableName(retriever, name));
}

SequenceCatalogEntry &BindSequence(Binder &binder, const Identifier &name) {
	QualifiedName qname;
	string error;
	if (!QualifiedName::TryParse(name.GetIdentifierName(), qname, error)) {
		throw ParserException(error);
	}
	return BindSequence(binder, qname);
}

struct NextValLocalState : public FunctionLocalState {
	explicit NextValLocalState(DuckTransaction &transaction, SequenceCatalogEntry &sequence)
	    : transaction(transaction), sequence(sequence) {
	}

	DuckTransaction &transaction;
	SequenceCatalogEntry &sequence;
};

template <class T>
bool RowIsValid(const unique_ptr<VectorIterator<T>> &entries, idx_t i) {
	return !entries || (*entries)[i].IsValid();
}

unique_ptr<FunctionLocalState> NextValLocalFunction(ExpressionState &state, const BoundFunctionExpression &expr,
                                                    FunctionData *bind_data) {
	if (!bind_data) {
		return nullptr;
	}
	auto &context = state.GetContext();
	auto &info = bind_data->Cast<NextvalBindData>();
	auto &sequence = info.sequence;
	auto &transaction = DuckTransaction::Get(context, sequence.catalog);
	return make_uniq<NextValLocalState>(transaction, sequence);
}

template <class OP>
void NextValFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	if (!func_expr.BindInfo()) {
		// no bind info - return null
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<NextValLocalState>();
	// sequence to use is hard coded
	// increment the sequence
	result.SetVectorType(VectorType::FLAT_VECTOR);

	unique_ptr<VectorIterator<int64_t>> new_val_entries;
	unique_ptr<VectorIterator<bool>> is_called_entries;
	if constexpr (std::is_same_v<OP, SetValValueOperator>) {
		const auto &new_vals = args.data[1];
		new_val_entries = make_uniq<VectorIterator<int64_t>>(new_vals.Values<int64_t>());

		if (args.ColumnCount() == 3) {
			const auto &is_called_val = args.data[2];
			is_called_entries = make_uniq<VectorIterator<bool>>(is_called_val.Values<bool>());
		}
	}

	auto result_data = FlatVector::Writer<int64_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		if (!RowIsValid(new_val_entries, i) || !RowIsValid(is_called_entries, i)) {
			result_data.WriteNull();
			continue;
		}

		int64_t value = new_val_entries ? (*new_val_entries)[i].GetValue() : 0;
		bool is_called = is_called_entries ? (*is_called_entries)[i].GetValue() : true;
		// get the next value from the sequence
		result_data.WriteValue(OP::Operation(lstate.transaction, lstate.sequence, value, is_called));
	}
}

unique_ptr<FunctionData> NextValBind(BindScalarFunctionInput &input) {
	// parameter to nextval function is a foldable constant
	// evaluate the constant and perform the catalog lookup already
	const auto seqname = input.GetConstant(0);
	if (seqname.IsNull()) {
		return nullptr;
	}
	auto &binder = input.GetBinder();
	auto &seq = BindSequence(binder, Identifier(seqname.ToString()));
	return make_uniq<NextvalBindData>(seq);
}

void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data, const BoundScalarFunction &) {
	auto &next_val_bind_data = bind_data->Cast<NextvalBindData>();
	serializer.WritePropertyWithDefault(100, "sequence_create_info", next_val_bind_data.create_info);
}

unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &) {
	auto create_info = deserializer.ReadPropertyWithExplicitDefault<unique_ptr<CreateInfo>>(100, "sequence_create_info",
	                                                                                        unique_ptr<CreateInfo>());
	if (!create_info) {
		return nullptr;
	}
	auto &seq_info = create_info->Cast<CreateSequenceInfo>();
	auto &context = deserializer.Get<ClientContext &>();
	auto &sequence = BindSequenceFromContext(context, seq_info.GetQualifiedName());
	return make_uniq<NextvalBindData>(sequence);
}

void NextValModifiedDatabases(ClientContext &context, FunctionModifiedDatabasesInput &input) {
	if (!input.bind_data) {
		return;
	}
	auto &seq = input.bind_data->Cast<NextvalBindData>();
	input.properties.RegisterDBModify(seq.sequence.ParentCatalog(), context, DatabaseModificationType::SEQUENCE);
}

//! currval reads the sequence but holds a reference to its catalog entry, so a prepared statement must be rebound
//! when the catalog changes (e.g. the sequence is dropped) without marking the database as modified
void CurrValReadDatabases(ClientContext &context, FunctionModifiedDatabasesInput &input) {
	if (!input.bind_data) {
		return;
	}
	auto &seq = input.bind_data->Cast<NextvalBindData>();
	input.properties.RegisterDBRead(seq.sequence.ParentCatalog(), context);
}

} // namespace

ScalarFunction NextvalFun::GetFunction() {
	ScalarFunction next_val("nextval", {}, LogicalType::BIGINT, NextValFunction<NextSequenceValueOperator>, nullptr,
	                        nullptr);
	next_val.GetSignature().AddParameter("sequence_name", LogicalType::VARCHAR);
	next_val.SetBindCallback(NextValBind);
	next_val.SetSerializeCallback(Serialize);
	next_val.SetDeserializeCallback(Deserialize);
	next_val.SetModifiedDatabasesCallback(NextValModifiedDatabases);
	next_val.SetInitStateCallback(NextValLocalFunction);
	next_val.SetVolatile();
	next_val.SetFallible();
	next_val.SetRequiresOrderedExecution(true);
	return next_val;
}

ScalarFunction CurrvalFun::GetFunction() {
	ScalarFunction curr_val("currval", {}, LogicalType::BIGINT, NextValFunction<CurrentSequenceValueOperator>, nullptr,
	                        nullptr);
	curr_val.GetSignature().AddParameter("sequence_name", LogicalType::VARCHAR);
	curr_val.SetBindCallback(NextValBind);
	curr_val.SetSerializeCallback(Serialize);
	curr_val.SetDeserializeCallback(Deserialize);
	curr_val.SetModifiedDatabasesCallback(CurrValReadDatabases);
	curr_val.SetInitStateCallback(NextValLocalFunction);
	curr_val.SetVolatile();
	curr_val.SetFallible();
	return curr_val;
}

ScalarFunctionSet SetvalFun::GetFunctions() {
	ScalarFunction set_val("setval", {}, LogicalType::BIGINT, NextValFunction<SetValValueOperator>, nullptr, nullptr);
	set_val.GetSignature()
	    .AddParameter("sequence_name", LogicalType::VARCHAR)
	    .AddParameter("value", LogicalType::BIGINT);
	set_val.SetBindCallback(NextValBind);
	set_val.SetSerializeCallback(Serialize);
	set_val.SetDeserializeCallback(Deserialize);
	set_val.SetModifiedDatabasesCallback(NextValModifiedDatabases);
	set_val.SetInitStateCallback(NextValLocalFunction);
	set_val.SetVolatile();
	set_val.SetFallible();
	set_val.SetRequiresOrderedExecution(true);

	ScalarFunctionSet set_val_set;
	set_val_set.AddFunction(set_val);

	// Add an overload that takes an additional boolean parameter
	set_val.GetSignature().AddParameter("is_called", LogicalType::BOOLEAN);
	set_val_set.AddFunction(set_val);

	return set_val_set;
}

} // namespace duckdb
