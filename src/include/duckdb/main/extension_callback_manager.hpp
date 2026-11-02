//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension_callback_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;
class DialectExtension;
class ExtensionCallback;
class OperatorExtension;
class OptimizerExtension;
class GrammarExtension;
class ParserExtension;
class PlannerExtension;
class ProfilerExtension;
class StorageExtension;
struct ExtensionCallbackRegistry;

template <class T>
class ExtensionCallbackIteratorHelper;

class ExtensionCallbackManager {
public:
	ExtensionCallbackManager();
	~ExtensionCallbackManager();

	void AddExtensionSchema(const Identifier &schema);
	vector<string> GetExtensionSchemas() const;

	static ExtensionCallbackManager &Get(ClientContext &context);
	static ExtensionCallbackManager &Get(DatabaseInstance &db);
	static const ExtensionCallbackManager &Get(const ClientContext &context);

	void Register(ParserExtension extension);
	void Register(shared_ptr<GrammarExtension> change);
	void Register(shared_ptr<DialectExtension> extension);
	void Register(PlannerExtension extension);
	void Register(OptimizerExtension extension);
	void Register(shared_ptr<OperatorExtension> extension);
	void Register(const string &name, shared_ptr<StorageExtension> extension);
	void Register(shared_ptr<ExtensionCallback> extension);
	void Register(const string &name, shared_ptr<ProfilerExtension> extension);

	ExtensionCallbackIteratorHelper<shared_ptr<OperatorExtension>> OperatorExtensions() const;
	ExtensionCallbackIteratorHelper<OptimizerExtension> OptimizerExtensions() const;
	ExtensionCallbackIteratorHelper<ParserExtension> ParserExtensions() const;
	ExtensionCallbackIteratorHelper<shared_ptr<DialectExtension>> DialectExtensions() const;
	ExtensionCallbackIteratorHelper<PlannerExtension> PlannerExtensions() const;
	ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>> ExtensionCallbacks() const;
	optional_ptr<StorageExtension> FindStorageExtension(const string &name) const;
	optional_ptr<GrammarExtension> FindGrammarExtension(const string &name) const;
	case_insensitive_map_t<shared_ptr<GrammarExtension>> GrammarExtensions() const;
	optional_ptr<ProfilerExtension> FindProfilerExtension(const string &name) const;
	bool HasParserExtensions() const;
	optional_ptr<DialectExtension> GetDialectExtension(const string &name) const;

private:
	mutex registry_lock;
	shared_ptr<ExtensionCallbackRegistry> callback_registry;
	vector<string> extension_schemas;
};

template <class T>
class ExtensionCallbackIteratorHelper {
public:
	ExtensionCallbackIteratorHelper(const vector<T> &vec, shared_ptr<ExtensionCallbackRegistry> callback_registry);
	~ExtensionCallbackIteratorHelper();

private:
	const vector<T> &vec;
	shared_ptr<ExtensionCallbackRegistry> callback_registry;

public:
	typename vector<T>::const_iterator begin() { // NOLINT: match stl API
		return vec.cbegin();
	}
	typename vector<T>::const_iterator end() { // NOLINT: match stl API
		return vec.cend();
	}
};

} // namespace duckdb
