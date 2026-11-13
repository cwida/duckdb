.PHONY: all opt unit clean debug release test unittest allunit benchmark docs doxygen format sqlite smoke runnertests sync_out_of_tree_extensions

all: release
opt: release
unit: unittest

EXTENSION_CONFIG_STEP ?=

GENERATOR ?=
FORCE_COLOR ?=
WARNINGS_AS_ERRORS ?=
FORCE_WARN_UNUSED_FLAG ?=
DISABLE_UNITY_FLAG ?=
DISABLE_SANITIZER_FLAG ?=
FORCE_32_BIT_FLAG ?=
CONFIGS_DIR = ./test/configs


MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
PROJ_DIR := $(dir $(MKFILE_PATH))

PYTHON ?= python3
PREPARE_VENV ?= .cache/prepare-venv
FORMAT_VENV ?= $(PREPARE_VENV)
FORMAT_PYTHON := $(FORMAT_VENV)/bin/python
FORMAT_SETUP_DEPS := format_venv
CAPIGEN_VENV ?= $(PREPARE_VENV)
CAPIGEN_PYTHON := $(CAPIGEN_VENV)/bin/python
CAPIGEN_SETUP_DEPS := capigen_venv

EXE_SUFFIX :=
ifeq ($(OS),Windows_NT)
EXE_SUFFIX := .exe
endif
UNITTEST_BINARY ?= test/unittest$(EXE_SUFFIX)
SMOKE_UNITTEST ?= build/relassert/$(UNITTEST_BINARY)
SMOKE_RUNNER ?= build/relassert/test/run
UNITTEST_SLOW_FLAGS ?= --track-runtime=100
UNITTEST_HUGE_FLAGS ?= --workers=50% $(UNITTEST_SLOW_FLAGS)

# Allow setting extra unit test parameters using `make smoke T=...`.
T ?=

CI_CPU_COUNT := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || printf '%s\n' "$${NUMBER_OF_PROCESSORS:-1}")
CI_BUILD_JOBS := $(shell jobs=$$(( $(CI_CPU_COUNT) * 80 / 100 )); [ $$jobs -lt 1 ] && jobs=1; echo $$jobs)
ifndef CMAKE_BUILD_PARALLEL_LEVEL
CMAKE_BUILD_PARALLEL_LEVEL := $(CI_BUILD_JOBS)
endif
export CMAKE_BUILD_PARALLEL_LEVEL
export CI_TIDY_JOBS := $(shell jobs=$$(( $(CI_CPU_COUNT) * 50 / 100 )); [ $$jobs -lt 1 ] && jobs=1; echo $$jobs)

# Assume Ninja is the default generator (if missing), but verify ninja exists.
# Cache Ninja detection so we only probe `ninja --version` once.
ifeq ($(GEN),)
NINJA_VERSION_FILE := build/ninja_version.txt
ifeq ($(wildcard $(NINJA_VERSION_FILE)),)
NINJA_DETECTED := $(strip $(shell mkdir -p build >/dev/null 2>&1; \
	v=$$(ninja --version 2>/dev/null | head -n 1); \
	if [ -n "$$v" ]; then \
		printf '%s\n' "$$v" > "$(NINJA_VERSION_FILE)"; \
		echo 1; \
	fi))
ifneq ($(NINJA_DETECTED),)
GEN := ninja
endif
else
GEN := ninja
endif
endif

ifeq ($(GEN),ninja)
	GENERATOR=-G "Ninja"
	FORCE_COLOR=-DFORCE_COLORED_OUTPUT=1
endif
DUCKDB_NINJA_FILTER ?= $(if $(CI),1,0)
NINJA_BUILD_WRAPPER :=
ifeq ($(GEN),ninja)
ifneq ($(DUCKDB_NINJA_FILTER),0)
	NINJA_BUILD_WRAPPER=$(PYTHON) ${PROJ_DIR}scripts/ci/filter_ninja_output.py --
endif
endif
ifeq (${TREAT_WARNINGS_AS_ERRORS}, 1)
	WARNINGS_AS_ERRORS=-DTREAT_WARNINGS_AS_ERRORS=1
endif
ifeq (${FORCE_32_BIT}, 1)
	FORCE_32_BIT_FLAG=-DFORCE_32_BIT=1
endif
ifeq (${FORCE_WARN_UNUSED}, 1)
	FORCE_WARN_UNUSED_FLAG=-DFORCE_WARN_UNUSED=1
endif
ifeq (${DISABLE_UNITY}, 1)
	DISABLE_UNITY_FLAG=-DDISABLE_UNITY=1
endif
ifeq (${DISABLE_SANITIZER}, 1)
	DISABLE_SANITIZER_FLAG=-DENABLE_SANITIZER=FALSE -DENABLE_UBSAN=0
endif
ifeq (${DISABLE_UBSAN}, 1)
	DISABLE_SANITIZER_FLAG:=${DISABLE_SANITIZER_FLAG} -DENABLE_UBSAN=0
endif
ifeq (${DISABLE_VPTR_SANITIZER}, 1)
	DISABLE_SANITIZER_FLAG:=${DISABLE_SANITIZER_FLAG} -DDISABLE_VPTR_SANITIZER=1
endif
ifeq (${RELEASE_SANITIZER}, 1)
	DISABLE_SANITIZER_FLAG:=${DISABLE_SANITIZER_FLAG} -DRELEASE_SANITIZER=1
endif
ifeq (${THREADSAN}, 1)
	DISABLE_SANITIZER_FLAG:=${DISABLE_SANITIZER_FLAG} -DENABLE_THREAD_SANITIZER=1
endif
ifeq (${STATIC_LIBCPP}, 1)
	STATIC_LIBCPP=-DSTATIC_LIBCPP=TRUE
endif
GIT_BASE_BRANCH:=main
ifneq (${DUCKDB_GIT_BASE_BRANCH}, )
	GIT_BASE_BRANCH:=${DUCKDB_GIT_BASE_BRANCH}
endif

COMMON_CMAKE_VARS ?=
CMAKE_VARS_BUILD ?=
CMAKE_LLVM_VARS ?=
UNSAFE_NUMERIC_CAST ?=

empty :=
space := $(empty) $(empty)
normalize_extension_list = $(strip $(subst ;,$(space),$(1)))
extension_list = $(subst $(space),;,$(strip $(1)))
true_values := 1 ON on TRUE true YES yes

# Compatibility for the extension selectors used before DUCKDB_USE/DUCKDB_EXTEND. Keep this until DuckDB 2.1.
LEGACY_EXTENSION_SELECTION_VARIABLES := BUILD_EXTENSIONS DUCKDB_EXTENSIONS CORE_EXTENSIONS BUILD_AUTOCOMPLETE \
	BUILD_ICU BUILD_TPCH BUILD_TPCDS BUILD_FTS BUILD_HTTPFS BUILD_JSON LINK_CORE_EXTENSIONS \
	STATICALLY_LINK_EXTENSIONS DUCKDB_CAPABILITIES SKIP_EXTENSIONS DISABLE_PARQUET DISABLE_CORE_FUNCTIONS \
	DISABLE_BUILTIN_EXTENSIONS DISABLE_EXTENSION_LOAD DISABLE_BUILTIN_HTTPLIB
LEGACY_EXTENSION_SELECTION_VARIABLES_USED := $(strip $(foreach legacy_var,$(LEGACY_EXTENSION_SELECTION_VARIABLES),\
	$(if $(filter undefined,$(origin $(legacy_var))),,$(legacy_var))))

LEGACY_BUILD_EXTENSIONS := $(call normalize_extension_list,$(BUILD_EXTENSIONS))
ifneq ($(strip $(DUCKDB_EXTENSIONS)),)
	LEGACY_BUILD_EXTENSIONS := $(call normalize_extension_list,$(DUCKDB_EXTENSIONS))
endif
LEGACY_BUILD_EXTENSIONS += $(call normalize_extension_list,$(CORE_EXTENSIONS))
LEGACY_BUILD_EXTENSIONS += $(if $(filter $(true_values),$(BUILD_AUTOCOMPLETE)),autocomplete)
LEGACY_BUILD_EXTENSIONS += $(if $(filter $(true_values),$(BUILD_ICU)),icu)
LEGACY_BUILD_EXTENSIONS += $(if $(filter $(true_values),$(BUILD_TPCH)),tpch)
LEGACY_BUILD_EXTENSIONS += $(if $(filter $(true_values),$(BUILD_TPCDS)),tpcds)
LEGACY_BUILD_EXTENSIONS += $(if $(filter $(true_values),$(BUILD_FTS)),fts)
LEGACY_BUILD_EXTENSIONS += $(if $(filter $(true_values),$(BUILD_HTTPFS)),httpfs)
LEGACY_BUILD_EXTENSIONS += $(if $(filter $(true_values),$(BUILD_JSON)),json)
LEGACY_BUILD_EXTENSIONS := $(sort $(LEGACY_BUILD_EXTENSIONS))

LEGACY_SKIP_EXTENSIONS := $(call normalize_extension_list,$(SKIP_EXTENSIONS))
LEGACY_SKIP_EXTENSIONS += $(if $(filter $(true_values),$(DISABLE_PARQUET)),parquet)
LEGACY_SKIP_EXTENSIONS += $(if $(filter $(true_values),$(DISABLE_CORE_FUNCTIONS)),core_functions)
LEGACY_SKIP_EXTENSIONS := $(sort $(LEGACY_SKIP_EXTENSIONS))

LEGACY_EXTENSION_USE := core_functions parquet json icu
ifneq ($(strip $(STATICALLY_LINK_EXTENSIONS)),)
	LEGACY_EXTENSION_USE := $(filter-out none,$(call normalize_extension_list,$(STATICALLY_LINK_EXTENSIONS)))
else ifneq ($(filter $(true_values),$(DISABLE_BUILTIN_EXTENSIONS)),)
	LEGACY_EXTENSION_USE := core_functions
else ifneq ($(filter $(true_values),$(LINK_CORE_EXTENSIONS)),)
	LEGACY_EXTENSION_USE += $(LEGACY_BUILD_EXTENSIONS)
endif
LEGACY_CAPABILITY_USE := httplib loadable_extensions local_extension_repository
ifneq ($(strip $(DUCKDB_CAPABILITIES)),)
	LEGACY_CAPABILITY_USE := $(filter-out none,$(call normalize_extension_list,$(DUCKDB_CAPABILITIES)))
endif
ifneq ($(filter $(true_values),$(DISABLE_EXTENSION_LOAD)),)
	LEGACY_CAPABILITY_USE := $(filter-out httplib loadable_extensions,$(LEGACY_CAPABILITY_USE))
endif
ifneq ($(filter $(true_values),$(DISABLE_BUILTIN_HTTPLIB)),)
	LEGACY_CAPABILITY_USE := $(filter-out httplib,$(LEGACY_CAPABILITY_USE))
endif
LEGACY_EXTENSION_USE := $(filter-out $(LEGACY_SKIP_EXTENSIONS),$(sort $(LEGACY_EXTENSION_USE)))
LEGACY_CAPABILITY_USE := $(filter-out $(LEGACY_SKIP_EXTENSIONS),$(sort $(LEGACY_CAPABILITY_USE)))
LEGACY_EXTENSION_EXTEND := $(filter-out $(LEGACY_SKIP_EXTENSIONS) $(LEGACY_EXTENSION_USE),\
	$(sort core_functions parquet json icu $(LEGACY_BUILD_EXTENSIONS)))
LEGACY_EXTENSION_SELECTION_ACTIVE := $(strip $(LEGACY_BUILD_EXTENSIONS) $(LEGACY_SKIP_EXTENSIONS) \
	$(STATICALLY_LINK_EXTENSIONS) $(DUCKDB_CAPABILITIES) \
	$(filter $(true_values),$(LINK_CORE_EXTENSIONS) $(DISABLE_BUILTIN_EXTENSIONS) \
		$(DISABLE_EXTENSION_LOAD) $(DISABLE_BUILTIN_HTTPLIB)))

ifneq ($(filter jemalloc,$(LEGACY_BUILD_EXTENSIONS)),)
	BUILD_JEMALLOC ?= 1
	LEGACY_EXTENSION_EXTEND := $(filter-out jemalloc,$(LEGACY_EXTENSION_EXTEND))
endif
ifneq ($(filter jemalloc,$(LEGACY_SKIP_EXTENSIONS)),)
	BUILD_JEMALLOC ?= 0
endif

ifneq ($(strip $(LEGACY_EXTENSION_SELECTION_VARIABLES_USED)),)
ifneq ($(LEGACY_EXTENSION_SELECTION_ACTIVE),)
ifneq ($(filter-out undefined,$(origin DUCKDB_USE) $(origin DUCKDB_EXTEND)),)
$(warning Legacy extension variables $(LEGACY_EXTENSION_SELECTION_VARIABLES_USED) are deprecated and ignored because DUCKDB_USE or DUCKDB_EXTEND is set; support will be removed in DuckDB 2.1)
else
	DUCKDB_USE := $(call extension_list,$(LEGACY_EXTENSION_USE) $(LEGACY_CAPABILITY_USE))
	DUCKDB_EXTEND := $(call extension_list,$(LEGACY_EXTENSION_EXTEND))
$(warning Legacy extension variables $(LEGACY_EXTENSION_SELECTION_VARIABLES_USED) are deprecated; use DUCKDB_USE='$(DUCKDB_USE)' DUCKDB_EXTEND='$(DUCKDB_EXTEND)' instead; support will be removed in DuckDB 2.1)
endif
else
$(warning Legacy extension variables $(LEGACY_EXTENSION_SELECTION_VARIABLES_USED) are deprecated and have no effect with their current values; support will be removed in DuckDB 2.1)
endif
endif

ifneq ($(filter $(true_values),$(BUILD_ALL_EXT)),)
	EXTENSION_CONFIGS := .github/config/in_tree_extensions.cmake;.github/config/out_of_tree_extensions.cmake;.github/config/rust_based_extensions.cmake
else ifneq ($(filter $(true_values),$(BUILD_ALL_IT_EXT)),)
	EXTENSION_CONFIGS := .github/config/in_tree_extensions.cmake
else ifneq ($(filter $(true_values),$(BUILD_ALL_OOT_EXT)),)
	EXTENSION_CONFIGS := .github/config/out_of_tree_extensions.cmake
endif
LEGACY_BUILD_ALL_VARIABLES_USED := $(strip $(foreach legacy_var,BUILD_ALL_EXT BUILD_ALL_IT_EXT BUILD_ALL_OOT_EXT,\
	$(if $(filter undefined,$(origin $(legacy_var))),,$(legacy_var))))
ifneq ($(strip $(LEGACY_BUILD_ALL_VARIABLES_USED)),)
ifneq ($(filter $(true_values),$(BUILD_ALL_EXT) $(BUILD_ALL_IT_EXT) $(BUILD_ALL_OOT_EXT)),)
$(warning Legacy extension variables $(LEGACY_BUILD_ALL_VARIABLES_USED) are deprecated; use EXTENSION_CONFIGS='$(EXTENSION_CONFIGS)' instead; support will be removed in DuckDB 2.1)
else
$(warning Legacy extension variables $(LEGACY_BUILD_ALL_VARIABLES_USED) are deprecated and have no effect with their current values; support will be removed in DuckDB 2.1)
endif
endif

LEGACY_UNSUPPORTED_EXTENSION_VARIABLES_USED := $(strip $(foreach legacy_var,DUCKDB_NEW_EXTENSION_BUILD USE_MERGED_VCPKG_MANIFEST,\
	$(if $(filter undefined,$(origin $(legacy_var))),,$(legacy_var))))
ifneq ($(strip $(LEGACY_UNSUPPORTED_EXTENSION_VARIABLES_USED)),)
$(warning $(LEGACY_UNSUPPORTED_EXTENSION_VARIABLES_USED) no longer change extension builds and can be removed; support will be removed in DuckDB 2.1)
endif
ifdef DUCKDB_VERSION
        COMMON_CMAKE_VARS:=${COMMON_CMAKE_VARS} -DDUCKDB_EXPLICIT_VERSION="${DUCKDB_VERSION}"
else ifdef OVERRIDE_GIT_DESCRIBE
        COMMON_CMAKE_VARS:=${COMMON_CMAKE_VARS} -DDUCKDB_EXPLICIT_VERSION="${OVERRIDE_GIT_DESCRIBE}"
else ifdef DUCKDB_EXPLICIT_VERSION
        COMMON_CMAKE_VARS:=${COMMON_CMAKE_VARS} -DDUCKDB_EXPLICIT_VERSION="${DUCKDB_EXPLICIT_VERSION}"
else
        COMMON_CMAKE_VARS:=${COMMON_CMAKE_VARS} -DDUCKDB_EXPLICIT_VERSION=""
endif

ifdef DUCKDB_COMMIT
        COMMON_CMAKE_VARS:=${COMMON_CMAKE_VARS} -DGIT_COMMIT_HASH="${DUCKDB_COMMIT}"
endif

ifneq (${CXX_STANDARD}, )
        CMAKE_VARS:=${CMAKE_VARS} -DCMAKE_CXX_STANDARD="${CXX_STANDARD}"
endif
ifeq (${DISABLE_MAIN_DUCKDB_LIBRARY}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DBUILD_MAIN_DUCKDB_LIBRARY=0
endif
ifneq (${EXTENSION_STATIC_BUILD}, )
	CMAKE_VARS:=${CMAKE_VARS} -DEXTENSION_STATIC_BUILD=${EXTENSION_STATIC_BUILD}
endif
ifeq (${GENERATE_EXTENSION_ENTRIES}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DGENERATE_EXTENSION_ENTRIES=1
endif
ifneq (${UNSAFE_NUMERIC_CAST}, )
	CMAKE_VARS:=${CMAKE_VARS} -DUNSAFE_NUMERIC_CAST=1
endif
ifeq (${BUILD_EXTENSIONS_ONLY}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DBUILD_EXTENSIONS_ONLY=1
endif

ifeq (${BUILD_BENCHMARK}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DBUILD_BENCHMARKS=1
endif
ifdef BUILD_JEMALLOC
	CMAKE_VARS:=${CMAKE_VARS} -DENABLE_JEMALLOC=${BUILD_JEMALLOC}
endif
ifneq ($(strip $(DUCKDB_USE)$(DUCKDB_EXTEND)$(EXTENSION_CONFIGS)),)
	DUCKDB_EXTENSION_SYNC_ENABLED:=1
endif

ifeq (${STATIC_OPENSSL}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DOPENSSL_USE_STATIC_LIBS=1
endif
ifeq (${BUILD_TPCE}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DBUILD_TPCE=1
endif
ifeq (${CONFIGURE_R}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DCONFIGURE_R=1
endif
ifneq ($(TIDY_BINARY),)
	TIDY_BINARY_PARAMETER := -clang-tidy-binary ${TIDY_BINARY}
endif
TIDY_SHARD_COUNT ?= 1
TIDY_SHARD_INDEX ?= 0
TIDY_SHARD_PARAMETERS := --shard-count ${TIDY_SHARD_COUNT} --shard-index ${TIDY_SHARD_INDEX}
CLANGD_TIDY_VERSION := 1.1.1
CLANGD_TIDY_VENV ?= $(abspath build/clangd-tidy-venv)
ifeq ($(CLANGD_TIDY_BINARY),)
ifneq ($(wildcard $(CLANGD_TIDY_VENV)/bin/clangd-tidy),)
CLANGD_TIDY_BINARY := $(CLANGD_TIDY_VENV)/bin/clangd-tidy
endif
endif
ifeq ($(CLANGD_BINARY),)
ifneq ("${CMAKE_LLVM_PATH}", "")
CLANGD_BINARY := ${CMAKE_LLVM_PATH}/bin/clangd
endif
endif
ifneq ($(CLANGD_TIDY_BINARY),)
	CLANGD_TIDY_BINARY_PARAMETER := --clangd-tidy-binary ${CLANGD_TIDY_BINARY}
endif
ifneq ($(CLANGD_BINARY),)
	CLANGD_BINARY_PARAMETER := --clangd-binary ${CLANGD_BINARY}
endif
ifneq ($(CLANGD_TIDY_QUERY_DRIVER),)
	CLANGD_TIDY_QUERY_DRIVER_PARAMETER := --query-driver ${CLANGD_TIDY_QUERY_DRIVER}
endif
ifneq ($(TIDY_CHECKS),)
        TIDY_PERFORM_CHECKS := '-checks=${TIDY_CHECKS}'
endif
ifneq ("${FORCE_QUERY_LOG}a", "a")
	CMAKE_VARS:=${CMAKE_VARS} -DFORCE_QUERY_LOG=${FORCE_QUERY_LOG}
endif
ifneq ($(origin DUCKDB_USE),undefined)
	CMAKE_VARS:=${CMAKE_VARS} -DDUCKDB_USE="$(DUCKDB_USE)"
endif
ifneq ($(origin DUCKDB_EXTEND),undefined)
	CMAKE_VARS:=${CMAKE_VARS} -DDUCKDB_EXTEND="$(DUCKDB_EXTEND)"
endif
ifneq ($(EXTENSION_CONFIG_BASE_DIR),)
	CMAKE_VARS:=${CMAKE_VARS} -DEXTENSION_CONFIG_BASE_DIR="$(EXTENSION_CONFIG_BASE_DIR)"
endif
ifeq ($(SHADOW_FORBIDDEN_FUNCTIONS),1)
	CMAKE_VARS:=${CMAKE_VARS} -DSHADOW_FORBIDDEN_FUNCTIONS=1
endif
ifneq ($(EXTENSION_CONFIGS),)
	CMAKE_VARS:=${CMAKE_VARS} -DDUCKDB_EXTENSION_CONFIGS="$(EXTENSION_CONFIGS)"
endif
ifeq ($(EXTENSION_TESTS_ONLY), 1)
	CMAKE_VARS:=${CMAKE_VARS} -DEXTENSION_TESTS_ONLY=1
endif
ifneq ($(EXTRA_CMAKE_VARIABLES),)
	CMAKE_VARS:=${CMAKE_VARS} ${EXTRA_CMAKE_VARIABLES}
endif
ifeq (${CRASH_ON_ASSERT}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DCRASH_ON_ASSERT=1
endif
ifeq (${FORCE_ASSERT}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DFORCE_ASSERT=1
endif
ifeq (${FORCE_DEBUG}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DFORCE_DEBUG=1
endif
ifneq (${SMALLER_BINARY},)
	CMAKE_VARS:=${CMAKE_VARS} -DSMALLER_BINARY='${SMALLER_BINARY}'
endif
ifneq (${SMALLER_BINARY_EXCEPT},)
	CMAKE_VARS:=${CMAKE_VARS} -DSMALLER_BINARY_EXCEPT='${SMALLER_BINARY_EXCEPT}'
endif
ifeq (${DISABLE_STRING_INLINE}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDISABLE_STR_INLINE=1
endif
ifeq (${DISABLE_MEMORY_SAFETY}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDISABLE_MEMORY_SAFETY=1
endif
ifeq (${DISABLE_RTTI}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDISABLE_RTTI=1
endif
ifeq (${DISABLE_ASSERTIONS}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDISABLE_ASSERTIONS=1
endif
ifeq (${DESTROY_UNPINNED_BLOCKS}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDESTROY_UNPINNED_BLOCKS=1
endif
ifeq (${FORCE_ASYNC_SINK_SOURCE}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DFORCE_ASYNC_SINK_SOURCE=1
endif
ifeq (${RUN_SLOW_VERIFIERS}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DRUN_SLOW_VERIFIERS=1
endif
ifeq (${ALTERNATIVE_VERIFY}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DALTERNATIVE_VERIFY=1
endif
ifeq (${DISABLE_POINTER_SALT}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDISABLE_POINTER_SALT=1
endif
ifeq (${HASH_ZERO}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DHASH_ZERO=1
endif
ifeq (${LATEST_STORAGE}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DLATEST_STORAGE=1
endif
ifeq (${BLOCK_VERIFICATION}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DBLOCK_VERIFICATION=1
endif
ifneq (${DISABLE_CPP_UNITTESTS}, )
	CMAKE_VARS:=${CMAKE_VARS} -DENABLE_UNITTEST_CPP_TESTS=0
endif
ifeq (${DEBUG_MOVE}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDEBUG_MOVE=1
endif
ifeq (${DEBUG_ALLOCATION}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDEBUG_ALLOCATION=1
endif
ifeq (${DEBUG_STACKTRACE}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DDEBUG_STACKTRACE=1
endif
ifeq (${DISABLE_SHELL}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DBUILD_SHELL=0
endif
CMAKE_VARS:=${CMAKE_VARS} -DLOCAL_EXTENSION_REPO="${LOCAL_EXTENSION_REPO}"
ifneq (${OSX_BUILD_ARCH}, )
	CMAKE_VARS:=${CMAKE_VARS} -DOSX_BUILD_ARCH=${OSX_BUILD_ARCH}
endif
ifeq (${OSX_BUILD_UNIVERSAL}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DOSX_BUILD_UNIVERSAL=1
endif
ifneq ("${DUCKDB_LINKER}", "")
	CMAKE_VARS:=${CMAKE_VARS} -DDUCKDB_LINKER=${DUCKDB_LINKER}
endif
ifdef SKIP_PLATFORM_UTIL
	CMAKE_VARS:=${CMAKE_VARS} -DSKIP_PLATFORM_UTIL=1
endif
ifdef DEBUG_STACKTRACE
	CMAKE_VARS:=${CMAKE_VARS} -DDEBUG_STACKTRACE=1
endif
ifeq (${NATIVE_ARCH}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DNATIVE_ARCH=1
endif
ifneq (${DUCKDB_OPTIMIZATION_PROFILE}, )
	CMAKE_VARS:=${CMAKE_VARS} -DDUCKDB_OPTIMIZATION_PROFILE=${DUCKDB_OPTIMIZATION_PROFILE}
endif
ifeq (${OVERRIDE_NEW_DELETE}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DOVERRIDE_NEW_DELETE=1
endif
ifeq (${STANDALONE_DEBUG}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DSTANDALONE_DEBUG=1
endif
ifneq (${DUCKDB_PREBUILT_LIBRARY}, )
	CMAKE_VARS:=${CMAKE_VARS} -DPREBUILT_BINARY=${DUCKDB_PREBUILT_LIBRARY}
endif
ifdef REDUCE_SYMBOLS
	CMAKE_VARS:=${CMAKE_VARS} -DREDUCE_SYMBOLS=1
endif


# Optional overrides
ifneq (${STANDARD_VECTOR_SIZE}, )
	CMAKE_VARS:=${CMAKE_VARS} -DSTANDARD_VECTOR_SIZE=${STANDARD_VECTOR_SIZE}
endif

# Enable VCPKG for this build
ifneq ("${VCPKG_TOOLCHAIN_PATH}", "")
	CMAKE_VARS_BUILD:=${CMAKE_VARS_BUILD} -DCMAKE_TOOLCHAIN_FILE='${VCPKG_TOOLCHAIN_PATH}' -DVCPKG_BUILD=1
endif
ifneq ("${VCPKG_TARGET_TRIPLET}", "")
	CMAKE_VARS_BUILD:=${CMAKE_VARS_BUILD} -DVCPKG_TARGET_TRIPLET='${VCPKG_TARGET_TRIPLET}'
endif
sync_extensions_into =
vcpkg_cmake_flag =
ifdef DUCKDB_EXTENSION_SYNC_ENABLED
sync_extensions_into = $(PYTHON) scripts/sync_out_of_tree_extensions.py $(if $(filter-out undefined,$(origin DUCKDB_USE)),--duckdb-use "$(DUCKDB_USE)") $(if $(filter-out undefined,$(origin DUCKDB_EXTEND)),--duckdb-extend "$(DUCKDB_EXTEND)") $(if $(EXTENSION_CONFIGS),--extension-configs "$(EXTENSION_CONFIGS)") $(if $(EXTENSION_CONFIG_BASE_DIR),--extension-config-base-dir "$(EXTENSION_CONFIG_BASE_DIR)") --output-dir '$(1)' &&
ifneq ("${VCPKG_TOOLCHAIN_PATH}", "")
vcpkg_cmake_flag = -DVCPKG_MANIFEST_DIR='$(1)'
endif
endif

ifneq ("${LTO}", "")
	CMAKE_VARS:=${CMAKE_VARS} -DCMAKE_LTO='${LTO}'
endif
ifneq ("${LTO_JOBS}", "")
	CMAKE_VARS:=${CMAKE_VARS} -DCMAKE_LTO_JOBS='${LTO_JOBS}'
endif
ifeq (${EXPORT_DYNAMIC_SYMBOLS}, 1)
	CMAKE_VARS:=${CMAKE_VARS} -DEXPORT_DYNAMIC_SYMBOLS=1
endif
ifneq ("${CMAKE_LLVM_PATH}", "")
	CMAKE_VARS:=${CMAKE_VARS} -DCMAKE_RANLIB='${CMAKE_LLVM_PATH}/bin/llvm-ranlib' -DCMAKE_AR='${CMAKE_LLVM_PATH}/bin/llvm-ar' -DCMAKE_CXX_COMPILER='${CMAKE_LLVM_PATH}/bin/clang++' -DCMAKE_C_COMPILER='${CMAKE_LLVM_PATH}/bin/clang' -DCMAKE_EXE_LINKER_FLAGS_INIT='-L${CMAKE_LLVM_PATH}/lib -L${CMAKE_LLVM_PATH}/lib/c++' -DCMAKE_SHARED_LINKER_FLAGS_INIT='-L${CMAKE_LLVM_PATH}/lib -L${CMAKE_LLVM_PATH}/lib/c++'  -DCMAKE_MODULE_LINKER_FLAGS_INIT='-L${CMAKE_LLVM_PATH}/lib -L${CMAKE_LLVM_PATH}/lib/c++'
endif

CMAKE_VARS:=${CMAKE_VARS} ${COMMON_CMAKE_VARS}

ifdef DUCKDB_PLATFORM
	ifneq ("${DUCKDB_PLATFORM}", "")
		CMAKE_VARS:=${CMAKE_VARS} -DDUCKDB_EXPLICIT_PLATFORM='${DUCKDB_PLATFORM}'
	endif
endif

clean:
	rm -rf build

EXTENSION_REPOSITORY_PATH ?= build/release/repository
EXTENSION_BUCKET ?= duckdb-core-extensions
SIGNED_EXTENSIONS_DIR ?= $(EXTENSION_REPOSITORY_PATH)
EXTENSION_SIGNING_PUBLIC_KEY_FILE ?=

.PHONY: upload-extensions
upload-extensions:
	CI_CPU_COUNT="$(CI_CPU_COUNT)" ./scripts/extension-upload-repository.sh "$(EXTENSION_REPOSITORY_PATH)" "$(EXTENSION_BUCKET)"

.PHONY: verify-extension-signing
verify-extension-signing:
	CI_CPU_COUNT="$(CI_CPU_COUNT)" ./scripts/verify-extension-signing.sh "$(SIGNED_EXTENSIONS_DIR)" "$(EXTENSION_SIGNING_PUBLIC_KEY_FILE)"

define cmake_build
	mkdir -p ./$(1) && \
	$(call sync_extensions_into,${PROJ_DIR}$(1)) \
	cd $(1) && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${FORCE_32_BIT_FLAG} ${DISABLE_UNITY_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${CMAKE_VARS} ${CMAKE_VARS_BUILD} $(call vcpkg_cmake_flag,${PROJ_DIR}$(1)) $(3) -DCMAKE_BUILD_TYPE=$(2) ../.. && \
	$(NINJA_BUILD_WRAPPER) cmake --build . --config $(2)
endef

debug: ${EXTENSION_CONFIG_STEP}
	$(call cmake_build,build/debug,Debug,-DDEBUG_MOVE=1)

release: ${EXTENSION_CONFIG_STEP}
	$(call cmake_build,build/release,Release,${FORCE_WARN_UNUSED_FLAG})

WINDOWS_GENERATOR_PLATFORM ?= x64
BUNDLED_EXTENSIONS_CONFIGS ?= $(PWD)/.github/config/bundled_extensions.cmake
windows_release: ${EXTENSION_CONFIG_STEP}
	$(call sync_extensions_into,${PROJ_DIR}) \
	cmake $(GENERATOR) $(FORCE_COLOR) $(if $(filter ninja,$(GEN)),,-DCMAKE_GENERATOR_PLATFORM=$(WINDOWS_GENERATOR_PLATFORM)) ${WARNINGS_AS_ERRORS} ${FORCE_WARN_UNUSED_FLAG} ${FORCE_32_BIT_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${CMAKE_VARS} ${CMAKE_VARS_BUILD} $(call vcpkg_cmake_flag,${PROJ_DIR}) -DCMAKE_BUILD_TYPE=Release -DDUCKDB_USE="core_functions;icu;json;parquet;autocomplete;httplib;loadable_extensions" -DDUCKDB_EXTENSION_CONFIGS="$(BUNDLED_EXTENSIONS_CONFIGS)" . && \
	$(NINJA_BUILD_WRAPPER) cmake --build . --config Release

windows_release_32: ${EXTENSION_CONFIG_STEP}
	$(call sync_extensions_into,${PROJ_DIR}) \
	cmake $(GENERATOR) $(FORCE_COLOR) $(if $(filter ninja,$(GEN)),,-DCMAKE_GENERATOR_PLATFORM=Win32) ${WARNINGS_AS_ERRORS} ${FORCE_WARN_UNUSED_FLAG} ${FORCE_32_BIT_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${CMAKE_VARS} ${CMAKE_VARS_BUILD} $(call vcpkg_cmake_flag,${PROJ_DIR}) -DCMAKE_BUILD_TYPE=Release -DDUCKDB_USE="core_functions;icu;json;parquet;autocomplete;httplib;loadable_extensions" -DDUCKDB_EXTENSION_CONFIGS="$(BUNDLED_EXTENSIONS_CONFIGS)" . && \
	$(NINJA_BUILD_WRAPPER) cmake --build . --config Release

# The wasm targets do not go through CMAKE_VARS, so DISABLE_RTTI is forwarded explicitly.
# Off by default, like every other target: a wasm build pulls in out-of-tree extensions that
# carry their own vcpkg dependencies, and whether those are built without RTTI is a decision
# for the extension, not for duckdb's Makefile.
ifeq (${DISABLE_RTTI}, 1)
WASM_RTTI_FLAG:=-DDISABLE_RTTI=1
endif

wasm_mvp: ${EXTENSION_CONFIG_STEP}
	mkdir -p ./build/wasm_mvp && \
	emcmake cmake $(GENERATOR) -DWASM_LOADABLE_EXTENSIONS=1 -DBUILD_EXTENSIONS_ONLY=1 -Bbuild/wasm_mvp ${WASM_RTTI_FLAG} -DCMAKE_CXX_FLAGS="-DDUCKDB_CUSTOM_PLATFORM=wasm_mvp" -DDUCKDB_EXPLICIT_PLATFORM="wasm_mvp" ${COMMON_CMAKE_VARS} ${TOOLCHAIN_FLAGS} && \
	emmake make -j${CI_BUILD_JOBS} -Cbuild/wasm_mvp

wasm_eh: WASM_EH_CMAKE_VARS=-DBUILD_EXTENSIONS_ONLY=1
wasm_ci: WASM_EH_CMAKE_VARS=
wasm_eh wasm_ci: ${EXTENSION_CONFIG_STEP}
	mkdir -p ./build/wasm_eh && \
	emcmake cmake $(GENERATOR) -DWASM_LOADABLE_EXTENSIONS=1 $(WASM_EH_CMAKE_VARS) -Bbuild/wasm_eh ${WASM_RTTI_FLAG} -DCMAKE_CXX_FLAGS="-fwasm-exceptions -DDUCKDB_NO_THREADS=1 -DWEBDB_FAST_EXCEPTIONS=1 -DDUCKDB_CUSTOM_PLATFORM=wasm_eh" -DDUCKDB_EXPLICIT_PLATFORM="wasm_eh" ${COMMON_CMAKE_VARS} ${TOOLCHAIN_FLAGS} && \
	emmake make -j${CI_BUILD_JOBS} -Cbuild/wasm_eh

wasm_threads: ${EXTENSION_CONFIG_STEP}
	mkdir -p ./build/wasm_threads && \
	emcmake cmake $(GENERATOR) -DWASM_LOADABLE_EXTENSIONS=1 -DBUILD_EXTENSIONS_ONLY=1 -Bbuild/wasm_threads ${WASM_RTTI_FLAG} -DCMAKE_CXX_FLAGS="-fwasm-exceptions -DWEBDB_FAST_EXCEPTIONS=1 -DWITH_WASM_THREADS=1 -DWITH_WASM_SIMD=1 -DWITH_WASM_BULK_MEMORY=1 -DDUCKDB_CUSTOM_PLATFORM=wasm_threads -pthread" -DDUCKDB_EXPLICIT_PLATFORM="wasm_threads" ${COMMON_CMAKE_VARS} -DUSE_WASM_THREADS=1 -DCMAKE_C_FLAGS="-pthread" ${TOOLCHAIN_FLAGS} && \
	emmake make -j${CI_BUILD_JOBS} -Cbuild/wasm_threads

cldebug: ${EXTENSION_CONFIG_STEP}
	$(call cmake_build,build/cldebug,Debug,-DENABLE_SANITIZER=0 -DENABLE_UBSAN=0)

codecov: ${EXTENSION_CONFIG_STEP}
	$(call cmake_build,build/codecov,Release,-DENABLE_SANITIZER=0 -DENABLE_UBSAN=0 -DCOVERAGE=1)

clreldebug:
	mkdir -p ./build/clreldebug && \
	cd build/clreldebug && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${FORCE_32_BIT_FLAG} ${DISABLE_UNITY_FLAG} ${STATIC_LIBCPP} ${CMAKE_VARS} -DENABLE_SANITIZER=0 -DENABLE_UBSAN=0 -DCMAKE_BUILD_TYPE=RelWithDebInfo ../.. && \
	$(NINJA_BUILD_WRAPPER) cmake --build . --config RelWithDebInfo

SYNC_OUTPUT_DIR ?= build
sync_out_of_tree_extensions:
	$(PYTHON) scripts/sync_out_of_tree_extensions.py $(if $(filter-out undefined,$(origin DUCKDB_USE)),--duckdb-use "$(DUCKDB_USE)") $(if $(filter-out undefined,$(origin DUCKDB_EXTEND)),--duckdb-extend "$(DUCKDB_EXTEND)") $(if $(EXTENSION_CONFIGS),--extension-configs "$(EXTENSION_CONFIGS)") $(if $(EXTENSION_CONFIG_BASE_DIR),--extension-config-base-dir "$(EXTENSION_CONFIG_BASE_DIR)") --output-dir '${PROJ_DIR}$(SYNC_OUTPUT_DIR)'

extension_configuration: build/extension_configuration/vcpkg.json

extension/extension_config_local.cmake:
	touch extension/extension_config_local.cmake

build/extension_configuration/vcpkg.json: extension/extension_config_local.cmake extension/extension_config.cmake
	$(PYTHON) scripts/sync_out_of_tree_extensions.py $(if $(filter-out undefined,$(origin DUCKDB_USE)),--duckdb-use "$(DUCKDB_USE)") $(if $(filter-out undefined,$(origin DUCKDB_EXTEND)),--duckdb-extend "$(DUCKDB_EXTEND)") $(if $(EXTENSION_CONFIGS),--extension-configs "$(EXTENSION_CONFIGS)") $(if $(EXTENSION_CONFIG_BASE_DIR),--extension-config-base-dir "$(EXTENSION_CONFIG_BASE_DIR)") --output-dir '${PROJ_DIR}build/extension_configuration'

unittest: debug
	build/debug/test/run $(T)

unittest_reldebug:
	build/reldebug/test/run $(UNITTEST_SLOW_FLAGS) $(T)

ifneq ($(SKIP_BUILD),1)
unittest_release: release
endif
unittest_release:
	build/release/test/run $(T)

TEST_CONFIGS_QUERY_VERIFICATION := \
	test/configs/verify_statement_explain_sql.json \
	test/configs/verify_statement_copy.json \
	test/configs/verify_statement_to_string.json \
	test/configs/verify_statement_explain.json \
	test/configs/verify_statement_prepare.json \
	test/configs/verify_serializer.json \
	test/configs/verify_stats.json \
	test/configs/verify_statement_serialization.json \
	test/configs/disable_optimizer.json \
	test/configs/verification_projection.json \
	test/configs/verify_column_bindings.json

TEST_CONFIGS_EXECUTION := \
	test/configs/internal_vector_serialization.json \
	test/configs/internal_vector_verification.json \
	test/configs/force_external.json \
	test/configs/verify_fetch_row.json \
	test/configs/disable_caching_operators.json \
	test/configs/variant_vector.json \
	test/configs/verify_aggregate_state_export.json \
	test/configs/verify_functions.json \
	test/configs/shredded_vector.json \
	test/configs/verify_progress.json

TEST_CONFIGS_PERSISTENCE := \
	test/configs/force_storage.json \
	test/configs/force_storage_restart.json \
	test/configs/latest_storage.json \
	test/configs/wal_verification.json \
	test/configs/vacuum_rebuild_indexes_force_storage.json \
	test/configs/no_local_filesystem.json \
	test/configs/encryption.json \
	test/configs/v1_storage.json \
	test/configs/v1_storage_block_size_16kB.json

TEST_CONFIGS_STORAGE_ENGINE := \
	test/configs/verify_compression.json \
	test/configs/block_verification.json \
	test/configs/block_verification_latest.json \
	test/configs/block_size_16kB.json \
	test/configs/latest_storage_block_size_16kB.json \
	test/configs/block_allocator_100mib.json \
	test/configs/compressed_in_memory.json \
	test/configs/prefetch_all_storage.json \
	test/configs/force_storage_mmap.json

TEST_CONFIGS := $(TEST_CONFIGS_QUERY_VERIFICATION) $(TEST_CONFIGS_EXECUTION) $(TEST_CONFIGS_PERSISTENCE) \
	$(TEST_CONFIGS_STORAGE_ENGINE)

.PHONY: test_configs test_configs_query_verification test_configs_execution test_configs_persistence \
	test_configs_storage_engine

test_configs:
	./build/release/test/run $(foreach cfg,$(TEST_CONFIGS),--test-config=$(cfg))

test_configs_query_verification:
	./build/release/test/run $(foreach cfg,$(TEST_CONFIGS_QUERY_VERIFICATION),--test-config=$(cfg))


test_configs_execution:
	./build/release/test/run $(foreach cfg,$(TEST_CONFIGS_EXECUTION),--test-config=$(cfg))

test_configs_persistence:
	./build/release/test/run $(foreach cfg,$(TEST_CONFIGS_PERSISTENCE),--test-config=$(cfg))

test_configs_storage_engine:
	./build/release/test/run $(foreach cfg,$(TEST_CONFIGS_STORAGE_ENGINE),--test-config=$(cfg))

test_vector:
	./build/release/test/run --test-flags="--verify-vector dictionary_expression --skip-compiled"
	./build/release/test/run --test-flags="--verify-vector dictionary_operator --skip-compiled"
	./build/release/test/run --test-flags="--verify-vector constant_operator --skip-compiled"
	./build/release/test/run --test-flags="--verify-vector sequence_operator --skip-compiled"
	./build/release/test/run --test-flags="--verify-vector nested_shuffle --skip-compiled"

test_table_scan:
	./build/release/test/run --test-flags='--on-init "SET debug_physical_table_scan_execution_strategy=SYNCHRONOUS;"'

test_storage:
	$(PYTHON) scripts/test_storage_compatibility.py --versions "1.2.1|1.3.2|1.4.3" --new-unittest build/release/$(UNITTEST_BINARY)

.PHONY: alltest_release_tag test_release_tag
alltest_release_tag:
	./build/release/test/run --test-flags="--select-tag release" '*' $(T)

test_release_tag:
	./build/release/test/run --test-flags="--select-tag release" $(T)

unittest_relassert:
	build/relassert/test/run $(UNITTEST_SLOW_FLAGS) $(T)

smoke:
	$(SMOKE_RUNNER) --batch-timeout 120 --test-list test/smoke_tests.list $(T)

unittestarrow:
	build/debug/test/run "[arrow]"

allunit:
	./build/release/test/run --workers=50% '*' $(T)
ifndef CI
allunit: release
endif

unittest_threadsan: export TSAN_OPTIONS ?= "suppressions=./.sanitizer-thread-suppressions.txt"
unittest_threadsan: unittest_reldebug
	build/reldebug/test/run $(UNITTEST_HUGE_FLAGS) --test-config test/configs/threadsan.json "[intraquery],[interquery],[detailed_profiler],test/sql/tpch/tpch_sf01.test_slow" $(T)
	build/reldebug/test/run $(UNITTEST_HUGE_FLAGS) --test-config test/configs/threadsan.json --test-flags="--force-storage --force-reload" "[interquery]" $(T)

.PHONY: unittest_threadsan_extra
unittest_threadsan_extra: export TSAN_OPTIONS ?= "suppressions=./.sanitizer-thread-suppressions.txt"
unittest_threadsan_extra: unittest_reldebug
	build/reldebug/test/run $(UNITTEST_HUGE_FLAGS) --test-config test/configs/threadsan.json --test-flags="--force-storage" "[interquery]" $(T)

docs:
	mkdir -p ./build/docs && \
	doxygen Doxyfile

doxygen: docs
	open build/docs/html/index.html

reldebug: ${EXTENSION_CONFIG_STEP}
	$(call cmake_build,build/reldebug,RelWithDebInfo,)

relassert: ${EXTENSION_CONFIG_STEP}
	$(call cmake_build,build/relassert,RelWithDebInfo,-DFORCE_ASSERT=1)

.PHONY: relassert-artifact

relassert-artifact:
	RELEASE_ARTIFACT_SOURCE_DIR="$(RELEASE_ARTIFACT_SOURCE_DIR)" \
	RELEASE_ARTIFACT_STAGING_DIR="$(RELEASE_ARTIFACT_STAGING_DIR)" \
	RELEASE_ARTIFACT_TARBALL="$(RELEASE_ARTIFACT_TARBALL)" \
	bash scripts/prepare_build_artifact.sh relassert

.PHONY: release-artifact

release-artifact:
	RELEASE_ARTIFACT_SOURCE_DIR="$(RELEASE_ARTIFACT_SOURCE_DIR)" \
	RELEASE_ARTIFACT_STAGING_DIR="$(RELEASE_ARTIFACT_STAGING_DIR)" \
	RELEASE_ARTIFACT_TARBALL="$(RELEASE_ARTIFACT_TARBALL)" \
	bash scripts/prepare_build_artifact.sh release

.PHONY: cli-release-artifact

cli-release-artifact:
	bash scripts/package_release_artifact.sh cli "$(ARTIFACT_SUFFIX)" "$(CLI_BINARY)"

.PHONY: cli-zip-release-artifact

cli-zip-release-artifact:
	PYTHON="$(PYTHON)" bash scripts/package_release_artifact.sh cli-zip "$(ARTIFACT_SUFFIX)" "$(CLI_BINARY)"

.PHONY: shared-libs-release-artifact

shared-libs-release-artifact:
	bash scripts/package_release_artifact.sh shared-libs "$(ARTIFACT_SUFFIX)" $(SHARED_LIBRARIES)

# Writes a C source defining duckdb_register_static_extensions(), which links statically built extensions and
# capabilities into a program: compile it next to your own sources, put their archives before libduckdb_static.a, and
# call the function before opening a database (or compile extension/loader/static_extension_autoregister.cpp too to have
# it called before main). DUCKDB_USE picks both extensions and capabilities and defaults to every built archive.
LOADER_BUILT_CAPABILITIES = $(if $(wildcard $(STATIC_EXTENSION_LOADER_BUILD_DIR)/src/main/http/libduckdb_httplib.a),httplib) $(if $(wildcard $(STATIC_EXTENSION_LOADER_BUILD_DIR)/src/main/extension/libduckdb_loadable_extensions.a),loadable_extensions)
LOADER_BUILT_EXTENSIONS = $(patsubst lib%_extension.a,%,$(notdir $(wildcard $(STATIC_EXTENSION_LOADER_BUILD_DIR)/extension/*/lib*_extension.a)))
LOADER_SELECTION = $(if $(filter-out undefined,$(origin DUCKDB_USE)),$(subst ;, ,$(DUCKDB_USE)),$(LOADER_BUILT_CAPABILITIES) $(LOADER_BUILT_EXTENSIONS))
STATIC_EXTENSION_LOADER_BUILD_DIR ?= build/release
STATIC_EXTENSION_LOADER_FILE ?= $(STATIC_EXTENSION_LOADER_BUILD_DIR)/static_extension_loader.c

.PHONY: static_extension_loader
static_extension_loader:
	$(PYTHON) scripts/generate_static_extension_loader.py --output "$(STATIC_EXTENSION_LOADER_FILE)" \
		$(LOADER_SELECTION)
	@echo "Wrote $(STATIC_EXTENSION_LOADER_FILE)"

.PHONY: static-libs-release-artifact

static-libs-release-artifact:
	bash scripts/package_release_artifact.sh static-libs "$(ARTIFACT_SUFFIX)" $(STATIC_LIBRARIES)

.PHONY: symbol-checks symbol-leakage-check banned-symbol-check

symbol-checks: symbol-leakage-check banned-symbol-check

symbol-leakage-check:
	$(PYTHON) scripts/exported_symbols_check.py build/release/src/libduckdb*.so

banned-symbol-check:
	$(PYTHON) scripts/banned_symbols_check.py --directory build/release/src

.PHONY: linux-release-link-checks

linux-release-link-checks:
	bash scripts/ci/linux_release_link_checks.sh

define ensure_apt_commands
	missing=0; \
	for cmd in $(1); do \
		command -v $$cmd >/dev/null 2>&1 || missing=1; \
	done; \
	if [ $$missing -eq 1 ]; then \
		sudo apt-get $(APT_TIMEOUT_OPTS) update -y -q && \
		sudo apt-get $(APT_TIMEOUT_OPTS) install -y -q $(2); \
	fi
endef

define ensure_apt_packages
	missing=0; \
	for pkg in $(1); do \
		dpkg-query -W -f='$${Status}' $$pkg 2>/dev/null | grep -q "install ok installed" || missing=1; \
	done; \
	if [ $$missing -eq 1 ]; then \
		sudo apt-get $(APT_TIMEOUT_OPTS) update -y -q && \
		sudo apt-get $(APT_TIMEOUT_OPTS) install -y -q $(1); \
	fi
endef

APT_TIMEOUT_OPTS=-o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o DPkg::Lock::Timeout=30

.PHONY: toolsci

toolsci:
	$(call ensure_apt_commands,ninja mold ccache pkg-config pigz clang++-20 clangd-20,ninja-build mold ccache pkg-config pigz clang++-20 clangd-20)
	$(call ensure_apt_packages,python3-requests libcurl4-openssl-dev llvm-20-dev libclang-rt-20-dev)
	ls -lh /usr/bin/gcc* /usr/bin/g++* /usr/bin/clang++*
	gcc --version
	g++ --version
	clang++ --version

test_ci:
	$(PYTHON) scripts/ci/run_unit_tests.py --jobs $(CI_CPU_COUNT) $(T)

.PHONY: prepare_tools format_tools parser_tools spell_tools
prepare_tools: spell_tools capigen_venv

format_tools: parser_tools spell_tools

parser_tools:
	$(call ensure_apt_commands,ninja,ninja-build)
	sudo pip3 install --break-system-packages cxxheaderparser pcpp
	@echo "::group::Installed Python packages"
	pip3 freeze
	@echo "::endgroup::"

spell_tools:
	@if [ "$$(uname -s)" != "Linux" ]; then \
		echo "Skipping spell_tools on non-Linux"; \
	else \
		set -eu; \
		VERSION=1.45.1; \
		if [ "$$(uname -m)" = "arm64" ] || [ "$$(uname -m)" = "aarch64" ]; then \
			ARCH="aarch64"; \
		else \
			ARCH="x86_64"; \
		fi; \
		TARGET_FILE="$${ARCH}-unknown-linux-musl"; \
		FILE_NAME="typos-v$${VERSION}-$${TARGET_FILE}.tar.gz"; \
		DOWNLOAD_URL="https://github.com/crate-ci/typos/releases/download/v$${VERSION}/$${FILE_NAME}"; \
		$(PYTHON) scripts/ci/retry.py -- curl --fail --location --silent --show-error --output "/tmp/$${FILE_NAME}" "$${DOWNLOAD_URL}"; \
		TMP_DIR="$$(mktemp -d)"; \
		tar -xzf "/tmp/$${FILE_NAME}" -C "$${TMP_DIR}"; \
		TYPOS_BIN="$$(find "$${TMP_DIR}" -type f -name typos | head -n 1)"; \
		if [ -w /usr/local/bin ]; then \
			install -m 0755 "$${TYPOS_BIN}" /usr/local/bin/typos; \
		else \
			sudo install -m 0755 "$${TYPOS_BIN}" /usr/local/bin/typos; \
		fi; \
		typos --version; \
	fi

.PHONY: enum-integrity-check
enum-integrity-check:
	$(PYTHON) scripts/verify_enum_integrity.py src/include/duckdb.h

.PHONY: extension-patch-check
extension-patch-check:
	cmake -DCONFIG_DIR=.github/config -DPATCH_DIR=.github/patches/extensions -P scripts/check_extension_patches.cmake

.PHONY: format_venv
format_venv:
	@if [ ! -x "$(FORMAT_PYTHON)" ]; then \
		mkdir -p "$(dir $(FORMAT_VENV))" && \
		$(PYTHON) -m venv "$(FORMAT_VENV)"; \
	fi
	@$(FORMAT_PYTHON) -m pip show black >/dev/null 2>&1 || $(FORMAT_PYTHON) -m pip install black==24.*
	@$(FORMAT_PYTHON) -m pip show cmake-format >/dev/null 2>&1 || $(FORMAT_PYTHON) -m pip install cmake-format
	@$(FORMAT_PYTHON) -m pip show clang_format >/dev/null 2>&1 || $(FORMAT_PYTHON) -m pip install clang_format==11.0.1

# Venv holding capigen (the C API generator) and the formatters it needs, pinned by api_spec/pyproject.toml
.PHONY: capigen_venv
capigen_venv:
	@if [ ! -x "$(CAPIGEN_PYTHON)" ]; then \
		mkdir -p "$(dir $(CAPIGEN_VENV))" && \
		$(PYTHON) -m venv "$(CAPIGEN_VENV)" && \
		$(CAPIGEN_PYTHON) -m pip install -U pip; \
	fi
	@$(CAPIGEN_PYTHON) -m pip show capigen >/dev/null 2>&1 || $(CAPIGEN_PYTHON) -m pip install --group api_spec/pyproject.toml:generate

benchmark:
	mkdir -p ./build/release && \
	cd build/release && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${FORCE_WARN_UNUSED_FLAG} ${FORCE_32_BIT_FLAG} ${DISABLE_UNITY_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${CMAKE_VARS} -DBUILD_BENCHMARKS=1 -DCMAKE_BUILD_TYPE=Release ../.. && \
	$(NINJA_BUILD_WRAPPER) cmake --build . --config Release

.PHONY: test_benchmark_sql
test_benchmark_sql:
	$(PYTHON) -u scripts/test_benchmark_sql_runner.py --shell build/relassert/duckdb


tidy-check:
	mkdir -p ./build/tidy && \
	cd build/tidy && \
	cmake -DCLANG_TIDY=1 -DDISABLE_UNITY=1 -DDUCKDB_USE=parquet -DBUILD_SHELL=0 ../.. && \
	$(PYTHON) ../../scripts/run-clang-tidy.py -quiet -j $(CI_CPU_COUNT) ${TIDY_BINARY_PARAMETER} ${TIDY_SHARD_PARAMETERS} ${TIDY_PERFORM_CHECKS}

install-clangd-tidy:
	mkdir -p $(dir $(CLANGD_TIDY_VENV)) && \
	$(PYTHON) -m venv $(CLANGD_TIDY_VENV) && \
	$(CLANGD_TIDY_VENV)/bin/pip install --upgrade 'clangd-tidy==$(CLANGD_TIDY_VERSION)'

tidy-check-clangd:
	mkdir -p ./build/tidy && \
	cd build/tidy && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${STATIC_LIBCPP} ${CMAKE_VARS} -DCLANG_TIDY=1 -DDISABLE_UNITY=1 -DDUCKDB_USE=parquet -DBUILD_SHELL=0 ../.. && \
	trap 'rm -rf ./pchs' EXIT && \
	$(PYTHON) -u ../../scripts/run-clangd-tidy.py -j $(CI_TIDY_JOBS) ${CLANGD_TIDY_BINARY_PARAMETER} ${CLANGD_BINARY_PARAMETER} ${CLANGD_TIDY_QUERY_DRIVER_PARAMETER}

tidy-check-diff:
	mkdir -p ./build/tidy && \
	cd build/tidy && \
	cmake -DCLANG_TIDY=1 -DDISABLE_UNITY=1 -DDUCKDB_USE=parquet -DBUILD_SHELL=0 ../.. && \
	cd ../../ && \
	git diff origin/${GIT_BASE_BRANCH} . ':(exclude)tools' ':(exclude)extension' ':(exclude)test' ':(exclude)benchmark' ':(exclude)third_party' ':(exclude)src/common/adbc' ':(exclude)src/main/capi' ':(exclude)examples/embedded-c' | $(PYTHON) scripts/clang-tidy-diff.py -path build/tidy -quiet -j $(CI_CPU_COUNT) ${TIDY_BINARY_PARAMETER} ${TIDY_PERFORM_CHECKS} -p1

tidy-fix:
	mkdir -p ./build/tidy && \
	cd build/tidy && \
	cmake -DCLANG_TIDY=1 -DDISABLE_UNITY=1 -DDUCKDB_USE=parquet -DBUILD_SHELL=0 ../.. && \
	$(PYTHON) ../../scripts/run-clang-tidy.py -fix

test_compile: # test compilation of individual cpp files
	$(PYTHON) scripts/test_compile.py

format-check: $(FORMAT_SETUP_DEPS)
	$(FORMAT_PYTHON) scripts/format.py --all --check $(T)

format-check-silent: $(FORMAT_SETUP_DEPS)
	$(FORMAT_PYTHON) scripts/format.py --all --check --silent $(T)

format-check-files: $(FORMAT_SETUP_DEPS)
	@test -n "$(FORMAT_FILES_FROM)" || (echo "FORMAT_FILES_FROM is required" >&2; exit 1)
	$(FORMAT_PYTHON) scripts/format.py --files-from "$(FORMAT_FILES_FROM)" --check --silent $(T)

format-fix-files: $(FORMAT_SETUP_DEPS)
	@test -n "$(FORMAT_FILES_FROM)" || (echo "FORMAT_FILES_FROM is required" >&2; exit 1)
	$(FORMAT_PYTHON) scripts/format.py --files-from "$(FORMAT_FILES_FROM)" --fix --noconfirm $(T)

format-fix: $(FORMAT_SETUP_DEPS)
	$(FORMAT_PYTHON) scripts/format.py --all --fix --noconfirm $(T)

format-parser-grammar: $(FORMAT_SETUP_DEPS)
	printf '%s\n' \
		src/include/duckdb/parser/peg/transformer/peg_transformer.hpp \
		src/parser/peg/transformer/transform_generated_trampoline.cpp \
		src/parser/peg/compiled_grammar.cpp \
		src/parser/peg/matcher_factory.cpp \
		src/parser/peg/peg_rule_id.cpp \
		src/include/duckdb/parser/peg/peg_rule_id.hpp \
		src/parser/peg/matcher.cpp | \
		$(FORMAT_PYTHON) scripts/format.py --files-from - --fix --noconfirm

.PHONY: parser-grammar-tools parser-grammar
parser-grammar-tools: $(FORMAT_SETUP_DEPS)
	@$(FORMAT_PYTHON) -m pip show pyyaml >/dev/null 2>&1 || $(FORMAT_PYTHON) -m pip install PyYAML

parser-grammar: parser-grammar-tools
	PYTHON="$(FORMAT_PYTHON)" ./scripts/parser/build_grammar.sh

.PHONY: check-extension-entries
check-extension-entries: extension_configuration $(FORMAT_SETUP_DEPS)
	$(PYTHON) scripts/generate_extensions_function.py
	$(FORMAT_PYTHON) scripts/format.py src/include/duckdb/main/extension_entries.hpp --fix --noconfirm
	@git diff -- src/include/duckdb/main/extension_entries.hpp > extension_entries.hpp.diff
	@if [ -s extension_entries.hpp.diff ]; then \
		cat extension_entries.hpp.diff; \
		echo "error: differences found in src/include/duckdb/main/extension_entries.hpp"; \
		exit 1; \
	else \
		rm -f extension_entries.hpp.diff; \
		echo "No differences found"; \
	fi

format-head: $(FORMAT_SETUP_DEPS)
	$(FORMAT_PYTHON) scripts/format.py HEAD --fix --noconfirm $(T)

format-changes: $(FORMAT_SETUP_DEPS)
	$(FORMAT_PYTHON) scripts/format.py HEAD --fix --noconfirm $(T)

format-main: $(FORMAT_SETUP_DEPS)
	$(FORMAT_PYTHON) scripts/format.py main --fix --noconfirm $(T)

format-feature: $(FORMAT_SETUP_DEPS)
	$(FORMAT_PYTHON) scripts/format.py feature --fix --noconfirm $(T)

format-configs:
	$(foreach file, $(wildcard $(CONFIGS_DIR)/*), jq . < "$(file)" > "$(file).tmp" && mv "$(file).tmp" "$(file)" ;)


third_party/sqllogictest:
	git clone --depth=1 --branch main https://github.com/duckdb/sqllogictest.git third_party/sqllogictest

sqlite: release | third_party/sqllogictest
	git --git-dir third_party/sqllogictest/.git pull
	./build/release/test/run "[sqlitelogic]"

sqlsmith: debug
	./build/debug/third_party/sqlsmith/sqlsmith --duckdb=:memory:

# Bloaty: a size profiler for binaries, is a project backed by Google engineers, https://github.com/google/bloaty for more info
# works both on executable, libraries (-> .duckdb_extension) and on WebAssembly
bloaty/bloaty:
	git clone https://github.com/google/bloaty.git
	cd bloaty && git submodule update --init --recursive && cmake -Bm build -G Ninja -S . && cmake --build build
	mv bloaty/build/bloaty bloaty/bloaty

bloaty: reldebug bloaty/bloaty
	cd build/reldebug && dsymutil duckdb
	./bloaty/bloaty  build/reldebug/duckdb -d symbols -n 20 --debug-file=build/reldebug/duckdb.dSYM/Contents/Resources/DWARF/duckdb
	# ./bloaty/bloaty  build/reldebug/extension/parquet/parquet.duckdb_extension -d symbols -n 20 # to execute on extension

# Generate compile commands without actually building
clangd:
	cmake -DCMAKE_BUILD_TYPE=Debug ${CMAKE_VARS} -B .cache/clangd/debug .
	cp .cache/clangd/debug/compile_commands.json .cache/clangd/compile_commands.json

coverage-check:
	./scripts/coverage_check.sh

generate-files-deps:
	$(PYTHON) -m pip install -U pip
	$(PYTHON) -m pip install --group api_spec/pyproject.toml:generate

.PHONY: generate-capi-v1 generate-functions generate-metrics generate-settings generate-serialization
.PHONY: generate-util generate-storage-info generate-enum-util generate-html-template generate-parser-grammar

generate-capi-v1:
	CAPIGEN_PYTHON="$(abspath $(CAPIGEN_PYTHON))" ./scripts/capi_v1_regen.sh

generate-functions:
	$(PYTHON) scripts/generate_functions.py

generate-metrics:
	$(PYTHON) scripts/generate_metrics.py

generate-settings:
	$(PYTHON) scripts/generate_settings.py

generate-serialization:
	$(PYTHON) scripts/generate_serialization.py

generate-util:
	$(PYTHON) scripts/generate_util.py

generate-storage-info:
	$(PYTHON) scripts/generate_storage_info.py

generate-enum-util:
	$(PYTHON) scripts/generate_enum_util.py

generate-html-template:
	$(PYTHON) scripts/generate_html_template.py

generate-parser-grammar:
	$(MAKE) parser-grammar

generate-files: $(CAPIGEN_SETUP_DEPS)
	DUCKDB_FORMAT_SKIP_TYPOS=1 FORMAT_VENV="$(abspath $(CAPIGEN_VENV))" \
		FORMAT_PYTHON="$(abspath $(CAPIGEN_PYTHON))" \
		$(MAKE) $(if $(filter -j%,$(MAKEFLAGS)),,-j $(CI_CPU_COUNT)) \
			generate-capi-v1 generate-functions generate-metrics generate-settings \
			generate-serialization generate-util generate-storage-info generate-html-template generate-parser-grammar
	$(MAKE) generate-enum-util
# Format files modified by the generators without walking every branch change.
	mkdir -p .cache
	git diff --name-only --diff-filter=ACMR > .cache/generated-format-files.txt
	DUCKDB_FORMAT_SKIP_TYPOS=1 FORMAT_FILES_FROM=.cache/generated-format-files.txt $(MAKE) format-fix-files

#### Setup VCPKG to correct version 2026.06.24 tag is cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3
vcpkg/scripts/buildsystems/vcpkg.cmake:
	if [ -d vcpkg/.git ]; then \
		git -C vcpkg fetch --tags && \
		git -C vcpkg checkout --detach 2026.06.24; \
	else \
		git clone --branch 2026.06.24 https://github.com/microsoft/vcpkg; \
	fi
	cd vcpkg && ./bootstrap-vcpkg.sh

setup-vcpkg: vcpkg/scripts/buildsystems/vcpkg.cmake
	@echo 'Consider exporting VCPKG_TOOLCHAIN_PATH=$(PWD)/vcpkg/scripts/buildsystems/vcpkg.cmake'

cleanup-vcpkg:
	rm -rf vcpkg

test-utils:
	make release EXTENSION_CONFIGS='.github/config/extensions/httpfs.cmake;.github/config/extensions/test-utils.cmake;.github/config/extensions/inet.cmake' DUCKDB_USE='core_functions;parquet;json;icu;tpcds;tpch;autocomplete;httpfs;inet;test_utils;httplib;loadable_extensions;local_extension_repository'
