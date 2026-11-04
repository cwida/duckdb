// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef NANOARROW_H_INCLUDED
#define NANOARROW_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "duckdb/common/arrow/arrow.hpp"

namespace duckdb_nanoarrow {

/// \file Arrow C Implementation
///
/// EXPERIMENTAL. Interface subject to change.

/// \page object-model Object Model
///
/// Except where noted, objects are not thread-safe and clients should
/// take care to serialize accesses to methods.
///
/// Because this library is intended to be vendored, it provides full type
/// definitions and encourages clients to stack or statically allocate
/// where convenient.

/// \defgroup nanoarrow-malloc Memory management
///
/// Non-buffer members of a struct ArrowSchema and struct ArrowArray
/// must be allocated using ArrowMalloc() or ArrowRealloc() and freed
/// using ArrowFree for schemas and arrays allocated here. Buffer members
/// are allocated using an ArrowBufferAllocator.

/// \brief Allocate like malloc()
void *ArrowMalloc(int64_t size);

/// \brief Reallocate like realloc()
void *ArrowRealloc(void *ptr, int64_t size);

/// \brief Free a pointer allocated using ArrowMalloc() or ArrowRealloc().
void ArrowFree(void *ptr);

/// \brief Array buffer allocation and deallocation
///
/// Container for allocate, reallocate, and free methods that can be used
/// to customize allocation and deallocation of buffers when constructing
/// an ArrowArray.
struct ArrowBufferAllocator {
	/// \brief Allocate a buffer or return NULL if it cannot be allocated
	uint8_t *(*allocate)(struct ArrowBufferAllocator *allocator, int64_t size);

	/// \brief Reallocate a buffer or return NULL if it cannot be reallocated
	uint8_t *(*reallocate)(struct ArrowBufferAllocator *allocator, uint8_t *ptr, int64_t old_size, int64_t new_size);

	/// \brief Deallocate a buffer allocated by this allocator
	void (*free)(struct ArrowBufferAllocator *allocator, uint8_t *ptr, int64_t size);

	/// \brief Opaque data specific to the allocator
	void *private_data;
};

/// \brief Return the default allocator
///
/// The default allocator uses ArrowMalloc(), ArrowRealloc(), and
/// ArrowFree().
struct ArrowBufferAllocator *ArrowBufferAllocatorDefault();

/// }@

/// \defgroup nanoarrow-errors Error handling primitives
/// Functions generally return an errno-compatible error code; functions that
/// need to communicate more verbose error information accept a pointer
/// to an ArrowError. This can be stack or statically allocated. The
/// content of the message is undefined unless an error code has been
/// returned.

/// \brief Error type containing a UTF-8 encoded message.
struct ArrowError {
	char message[1024];
};

/// \brief Return code for success.
#define NANOARROW_OK 0

/// \brief Represents an errno-compatible error code
typedef int ArrowErrorCode;

/// \brief Set the contents of an error using printf syntax
ArrowErrorCode ArrowErrorSet(struct ArrowError *error, const char *fmt, ...);

/// \brief Get the contents of an error
const char *ArrowErrorMessage(struct ArrowError *error);

/// }@

/// \defgroup nanoarrow-utils Utility data structures

/// \brief An non-owning view of a string
struct ArrowStringView {
	/// \brief A pointer to the start of the string
	///
	/// If n_bytes is 0, this value may be NULL.
	const char *data;

	/// \brief The size of the string in bytes,
	///
	/// (Not including the null terminator.)
	int64_t n_bytes;
};

/// \brief Arrow type enumerator
///
/// These names are intended to map to the corresponding arrow::Type::type
/// enumerator; however, the numeric values are specifically not equal
/// (i.e., do not rely on numeric comparison).
enum ArrowType {
	NANOARROW_TYPE_UNINITIALIZED = 0,
	NANOARROW_TYPE_NA = 1,
	NANOARROW_TYPE_BOOL,
	NANOARROW_TYPE_UINT8,
	NANOARROW_TYPE_INT8,
	NANOARROW_TYPE_UINT16,
	NANOARROW_TYPE_INT16,
	NANOARROW_TYPE_UINT32,
	NANOARROW_TYPE_INT32,
	NANOARROW_TYPE_UINT64,
	NANOARROW_TYPE_INT64,
	NANOARROW_TYPE_HALF_FLOAT,
	NANOARROW_TYPE_FLOAT,
	NANOARROW_TYPE_DOUBLE,
	NANOARROW_TYPE_STRING,
	NANOARROW_TYPE_BINARY,
	NANOARROW_TYPE_FIXED_SIZE_BINARY,
	NANOARROW_TYPE_DATE32,
	NANOARROW_TYPE_DATE64,
	NANOARROW_TYPE_TIMESTAMP,
	NANOARROW_TYPE_TIME32,
	NANOARROW_TYPE_TIME64,
	NANOARROW_TYPE_INTERVAL_MONTHS,
	NANOARROW_TYPE_INTERVAL_DAY_TIME,
	NANOARROW_TYPE_DECIMAL128,
	NANOARROW_TYPE_DECIMAL256,
	NANOARROW_TYPE_LIST,
	NANOARROW_TYPE_STRUCT,
	NANOARROW_TYPE_SPARSE_UNION,
	NANOARROW_TYPE_DENSE_UNION,
	NANOARROW_TYPE_DICTIONARY,
	NANOARROW_TYPE_MAP,
	NANOARROW_TYPE_EXTENSION,
	NANOARROW_TYPE_FIXED_SIZE_LIST,
	NANOARROW_TYPE_DURATION,
	NANOARROW_TYPE_LARGE_STRING,
	NANOARROW_TYPE_LARGE_BINARY,
	NANOARROW_TYPE_LARGE_LIST,
	NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO
};

/// \brief Arrow time unit enumerator
///
/// These names and values map to the corresponding arrow::TimeUnit::type
/// enumerator.
enum ArrowTimeUnit {
	NANOARROW_TIME_UNIT_SECOND = 0,
	NANOARROW_TIME_UNIT_MILLI = 1,
	NANOARROW_TIME_UNIT_MICRO = 2,
	NANOARROW_TIME_UNIT_NANO = 3
};

/// }@

/// \defgroup nanoarrow-schema Schema producer helpers
/// These functions allocate, copy, and destroy ArrowSchema structures

/// \brief Initialize the fields of a schema
///
/// Initializes the fields and release callback of schema_out. Caller
/// is responsible for calling the schema->release callback if
/// NANOARROW_OK is returned.
ArrowErrorCode ArrowSchemaInit(struct ArrowSchema *schema, enum ArrowType type);

/// \brief Initialize the fields of a fixed-size schema
///
/// Returns EINVAL for fixed_size <= 0 or for data_type that is not
/// NANOARROW_TYPE_FIXED_SIZE_BINARY or NANOARROW_TYPE_FIXED_SIZE_LIST.
ArrowErrorCode ArrowSchemaInitFixedSize(struct ArrowSchema *schema, enum ArrowType data_type, int32_t fixed_size);

/// \brief Initialize the fields of a decimal schema
///
/// Returns EINVAL for scale <= 0 or for data_type that is not
/// NANOARROW_TYPE_DECIMAL128 or NANOARROW_TYPE_DECIMAL256.
ArrowErrorCode ArrowSchemaInitDecimal(struct ArrowSchema *schema, enum ArrowType data_type, int32_t decimal_precision,
                                      int32_t decimal_scale);

/// \brief Initialize the fields of a time, timestamp, or duration schema
///
/// Returns EINVAL for data_type that is not
/// NANOARROW_TYPE_TIME32, NANOARROW_TYPE_TIME64,
/// NANOARROW_TYPE_TIMESTAMP, or NANOARROW_TYPE_DURATION. The
/// timezone parameter must be NULL for a non-timestamp data_type.
ArrowErrorCode ArrowSchemaInitDateTime(struct ArrowSchema *schema, enum ArrowType data_type,
                                       enum ArrowTimeUnit time_unit, const char *timezone);

/// \brief Make a (recursive) copy of a schema
///
/// Allocates and copies fields of schema into schema_out.
ArrowErrorCode ArrowSchemaDeepCopy(const struct ArrowSchema *schema, struct ArrowSchema *schema_out);

/// \brief Copy format into schema->format
///
/// schema must have been allocated using ArrowSchemaInit or
/// ArrowSchemaDeepCopy.
ArrowErrorCode ArrowSchemaSetFormat(struct ArrowSchema *schema, const char *format);

/// \brief Copy name into schema->name
///
/// schema must have been allocated using ArrowSchemaInit or
/// ArrowSchemaDeepCopy.
ArrowErrorCode ArrowSchemaSetName(struct ArrowSchema *schema, const char *name);

/// \brief Copy metadata into schema->metadata
///
/// schema must have been allocated using ArrowSchemaInit or
/// ArrowSchemaDeepCopy.
ArrowErrorCode ArrowSchemaSetMetadata(struct ArrowSchema *schema, const char *metadata);

/// \brief Allocate the schema->children array
///
/// Includes the memory for each child struct ArrowSchema.
/// schema must have been allocated using ArrowSchemaInit or
/// ArrowSchemaDeepCopy.
ArrowErrorCode ArrowSchemaAllocateChildren(struct ArrowSchema *schema, int64_t n_children);

/// \brief Allocate the schema->dictionary member
///
/// schema must have been allocated using ArrowSchemaInit or
/// ArrowSchemaDeepCopy.
ArrowErrorCode ArrowSchemaAllocateDictionary(struct ArrowSchema *schema);

/// \brief Reader for key/value pairs in schema metadata
struct ArrowMetadataReader {
	const char *metadata;
	int64_t offset;
	int32_t remaining_keys;
};

/// \brief Initialize an ArrowMetadataReader
ArrowErrorCode ArrowMetadataReaderInit(struct ArrowMetadataReader *reader, const char *metadata);

/// \brief Read the next key/value pair from an ArrowMetadataReader
ArrowErrorCode ArrowMetadataReaderRead(struct ArrowMetadataReader *reader, struct ArrowStringView *key_out,
                                       struct ArrowStringView *value_out);

/// \brief The number of bytes in in a key/value metadata string
int64_t ArrowMetadataSizeOf(const char *metadata);

/// \brief Check for a key in schema metadata
char ArrowMetadataHasKey(const char *metadata, const char *key);

/// \brief Extract a value from schema metadata
ArrowErrorCode ArrowMetadataGetValue(const char *metadata, const char *key, const char *default_value,
                                     struct ArrowStringView *value_out);

/// }@

/// \defgroup nanoarrow-schema-view Schema consumer helpers

/// \brief A non-owning view of a parsed ArrowSchema
///
/// Contains more readily extractable values than a raw ArrowSchema.
/// Clients can stack or statically allocate this structure but are
/// encouraged to use the provided getters to ensure forward
/// compatibility.
struct ArrowSchemaView {
	/// \brief A pointer to the schema represented by this view
	struct ArrowSchema *schema;

	/// \brief The data type represented by the schema
	///
	/// This value may be NANOARROW_TYPE_DICTIONARY if the schema has a
	/// non-null dictionary member; datetime types are valid values.
	/// This value will never be NANOARROW_TYPE_EXTENSION (see
	/// extension_name and/or extension_metadata to check for
	/// an extension type).
	enum ArrowType data_type;

	/// \brief The storage data type represented by the schema
	///
	/// This value will never be NANOARROW_TYPE_DICTIONARY, NANOARROW_TYPE_EXTENSION
	/// or any datetime type. This value represents only the type required to
	/// interpret the buffers in the array.
	enum ArrowType storage_data_type;

	/// \brief The extension type name if it exists
	///
	/// If the ARROW:extension:name key is present in schema.metadata,
	/// extension_name.data will be non-NULL.
	struct ArrowStringView extension_name;

	/// \brief The extension type metadata if it exists
	///
	/// If the ARROW:extension:metadata key is present in schema.metadata,
	/// extension_metadata.data will be non-NULL.
	struct ArrowStringView extension_metadata;

	/// \brief The expected number of buffers in a paired ArrowArray
	int32_t n_buffers;

	/// \brief The index of the validity buffer or -1 if one does not exist
	int32_t validity_buffer_id;

	/// \brief The index of the offset buffer or -1 if one does not exist
	int32_t offset_buffer_id;

	/// \brief The index of the data buffer or -1 if one does not exist
	int32_t data_buffer_id;

	/// \brief The index of the type_ids buffer or -1 if one does not exist
	int32_t type_id_buffer_id;

	/// \brief Format fixed size parameter
	///
	/// This value is set when parsing a fixed-size binary or fixed-size
	/// list schema; this value is undefined for other types. For a
	/// fixed-size binary schema this value is in bytes; for a fixed-size
	/// list schema this value refers to the number of child elements for
	/// each element of the parent.
	int32_t fixed_size;

	/// \brief Decimal bitwidth
	///
	/// This value is set when parsing a decimal type schema;
	/// this value is undefined for other types.
	int32_t decimal_bitwidth;

	/// \brief Decimal precision
	///
	/// This value is set when parsing a decimal type schema;
	/// this value is undefined for other types.
	int32_t decimal_precision;

	/// \brief Decimal scale
	///
	/// This value is set when parsing a decimal type schema;
	/// this value is undefined for other types.
	int32_t decimal_scale;

	/// \brief Format time unit parameter
	///
	/// This value is set when parsing a date/time type. The value is
	/// undefined for other types.
	enum ArrowTimeUnit time_unit;

	/// \brief Format timezone parameter
	///
	/// This value is set when parsing a timestamp type and represents
	/// the timezone format parameter. The ArrowStrintgView points to
	/// data within the schema and the value is undefined for other types.
	struct ArrowStringView timezone;

	/// \brief Union type ids parameter
	///
	/// This value is set when parsing a union type and represents
	/// type ids parameter. The ArrowStringView points to
	/// data within the schema and the value is undefined for other types.
	struct ArrowStringView union_type_ids;
};

/// \brief Initialize an ArrowSchemaView
ArrowErrorCode ArrowSchemaViewInit(struct ArrowSchemaView *schema_view, struct ArrowSchema *schema,
                                   struct ArrowError *error);

/// }@

/// \defgroup nanoarrow-buffer-builder Growable buffer builders

/// \brief An owning mutable view of a buffer
struct ArrowBuffer {
	/// \brief A pointer to the start of the buffer
	///
	/// If capacity_bytes is 0, this value may be NULL.
	uint8_t *data;

	/// \brief The size of the buffer in bytes
	int64_t size_bytes;

	/// \brief The capacity of the buffer in bytes
	int64_t capacity_bytes;

	/// \brief The allocator that will be used to reallocate and/or free the buffer
	struct ArrowBufferAllocator *allocator;
};

/// \brief Initialize an ArrowBuffer
///
/// Initialize a buffer with a NULL, zero-size buffer using the default
/// buffer allocator.
void ArrowBufferInit(struct ArrowBuffer *buffer);

/// \brief Set a newly-initialized buffer's allocator
///
/// Returns EINVAL if the buffer has already been allocated.
ArrowErrorCode ArrowBufferSetAllocator(struct ArrowBuffer *buffer, struct ArrowBufferAllocator *allocator);

/// \brief Reset an ArrowBuffer
///
/// Releases the buffer using the allocator's free method if
/// the buffer's data member is non-null, sets the data member
/// to NULL, and sets the buffer's size and capacity to 0.
void ArrowBufferReset(struct ArrowBuffer *buffer);

/// \brief Move an ArrowBuffer
///
/// Transfers the buffer data and lifecycle management to another
/// address and resets buffer.
void ArrowBufferMove(struct ArrowBuffer *buffer, struct ArrowBuffer *buffer_out);

/// \brief Grow or shrink a buffer to a given capacity
///
/// When shrinking the capacity of the buffer, the buffer is only reallocated
/// if shrink_to_fit is non-zero. Calling ArrowBufferResize() does not
/// adjust the buffer's size member except to ensure that the invariant
/// capacity >= size remains true.
ArrowErrorCode ArrowBufferResize(struct ArrowBuffer *buffer, int64_t new_capacity_bytes, char shrink_to_fit);

/// \brief Ensure a buffer has at least a given additional capacity
///
/// Ensures that the buffer has space to append at least
/// additional_size_bytes, overallocating when required.
ArrowErrorCode ArrowBufferReserve(struct ArrowBuffer *buffer, int64_t additional_size_bytes);

/// \brief Write data to buffer and increment the buffer size
///
/// This function does not check that buffer has the required capacity
void ArrowBufferAppendUnsafe(struct ArrowBuffer *buffer, const void *data, int64_t size_bytes);

/// \brief Write data to buffer and increment the buffer size
///
/// This function writes and ensures that the buffer has the required capacity,
/// possibly by reallocating the buffer. Like ArrowBufferReserve, this will
/// overallocate when reallocation is required.
ArrowErrorCode ArrowBufferAppend(struct ArrowBuffer *buffer, const void *data, int64_t size_bytes);

/// }@

} // namespace duckdb_nanoarrow

#endif // NANOARROW_H_INCLUDED
