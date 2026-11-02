//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/function_set_base.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function.hpp"

namespace duckdb {

//! The overloads of a function set, regardless of the kind of function - overload selection only needs their
//! signatures
class BaseFunctionSet {
public:
	virtual ~BaseFunctionSet() {
	}
	virtual auto GetOverload(idx_t offset) const -> const SimpleFunction & = 0;
	virtual auto GetSharedOverload(idx_t offset) const -> shared_ptr<const SimpleFunction> = 0;
	virtual auto GetOverloadCount() const -> idx_t = 0;
	virtual auto GetName() const -> const Identifier & = 0;
};

template <class T>
class FunctionSet : public BaseFunctionSet {
public:
	explicit FunctionSet(Identifier name) : name(std::move(name)) { // NOLINT
	}

	//! The name of the function set
	Identifier name;

public:
	void SetName(Identifier name_p) {
		name = std::move(name_p);
	}
	//! The set of functions. The overloads are immutable and shared with every function bound from them - use
	//! ApplyToFunctions() to change them.
	vector<shared_ptr<const T>> functions;

public:
	auto AddFunction(T function) & -> FunctionSet<T> & {
		functions.push_back(make_shared_ptr<T>(std::move(function)));
		return *this;
	}
	void AddFunction(shared_ptr<const T> function) {
		functions.push_back(std::move(function));
	}
	idx_t Size() {
		return functions.size();
	}

	const shared_ptr<const T> &GetFunctionByOffset(idx_t offset) const {
		D_ASSERT(offset < functions.size());
		return functions[offset];
	}

	auto GetName() const -> const Identifier & override {
		return name;
	}
	auto GetOverload(idx_t offset) const -> const SimpleFunction & override {
		D_ASSERT(offset < functions.size());
		return *functions[offset];
	}
	auto GetSharedOverload(idx_t offset) const -> shared_ptr<const SimpleFunction> override {
		D_ASSERT(offset < functions.size());
		return functions[offset];
	}
	auto GetOverloadCount() const -> idx_t override {
		return functions.size();
	}

	//! Apply a modification to every overload in the set. The overloads are immutable once bound from, so each is
	//! replaced by a modified copy rather than being changed in place.
	template <class FUNC>
	void ApplyToFunctions(FUNC &&callback) {
		for (auto &function : functions) {
			auto modified = make_shared_ptr<T>(*function);
			callback(*modified);
			function = std::move(modified);
		}
	}

	bool MergeFunctionSet(const FunctionSet<T> &new_functions, bool override = false) {
		D_ASSERT(!new_functions.functions.empty());
		for (auto &new_func : new_functions.functions) {
			bool overwritten = false;
			for (auto &func : functions) {
				// the same rule for every kind of function - see FunctionSignature::IsSameOverload
				if (new_func->GetSignature().IsSameOverload(func->GetSignature())) {
					// function overload already exists
					if (override) {
						// override it
						overwritten = true;
						func = new_func;
					} else {
						// throw an error
						return false;
					}
					break;
				}
			}
			if (!overwritten) {
				functions.push_back(new_func);
			}
		}
		return true;
	}
};

} // namespace duckdb
