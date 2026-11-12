//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/types/value.hpp"
#include <memory>

namespace duckdb {

class Catalog;
class CatalogSet;
class ClientContext;
class Deserializer;
class SchemaCatalogEntry;
class Serializer;
class Value;

struct AlterInfo;
struct CatalogTransaction;
struct CreateInfo;

//! Abstract base class of an entry in the catalog
class CatalogEntry {
public:
	CatalogEntry(CatalogType type, Catalog &catalog, Identifier name);
	CatalogEntry(CatalogType type, Identifier name, idx_t oid);
	virtual ~CatalogEntry();

	//! The oid of the entry
	idx_t oid;
	//! The type of this catalog entry
	CatalogType type;
	//! Reference to the catalog set this entry is stored in
	optional_ptr<CatalogSet> set;
	//! The name of the entry
	Identifier name;
	//! Whether or not the object is deleted
	bool deleted;
	//! Whether or not the object is temporary and should not be added to the WAL
	bool temporary;
	//! Whether or not the entry is an internal entry (cannot be deleted, not dumped, etc)
	bool internal;
	//! The name of the extension that registered this entry (empty for core entries)
	Identifier extension_name;
	//! Timestamp at which the catalog entry was created
	atomic<transaction_t> timestamp;
	//! (optional) comment on this entry
	Value comment;
	//! (optional) extra data associated with this entry
	InsertionOrderPreservingMap<string> tags;

private:
	//! Child entry
	unique_ptr<CatalogEntry> child;
	//! Parent entry (the node that dependents_map this node)
	atomic<CatalogEntry *> parent;

public:
	virtual unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info);
	virtual unique_ptr<CatalogEntry> AlterEntry(CatalogTransaction transaction, AlterInfo &info);
	virtual void UndoAlter(ClientContext &context, AlterInfo &info);
	virtual void Rollback(CatalogEntry &prev_entry);
	virtual void OnDrop();
	//! Whether this version breaks what dependents bound against previous rely on. Previous is any earlier
	//! version of this entry, possibly several alters back, so the answer has to hold across a chain of alters
	virtual bool InvalidatesDependents(const CatalogEntry &previous) const;

	virtual unique_ptr<CatalogEntry> Copy(ClientContext &context) const;

	virtual unique_ptr<CreateInfo> GetInfo() const;

	//! Sets the CatalogEntry as the new root entry (i.e. the newest entry)
	// this is called on a rollback to an AlterEntry
	virtual void SetAsRoot();

	//! Convert the catalog entry to a SQL string that can be used to re-construct the catalog entry
	virtual string ToSQL() const;

	virtual Catalog &ParentCatalog();
	virtual const Catalog &ParentCatalog() const;
	virtual SchemaCatalogEntry &ParentSchema();
	virtual const SchemaCatalogEntry &ParentSchema() const;

	virtual void Verify(Catalog &catalog);

	void Serialize(Serializer &serializer) const;
	static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

public:
	void SetChild(unique_ptr<CatalogEntry> child);
	unique_ptr<CatalogEntry> TakeChild();
	bool HasChild() const;
	bool HasParent() const;
	CatalogEntry &Child();
	CatalogEntry &Parent();
	const CatalogEntry &Parent() const;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class InCatalogEntry : public CatalogEntry {
public:
	InCatalogEntry(CatalogType type, Catalog &catalog, Identifier name);
	~InCatalogEntry() override;

	//! The catalog the entry belongs to
	Catalog &catalog;

public:
	Catalog &ParentCatalog() override {
		return catalog;
	}
	const Catalog &ParentCatalog() const override {
		return catalog;
	}

	void Verify(Catalog &catalog) override;
};

} // namespace duckdb
