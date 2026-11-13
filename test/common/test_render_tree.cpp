#include "catch.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/tree_renderer/graphviz_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/html_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/json_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/mermaid_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/text_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/yaml_tree_renderer.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

using namespace duckdb;

namespace {

class TestLogicalSubPlan : public LogicalOperator {
public:
	explicit TestLogicalSubPlan(string name_p)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_INVALID), name(std::move(name_p)) {
	}

	TestLogicalSubPlan(string name_p, string label_p, duckdb::unique_ptr<LogicalOperator> sub_plan_p)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_INVALID), name(std::move(name_p)), label(std::move(label_p)),
	      sub_plan(std::move(sub_plan_p)) {
	}

	string GetName() const override {
		return name;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		return {};
	}

	duckdb::vector<ExplainSubPlan> GetExplainSubPlans() const override {
		duckdb::vector<ExplainSubPlan> result;
		if (sub_plan) {
			result.push_back({label, RenderTree::CreateRenderTree(*sub_plan)});
		}
		return result;
	}

protected:
	void ResolveTypes() override {
	}

private:
	string name;
	string label;
	duckdb::unique_ptr<LogicalOperator> sub_plan;
};

class TestPhysicalSubPlan : public PhysicalOperator {
public:
	TestPhysicalSubPlan(PhysicalPlan &physical_plan, string name_p)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::INVALID, {}, 0), name(std::move(name_p)) {
	}

	TestPhysicalSubPlan(PhysicalPlan &physical_plan, string name_p, string label_p,
	                    duckdb::unique_ptr<LogicalOperator> sub_plan_p)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::INVALID, {}, 0), name(std::move(name_p)),
	      label(std::move(label_p)), sub_plan(std::move(sub_plan_p)) {
	}

	string GetName() const override {
		return name;
	}

	duckdb::vector<ExplainSubPlan> GetExplainSubPlans() const override {
		duckdb::vector<ExplainSubPlan> result;
		if (sub_plan) {
			result.push_back({label, RenderTree::CreateRenderTree(*sub_plan)});
		}
		return result;
	}

private:
	string name;
	string label;
	duckdb::unique_ptr<LogicalOperator> sub_plan;
};

} // namespace

TEST_CASE("EXPLAIN renderers show nested operator-owned sub-plans", "[render_tree]") {
	auto nested = make_uniq<TestLogicalSubPlan>("NESTED_SCAN");
	auto remote = make_uniq<TestLogicalSubPlan>("REMOTE_FILTER", "Nested fragment", std::move(nested));
	PhysicalPlan physical_plan(Allocator::DefaultAllocator());
	TestPhysicalSubPlan root(physical_plan, "REMOTE_EXECUTION", "Remote fragment", std::move(remote));

	duckdb::vector<std::pair<string, string>> outputs;
	outputs.emplace_back("text", TextTreeRenderer().ToString(root));
	outputs.emplace_back("json", JSONTreeRenderer().ToString(root));
	outputs.emplace_back("yaml", YAMLTreeRenderer().ToString(root));
	outputs.emplace_back("html", HTMLTreeRenderer().ToString(root));
	outputs.emplace_back("graphviz", GRAPHVIZTreeRenderer().ToString(root));
	outputs.emplace_back("mermaid", MermaidTreeRenderer().ToString(root));

	for (auto &output : outputs) {
		CAPTURE(output.first);
		REQUIRE(StringUtil::Contains(output.second, "Remote fragment"));
		REQUIRE(StringUtil::Contains(output.second, "REMOTE_FILTER"));
		REQUIRE(StringUtil::Contains(output.second, "Nested fragment"));
		REQUIRE(
		    (StringUtil::Contains(output.second, "NESTED_SCAN") || StringUtil::Contains(output.second, "Nested Scan")));
	}
}

TEST_CASE("Operators without sub-plans keep the existing JSON shape", "[render_tree]") {
	PhysicalPlan physical_plan(Allocator::DefaultAllocator());
	TestPhysicalSubPlan root(physical_plan, "LEAF");

	REQUIRE(JSONTreeRenderer().ToString(root) == R"([
    {
        "name": "LEAF",
        "children": [],
        "extra_info": {}
    }
])");
}
