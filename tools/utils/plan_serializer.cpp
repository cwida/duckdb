
#include "duckdb.hpp"
#include "duckdb/common/serializer/buffered_file_reader.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"

#include <fstream>

// This tool can be used to serialize and deserialize plans logical plans
// It takes a file with SQL statements as input, executes all statements except the last one and then either
// - serializes the plan of the last statement into the <plan_file> (if mode is "serialize")
// - deserializes the <plan_file> and executes it, comparing the results to executing the last statement directly.

int main(int argc, char **argv) {
	bool serialize = true;
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <mode>\n", argv[0]);
		return 1;
	}

	std::string source_location;
	std::string target_location;

	if (argc < 4) {
		fprintf(stderr, "Usage: %s <mode> <sql_file> <plan_file>\n", argv[0]);
		return 1;
	}
	if (duckdb::StringUtil::CIEquals(argv[1], "serialize") || duckdb::StringUtil::CIEquals(argv[1], "s")) {
		serialize = true;
	} else if (duckdb::StringUtil::CIEquals(argv[1], "deserialize") || duckdb::StringUtil::CIEquals(argv[1], "d")) {
		serialize = false;
	} else {
		fprintf(stderr, "Invalid mode: %s. Use 's, serialize' or 'd, deserialize'.\n", argv[1]);
		return 1;
	}

	source_location = argv[2];
	target_location = argv[3];

	duckdb::DuckDB db;
	duckdb::Connection con(db);

	// Collect all statements
	std::vector<std::string> statements;
	std::ifstream file(source_location);
	std::string line;
	while (std::getline(file, line)) {
		if (line.empty()) {
			continue;
		}
		statements.push_back(line);
	}

	// Now execute all statements, except the last one which we will serialize
	for (duckdb::idx_t i = 0; i < statements.size() - 1; i++) {
		auto result = con.Query(statements[i]);
		if (result->HasError()) {
			fprintf(stderr, "Error executing statement %s\n", result->GetError().c_str());
			return 1;
		}
	}

	auto target_stmt = statements.back();

	// Serialize the statement
	if (serialize) {
		duckdb::BufferedFileWriter target(db.GetFileSystem(), target_location);

		con.BeginTransaction();
		duckdb::Parser p(*con.context);
		p.ParseQuery(target_stmt);

		duckdb::Planner planner(*con.context);

		planner.CreatePlan(std::move(p.statements[0]));
		auto plan = std::move(planner.plan);

		duckdb::BinarySerializer serializer(target);
		serializer.Begin();
		plan->Serialize(serializer);
		serializer.End();

		con.Rollback();

		target.Sync();

		printf("Serialization successful!\n");
		printf("Serialized statement: \n%s\n", target_stmt.c_str());
		printf("Serialized plan written to: %s\n", target_location.c_str());
	}
	// Deserialize the statement and execute
	else {
		duckdb::BufferedFileReader file_source(db.GetFileSystem(), target_location.c_str());

		con.BeginTransaction();

		duckdb::BinaryDeserializer deserializer(file_source);
		deserializer.Set<duckdb::ClientContext &>(*con.context);
		deserializer.Begin();
		auto deserialized_plan = duckdb::LogicalOperator::Deserialize(deserializer);
		deserializer.End();

		deserialized_plan->ResolveOperatorTypes();

		auto deserialized_results = con.context->Query(
		    duckdb::make_uniq<duckdb::LogicalPlanStatement>(std::move(deserialized_plan)), duckdb::QueryParameters());
		if (deserialized_results->HasError()) {
			fprintf(stderr, "Error executing deserialized plan: %s\n", deserialized_results->GetError().c_str());
			return 1;
		}

		con.Rollback();

		// Now execute the original statement as well and compare results
		con.BeginTransaction();
		duckdb::Parser p(*con.context);
		p.ParseQuery(target_stmt);
		duckdb::Planner planner(*con.context);
		planner.CreatePlan(std::move(p.statements[0]));
		auto expected_plan = std::move(planner.plan);
		expected_plan->ResolveOperatorTypes();
		auto expected_results = con.Query(target_stmt);
		if (expected_results->HasError()) {
			fprintf(stderr, "Error executing expected plan: %s\n", expected_results->GetError().c_str());
			return 1;
		}

		// ignore names
		if (!deserialized_results->Equals(*expected_results, false)) {
			fprintf(stderr, "-----------------------------------\n");
			fprintf(stderr, "Deserialized result does not match!\n");
			fprintf(stderr, "-----------------------------------\n");
			fprintf(stderr, "Query: %s\n", target_stmt.c_str());
			fprintf(stderr, "-------------Deserialized----------\n");
			deserialized_results->Print();
			fprintf(stderr, "---------------Expected------------\n");
			expected_results->Print();
			fprintf(stderr, "-----------------------------------\n");
			return 1;
		}

		con.Rollback();

		// Write the results to the result location
		printf("Deserialization successful!");
	}
}
