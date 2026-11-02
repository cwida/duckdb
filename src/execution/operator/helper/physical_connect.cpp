#include "duckdb/execution/operator/helper/physical_connect.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/execution/operator/helper/launch_external_resource.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/main/database_path_and_type.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

SourceResultType PhysicalConnect::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                  OperatorSourceInput &input) const {
	// The 4 grammar forms are accepted at parse time. `CONNECT LOCAL` and bare `CONNECT` stay as
	// per-form NotImplementedException placeholders and get swapped in by follow-up PRs.
	if (info->target_is_local) {
		throw NotImplementedException("CONNECT LOCAL is not yet implemented");
	}

	auto &client = context.client;
	auto &db_manager = DatabaseManager::Get(client);

	// At most one active connection; only DISCONNECT clears it (even if the target was detached
	// elsewhere, so the user explicitly acknowledges the broken connection).
	auto ensure_not_connected = [&]() {
		if (client.IsConnected()) {
			auto current = client.TryGetConnectedCatalog();
			throw InvalidInputException("Already connected to \"%s\"; DISCONNECT first before issuing another CONNECT",
			                            current ? current->GetName().GetIdentifierName() : "<detached>");
		}
	};

	// `CONNECT TO [NEW TEMPORARY] EXTERNAL RESOURCE`: get the endpoint, then ephemeral-attach it
	// under a hidden alias and connect to it. The deleter is bound so the DISCONNECT reap tears it down.
	if (info->external_resource) {
		ensure_not_connected();
		auto &external_resource = *info->external_resource;
		// A string type PROVISIONS a resource this connection OWNS (deleter bound, DISCONNECT reaps it);
		// a bare identifier REFERENCES a registered resource it only BORROWS (no deleter).
		LaunchedResource launched;
		string resource_type, borrowed_resource_name;
		bool owns_resource = false;
		if (!external_resource.reference_name.empty()) {
			auto instance = ExternalResourcesManager::Get(client).Lookup(external_resource.reference_name);
			if (!instance) {
				throw InvalidInputException("no external resource named \"%s\" is registered",
				                            external_resource.reference_name);
			}
			launched = ProvisionExternalResource(client, instance->type, {}, instance->name, instance->handle);
			// Record the borrow so DESTROY refuses while this connection is still using the resource.
			borrowed_resource_name = instance->name;
		} else {
			resource_type = external_resource.provider;
			// CONNECT has no alias to borrow a label from, so the resource stays unnamed in the logs.
			launched = ProvisionExternalResource(client, external_resource.provider, external_resource.params);
			owns_resource = true;
		}
		AttachInfo attach_info;
		attach_info.name = GenerateInternalName("__connect_");
		// Applying the resource, parsing its options and the attach can all throw, and by now the
		// resource exists with nothing owning it. One guard covers the lot; a narrower one strands it.
		auto reap_if_owned = [&]() {
			if (owns_resource) {
				ResourceDeleter(DatabaseInstance::GetDatabase(client), launched.deleter_function,
				                launched.deleter_payload, resource_type, string())
				    .TryDelete();
			}
		};
		shared_ptr<AttachedDatabase> target;
		try {
			ApplyLaunchedResource(launched, attach_info);

			auto &config = DBConfig::GetConfig(client);
			AttachOptions options(attach_info.options, config.options.access_mode);
			options.visibility = AttachVisibility::HIDDEN;
			options.ephemeral = true;
			if (owns_resource) {
				options.deleter_function = launched.deleter_function;
				options.deleter_payload = launched.deleter_payload;
				options.deleter_resource_type = resource_type;
			}
			options.borrowed_resource_name = borrowed_resource_name;
			options.original_path = attach_info.path;
			if (options.db_type.empty()) {
				DBPathAndType::ExtractExtensionPrefix(attach_info.path, options.db_type);
			}
			target = db_manager.AttachDatabase(client, attach_info, options);
		} catch (...) {
			// Compensating teardown (best-effort): only for a resource we just PROVISIONED; a borrowed
			// (referenced) resource is never torn down here.
			reap_if_owned();
			throw;
		}
		if (!target->GetCatalog().Supports(RemoteCapability::CONNECT)) {
			// The detach also runs the extracted deleter, destroying the provisioned resource.
			db_manager.DetachDatabase(client, attach_info.name, OnEntryNotFound::RETURN_NULL);
			throw InvalidInputException("CONNECT TO EXTERNAL RESOURCE: the provisioned resource does not "
			                            "support CONNECT");
		}
		client.ConnectToCatalog(target);
		return SourceResultType::FINISHED;
	}

	if (info->name.empty()) {
		throw NotImplementedException("CONNECT with no target is not yet implemented");
	}

	ensure_not_connected();
	if (info->name_is_string_literal) {
		// `CONNECT '<uri>'`: attach the connection string under an internal, hidden, ephemeral alias and
		// bind to it in one shot. The name is a random UUID (like __pivot_enum_<uuid>): unique, ASCII
		// (backend-safe), and unguessable, so it is not referenceable in SQL. It is owned by this
		// connection and detached again by DISCONNECT (see PhysicalDisconnect).
		AttachInfo attach_info;
		attach_info.name = GenerateInternalName("__connect_");
		attach_info.path = info->name.GetIdentifierName();
		attach_info.options = info->options;

		auto &config = DBConfig::GetConfig(client);
		AttachOptions options(attach_info.options, config.options.access_mode);
		options.visibility = AttachVisibility::HIDDEN;
		options.ephemeral = true;
		options.original_path = attach_info.path;
		if (options.db_type.empty()) {
			DBPathAndType::ExtractExtensionPrefix(attach_info.path, options.db_type);
		}
		auto target = db_manager.AttachDatabase(client, attach_info, options);
		if (!target->GetCatalog().Supports(RemoteCapability::CONNECT)) {
			// No CONNECT capability: roll back the implicit attach so it does not leak.
			db_manager.DetachDatabase(client, attach_info.name, OnEntryNotFound::RETURN_NULL);
			throw InvalidInputException("CONNECT '%s': the attached database does not support CONNECT",
			                            attach_info.path);
		}
		// Capability confirmed above, so ConnectToCatalog's own support check will not fire.
		client.ConnectToCatalog(target);
		return SourceResultType::FINISHED;
	}

	auto target = db_manager.GetDatabase(info->name);
	if (!target) {
		throw InvalidInputException("Database %s is not attached", info->name);
	}
	// ConnectToCatalog resolves and caches the dispatch function name; throws if the catalog returns
	// empty from GetConnectFunctionName (i.e. CONNECT is not supported in this context).
	client.ConnectToCatalog(target);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
