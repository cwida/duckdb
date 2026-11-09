//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension_callback_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
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
	//! Monotonic version bumped on every grammar extension registration, so sessions can detect
	//! that their composed grammar is stale (e.g. after a LOAD mid-session)
	idx_t GrammarRegistryVersion() const {
		return grammar_registry_version.load();
	}
	optional_ptr<ProfilerExtension> FindProfilerExtension(const string &name) const;
	bool HasParserExtensions() const;
	optional_ptr<DialectExtension> GetDialectExtension(const string &name) const;

private:
	shared_ptr<ExtensionCallbackRegistry> CopyRegistry() const;
	void PublishRegistry(shared_ptr<ExtensionCallbackRegistry> new_registry);

private:
	mutable mutex registry_lock;
	//! The current registry, read without locking or reference counting
	atomic<ExtensionCallbackRegistry *> callback_registry;
	//! Every published registry, kept alive so that readers of an older one stay valid
	vector<shared_ptr<ExtensionCallbackRegistry>> registry_versions;
	vector<string> extension_schemas;
	atomic<idx_t> grammar_registry_version {0};
};

template <class T>
class ExtensionCallbackIteratorHelper {
public:
	explicit ExtensionCallbackIteratorHelper(const vector<T> &vec);
	~ExtensionCallbackIteratorHelper();

private:
	const vector<T> &vec;

public:
	typename vector<T>::const_iterator begin() { // NOLINT: match stl API
		return vec.cbegin();
	}
	typename vector<T>::const_iterator end() { // NOLINT: match stl API
		return vec.cend();
	}
};

} // namespace duckdb
