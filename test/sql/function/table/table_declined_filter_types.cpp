#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

const idx_t ROW_COUNT = 3;

//! The scans below decline every filter, so the planner re-creates the filters above them
bool DeclineFilter(const FunctionData &, idx_t) {
	return false;
}

struct DeclinedFilterScanState : public GlobalTableFunctionState {
	explicit DeclinedFilterScanState(const TableFunctionInitInput &input) {
		if (input.projection_ids.empty()) {
			output_columns = input.column_indexes;
			return;
		}
		for (auto &projection_id : input.projection_ids) {
			output_columns.push_back(input.column_indexes[projection_id]);
		}
	}

	//! The columns the scan emits, in order
	vector<ColumnIndex> output_columns;
	//! The first row of the next chunk
	idx_t offset = 0;
};

unique_ptr<GlobalTableFunctionState> DeclinedFilterInit(ClientContext &, TableFunctionInitInput &input) {
	return make_uniq<DeclinedFilterScanState>(input);
}

//! A table whose scan has no get_virtual_columns, so its rowid comes from TableCatalogEntry::GetVirtualColumns,
//! as in the SQLite scanner. Rows are (i, rowid): (10, 0), (20, 1), (30, 2).
class DeclinedFilterTableEntry : public TableCatalogEntry {
public:
	DeclinedFilterTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, bool filter_prune)
	    : TableCatalogEntry(catalog, schema, info), table_columns(info.columns.Copy()), filter_prune(filter_prune) {
	}

	struct BindData : public TableFunctionData {
		explicit BindData(TableCatalogEntry &table) : table(table) {
		}

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindData>(table);
		}
		bool Equals(const FunctionData &other) const override {
			return &table == &other.Cast<BindData>().table;
		}

		TableCatalogEntry &table;
	};

	const ColumnList &GetColumns() const override {
		return table_columns;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		return nullptr;
	}
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		bind_data = make_uniq<BindData>(*this);
		TableFunction function(Identifier("declined_filter_table_scan"), {}, Scan, nullptr, DeclinedFilterInit);
		function.projection_pushdown = true;
		function.filter_pushdown = true;
		function.filter_prune = filter_prune;
		function.supports_pushdown_type = DeclineFilter;
		function.get_table_entry = GetTableEntry;
		return function;
	}
	TableStorageInfo GetStorageInfo(ClientContext &context) override {
		return TableStorageInfo();
	}

private:
	static optional_ptr<TableCatalogEntry> GetTableEntry(optional_ptr<const FunctionData> bind_data) {
		return bind_data->Cast<BindData>().table;
	}

	static void Scan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
		auto &state = data.global_state->Cast<DeclinedFilterScanState>();
		if (state.offset == ROW_COUNT) {
			output.SetChildCardinality(0);
			return;
		}
		auto count = MinValue<idx_t>(ROW_COUNT - state.offset, STANDARD_VECTOR_SIZE);
		for (idx_t col_idx = 0; col_idx < state.output_columns.size(); col_idx++) {
			auto is_rowid = state.output_columns[col_idx].IsRowIdColumn();
			auto writer = FlatVector::Writer<int64_t>(output.data[col_idx], count);
			for (idx_t row = state.offset; row < state.offset + count; row++) {
				auto rowid = NumericCast<int64_t>(row);
				writer.WriteValue(is_rowid ? rowid : (rowid + 1) * 10);
			}
		}
		state.offset += count;
		output.SetChildCardinality(count);
	}

private:
	ColumnList table_columns;
	bool filter_prune;
};

//! The main schema of the attached catalog, with tables t (filter_prune) and t_no_prune
class DeclinedFilterSchemaEntry : public DuckSchemaEntry {
public:
	DeclinedFilterSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : DuckSchemaEntry(catalog, info) {
		AddTable(catalog, "t", true);
		AddTable(catalog, "t_no_prune", false);
	}

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
	                                       const EntryLookupInfo &lookup_info) override {
		if (lookup_info.GetCatalogType() == CatalogType::TABLE_ENTRY) {
			for (auto &table : scan_tables) {
				if (table->name == lookup_info.GetEntryName()) {
					return table.get();
				}
			}
		}
		return DuckSchemaEntry::LookupEntry(transaction, lookup_info);
	}

private:
	void AddTable(Catalog &catalog, const char *name, bool filter_prune) {
		CreateTableInfo info(*this, Identifier(name));
		info.columns.AddColumn(ColumnDefinition(Identifier("i"), LogicalType::BIGINT));
		scan_tables.push_back(make_uniq<DeclinedFilterTableEntry>(catalog, *this, info, filter_prune));
	}

private:
	vector<unique_ptr<DeclinedFilterTableEntry>> scan_tables;
};

class DeclinedFilterCatalog : public DuckCatalog {
public:
	explicit DeclinedFilterCatalog(AttachedDatabase &db) : DuckCatalog(db) {
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName({Identifier::DefaultSchema()}, Identifier()));
		main_schema = make_uniq<DeclinedFilterSchemaEntry>(*this, info);
	}

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override {
		if (main_schema->name == schema_lookup.GetEntryName()) {
			return main_schema.get();
		}
		return DuckCatalog::LookupSchema(transaction, schema_lookup, if_not_found);
	}

private:
	unique_ptr<DeclinedFilterSchemaEntry> main_schema;
};

struct DeclinedFilterStorageExtension : StorageExtension {
	DeclinedFilterStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, DeclinedFilterCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

//! One STRUCT(a INTEGER, b VARCHAR) column s with rows {a: 1, b: x1} to {a: 3, b: x3}. Supports pushing down
//! extracts of s.a and s.b.
struct DeclinedStructScan {
	static unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
	                                     vector<Identifier> &names) {
		return_types.push_back(LogicalType::STRUCT({{"a", LogicalType::INTEGER}, {"b", LogicalType::VARCHAR}}));
		names.emplace_back("s");
		return make_uniq<TableFunctionData>();
	}

	static void Scan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
		static const char *const B_VALUES[] = {"x1", "x2", "x3"};
		auto &state = data.global_state->Cast<DeclinedFilterScanState>();
		if (state.offset == ROW_COUNT) {
			output.SetChildCardinality(0);
			return;
		}
		auto count = MinValue<idx_t>(ROW_COUNT - state.offset, STANDARD_VECTOR_SIZE);
		for (idx_t col_idx = 0; col_idx < state.output_columns.size(); col_idx++) {
			auto &column = state.output_columns[col_idx];
			auto &result = output.data[col_idx];
			if (!column.IsPushdownExtract()) {
				auto writer = FlatVector::Writer<VectorStructType<int32_t, string_t>>(result, count);
				for (idx_t row = state.offset; row < state.offset + count; row++) {
					writer.WriteValue([&](auto &a_writer, auto &b_writer) {
						a_writer.WriteValue(NumericCast<int32_t>(row + 1));
						b_writer.WriteValue(string_t(B_VALUES[row]));
					});
				}
			} else if (column.GetChildIndex(0).GetPrimaryIndex() == 0) {
				auto writer = FlatVector::Writer<int32_t>(result, count);
				for (idx_t row = state.offset; row < state.offset + count; row++) {
					writer.WriteValue(NumericCast<int32_t>(row + 1));
				}
			} else {
				auto writer = FlatVector::Writer<string_t>(result, count);
				for (idx_t row = state.offset; row < state.offset + count; row++) {
					writer.WriteValue(string_t(B_VALUES[row]));
				}
			}
		}
		state.offset += count;
		output.SetChildCardinality(count);
	}

	static bool SupportsPushdownExtract(const FunctionData &, const LogicalIndex &) {
		return true;
	}

	static void Register(Connection &con) {
		con.BeginTransaction();
		TableFunction function(Identifier("declined_struct_scan"), {}, Scan, Bind, DeclinedFilterInit);
		function.projection_pushdown = true;
		function.filter_pushdown = true;
		function.filter_prune = true;
		function.supports_pushdown_extract = SupportsPushdownExtract;
		function.supports_pushdown_type = DeclineFilter;
		CreateTableFunctionInfo info(function);
		Catalog::GetSystemCatalog(*con.context).CreateTableFunction(*con.context, info);
		con.Commit();
	}
};

//! Ten rows of (id, total, region), or (region, id, total) when called with true. Declines filters on region. Emits
//! projection_ids order only when CanRemoveFilterColumns allows it, column_ids order otherwise.
struct DeclinedRegionScan {
	struct BindData : public TableFunctionData {
		BindData(vector<string> names_p, bool projection_pushdown_p)
		    : names(std::move(names_p)), projection_pushdown(projection_pushdown_p) {
		}

		vector<string> names;
		bool projection_pushdown;
	};

	struct GlobalState : public GlobalTableFunctionState {
		vector<column_t> column_ids;
		vector<idx_t> projection_ids;
		idx_t offset = 0;
	};

	static unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
		auto region_first = input.inputs[0].GetValue<bool>();
		vector<string> column_names =
		    region_first ? vector<string> {"region", "id", "total"} : vector<string> {"id", "total", "region"};
		for (auto &name : column_names) {
			return_types.push_back(name == "region" ? LogicalType::VARCHAR : LogicalType::BIGINT);
			names.emplace_back(name);
		}
		return make_uniq<BindData>(std::move(column_names), input.table_function.projection_pushdown);
	}

	static unique_ptr<GlobalTableFunctionState> GlobalInit(ClientContext &, TableFunctionInitInput &input) {
		auto &bind_data = input.bind_data->Cast<BindData>();
		auto state = make_uniq<GlobalState>();
		if (!bind_data.projection_pushdown) {
			state->column_ids = {0, 1, 2};
		} else {
			state->column_ids = input.column_ids;
			if (input.CanRemoveFilterColumns()) {
				state->projection_ids = input.projection_ids;
			}
		}
		return std::move(state);
	}

	static void Function(ClientContext &, TableFunctionInput &data, DataChunk &output) {
		auto &bind_data = data.bind_data->Cast<BindData>();
		auto &state = data.global_state->Cast<GlobalState>();
		const idx_t row_count = 10;
		if (state.offset >= row_count) {
			output.SetChildCardinality(0);
			return;
		}
		const auto batch_count = MinValue<idx_t>(row_count - state.offset, output.GetCapacity());
		const auto output_count = state.projection_ids.empty() ? state.column_ids.size() : state.projection_ids.size();
		for (idx_t output_idx = 0; output_idx < output_count; output_idx++) {
			auto scan_idx = state.projection_ids.empty() ? output_idx : state.projection_ids[output_idx];
			auto &name = bind_data.names[state.column_ids[scan_idx]];
			if (name == "region") {
				auto writer = FlatVector::Writer<string_t>(output.data[output_idx], batch_count);
				for (idx_t row = 0; row < batch_count; row++) {
					writer.WriteValue(string_t((state.offset + row + 1) % 2 == 0 ? "b" : "a"));
				}
				continue;
			}
			auto writer = FlatVector::Writer<int64_t>(output.data[output_idx], batch_count);
			for (idx_t row = 0; row < batch_count; row++) {
				auto id = NumericCast<int64_t>(state.offset + row + 1);
				writer.WriteValue(name == "id" ? id : id * 10);
			}
		}
		state.offset += batch_count;
		output.SetChildCardinality(batch_count);
	}

	static bool SupportsPushdownType(const FunctionData &bind_data, idx_t column_idx) {
		return bind_data.Cast<BindData>().names[column_idx] != "region";
	}

	static void Register(Connection &con, const string &name, bool filter_prune, bool projection_pushdown) {
		con.BeginTransaction();
		TableFunction function(Identifier(name), {LogicalType::BOOLEAN}, Function, Bind, GlobalInit);
		function.projection_pushdown = projection_pushdown;
		function.filter_pushdown = true;
		function.filter_prune = filter_prune;
		function.supports_pushdown_type = SupportsPushdownType;
		CreateTableFunctionInfo info(function);
		Catalog::GetSystemCatalog(*con.context).CreateTableFunction(*con.context, info);
		con.Commit();
	}
};

void RequireColumns(Connection &con, const string &query, const vector<vector<Value>> &columns) {
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == columns.size());
	for (idx_t column_idx = 0; column_idx < columns.size(); column_idx++) {
		REQUIRE(CHECK_COLUMN(result, column_idx, columns[column_idx]));
	}
}

} // namespace

TEST_CASE("Declined filters on the rowid of a table whose scan has no virtual columns", "[tablefunction]") {
	DBConfig config;
	StorageExtension::Register(config, "declined_filter_test", make_shared_ptr<DeclinedFilterStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH 'test' AS s (TYPE declined_filter_test)"));

	for (string table : {"s.t", "s.t_no_prune"}) {
		// rowid only filtered, and also selected
		RequireColumns(con, "SELECT i FROM " + table + " WHERE rowid = 1", {{Value::BIGINT(20)}});
		RequireColumns(con, "SELECT rowid, i FROM " + table + " WHERE rowid = 1",
		               {{Value::BIGINT(1)}, {Value::BIGINT(20)}});
		RequireColumns(con, "SELECT i FROM " + table + " WHERE rowid >= 1 AND i < 30", {{Value::BIGINT(20)}});
	}
}

TEST_CASE("Declined filters on pushed-down struct fields", "[tablefunction]") {
	DuckDB db(nullptr);
	Connection con(db);
	DeclinedStructScan::Register(con);

	// filtered field selected, not selected, and selected next to another field
	RequireColumns(con, "SELECT s.a FROM declined_struct_scan() WHERE s.a = 2", {{Value::INTEGER(2)}});
	RequireColumns(con, "SELECT s.b FROM declined_struct_scan() WHERE s.a = 2", {{Value("x2")}});
	RequireColumns(con, "SELECT s.a, s.b FROM declined_struct_scan() WHERE s.b = 'x3'",
	               {{Value::INTEGER(3)}, {Value("x3")}});
}

TEST_CASE("Declined filters on columns the query does not select", "[tablefunction]") {
	DuckDB db(nullptr);
	Connection con(db);
	DeclinedRegionScan::Register(con, "declined_region_scan", true, true);
	DeclinedRegionScan::Register(con, "declined_region_scan_no_prune", false, true);
	DeclinedRegionScan::Register(con, "declined_region_scan_no_projection", false, false);

	const vector<Value> ids {2, 4, 6, 8, 10};
	const vector<Value> totals {20, 40, 60, 80, 100};
	const vector<Value> regions {"b", "b", "b", "b", "b"};

	// filter column not selected, selected, and output reordered
	RequireColumns(con, "SELECT id, total FROM declined_region_scan(false) WHERE region = 'b' ORDER BY id",
	               {ids, totals});
	RequireColumns(con, "SELECT region, id FROM declined_region_scan(false) WHERE region = 'b' ORDER BY id",
	               {regions, ids});
	RequireColumns(con, "SELECT total, id FROM declined_region_scan(false) WHERE region = 'b' ORDER BY id",
	               {totals, ids});
	// the filter column completes a reordered projection of the scan's full width
	RequireColumns(con, "SELECT total FROM declined_region_scan(true) WHERE region = 'b' ORDER BY total", {totals});
	RequireColumns(con, "SELECT id, total FROM declined_region_scan_no_prune(false) WHERE region = 'b' ORDER BY id",
	               {ids, totals});
	RequireColumns(con, "SELECT total FROM declined_region_scan_no_projection(false) WHERE region = 'b' ORDER BY total",
	               {totals});
	RequireColumns(
	    con, "SELECT region, total FROM declined_region_scan_no_projection(true) WHERE region = 'b' ORDER BY total",
	    {regions, totals});
	// a hash join builds from the filtered scan
	RequireColumns(
	    con,
	    "SELECT f.total FROM declined_region_scan(false) f JOIN declined_region_scan(false) d ON f.id = d.id "
	    "WHERE d.region = 'b' ORDER BY f.total",
	    {totals});
}
