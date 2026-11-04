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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duckdb/common/arrow/nanoarrow/nanoarrow.hpp"

namespace duckdb_nanoarrow {

void ArrowSchemaRelease(struct ArrowSchema *schema) {
	if (schema->format != NULL)
		ArrowFree((void *)schema->format);
	if (schema->name != NULL)
		ArrowFree((void *)schema->name);
	if (schema->metadata != NULL)
		ArrowFree((void *)schema->metadata);

	// This object owns the memory for all the children, but those
	// children may have been generated elsewhere and might have
	// their own release() callback.
	if (schema->children != NULL) {
		for (int64_t i = 0; i < schema->n_children; i++) {
			if (schema->children[i] != NULL) {
				if (schema->children[i]->release != NULL) {
					schema->children[i]->release(schema->children[i]);
				}

				ArrowFree(schema->children[i]);
			}
		}

		ArrowFree(schema->children);
	}

	// This object owns the memory for the dictionary but it
	// may have been generated somewhere else and have its own
	// release() callback.
	if (schema->dictionary != NULL) {
		if (schema->dictionary->release != NULL) {
			schema->dictionary->release(schema->dictionary);
		}

		ArrowFree(schema->dictionary);
	}

	// private data not currently used
	if (schema->private_data != NULL) {
		ArrowFree(schema->private_data);
	}

	schema->release = NULL;
}

const char *ArrowSchemaFormatTemplate(enum ArrowType data_type) {
	switch (data_type) {
	case NANOARROW_TYPE_UNINITIALIZED:
		return NULL;
	case NANOARROW_TYPE_NA:
		return "n";
	case NANOARROW_TYPE_BOOL:
		return "b";

	case NANOARROW_TYPE_UINT8:
		return "C";
	case NANOARROW_TYPE_INT8:
		return "c";
	case NANOARROW_TYPE_UINT16:
		return "S";
	case NANOARROW_TYPE_INT16:
		return "s";
	case NANOARROW_TYPE_UINT32:
		return "I";
	case NANOARROW_TYPE_INT32:
		return "i";
	case NANOARROW_TYPE_UINT64:
		return "L";
	case NANOARROW_TYPE_INT64:
		return "l";

	case NANOARROW_TYPE_HALF_FLOAT:
		return "e";
	case NANOARROW_TYPE_FLOAT:
		return "f";
	case NANOARROW_TYPE_DOUBLE:
		return "g";

	case NANOARROW_TYPE_STRING:
		return "u";
	case NANOARROW_TYPE_LARGE_STRING:
		return "U";
	case NANOARROW_TYPE_BINARY:
		return "z";
	case NANOARROW_TYPE_LARGE_BINARY:
		return "Z";

	case NANOARROW_TYPE_DATE32:
		return "tdD";
	case NANOARROW_TYPE_DATE64:
		return "tdm";
	case NANOARROW_TYPE_INTERVAL_MONTHS:
		return "tiM";
	case NANOARROW_TYPE_INTERVAL_DAY_TIME:
		return "tiD";
	case NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO:
		return "tin";

	case NANOARROW_TYPE_LIST:
		return "+l";
	case NANOARROW_TYPE_LARGE_LIST:
		return "+L";
	case NANOARROW_TYPE_STRUCT:
		return "+s";
	case NANOARROW_TYPE_MAP:
		return "+m";

	default:
		return NULL;
	}
}

ArrowErrorCode ArrowSchemaInit(struct ArrowSchema *schema, enum ArrowType data_type) {
	schema->format = NULL;
	schema->name = NULL;
	schema->metadata = NULL;
	schema->flags = ARROW_FLAG_NULLABLE;
	schema->n_children = 0;
	schema->children = NULL;
	schema->dictionary = NULL;
	schema->private_data = NULL;
	schema->release = &ArrowSchemaRelease;

	// We don't allocate the dictionary because it has to be nullptr
	// for non-dictionary-encoded arrays.

	// Set the format to a valid format string for data_type
	const char *template_format = ArrowSchemaFormatTemplate(data_type);

	// If data_type isn't recognized and not explicitly unset
	if (template_format == NULL && data_type != NANOARROW_TYPE_UNINITIALIZED) {
		schema->release(schema);
		return EINVAL;
	}

	int result = ArrowSchemaSetFormat(schema, template_format);
	if (result != NANOARROW_OK) {
		schema->release(schema);
		return result;
	}

	return NANOARROW_OK;
}

ArrowErrorCode ArrowSchemaInitFixedSize(struct ArrowSchema *schema, enum ArrowType data_type, int32_t fixed_size) {
	int result = ArrowSchemaInit(schema, NANOARROW_TYPE_UNINITIALIZED);
	if (result != NANOARROW_OK) {
		return result;
	}

	if (fixed_size <= 0) {
		schema->release(schema);
		return EINVAL;
	}

	char buffer[64];
	int n_chars;
	switch (data_type) {
	case NANOARROW_TYPE_FIXED_SIZE_BINARY:
		n_chars = snprintf(buffer, sizeof(buffer), "w:%d", (int)fixed_size);
		break;
	case NANOARROW_TYPE_FIXED_SIZE_LIST:
		n_chars = snprintf(buffer, sizeof(buffer), "+w:%d", (int)fixed_size);
		break;
	default:
		schema->release(schema);
		return EINVAL;
	}

	buffer[n_chars] = '\0';
	result = ArrowSchemaSetFormat(schema, buffer);
	if (result != NANOARROW_OK) {
		schema->release(schema);
	}

	return result;
}

ArrowErrorCode ArrowSchemaInitDecimal(struct ArrowSchema *schema, enum ArrowType data_type, int32_t decimal_precision,
                                      int32_t decimal_scale) {
	int result = ArrowSchemaInit(schema, NANOARROW_TYPE_UNINITIALIZED);
	if (result != NANOARROW_OK) {
		return result;
	}

	if (decimal_precision <= 0) {
		schema->release(schema);
		return EINVAL;
	}

	char buffer[64];
	int n_chars;
	switch (data_type) {
	case NANOARROW_TYPE_DECIMAL128:
		n_chars = snprintf(buffer, sizeof(buffer), "d:%d,%d", decimal_precision, decimal_scale);
		break;
	case NANOARROW_TYPE_DECIMAL256:
		n_chars = snprintf(buffer, sizeof(buffer), "d:%d,%d,256", decimal_precision, decimal_scale);
		break;
	default:
		schema->release(schema);
		return EINVAL;
	}

	buffer[n_chars] = '\0';

	result = ArrowSchemaSetFormat(schema, buffer);
	if (result != NANOARROW_OK) {
		schema->release(schema);
		return result;
	}

	return NANOARROW_OK;
}

static const char *ArrowTimeUnitString(enum ArrowTimeUnit time_unit) {
	switch (time_unit) {
	case NANOARROW_TIME_UNIT_SECOND:
		return "s";
	case NANOARROW_TIME_UNIT_MILLI:
		return "m";
	case NANOARROW_TIME_UNIT_MICRO:
		return "u";
	case NANOARROW_TIME_UNIT_NANO:
		return "n";
	default:
		return NULL;
	}
}

ArrowErrorCode ArrowSchemaInitDateTime(struct ArrowSchema *schema, enum ArrowType data_type,
                                       enum ArrowTimeUnit time_unit, const char *timezone) {
	int result = ArrowSchemaInit(schema, NANOARROW_TYPE_UNINITIALIZED);
	if (result != NANOARROW_OK) {
		return result;
	}

	const char *time_unit_str = ArrowTimeUnitString(time_unit);
	if (time_unit_str == NULL) {
		schema->release(schema);
		return EINVAL;
	}

	char buffer[128];
	int n_chars;
	switch (data_type) {
	case NANOARROW_TYPE_TIME32:
	case NANOARROW_TYPE_TIME64:
		if (timezone != NULL) {
			schema->release(schema);
			return EINVAL;
		}
		n_chars = snprintf(buffer, sizeof(buffer), "tt%s", time_unit_str);
		break;
	case NANOARROW_TYPE_TIMESTAMP:
		if (timezone == NULL) {
			timezone = "";
		}
		n_chars = snprintf(buffer, sizeof(buffer), "ts%s:%s", time_unit_str, timezone);
		break;
	case NANOARROW_TYPE_DURATION:
		if (timezone != NULL) {
			schema->release(schema);
			return EINVAL;
		}
		n_chars = snprintf(buffer, sizeof(buffer), "tD%s", time_unit_str);
		break;
	default:
		schema->release(schema);
		return EINVAL;
	}

	if (static_cast<size_t>(n_chars) >= sizeof(buffer)) {
		schema->release(schema);
		return ERANGE;
	}

	buffer[n_chars] = '\0';

	result = ArrowSchemaSetFormat(schema, buffer);
	if (result != NANOARROW_OK) {
		schema->release(schema);
		return result;
	}

	return NANOARROW_OK;
}

ArrowErrorCode ArrowSchemaSetFormat(struct ArrowSchema *schema, const char *format) {
	if (schema->format != NULL) {
		ArrowFree((void *)schema->format);
	}

	if (format != NULL) {
		size_t format_size = strlen(format) + 1;
		schema->format = (const char *)ArrowMalloc(int64_t(format_size));
		if (schema->format == NULL) {
			return ENOMEM;
		}

		memcpy((void *)schema->format, format, format_size);
	} else {
		schema->format = NULL;
	}

	return NANOARROW_OK;
}

ArrowErrorCode ArrowSchemaSetName(struct ArrowSchema *schema, const char *name) {
	if (schema->name != NULL) {
		ArrowFree((void *)schema->name);
	}

	if (name != NULL) {
		size_t name_size = strlen(name) + 1;
		schema->name = (const char *)ArrowMalloc(int64_t(name_size));
		if (schema->name == NULL) {
			return ENOMEM;
		}

		memcpy((void *)schema->name, name, name_size);
	} else {
		schema->name = NULL;
	}

	return NANOARROW_OK;
}

ArrowErrorCode ArrowSchemaSetMetadata(struct ArrowSchema *schema, const char *metadata) {
	if (schema->metadata != NULL) {
		ArrowFree((void *)schema->metadata);
	}

	if (metadata != NULL) {
		auto metadata_size = ArrowMetadataSizeOf(metadata);
		schema->metadata = (const char *)ArrowMalloc(metadata_size);
		if (schema->metadata == NULL) {
			return ENOMEM;
		}

		memcpy((void *)schema->metadata, metadata, size_t(metadata_size));
	} else {
		schema->metadata = NULL;
	}

	return NANOARROW_OK;
}

ArrowErrorCode ArrowSchemaAllocateChildren(struct ArrowSchema *schema, int64_t n_children) {
	if (schema->children != NULL) {
		return EEXIST;
	}

	if (n_children > 0) {
		schema->children =
		    (struct ArrowSchema **)ArrowMalloc(int64_t(uint64_t(n_children) * sizeof(struct ArrowSchema *)));

		if (schema->children == NULL) {
			return ENOMEM;
		}

		schema->n_children = n_children;

		memset(schema->children, 0, uint64_t(n_children) * sizeof(struct ArrowSchema *));

		for (int64_t i = 0; i < n_children; i++) {
			schema->children[i] = (struct ArrowSchema *)ArrowMalloc(sizeof(struct ArrowSchema));

			if (schema->children[i] == NULL) {
				return ENOMEM;
			}

			schema->children[i]->release = NULL;
		}
	}

	return NANOARROW_OK;
}

ArrowErrorCode ArrowSchemaAllocateDictionary(struct ArrowSchema *schema) {
	if (schema->dictionary != NULL) {
		return EEXIST;
	}

	schema->dictionary = (struct ArrowSchema *)ArrowMalloc(sizeof(struct ArrowSchema));
	if (schema->dictionary == NULL) {
		return ENOMEM;
	}

	schema->dictionary->release = NULL;
	return NANOARROW_OK;
}

int ArrowSchemaDeepCopy(const struct ArrowSchema *schema, struct ArrowSchema *schema_out) {
	int result;
	result = ArrowSchemaInit(schema_out, NANOARROW_TYPE_NA);
	if (result != NANOARROW_OK) {
		return result;
	}

	result = ArrowSchemaSetFormat(schema_out, schema->format);
	if (result != NANOARROW_OK) {
		schema_out->release(schema_out);
		return result;
	}

	schema_out->flags = schema->flags;

	result = ArrowSchemaSetName(schema_out, schema->name);
	if (result != NANOARROW_OK) {
		schema_out->release(schema_out);
		return result;
	}

	result = ArrowSchemaSetMetadata(schema_out, schema->metadata);
	if (result != NANOARROW_OK) {
		schema_out->release(schema_out);
		return result;
	}

	result = ArrowSchemaAllocateChildren(schema_out, schema->n_children);
	if (result != NANOARROW_OK) {
		schema_out->release(schema_out);
		return result;
	}

	for (int64_t i = 0; i < schema->n_children; i++) {
		result = ArrowSchemaDeepCopy(schema->children[i], schema_out->children[i]);
		if (result != NANOARROW_OK) {
			schema_out->release(schema_out);
			return result;
		}
	}

	if (schema->dictionary != NULL) {
		result = ArrowSchemaAllocateDictionary(schema_out);
		if (result != NANOARROW_OK) {
			schema_out->release(schema_out);
			return result;
		}

		result = ArrowSchemaDeepCopy(schema->dictionary, schema_out->dictionary);
		if (result != NANOARROW_OK) {
			schema_out->release(schema_out);
			return result;
		}
	}

	return NANOARROW_OK;
}

} // namespace duckdb_nanoarrow
