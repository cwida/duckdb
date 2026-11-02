#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/query_context.hpp"

namespace duckdb::capiv2 {

// How a file is opened, owned. Holds the flag word as given rather than the engine's FileOpenFlags, so that
// "no flags set yet" stays distinguishable and is reported when the options are actually used.
class CV2FileOpenOptions {
public:
	FileOpenFlags flags;
	//! Flags are applied one at a time, so this is what distinguishes "none applied yet" from any particular set.
	bool has_flags = false;
	//! Built on first use: an absent extended info is meaningfully different from an empty one.
	shared_ptr<ExtendedOpenFileInfo> extended_info;

	auto Options() -> unordered_map<string, Value> & {
		if (!extended_info) {
			extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		}
		return extended_info->options;
	}
};

// An open file, owned. Carries the context for the same reason the file system handle does.
class CV2File {
public:
	unique_ptr<FileHandle> handle;
	QueryContext query;

	auto Handle() const -> FileHandle & {
		return *handle;
	}
};

static auto Convert(duckdb_v2_file_system_handle fs) -> CV2FileSystem * {
	return reinterpret_cast<CV2FileSystem *>(fs);
}
static auto Convert(CV2FileSystem *fs) -> duckdb_v2_file_system_handle {
	return reinterpret_cast<duckdb_v2_file_system_handle>(fs);
}

static auto Convert(duckdb_v2_file_open_options_handle options) -> CV2FileOpenOptions * {
	return reinterpret_cast<CV2FileOpenOptions *>(options);
}
static auto Convert(CV2FileOpenOptions *options) -> duckdb_v2_file_open_options_handle {
	return reinterpret_cast<duckdb_v2_file_open_options_handle>(options);
}

static auto Convert(duckdb_v2_file_handle handle) -> CV2File * {
	return reinterpret_cast<CV2File *>(handle);
}
static auto Convert(CV2File *handle) -> duckdb_v2_file_handle {
	return reinterpret_cast<duckdb_v2_file_handle>(handle);
}

// Applies one C flag to the engine's flag set. The C enum is a list of names rather than a bitmask, so each value
// maps to exactly one engine flag and anything else is a caller error.
static void ApplyFileFlag(CV2FileOpenOptions &options, DUCKDB_V2_FILE_FLAG flag) {
	switch (flag) {
	case DUCKDB_V2_FILE_FLAG_READ:
		options.flags |= FileOpenFlags::FILE_FLAGS_READ;
		break;
	case DUCKDB_V2_FILE_FLAG_WRITE:
		options.flags |= FileOpenFlags::FILE_FLAGS_WRITE;
		break;
	case DUCKDB_V2_FILE_FLAG_CREATE:
		options.flags |= FileOpenFlags::FILE_FLAGS_FILE_CREATE;
		break;
	case DUCKDB_V2_FILE_FLAG_CREATE_NEW:
		options.flags |= FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW;
		break;
	case DUCKDB_V2_FILE_FLAG_APPEND:
		options.flags |= FileOpenFlags::FILE_FLAGS_APPEND;
		break;
	case DUCKDB_V2_FILE_FLAG_EXCLUSIVE_CREATE:
		options.flags |= FileOpenFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
		break;
	case DUCKDB_V2_FILE_FLAG_PARALLEL_ACCESS:
		options.flags |= FileOpenFlags::FILE_FLAGS_PARALLEL_ACCESS;
		break;
	case DUCKDB_V2_FILE_FLAG_EXTERNAL_FILE_CACHE:
		options.flags.SetCachingMode(CachingMode::ALWAYS_CACHE);
		break;
	default:
		// Includes FILE_FLAG_INVALID, which names no behaviour.
		throw InvalidInputException("'%d' is not a file flag.", static_cast<int>(flag));
	}
	options.has_flags = true;
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_context_get_file_system(duckdb_v2_context_handle context,
                                                  duckdb_v2_file_system_handle *out_file_system,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(context);
	DUCKDB_CHECK_ARG(out_file_system);
	*out_file_system = nullptr;
	return WithErrorHandler(err, [&]() { *out_file_system = Convert(&Convert(context)->file_system); });
}

DUCKDB_V2_ERROR duckdb_v2_instance_get_file_system(duckdb_v2_instance_handle instance,
                                                   duckdb_v2_file_system_handle *out_file_system,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(out_file_system);
	*out_file_system = nullptr;
	return WithErrorHandler(err, [&]() { *out_file_system = Convert(&Convert(instance)->file_system); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_get_file_system(duckdb_v2_connection_handle conn,
                                                     duckdb_v2_file_system_handle *out_file_system,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(out_file_system);
	*out_file_system = nullptr;
	return WithErrorHandler(err, [&]() { *out_file_system = Convert(&Convert(conn)->context_handle.file_system); });
}

DUCKDB_V2_ERROR duckdb_v2_file_open_options_create(duckdb_v2_file_system_handle file_system,
                                                   duckdb_v2_file_open_options_handle *out_options,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(out_options);
	*out_options = nullptr;
	return WithErrorHandler(err, [&]() { *out_options = Convert(duckdb::make_uniq<CV2FileOpenOptions>().release()); });
}

DUCKDB_V2_ERROR duckdb_v2_file_open_options_set_flag(duckdb_v2_file_open_options_handle options,
                                                     DUCKDB_V2_FILE_FLAG flag, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(options);
	return WithErrorHandler(err, [&]() { ApplyFileFlag(*Convert(options), flag); });
}

DUCKDB_V2_ERROR duckdb_v2_file_open_options_set_value(duckdb_v2_file_open_options_handle options,
                                                      const duckdb_v2_str *name, duckdb_v2_value_handle value,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(options);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto key = duckdb::string(Convert(name));
		if (key.empty()) {
			throw duckdb::InvalidInputException("A file option name cannot be empty.");
		}
		Convert(options)->Options()[key] = *Convert(value);
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_open_options_destroy(duckdb_v2_file_open_options_handle *options) {
	return WithErrorHandler(nullptr, [&]() {
		if (!options) {
			return;
		}
		if (*options) {
			delete Convert(*options);
			*options = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_system_open(duckdb_v2_file_system_handle file_system, const duckdb_v2_str *file_path,
                                           duckdb_v2_file_open_options_handle options,
                                           duckdb_v2_file_handle *out_file_handle, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(file_path);
	DUCKDB_CHECK_ARG(options);
	DUCKDB_CHECK_ARG(out_file_handle);
	*out_file_handle = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &slot = *Convert(file_system);
		auto &opts = *Convert(options);
		if (!opts.has_flags) {
			throw duckdb::InvalidInputException(
			    "The open options carry no flags, so they cannot say whether the file is being read or written.");
		}
		if (opts.flags.GetCachingMode() != duckdb::CachingMode::NO_CACHING &&
		    (opts.flags.OpenForWriting() || opts.flags.OpenForAppending())) {
			throw duckdb::InvalidInputException("Only a file opened for reading can be read through the external file "
			                                    "cache.");
		}

		duckdb::OpenFileInfo info(duckdb::string(Convert(file_path)));
		info.extended_info = opts.extended_info;

		// No opener is passed: FileSystem::GetFileSystem hands back the context's own OpenerFileSystem, which
		// pushes the opener itself -- which is how a remote file system reaches settings and secrets. Supplying one
		// here is rejected outright ("the opener is pushed automatically").
		auto handle = slot.fs.OpenFile(info, opts.flags);
		if (!handle) {
			throw duckdb::IOException("Failed to open file: %s", info.path);
		}
		auto file = duckdb::make_uniq<CV2File>();
		file->handle = std::move(handle);
		file->query = slot.query;
		*out_file_handle = Convert(file.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_system_path_exists(duckdb_v2_file_system_handle file_system, const duckdb_v2_str *path,
                                                  bool *out_exists, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(path);
	DUCKDB_CHECK_ARG(out_exists);
	return WithErrorHandler(
	    err, [&]() { *out_exists = Convert(file_system)->fs.FileExists(duckdb::string(Convert(path))); });
}

DUCKDB_V2_ERROR duckdb_v2_file_system_path_is_remote(duckdb_v2_file_system_handle file_system,
                                                     const duckdb_v2_str *path, bool *out_is_remote,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(path);
	DUCKDB_CHECK_ARG(out_is_remote);
	return WithErrorHandler(
	    err, [&]() { *out_is_remote = duckdb::FileSystem::IsRemoteFile(duckdb::string(Convert(path))); });
}

DUCKDB_V2_ERROR duckdb_v2_file_system_path_expand(duckdb_v2_file_system_handle file_system, const duckdb_v2_str *path,
                                                  duckdb_v2_text_sink_fn sink, void *user_data,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(path);
	DUCKDB_CHECK_ARG(sink);
	return WithErrorHandler(err, [&]() {
		auto expanded = Convert(file_system)->fs.ExpandPath(duckdb::string(Convert(path)));
		InvokeTextSink(sink, duckdb_v2_str {expanded.data(), expanded.size()}, user_data);
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_system_path_glob(duckdb_v2_file_system_handle file_system, const duckdb_v2_str *pattern,
                                                duckdb_v2_file_system_path_glob_callback_fn callback, void *user_data,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(pattern);
	DUCKDB_CHECK_ARG(callback);
	return WithErrorHandler(err, [&]() {
		auto matches = Convert(file_system)->fs.Glob(duckdb::string(Convert(pattern)));
		for (auto &match : matches) {
			CV2ErrorInfo info;
			auto handle = Convert(&info);
			duckdb_v2_str path {match.path.data(), match.path.size()};
			callback(&path, user_data, &handle);
			if (info.HasError()) {
				info.ThrowAsException();
			}
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_system_path_move(duckdb_v2_file_system_handle file_system, const duckdb_v2_str *source,
                                                const duckdb_v2_str *target, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(source);
	DUCKDB_CHECK_ARG(target);
	return WithErrorHandler(err, [&]() {
		Convert(file_system)->fs.MoveFile(duckdb::string(Convert(source)), duckdb::string(Convert(target)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_system_path_remove(duckdb_v2_file_system_handle file_system, const duckdb_v2_str *path,
                                                  bool *out_removed, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file_system);
	DUCKDB_CHECK_ARG(path);
	return WithErrorHandler(err, [&]() {
		auto removed = Convert(file_system)->fs.TryRemoveFile(duckdb::string(Convert(path)));
		if (out_removed) {
			*out_removed = removed;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_read(duckdb_v2_file_handle file, void *buffer, idx_t buffer_size, idx_t *bytes_read,
                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	DUCKDB_CHECK_ARG(buffer);
	DUCKDB_CHECK_ARG(bytes_read);
	return WithErrorHandler(err, [&]() {
		auto &f = *Convert(file);
		*bytes_read = duckdb::NumericCast<idx_t>(f.Handle().Read(f.query, buffer, buffer_size));
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_write(duckdb_v2_file_handle file, const void *buffer, idx_t buffer_size,
                                     idx_t *bytes_written, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	DUCKDB_CHECK_ARG(buffer);
	DUCKDB_CHECK_ARG(bytes_written);
	return WithErrorHandler(err, [&]() {
		// The engine's Write takes a mutable pointer but does not write through it.
		auto *data = const_cast<void *>(buffer); // NOLINT: the engine's signature is not const-correct
		auto &f = *Convert(file);
		*bytes_written = duckdb::NumericCast<idx_t>(f.Handle().Write(f.query, data, buffer_size));
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_read_at(duckdb_v2_file_handle file, void *buffer, idx_t buffer_size, idx_t location,
                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	DUCKDB_CHECK_ARG(buffer);
	return WithErrorHandler(err, [&]() {
		// Reads all of buffer_size or throws, and leaves the file's position alone.
		auto &f = *Convert(file);
		f.Handle().Read(f.query, buffer, buffer_size, location);
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_write_at(duckdb_v2_file_handle file, const void *buffer, idx_t buffer_size,
                                        idx_t location, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	DUCKDB_CHECK_ARG(buffer);
	return WithErrorHandler(err, [&]() {
		auto *data = const_cast<void *>(buffer); // NOLINT: the engine's signature is not const-correct
		auto &f = *Convert(file);
		f.Handle().Write(f.query, data, buffer_size, location);
	});
}

DUCKDB_V2_ERROR duckdb_v2_file_tell(duckdb_v2_file_handle file, idx_t *position, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	DUCKDB_CHECK_ARG(position);
	return WithErrorHandler(err, [&]() { *position = Convert(file)->Handle().SeekPosition(); });
}

DUCKDB_V2_ERROR duckdb_v2_file_size(duckdb_v2_file_handle file, idx_t *size, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	DUCKDB_CHECK_ARG(size);
	return WithErrorHandler(err, [&]() { *size = Convert(file)->Handle().GetFileSize(); });
}

DUCKDB_V2_ERROR duckdb_v2_file_seek(duckdb_v2_file_handle file, idx_t position, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	return WithErrorHandler(err, [&]() { Convert(file)->Handle().Seek(position); });
}

DUCKDB_V2_ERROR duckdb_v2_file_sync(duckdb_v2_file_handle file, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	return WithErrorHandler(err, [&]() { Convert(file)->Handle().Sync(); });
}

DUCKDB_V2_ERROR duckdb_v2_file_close(duckdb_v2_file_handle file, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(file);
	return WithErrorHandler(err, [&]() { Convert(file)->Handle().Close(); });
}

DUCKDB_V2_ERROR duckdb_v2_file_destroy(duckdb_v2_file_handle *file_handle) {
	return WithErrorHandler(nullptr, [&]() {
		if (!file_handle) {
			return;
		}
		if (*file_handle) {
			delete Convert(*file_handle);
			*file_handle = nullptr;
		}
	});
}
