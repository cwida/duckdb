#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/planner/binder.hpp"

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_connection_describe_table(duckdb_v2_connection_handle conn, duckdb_v2_qname_handle name,
                                                    duckdb_v2_table_description_handle *desc,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(desc);
	*desc = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		auto &qname = *Convert(name);
		auto &path = qname.Path();
		// The qname invariant guarantees one to three non-empty parts. A two-part name is read as SQL reads it: the
		// first part tries as a schema and as an attached database, and BindSchemaOrCatalog rejects the ambiguous case.
		auto catalog = duckdb::Identifier::InvalidCatalog();
		auto schema = duckdb::Identifier::InvalidSchema();
		if (path.size() == 3) {
			catalog = path[0];
			schema = path[1];
		} else if (path.size() == 2) {
			schema = path[0];
			// The attached-database lookup reads catalog state and needs a transaction.
			context.RunFunctionInTransaction([&]() { duckdb::Binder::BindSchemaOrCatalog(context, catalog, schema); });
		}
		auto description = context.TableInfo(catalog, schema, path.back());
		if (!description) {
			throw duckdb::CatalogException("Table with name %s does not exist!", qname.ToString());
		}
		*desc = Convert(description.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_description_get_qname(duckdb_v2_table_description_handle desc,
                                                      duckdb_v2_qname_handle *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(desc);
	DUCKDB_CHECK_ARG(name);
	*name = nullptr;
	return WithErrorHandler(err, [&]() { *name = Convert(new duckdb::QualifiedName(Convert(desc)->qualified_name)); });
}

DUCKDB_V2_ERROR duckdb_v2_table_description_get_column_count(duckdb_v2_table_description_handle desc, idx_t *count,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(desc);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(desc)->columns.size(); });
}

DUCKDB_V2_ERROR duckdb_v2_table_description_get_column(duckdb_v2_table_description_handle desc, idx_t index,
                                                       duckdb_v2_column_description_handle *column,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(desc);
	DUCKDB_CHECK_ARG(column);
	*column = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &columns = Convert(desc)->columns;
		if (index >= columns.size()) {
			throw duckdb::Exception(
			    duckdb::ExceptionType::OUT_OF_RANGE,
			    duckdb::StringUtil::Format("Column index %llu is out of range for a table with %llu "
			                               "columns in duckdb_v2_table_description_get_column.",
			                               static_cast<uint64_t>(index), static_cast<uint64_t>(columns.size())));
		}
		*column = Convert(new duckdb::ColumnDefinition(columns[index].Copy()));
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_description_is_readonly(duckdb_v2_table_description_handle desc, bool *readonly,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(desc);
	DUCKDB_CHECK_ARG(readonly);
	return WithErrorHandler(err, [&]() { *readonly = Convert(desc)->readonly; });
}

DUCKDB_V2_ERROR duckdb_v2_table_description_destroy(duckdb_v2_table_description_handle *desc) {
	return WithErrorHandler(nullptr, [&]() {
		if (!desc) {
			return;
		}
		if (*desc) {
			delete Convert(*desc);
			*desc = nullptr;
		}
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Column Description
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_column_description_get_name(duckdb_v2_column_description_handle column,
                                                      duckdb_v2_identifier_t *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(column);
	DUCKDB_CHECK_ARG(name);
	*name = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() { *name = Convert(Convert(column)->Name()); });
}

DUCKDB_V2_ERROR duckdb_v2_column_description_get_type(duckdb_v2_column_description_handle column,
                                                      duckdb_v2_logical_type_handle *type,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(column);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	// Borrowed: the column definition owns the type for as long as the description lives.
	return WithErrorHandler(err, [&]() { *type = ConvertBorrowed(Convert(column)->Type()); });
}

DUCKDB_V2_ERROR duckdb_v2_column_description_has_default(duckdb_v2_column_description_handle column, bool *has_default,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(column);
	DUCKDB_CHECK_ARG(has_default);
	return WithErrorHandler(err, [&]() {
		auto &definition = *Convert(column);
		*has_default = !definition.Generated() && definition.HasDefaultValue();
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_description_has_generated(duckdb_v2_column_description_handle column,
                                                           bool *has_generated, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(column);
	DUCKDB_CHECK_ARG(has_generated);
	return WithErrorHandler(err, [&]() { *has_generated = Convert(column)->Generated(); });
}

DUCKDB_V2_ERROR duckdb_v2_column_description_destroy(duckdb_v2_column_description_handle *column) {
	return WithErrorHandler(nullptr, [&]() {
		if (!column) {
			return;
		}
		if (*column) {
			delete Convert(*column);
			*column = nullptr;
		}
	});
}
