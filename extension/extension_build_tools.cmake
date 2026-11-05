# Adds extensions to what the DuckDB targets link by default, for extension configs; STATICALLY_LINK_EXTENSIONS
# replaces the whole default. An extension that is only loaded with duckdb_extension_load is built, not linked, and can be
# installed from the build's extension repository.
set(DUCKDB_NEW_EXTENSION_BUILD_ENABLED FALSE)
if(DEFINED ENV{DUCKDB_NEW_EXTENSION_BUILD})
    if(NOT "$ENV{DUCKDB_NEW_EXTENSION_BUILD}" STREQUAL "" AND NOT "$ENV{DUCKDB_NEW_EXTENSION_BUILD}" STREQUAL "0")
        set(DUCKDB_NEW_EXTENSION_BUILD_ENABLED TRUE)
    endif()
endif()

function(duckdb_extension_statically_link)
    set_property(GLOBAL APPEND PROPERTY DUCKDB_EXTENSIONS_STATICALLY_LINKED_BY_CONFIG ${ARGN})
endfunction()

# Deprecated no-op, kept so extensions that still call it configure.
function(add_extension_definitions)
    message(DEPRECATION "add_extension_definitions() no longer does anything and can be removed")
endfunction()

# Writes OUT_FILE from extension/loader/static_extension_loader.c.in: a C source defining
# duckdb_register_static_extensions, which registers each named extension through its describe function.
# scripts/generate_static_extension_loader.py renders the same template outside CMake.
function(duckdb_write_static_extension_loader OUT_FILE)
    set(LINK_EXTENSION_LIST "")
    set(DESCRIBE_DECLARATIONS "")
    set(DESCRIBE_REGISTRATIONS "")
    foreach(EXT_NAME IN LISTS ARGN)
        string(APPEND LINK_EXTENSION_LIST " ${EXT_NAME}")
        string(APPEND DESCRIBE_DECLARATIONS "int32_t duckdb_extension_${EXT_NAME}_describe(duckdb_extension_descriptor *descriptor);\n")
        string(APPEND DESCRIBE_REGISTRATIONS "\tif (duckdb_register_static_extension(duckdb_extension_${EXT_NAME}_describe) != 0) {\n\t\tresult = 1;\n\t}\n")
    endforeach()
    string(STRIP "${LINK_EXTENSION_LIST}" LINK_EXTENSION_LIST)
    foreach(PART DESCRIBE_DECLARATIONS DESCRIBE_REGISTRATIONS)
        string(REGEX REPLACE "\n$" "" ${PART} "${${PART}}")
    endforeach()
    configure_file(${DUCKDB_MODULE_BASE_DIR}/extension/loader/static_extension_loader.c.in ${OUT_FILE} @ONLY)
endfunction()

# Links the named extensions into TARGET, in the given order, which is also their load order: their archives, plus a
# generated <TARGET>_static_extension_loader.c defining duckdb_register_static_extensions and, unless EXPLICIT is
# given, extension/loader/static_extension_autoregister.cpp calling it before main. Extensions this build does not build
# are skipped and reported. Each target picks its own set, so a shell and a test binary in the same build can link
# different extensions.
function(duckdb_link_extensions TARGET)
    set(LINKAGE "")
    set(AUTOLOAD TRUE)
    set(EXTENSIONS ${ARGN})
    list(GET EXTENSIONS 0 FIRST_ARG)
    if(EXTENSIONS AND ("${FIRST_ARG}" STREQUAL "PRIVATE" OR "${FIRST_ARG}" STREQUAL "PUBLIC"))
        set(LINKAGE ${FIRST_ARG})
        list(REMOVE_AT EXTENSIONS 0)
    endif()
    list(FIND EXTENSIONS "EXPLICIT" EXPLICIT_INDEX)
    if(NOT EXPLICIT_INDEX EQUAL -1)
        set(AUTOLOAD FALSE)
        list(REMOVE_AT EXTENSIONS ${EXPLICIT_INDEX})
    endif()
    set(LINKED "")
    set(MISSING "")
    foreach(EXT_NAME IN LISTS EXTENSIONS)
        if(TARGET ${EXT_NAME}_extension)
            target_link_libraries(${TARGET} ${LINKAGE} ${EXT_NAME}_extension)
            list(APPEND LINKED ${EXT_NAME})
        elseif(TARGET duckdb_${EXT_NAME})
            # linked and registered like an extension without being one, e.g. httplib
            target_link_libraries(${TARGET} ${LINKAGE} duckdb_${EXT_NAME})
            list(APPEND LINKED ${EXT_NAME})
        else()
            list(APPEND MISSING ${EXT_NAME})
        endif()
    endforeach()
    if(NOT "${MISSING}" STREQUAL "")
        string(REPLACE ";" ", " MISSING_TEXT "${MISSING}")
        message(STATUS "Extensions requested by ${TARGET} but not built: ${MISSING_TEXT}")
    endif()
    if("${LINKED}" STREQUAL "")
        return()
    endif()
    set(HELPER "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_static_extension_loader.c")
    duckdb_write_static_extension_loader(${HELPER} ${LINKED})
    target_sources(${TARGET} PRIVATE ${HELPER})
    if(AUTOLOAD)
        target_sources(${TARGET} PRIVATE ${DUCKDB_MODULE_BASE_DIR}/extension/loader/static_extension_autoregister.cpp)
    endif()
endfunction()

# Resolves what the DuckDB targets link statically. DUCKDB_CAPABILITIES picks among the capabilities this
# build makes: httplib, loadable_extensions and local_extension_repository (automatic installs come from this build's
# repository instead of the core one), all of them by default. STATICALLY_LINK_EXTENSIONS picks the extensions, by
# default the built ones the extension configs list with duckdb_extension_statically_link(), plus the prebuilt ones.
# Either can be 'none', and takes names separated by spaces or ';'.
function(duckdb_resolve_static_link OUT_CAPABILITIES OUT_EXTENSIONS)
    set(KNOWN_CAPABILITIES httplib loadable_extensions local_extension_repository)
    set(BUILT_CAPABILITIES "")
    if(ENABLE_BUILTIN_HTTPLIB)
        list(APPEND BUILT_CAPABILITIES httplib)
    endif()
    if(ENABLE_EXTENSION_LOAD)
        list(APPEND BUILT_CAPABILITIES loadable_extensions)
    endif()
    list(APPEND BUILT_CAPABILITIES local_extension_repository)
    set(DEFAULT_CAPABILITIES ${BUILT_CAPABILITIES})

    string(REPLACE " " ";" CAPABILITIES "${DUCKDB_CAPABILITIES}")
    list(REMOVE_ITEM CAPABILITIES "")
    if("${CAPABILITIES}" STREQUAL "")
        set(CAPABILITIES ${DEFAULT_CAPABILITIES})
    elseif("${CAPABILITIES}" STREQUAL "none")
        set(CAPABILITIES "")
    endif()
    foreach(CAPABILITY IN LISTS CAPABILITIES)
        if(NOT CAPABILITY IN_LIST KNOWN_CAPABILITIES)
            message(FATAL_ERROR "Unknown capability '${CAPABILITY}' in DUCKDB_CAPABILITIES, known are: ${KNOWN_CAPABILITIES}")
        elseif(NOT CAPABILITY IN_LIST BUILT_CAPABILITIES)
            message(FATAL_ERROR "Capability '${CAPABILITY}' is not built (ENABLE_BUILTIN_HTTPLIB / ENABLE_EXTENSION_LOAD)")
        endif()
    endforeach()

    string(REPLACE " " ";" EXTENSIONS "${STATICALLY_LINK_EXTENSIONS}")
    list(REMOVE_ITEM EXTENSIONS "")
    if("${EXTENSIONS}" STREQUAL "")
        get_property(REQUESTED GLOBAL PROPERTY DUCKDB_EXTENSIONS_STATICALLY_LINKED_BY_CONFIG)
        foreach(EXT_NAME IN LISTS REQUESTED)
            string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
            if(EXT_NAME IN_LIST KNOWN_CAPABILITIES OR DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_BUILD)
                list(APPEND EXTENSIONS ${EXT_NAME})
            endif()
        endforeach()
        # a prebuilt extension only exists as an archive, so linking is the only way to make it available
        foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
            string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
            if(DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_BUILD AND NOT "${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PREBUILT_PATH}" STREQUAL "")
                list(APPEND EXTENSIONS ${EXT_NAME})
            endif()
        endforeach()
        if(EXTENSIONS)
            list(REMOVE_DUPLICATES EXTENSIONS)
        endif()
    elseif("${EXTENSIONS}" STREQUAL "none")
        set(EXTENSIONS "")
    endif()
    # a capability listed with the extensions is linked like the others
    foreach(EXT_NAME IN LISTS EXTENSIONS)
        if(EXT_NAME IN_LIST KNOWN_CAPABILITIES)
            list(APPEND CAPABILITIES ${EXT_NAME})
        endif()
    endforeach()
    list(REMOVE_ITEM EXTENSIONS ${KNOWN_CAPABILITIES})
    if(CAPABILITIES)
        list(REMOVE_DUPLICATES CAPABILITIES)
    endif()

    set(${OUT_CAPABILITIES} ${CAPABILITIES} PARENT_SCOPE)
    set(${OUT_EXTENSIONS} ${EXTENSIONS} PARENT_SCOPE)
endfunction()

# Links what duckdb_resolve_static_link picked at configure time, capabilities first, into a DuckDB target.
function(link_extension_libraries LIBRARY LINKAGE)
    # From an extension's own CMakeLists, while the extensions are added: what to link isn't resolved yet, and neither
    # the DuckDB targets nor the extensions after this one exist. Done by duckdb_link_deferred_extension_libraries.
    if(DUCKDB_ADDING_EXTENSIONS)
        set_property(GLOBAL APPEND PROPERTY DUCKDB_DEFERRED_EXTENSION_LINKS "${LIBRARY}" "${LINKAGE}")
        return()
    endif()
    duckdb_link_extensions(${LIBRARY} ${LINKAGE} ${DUCKDB_STATICALLY_LINKED_CAPABILITIES} ${DUCKDB_STATICALLY_LINKED_EXTENSIONS})
endfunction()

# Called by the top-level CMakeLists once every target exists
function(duckdb_link_deferred_extension_libraries)
    get_property(DEFERRED GLOBAL PROPERTY DUCKDB_DEFERRED_EXTENSION_LINKS)
    list(LENGTH DEFERRED COUNT)
    if(COUNT EQUAL 0)
        return()
    endif()
    math(EXPR LAST "${COUNT} - 1")
    foreach(I RANGE 0 ${LAST} 2)
        math(EXPR J "${I} + 1")
        list(GET DEFERRED ${I} LIBRARY)
        list(GET DEFERRED ${J} LINKAGE)
        link_extension_libraries(${LIBRARY} "${LINKAGE}")
    endforeach()
endfunction()

function(link_threads LIBRARY LINKAGE)
    target_link_libraries(${LIBRARY} ${LINKAGE} Threads::Threads)
endfunction()

# Resolve archive LLVM IR into native code for non-LTO linkers.
function(duckdb_make_native_lto_archive TARGET)
    if(NOT NATIVE_LTO_STATIC_LIBRARIES)
        return()
    endif()
    set(LTO_ARCHIVE "$<TARGET_FILE:${TARGET}>.lto.a")
    set(LTO_OBJECT "$<TARGET_FILE:${TARGET}>.lto.o")
    set(LTO_JOB_FLAG "")
    if(CMAKE_LTO STREQUAL "thin" AND CMAKE_LTO_JOBS)
        set(LTO_JOB_FLAG "-flto-jobs=${CMAKE_LTO_JOBS}")
    endif()
    add_custom_command(
        TARGET ${TARGET}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rename "$<TARGET_FILE:${TARGET}>" "${LTO_ARCHIVE}"
        COMMAND ${CMAKE_CXX_COMPILER} -O3 -flto=${CMAKE_LTO} ${LTO_JOB_FLAG}
                -fuse-ld=lld -nostdlib -r -Wl,--whole-archive "${LTO_ARCHIVE}"
                -Wl,--no-whole-archive -o "${LTO_OBJECT}"
        COMMAND ${CMAKE_AR} qc "$<TARGET_FILE:${TARGET}>" "${LTO_OBJECT}"
        COMMAND ${CMAKE_RANLIB} "$<TARGET_FILE:${TARGET}>"
        COMMAND ${CMAKE_COMMAND} -E remove "${LTO_ARCHIVE}" "${LTO_OBJECT}"
        COMMENT "Resolving LTO in $<TARGET_FILE_NAME:${TARGET}>"
        VERBATIM)
endfunction()

# Deploys extensions to a local repository (a folder structure that contains the duckdb version + binary arch)
if ("${LOCAL_EXTENSION_REPO}" STREQUAL "")
    set(LOCAL_EXTENSION_REPO_DIR ${CMAKE_BINARY_DIR}/repository)
else()
    if (NOT Python3_FOUND)
        MESSAGE(FATAL_ERROR "Could not find python3 executable, when providing LOCAL_EXTENSION_REPO this is compulsory")
    endif()
    set(LOCAL_EXTENSION_REPO_DIR ${LOCAL_EXTENSION_REPO})
endif()

set(LOCAL_EXTENSION_REPO FALSE)
if (NOT ${EXTENSION_CONFIG_BUILD} AND NOT ${EXTENSION_TESTS_ONLY} AND NOT CLANG_TIDY)
    if (NOT Python3_FOUND)
        add_custom_target(
                duckdb_local_extension_repo ALL)
        MESSAGE(STATUS "Could not find python3, create extension directory step will be skipped")
    else()
        add_custom_target(
                duckdb_local_extension_repo ALL
                COMMAND
                ${Python3_EXECUTABLE} scripts/create_local_extension_repo.py "${DUCKDB_NORMALIZED_VERSION}" "${CMAKE_CURRENT_BINARY_DIR}/duckdb_platform_out" "${CMAKE_CURRENT_BINARY_DIR}" "${LOCAL_EXTENSION_REPO_DIR}" "duckdb_extension${EXTENSION_POSTFIX}"
                WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                COMMENT Create local extension repository)
        add_dependencies(duckdb_local_extension_repo duckdb_platform)
        set(LOCAL_EXTENSION_REPO TRUE)
        message(STATUS "Extensions will be deployed to: ${LOCAL_EXTENSION_REPO_DIR}")
    endif()
endif()

# The entrypoint a C ABI extension is expected to export. The CAPI version major selects the API family, and
# C_STRUCT_UNSTABLE always means V2 (nothing is unstable in the V1 API anymore).
function(capi_entrypoint_symbol NAME ABI_TYPE CAPI_VERSION OUT_VAR)
    if (${ABI_TYPE} STREQUAL "C_STRUCT_UNSTABLE")
        set(${OUT_VAR} "${NAME}_init_c_api_v2" PARENT_SCOPE)
    elseif (${ABI_TYPE} STREQUAL "C_STRUCT")
        string(REGEX MATCH "^v([0-9]+)\\." _unused "${CAPI_VERSION}")
        if ("${CMAKE_MATCH_1}" STREQUAL "2")
            set(${OUT_VAR} "${NAME}_init_c_api_v2" PARENT_SCOPE)
        else()
            set(${OUT_VAR} "${NAME}_init_c_api" PARENT_SCOPE)
        endif()
    else()
        message(FATAL_ERROR "capi_entrypoint_symbol called with non-C ABI type '${ABI_TYPE}'")
    endif()
endfunction()

function(build_loadable_extension_directory NAME ABI_TYPE OUTPUT_DIRECTORY EXTENSION_VERSION CAPI_VERSION PARAMETERS)
    set(TARGET_NAME ${NAME}_loadable_extension)
    set(PREBUILT_EXTENSION FALSE)
    if(TARGET ${NAME}_extension)
        get_property(PREBUILT_EXTENSION TARGET ${NAME}_extension PROPERTY DUCKDB_PREBUILT_EXTENSION)
    endif()
    if (LOCAL_EXTENSION_REPO AND NOT PREBUILT_EXTENSION)
        add_dependencies(duckdb_local_extension_repo ${NAME}_loadable_extension)
    endif()
    # all parameters after output_directory
    set(FILES "${ARGV}")
    # remove name, abi_type, output_directory, extension_version, capi_version, parameters
    list(REMOVE_AT FILES 0 1 2 3 4 5)

    # parse parameters
    string(FIND "${PARAMETERS}" "-no-warnings" IGNORE_WARNINGS)

    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)

    if(EMSCRIPTEN)
        add_library(${TARGET_NAME} STATIC ${FILES})
    else()
        add_library(${TARGET_NAME} SHARED ${FILES})
    endif()
    # this disables the -Dsome_target_EXPORTS define being added by cmake which otherwise trips clang-tidy (yay)
    set_target_properties(${TARGET_NAME} PROPERTIES DEFINE_SYMBOL "")
    set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME ${NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")
    if(NOT EXTENSION_STATIC_BUILD)
        set_property(TARGET ${TARGET_NAME} PROPERTY DUCKDB_DYNAMIC_LOADABLE_EXTENSION TRUE)
    endif()
    if(${IGNORE_WARNINGS} GREATER -1)
        disable_target_warnings(${TARGET_NAME})
    endif()
    # loadable extension binaries can be built two ways:
    # 1. EXTENSION_STATIC_BUILD=1
    #    DuckDB is statically linked into each extension binary. This increases portability because in several situations
    #    DuckDB itself may have been loaded with RTLD_LOCAL. This is currently the main way we distribute the loadable
    #    extension binaries
    # 2. EXTENSION_STATIC_BUILD=0
    #    The DuckDB symbols required by the loadable extensions are left unresolved. This will reduce the size of the binaries
    #    and works well when running the DuckDB cli directly. For windows this uses delay loading. For MacOS and linux the
    #    dynamic loader will look up the missing symbols when the extension is dlopen-ed.
    if(WASM_LOADABLE_EXTENSIONS)
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -sSIDE_MODULE=1 -DWASM_LOADABLE_EXTENSIONS")
    elseif(${ABI_TYPE} STREQUAL "C_STRUCT" OR ${ABI_TYPE} STREQUAL "C_STRUCT_UNSTABLE")
        # TODO strip all symbols except the capi init
    elseif (EXTENSION_STATIC_BUILD)
        if (WIN32)
            target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
        elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
            if (APPLE)
                set_target_properties(${TARGET_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
                # Note that on MacOS we need to use the -exported_symbol whitelist feature due to a lack of -exclude-libs flag in mac's ld variant
                set(WHITELIST "-Wl,-exported_symbol,_${NAME}_duckdb_cpp_init")
                target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS} -Wl,-dead_strip ${WHITELIST})
            elseif (ZOS)
                target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
            else()
                # For GNU we rely on fvisibility=hidden to hide the extension symbols and use -exclude-libs to hide the duckdb symbols
                set_target_properties(${TARGET_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
                if(DUCKDB_LOADABLE_FROM_ARCHIVE)
                    # The entry point comes from an archive too, so --exclude-libs,ALL would hide it; export it alone
                    # through a version script instead.
                    set(EXPORTS_MAP "${CMAKE_CURRENT_BINARY_DIR}/${NAME}_loadable_exports.map")
                    file(WRITE "${EXPORTS_MAP}" "{ global: ${NAME}_duckdb_cpp_init; local: *; };\n")
                    target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS} -Wl,--gc-sections -Wl,--version-script=${EXPORTS_MAP})
                    set_property(TARGET ${TARGET_NAME} APPEND PROPERTY LINK_DEPENDS "${EXPORTS_MAP}")
                else()
                    target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS} -Wl,--gc-sections -Wl,--exclude-libs,ALL)
                endif()
            endif()
        else()
            message(FATAL_ERROR "EXTENSION static build is only intended for Linux and Windows on MVSC")
        endif()
    else()
        if (WIN32)
            target_link_libraries(${TARGET_NAME} duckdb ${DUCKDB_EXTRA_LINK_FLAGS})
        elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$")
            if (APPLE)
                set_target_properties(${TARGET_NAME} PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
            endif()
        endif()
    endif()
    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /UDUCKDB_BUILD_LIBRARY)
    else()
        target_compile_options(${TARGET_NAME} PRIVATE -UDUCKDB_BUILD_LIBRARY)
    endif()
    set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX
            ".duckdb_extension")

    if(MSVC)
        set_target_properties(
                ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG
                "${CMAKE_BINARY_DIR}/${OUTPUT_DIRECTORY}")
        set_target_properties(
                ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE
                "${CMAKE_BINARY_DIR}/${OUTPUT_DIRECTORY}")
    endif()

    if(EMSCRIPTEN)
        # Compile the library into the actual wasm file
        string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)
        set(TO_BE_LINKED ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LINKED_LIBS} )
        separate_arguments(TO_BE_LINKED)
        if (${ABI_TYPE} STREQUAL "CPP")
            set(EXPORTED_FUNCTIONS "_${NAME}_duckdb_cpp_init")
        else()
            capi_entrypoint_symbol(${NAME} ${ABI_TYPE} "${CAPI_VERSION}" CAPI_ENTRYPOINT)
            set(EXPORTED_FUNCTIONS "_${CAPI_ENTRYPOINT}")
        endif()
        add_custom_command(
                TARGET ${TARGET_NAME}
                POST_BUILD
                COMMAND emcc $<TARGET_FILE:${TARGET_NAME}> -o $<TARGET_FILE:${TARGET_NAME}>.wasm -O3 -sSIDE_MODULE=2 -sEXPORTED_FUNCTIONS="${EXPORTED_FUNCTIONS}" ${WASM_THREAD_FLAGS} ${TO_BE_LINKED}
        )
    endif()

    if (${ABI_TYPE} STREQUAL "CPP")
        set(FOOTER_VERSION_VALUE ${DUCKDB_NORMALIZED_VERSION})
    elseif (${ABI_TYPE} STREQUAL "C_STRUCT_UNSTABLE")
        set(FOOTER_VERSION_VALUE ${DUCKDB_NORMALIZED_VERSION})
    elseif (${ABI_TYPE} STREQUAL "C_STRUCT")
        set(FOOTER_VERSION_VALUE ${CAPI_VERSION})
    endif()

    add_custom_command(
            TARGET ${TARGET_NAME}
            POST_BUILD
            COMMAND
            ${CMAKE_COMMAND} -DABI_TYPE=${ABI_TYPE} -DEXTENSION=$<TARGET_FILE:${TARGET_NAME}>${EXTENSION_POSTFIX} -DPLATFORM_FILE=${DuckDB_BINARY_DIR}/duckdb_platform_out -DVERSION_FIELD="${FOOTER_VERSION_VALUE}" -DEXTENSION_VERSION="${EXTENSION_VERSION}" -DNULL_FILE=${DUCKDB_MODULE_BASE_DIR}/scripts/null.txt -P ${DUCKDB_MODULE_BASE_DIR}/scripts/append_metadata.cmake
    )
    add_dependencies(${TARGET_NAME} duckdb_platform)
    if (NOT ${EXTENSION_CONFIG_BUILD} AND NOT ${EXTENSION_TESTS_ONLY} AND NOT CLANG_TIDY AND NOT PREBUILT_EXTENSION)
        add_dependencies(duckdb_local_extension_repo ${TARGET_NAME})
    endif()
endfunction()

function(build_loadable_extension NAME PARAMETERS)
    # all parameters after name
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0 1)
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)

    build_loadable_extension_directory(${NAME} "CPP" "extension/${NAME}" "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION}" "" "${PARAMETERS}" ${FILES})
endfunction()

function(build_loadable_extension_capi NAME CAPI_VERSION_MAJOR CAPI_VERSION_MINOR CAPI_VERSION_PATCH PARAMETERS)
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0 1 2 3)
    set(CAPI_VERSION v${CAPI_VERSION_MAJOR}.${CAPI_VERSION_MINOR}.${CAPI_VERSION_PATCH})
    build_loadable_extension_capi_internal(${NAME} ${CAPI_VERSION} "C_STRUCT" ${FILES})
    target_compile_definitions(${NAME}_loadable_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_MAJOR=${CAPI_VERSION_MAJOR})
    target_compile_definitions(${NAME}_loadable_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_MINOR=${CAPI_VERSION_MINOR})
    target_compile_definitions(${NAME}_loadable_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_PATCH=${CAPI_VERSION_PATCH})
    target_compile_definitions(${NAME}_loadable_extension PRIVATE DUCKDB_EXTENSION_NAME=${NAME})
endfunction()

# Versioned build against the V2 C API. Produces a C_STRUCT footer carrying a v2.x.y CAPI version, which is what
# selects the <name>_init_c_api_v2 entrypoint at load time.
function(build_loadable_extension_capi_v2 NAME CAPI_VERSION_MAJOR CAPI_VERSION_MINOR CAPI_VERSION_PATCH PARAMETERS)
    if (NOT ${CAPI_VERSION_MAJOR} EQUAL 2)
        message(FATAL_ERROR "build_loadable_extension_capi_v2 requires a v2.x.y API version, got v${CAPI_VERSION_MAJOR}.${CAPI_VERSION_MINOR}.${CAPI_VERSION_PATCH}")
    endif()
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0 1 2 3)
    build_loadable_extension_capi(${NAME} ${CAPI_VERSION_MAJOR} ${CAPI_VERSION_MINOR} ${CAPI_VERSION_PATCH} ${FILES})
endfunction()

function(build_loadable_extension_capi_unstable NAME PARAMETERS)
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0)
    build_loadable_extension_capi_internal(${NAME} "" "C_STRUCT_UNSTABLE" ${FILES})
    target_compile_definitions(${NAME}_loadable_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_NORMALIZED_VERSION})
    target_compile_definitions(${NAME}_loadable_extension PRIVATE DUCKDB_EXTENSION_NAME=${NAME})
endfunction()

function(build_loadable_extension_capi_internal NAME VERSION ABI_TYPE PARAMETERS)
    # all parameters after name
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0 1 2)
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)

    build_loadable_extension_directory(${NAME} ${ABI_TYPE} "extension/${NAME}" "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION}" "${VERSION}" "${PARAMETERS}" ${FILES})
endfunction()

# Adds the describe function of a static extension archive, duckdb_extension_<NAME>_describe, to <NAME>_extension. It
# describes the extension's entrypoint to duckdb_register_static_extension. KIND is CPP, CAPI or CAPI_V2. The optional
# third argument is what the entrypoint was built against, the DuckDB version by default, as the metadata of a loadable
# extension carries it.
function(duckdb_add_extension_describe NAME KIND)
    set(API_VERSION "${DUCKDB_NORMALIZED_VERSION}")
    if(ARGC GREATER 2)
        set(API_VERSION "${ARGV2}")
    endif()
    if("${KIND}" STREQUAL "CAPI")
        set(ENTRY_NAME "${NAME}_init_c_api")
        set(ENTRY_FIELD "entry_capi_v1")
        # takes a duckdb_extension_info and a duckdb_extension_access pointer, returns bool
        set(ENTRY_DECLARATION "int ${ENTRY_NAME}(void *info, void *access);")
    elseif("${KIND}" STREQUAL "CAPI_V2")
        set(ENTRY_NAME "${NAME}_init_c_api_v2")
        set(ENTRY_FIELD "entry_capi_v2")
        # takes a duckdb_v2_extension_input pointer
        set(ENTRY_DECLARATION "void ${ENTRY_NAME}(void *input);")
    else()
        set(ENTRY_NAME "${NAME}_duckdb_cpp_init")
        set(ENTRY_FIELD "entry_cpp")
        # takes a duckdb::ExtensionLoader reference, passed as a pointer
        set(ENTRY_DECLARATION "void ${ENTRY_NAME}(void *loader);")
    endif()
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)
    string(REGEX REPLACE "[\"\\\\]" "" EXTENSION_VERSION "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION}")
    set(EXTENSION_VERSION "\"${EXTENSION_VERSION}\"")
    string(REGEX REPLACE "[\"\\\\]" "" API_VERSION "${API_VERSION}")
    set(API_VERSION "\"${API_VERSION}\"")
    set(DESCRIBE_FILE "${DuckDB_BINARY_DIR}/codegen/link/${NAME}_describe.c")
    configure_file(${DUCKDB_MODULE_BASE_DIR}/extension/loader/extension_describe.c.in ${DESCRIBE_FILE} @ONLY)
    target_sources(${NAME}_extension PRIVATE ${DESCRIBE_FILE})
    set_property(TARGET ${NAME}_extension PROPERTY DUCKDB_EXTENSION_KIND "${KIND}")
endfunction()

function(build_static_extension NAME PARAMETERS)
    # all parameters after name
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0)
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)
    set(PREBUILT_PATH "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PREBUILT_PATH}")
    if(NOT "${PREBUILT_PATH}" STREQUAL "")
        set(DUMMY_SOURCE "${DuckDB_BINARY_DIR}/codegen/prebuilt_extension.cpp")
        if(NOT EXISTS "${DUMMY_SOURCE}")
            file(WRITE "${DUMMY_SOURCE}" "// Wrapper for prebuilt extension archives.\n")
        endif()
        # Keep the DuckDB archive after the extension archive for one-pass linkers.
        set(PREBUILT_TARGET ${NAME}_prebuilt_extension)
        add_library(${PREBUILT_TARGET} STATIC IMPORTED GLOBAL)
        set_property(TARGET ${PREBUILT_TARGET} PROPERTY IMPORTED_LOCATION "${PREBUILT_PATH}")
        target_link_libraries(${PREBUILT_TARGET} INTERFACE
            "$<$<NOT:$<BOOL:$<TARGET_PROPERTY:DUCKDB_DYNAMIC_LOADABLE_EXTENSION>>>:duckdb_static>")
        add_library(${NAME}_extension STATIC "${DUMMY_SOURCE}")
        target_link_libraries(${NAME}_extension "$<BUILD_INTERFACE:${PREBUILT_TARGET}>")
        set_property(TARGET ${NAME}_extension PROPERTY DUCKDB_EXTENSION_KIND CPP)
        set_property(TARGET ${NAME}_extension PROPERTY DUCKDB_PREBUILT_EXTENSION TRUE)
        return()
    endif()
    add_library(${NAME}_extension STATIC ${FILES})
    # Dynamic loadables resolve DuckDB symbols from the host, including when linked through this archive.
    target_link_libraries(${NAME}_extension
        "$<$<NOT:$<BOOL:$<TARGET_PROPERTY:DUCKDB_DYNAMIC_LOADABLE_EXTENSION>>>:duckdb_static>")
    duckdb_add_extension_describe(${NAME} CPP)
    duckdb_make_native_lto_archive(${NAME}_extension)
endfunction()

# Adds the describe function to an extension archive created without build_static_extension.
function(duckdb_add_missing_extension_describe NAME)
    if(NOT TARGET ${NAME}_extension)
        return()
    endif()
    get_target_property(TARGET_TYPE ${NAME}_extension TYPE)
    get_property(KIND TARGET ${NAME}_extension PROPERTY DUCKDB_EXTENSION_KIND)
    if(NOT "${TARGET_TYPE}" STREQUAL "STATIC_LIBRARY" OR NOT "${KIND}" STREQUAL "")
        return()
    endif()
    duckdb_add_extension_describe(${NAME} CPP)
endfunction()

# Compiles a C++ extension once into lib<NAME>_extension.a and links its loadable from that archive.
# NO_LOADABLE builds the archive only; NO_WARNINGS silences compiler warnings.
function(build_extension_library NAME)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_LOADABLE;NO_WARNINGS;DEFAULT_VISIBILITY" "" "")
    set(FILES ${ARG_UNPARSED_ARGUMENTS})
    set(PARAMETERS "-warnings")
    if(ARG_NO_WARNINGS)
        set(PARAMETERS "-no-warnings")
    endif()

    build_static_extension(${NAME} ${FILES})
    if(ARG_NO_WARNINGS)
        disable_target_warnings(${NAME}_extension)
    endif()
    if(ARG_NO_LOADABLE)
        return()
    endif()
    if(EMSCRIPTEN OR WASM_LOADABLE_EXTENSIONS)
        # the wasm side module is produced from the loadable target's own objects
        build_loadable_extension(${NAME} "${PARAMETERS}" ${FILES})
        return()
    endif()

    # the loadable exports only its entry point either way; DEFAULT_VISIBILITY keeps internals visible through libduckdb
    if(NOT ARG_DEFAULT_VISIBILITY)
        set_target_properties(${NAME}_extension PROPERTIES CXX_VISIBILITY_PRESET hidden)
    endif()
    set(LOADABLE_SOURCE "${DuckDB_BINARY_DIR}/codegen/loadable_from_archive.cpp")
    if(NOT EXISTS "${LOADABLE_SOURCE}")
        file(WRITE "${LOADABLE_SOURCE}" "// A loadable extension built by build_extension_library takes its code from its static archive.\n")
    endif()
    set(DUCKDB_LOADABLE_FROM_ARCHIVE TRUE)
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)
    build_loadable_extension_directory(${NAME} "CPP" "extension/${NAME}" "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION}" "" "${PARAMETERS}" ${LOADABLE_SOURCE})
    target_link_libraries(${NAME}_loadable_extension ${NAME}_extension)
    # the entrypoint lives in the archive, so the link has to ask for it
    set(ENTRY ${NAME}_duckdb_cpp_init)
    if(MSVC)
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(ENTRY "_${ENTRY}")
        endif()
        target_link_options(${NAME}_loadable_extension PRIVATE "/INCLUDE:${ENTRY}")
    elseif(APPLE)
        target_link_options(${NAME}_loadable_extension PRIVATE "-Wl,-u,_${ENTRY}")
    else()
        if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(ENTRY "_${ENTRY}")
        endif()
        target_link_options(${NAME}_loadable_extension PRIVATE "-Wl,-u,${ENTRY}")
    endif()
endfunction()

function(build_static_extension_capi_internal NAME KIND API_VERSION FILES)
    add_library(${NAME}_extension STATIC ${FILES})
    target_link_libraries(${NAME}_extension
        "$<$<NOT:$<BOOL:$<TARGET_PROPERTY:DUCKDB_DYNAMIC_LOADABLE_EXTENSION>>>:duckdb_static>")
    target_compile_definitions(${NAME}_extension PRIVATE DUCKDB_BUILD_STATIC_EXTENSION)
    duckdb_add_extension_describe(${NAME} ${KIND} "${API_VERSION}")
    duckdb_make_native_lto_archive(${NAME}_extension)
endfunction()

function(build_static_extension_capi NAME CAPI_VERSION_MAJOR CAPI_VERSION_MINOR CAPI_VERSION_PATCH PARAMETERS)
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0 1 2 3)
    set(CAPI_VERSION "v${CAPI_VERSION_MAJOR}.${CAPI_VERSION_MINOR}.${CAPI_VERSION_PATCH}")
    if (${CAPI_VERSION_MAJOR} EQUAL 2)
        build_static_extension_capi_internal(${NAME} CAPI_V2 "${CAPI_VERSION}" "${FILES}")
    else()
        build_static_extension_capi_internal(${NAME} CAPI "${CAPI_VERSION}" "${FILES}")
    endif()
    target_compile_definitions(${NAME}_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_MAJOR=${CAPI_VERSION_MAJOR})
    target_compile_definitions(${NAME}_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_MINOR=${CAPI_VERSION_MINOR})
    target_compile_definitions(${NAME}_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_PATCH=${CAPI_VERSION_PATCH})
    target_compile_definitions(${NAME}_extension PRIVATE DUCKDB_EXTENSION_NAME=${NAME})
endfunction()

# Versioned build against the V2 C API, statically linked into DuckDB.
function(build_static_extension_capi_v2 NAME CAPI_VERSION_MAJOR CAPI_VERSION_MINOR CAPI_VERSION_PATCH PARAMETERS)
    if (NOT ${CAPI_VERSION_MAJOR} EQUAL 2)
        message(FATAL_ERROR "build_static_extension_capi_v2 requires a v2.x.y API version, got v${CAPI_VERSION_MAJOR}.${CAPI_VERSION_MINOR}.${CAPI_VERSION_PATCH}")
    endif()
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0 1 2 3)
    build_static_extension_capi(${NAME} ${CAPI_VERSION_MAJOR} ${CAPI_VERSION_MINOR} ${CAPI_VERSION_PATCH} ${FILES})
endfunction()

# The unstable API is the V2 API: everything that was unstable in V1 was stabilized into v1.5.6.
function(build_static_extension_capi_unstable NAME PARAMETERS)
    set(FILES "${ARGV}")
    list(REMOVE_AT FILES 0)
    build_static_extension_capi_internal(${NAME} CAPI_V2 "${DUCKDB_NORMALIZED_VERSION}" "${FILES}")
    target_compile_definitions(${NAME}_extension PRIVATE DUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_NORMALIZED_VERSION})
    target_compile_definitions(${NAME}_extension PRIVATE DUCKDB_EXTENSION_NAME=${NAME})
endfunction()

# Internal extension register function
function(register_extension NAME DONT_BUILD LOAD_TESTS PATH INCLUDE_PATH TEST_PATH LINKED_LIBS EXTENSION_VERSION)
    string(TOLOWER ${NAME} EXTENSION_NAME_LOWERCASE)
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)

    set(DUCKDB_EXTENSION_NAMES ${DUCKDB_EXTENSION_NAMES} ${EXTENSION_NAME_LOWERCASE} PARENT_SCOPE)

    if ("${LOAD_TESTS}")
        set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LOAD_TESTS TRUE PARENT_SCOPE)
    else()
        set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LOAD_TESTS FALSE PARENT_SCOPE)
    endif()
    # decided once every config is loaded, see duckdb_resolve_static_link
    set(LINK_EXTENSION ${BUILD_EXTENSIONS_ONLY})
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_LINK ${LINK_EXTENSION} PARENT_SCOPE)

    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LINKED_LIBS "${LINKED_LIBS}" PARENT_SCOPE)

    # Allows explicitly disabling extensions that may be specified in other configurations
    if (NOT ${DONT_BUILD} AND NOT ${EXTENSION_TESTS_ONLY})
        set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_BUILD TRUE PARENT_SCOPE)
    elseif(NOT ${GENERATE_EXTENSION_ENTRIES} AND ${EXTENSION_NAME_UPPERCASE} STREQUAL "CORE_FUNCTIONS")
        set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_BUILD TRUE PARENT_SCOPE)
    else()
        set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_BUILD FALSE PARENT_SCOPE)
        set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_LINK FALSE PARENT_SCOPE)
    endif()

    if ("${PATH}" STREQUAL "")
        message(FATAL_ERROR "Invalid path set for extension '${NAME}' : '${INCLUDE}'")
    endif()
    if ("${INCLUDE_PATH}" STREQUAL "")
        message(FATAL_ERROR "Invalid include path for extension '${NAME}' : '${INCLUDE_PATH}'")
    endif()
    if ("${TEST_PATH}" STREQUAL "" AND "${LOAD_TESTS}")
        message(FATAL_ERROR "Invalid include path for extension '${NAME}' : '${INCLUDE_PATH}'")
    endif()

    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PATH ${PATH} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_INCLUDE_PATH ${INCLUDE_PATH} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_TEST_PATH ${TEST_PATH} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION ${EXTENSION_VERSION} PARENT_SCOPE)

endfunction()

# Downloads the external extension repo at the specified commit and calls register_extension
macro(register_external_extension NAME URL COMMIT DONT_BUILD LOAD_TESTS PATH INCLUDE_PATH TEST_PATH APPLY_PATCHES LINKED_LIBS SUBMODULES EXTENSION_VERSION)
    include(FetchContent)

    string(TOUPPER "DUCKDB_${NAME}_DIRECTORY" DIRECTORY_OVERRIDE)
    if(DEFINED ENV{${DIRECTORY_OVERRIDE}})
        set("${NAME}_extension_fc_SOURCE_DIR" "$ENV{${DIRECTORY_OVERRIDE}}")
    elseif(DUCKDB_NEW_EXTENSION_BUILD_ENABLED)
        # Use the pre-cloned source from extension/external/<name> (populated by
        # scripts/sync_out_of_tree_extensions.py via `make sync_out_of_tree_extensions`).
        set("${NAME}_extension_fc_SOURCE_DIR" "${DUCKDB_MODULE_BASE_DIR}/extension/external/${NAME}")
        if(NOT EXISTS "${${NAME}_extension_fc_SOURCE_DIR}/.git")
            message(FATAL_ERROR
                "DUCKDB_NEW_EXTENSION_BUILD is set but extension '${NAME}' was not found at "
                "extension/external/${NAME}. Run 'make sync_out_of_tree_extensions' first.")
        endif()
    else()
        unset(PATCH_COMMAND)
        if (${APPLY_PATCHES})
            set(PATCH_COMMAND ${Python3_EXECUTABLE} ${DUCKDB_MODULE_BASE_DIR}/scripts/apply_extension_patches.py ${DUCKDB_MODULE_BASE_DIR}/.github/patches/extensions/${NAME}/)
        endif()
        FETCHCONTENT_DECLARE(
                ${NAME}_extension_fc
                GIT_REPOSITORY ${URL}
                GIT_TAG ${COMMIT}
                GIT_SUBMODULES "${SUBMODULES}"
                PATCH_COMMAND ${PATCH_COMMAND}
                SOURCE_SUBDIR __duckdb_no_add_subdirectory__
        )
        FETCHCONTENT_MAKEAVAILABLE(${NAME}_EXTENSION_FC)
    endif()

    # Autogenerate version tag if not provided
    if ("${EXTENSION_VERSION}" STREQUAL "")
        duckdb_extension_generate_version(EXTERNAL_EXTENSION_VERSION ${${NAME}_extension_fc_SOURCE_DIR})
    else()
        set(EXTERNAL_EXTENSION_VERSION "${EXTENSION_VERSION}")
    endif()

    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION "${EXTERNAL_EXTENSION_VERSION}" PARENT_SCOPE)

    if(DEFINED ENV{${DIRECTORY_OVERRIDE}})
        message(STATUS "Load extension '${NAME}' from local path \"${${NAME}_extension_fc_SOURCE_DIR}\" @ ${EXTERNAL_EXTENSION_VERSION}")
    elseif(DUCKDB_NEW_EXTENSION_BUILD_ENABLED)
        message(STATUS "Load extension '${NAME}' from extension/external/${NAME} @ ${EXTERNAL_EXTENSION_VERSION}")
    else()
        message(STATUS "Load extension '${NAME}' from ${URL} @ ${EXTERNAL_EXTENSION_VERSION}")
    endif()

    if ("${INCLUDE_PATH}" STREQUAL "")
        set(INCLUDE_FULL_PATH "${${NAME}_extension_fc_SOURCE_DIR}/src/include")
    else()
        set(INCLUDE_FULL_PATH "${${NAME}_extension_fc_SOURCE_DIR}/${INCLUDE_PATH}")
    endif()

    if ("${TEST_PATH}" STREQUAL "")
        set(TEST_FULL_PATH "${${NAME}_extension_fc_SOURCE_DIR}/test/sql")
    else()
        set(TEST_FULL_PATH "${${NAME}_extension_fc_SOURCE_DIR}/${TEST_PATH}")
    endif()

    register_extension(${NAME} ${DONT_BUILD} ${LOAD_TESTS} ${${NAME}_extension_fc_SOURCE_DIR}/${PATH} "${INCLUDE_FULL_PATH}" "${TEST_FULL_PATH}" "${LINKED_LIBS}" "${EXTERNAL_EXTENSION_VERSION}")
endmacro()

# This function sets OUTPUT_VAR to the VERSION using DuckDB's standard versioning convention (using WORKING_DIR)
# TODO: unify this with the base DuckDB logic (note that this is slightly different as it has ReleaseCandidate tag support
function(duckdb_extension_generate_version OUTPUT_VAR WORKING_DIR)
    find_package(Git)
    if(Git_FOUND)
        execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --is-inside-work-tree
                WORKING_DIRECTORY ${WORKING_DIR}
                OUTPUT_VARIABLE IS_IN_GIT_DIR
                ERROR_QUIET
        )
    endif()
    if (IS_IN_GIT_DIR)
        execute_process(
                COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
                WORKING_DIRECTORY ${WORKING_DIR}
                RESULT_VARIABLE GIT_RESULT
                OUTPUT_VARIABLE GIT_COMMIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE)
        if (GIT_RESULT)
            message(FATAL_ERROR
                    "git is available (at ${GIT_EXECUTABLE}) but has failed to execute 'log -1 --format=%H'.")
        endif()
        string(SUBSTRING "${GIT_COMMIT_HASH}" 0 10 GIT_COMMIT_HASH)
        execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --tags --always --match "${VERSIONING_TAG_MATCH}"
                WORKING_DIRECTORY ${WORKING_DIR}
                RESULT_VARIABLE GIT_RESULT
                OUTPUT_VARIABLE GIT_DESCRIBE
                OUTPUT_STRIP_TRAILING_WHITESPACE)
        if (GIT_RESULT)
            set(VERSION "${GIT_COMMIT_HASH}")
        elseif (GIT_DESCRIBE MATCHES "^v[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9\.]+)?$")
            # We are on a valid SemVer version in the format v{MAJOR}.{MINOR}.{PATH}(-{RC})
            set(VERSION "${GIT_DESCRIBE}")
        else()
            set(VERSION "${GIT_COMMIT_HASH}")
        endif()
    else()
        # No git found, we set empty string
        set(VERSION "")
    endif()

    # Propagate the version
    set(${OUTPUT_VAR} ${VERSION} PARENT_SCOPE)
endfunction()

function(duckdb_extension_load NAME)
    # Parameter parsing
    set(options DONT_BUILD LOAD_TESTS APPLY_PATCHES)
    set(oneValueArgs SOURCE_DIR INCLUDE_DIR TEST_DIR GIT_URL GIT_TAG SUBMODULES EXTENSION_VERSION LINKED_LIBS)
    cmake_parse_arguments(duckdb_extension_load "${options}" "${oneValueArgs}" "" ${ARGN})
    if("DONT_LINK" IN_LIST duckdb_extension_load_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "duckdb_extension_load(${NAME} DONT_LINK): DONT_LINK was removed, extensions are only linked when listed with duckdb_extension_statically_link() or STATICALLY_LINK_EXTENSIONS")
    endif()

    string(TOLOWER ${NAME} EXTENSION_NAME_LOWERCASE)
    string(TOUPPER ${NAME} EXTENSION_NAME_UPPERCASE)

    list(FIND PREBUILT_EXTENSIONS ${EXTENSION_NAME_LOWERCASE} PREBUILT_EXTENSION_INDEX)
    if(PREBUILT_EXTENSION_INDEX GREATER -1)
        get_filename_component(PREBUILT_BINARY_PATH "${PREBUILT_BINARY}" ABSOLUTE)
        get_filename_component(PREBUILT_EXTENSION_DIRECTORY "${PREBUILT_BINARY_PATH}" DIRECTORY)
        if(MSVC)
            set(PREBUILT_EXTENSION_FILENAME "${EXTENSION_NAME_LOWERCASE}_extension.lib")
        else()
            set(PREBUILT_EXTENSION_FILENAME "lib${EXTENSION_NAME_LOWERCASE}_extension.a")
        endif()
        set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PREBUILT_PATH
            "${PREBUILT_EXTENSION_DIRECTORY}/${PREBUILT_EXTENSION_FILENAME}")
        if(NOT EXISTS "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PREBUILT_PATH}")
            message(FATAL_ERROR "Prebuilt extension library does not exist: ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PREBUILT_PATH}")
        endif()
        set(duckdb_extension_load_LOAD_TESTS FALSE)
        message(STATUS "Use prebuilt extension '${EXTENSION_NAME_LOWERCASE}' from '${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PREBUILT_PATH}'")
    endif()

    # If extension was set already, we ignore subsequent calls
    list (FIND DUCKDB_EXTENSION_NAMES ${EXTENSION_NAME_LOWERCASE} _index)
    if (${_index} GREATER -1)
        return()
    endif()

    list (FIND SKIP_EXTENSIONS ${EXTENSION_NAME_LOWERCASE} _index)
    if (${_index} GREATER -1)
        return()
    endif()

    # Remote Git extension
    if (${duckdb_extension_load_DONT_BUILD})
        register_extension(${NAME} "${duckdb_extension_load_DONT_BUILD}" "" "" "" "" "" "${duckdb_extension_load_EXTENSION_VERSION}")
    elseif (NOT "${duckdb_extension_load_GIT_URL}" STREQUAL "")
        if ("${duckdb_extension_load_GIT_TAG}" STREQUAL "")
            message(FATAL_ERROR "Git URL specified but no valid GIT_TAG was found for ${NAME} extension")
        endif()
        register_external_extension(${NAME} "${duckdb_extension_load_GIT_URL}" "${duckdb_extension_load_GIT_TAG}" "${duckdb_extension_load_DONT_BUILD}" "${duckdb_extension_load_LOAD_TESTS}" "${duckdb_extension_load_SOURCE_DIR}" "${duckdb_extension_load_INCLUDE_DIR}" "${duckdb_extension_load_TEST_DIR}" "${duckdb_extension_load_APPLY_PATCHES}" "${duckdb_extension_load_LINKED_LIBS}" "${duckdb_extension_load_SUBMODULES}" "${duckdb_extension_load_EXTENSION_VERSION}")
        if (NOT "${duckdb_extension_load_EXTENSION_VERSION}" STREQUAL "")
            set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION "${duckdb_extension_load_EXTENSION_VERSION}" PARENT_SCOPE)
        endif()
    elseif (NOT "${duckdb_extension_load_SOURCE_DIR}" STREQUAL "")
        # Version detection
        if ("${duckdb_extension_load_EXTENSION_VERSION}" STREQUAL "")
            duckdb_extension_generate_version(EXT_VERSION ${duckdb_extension_load_SOURCE_DIR})
        else()
            set(EXT_VERSION ${duckdb_extension_load_EXTENSION_VERSION})
        endif()

        # Local extension, custom path
        message(STATUS "Load extension '${NAME}' from '${duckdb_extension_load_SOURCE_DIR}' @ ${EXT_VERSION}")

        # If no include path specified, use default
        if ("${duckdb_extension_load_INCLUDE_DIR}" STREQUAL "")
            set(INCLUDE_PATH_DEFAULT "${duckdb_extension_load_SOURCE_DIR}/src/include")
        else()
            set(INCLUDE_PATH_DEFAULT ${duckdb_extension_load_INCLUDE_DIR})
        endif()

        # If no test path specified, use default
        if ("${duckdb_extension_load_TEST_DIR}" STREQUAL "")
            set(TEST_PATH_DEFAULT "${duckdb_extension_load_SOURCE_DIR}/test/sql")
        else()
            set(TEST_PATH_DEFAULT ${duckdb_extension_load_TEST_DIR})
        endif()

        register_extension(${NAME} "${duckdb_extension_load_DONT_BUILD}" "${duckdb_extension_load_LOAD_TESTS}" "${duckdb_extension_load_SOURCE_DIR}" "${INCLUDE_PATH_DEFAULT}" "${TEST_PATH_DEFAULT}" "${duckdb_extension_load_LINKED_LIBS}" "${EXT_VERSION}")
    elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/extension_external/${NAME})
        # Local extension, default path
        message(STATUS "Load extension '${NAME}' from '${CMAKE_CURRENT_SOURCE_DIR}/extension_external' @ ${duckdb_extension_load_EXTENSION_VERSION}")
        register_extension(${NAME} "${duckdb_extension_load_DONT_BUILD}" "${duckdb_extension_load_LOAD_TESTS}"  "${CMAKE_CURRENT_SOURCE_DIR}/extension_external/${NAME}" "${CMAKE_CURRENT_SOURCE_DIR}/extension_external/${NAME}/src/include" "${CMAKE_CURRENT_SOURCE_DIR}/extension_external/${NAME}/test/sql" "${duckdb_extension_load_LINKED_LIBS}" "${duckdb_extension_load_EXTENSION_VERSION}")
    else()
        # For in-tree extensions of the default path, we set the extension version to GIT_COMMIT_HASH by default
        if ("${duckdb_extension_load_EXTENSION_VERSION}" STREQUAL "")
            set(duckdb_extension_load_EXTENSION_VERSION ${DUCKDB_NORMALIZED_VERSION})
        endif()

        # Local extension, default path
        message(STATUS "Load extension '${NAME}' from '${CMAKE_CURRENT_SOURCE_DIR}/extensions' @ ${duckdb_extension_load_EXTENSION_VERSION}")

        register_extension(${NAME} "${duckdb_extension_load_DONT_BUILD}" "${duckdb_extension_load_LOAD_TESTS}" "${CMAKE_CURRENT_SOURCE_DIR}/extension/${NAME}" "${CMAKE_CURRENT_SOURCE_DIR}/extension/${NAME}/include" "${CMAKE_CURRENT_SOURCE_DIR}/extension/${NAME}/test/sql" "${duckdb_extension_load_LINKED_LIBS}" "${duckdb_extension_load_EXTENSION_VERSION}")
    endif()

    # Propagate variables set by register_extension
    set(DUCKDB_EXTENSION_NAMES ${DUCKDB_EXTENSION_NAMES} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_BUILD ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_BUILD} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_LINK ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_SHOULD_LINK} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LOAD_TESTS ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LOAD_TESTS} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PATH ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PATH} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_INCLUDE_PATH ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_INCLUDE_PATH} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_TEST_PATH ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_TEST_PATH} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LINKED_LIBS ${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_LINKED_LIBS} PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_EXT_VERSION}" PARENT_SCOPE)
    set(DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PREBUILT_PATH "${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PREBUILT_PATH}" PARENT_SCOPE)
endfunction()

if(${EXPORT_DLL_SYMBOLS})
    # For Windows DLL export symbols
    add_definitions(-DDUCKDB_BUILD_LIBRARY)
endif()

# Log extensions that are built by directly passing cmake variables
foreach(EXT IN LISTS DUCKDB_EXTENSION_NAMES)
    if (NOT "${EXT}" STREQUAL "")
        string(TOUPPER ${EXT} EXTENSION_NAME_UPPERCASE)
        message(STATUS "Load extension '${EXT}' from '${DUCKDB_EXTENSION_${EXTENSION_NAME_UPPERCASE}_PATH}'")
    endif()
endforeach()

if(NOT DEFINED EXTENSION_CONFIG_BASE_DIR)
    if(DEFINED ENV{EXTENSION_CONFIG_BASE_DIR} AND NOT "$ENV{EXTENSION_CONFIG_BASE_DIR}" STREQUAL "")
        set(EXTENSION_CONFIG_BASE_DIR "$ENV{EXTENSION_CONFIG_BASE_DIR}")
    else()
        set(EXTENSION_CONFIG_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.github/config/extensions")
    endif()
endif()
get_filename_component(EXTENSION_CONFIG_BASE_DIR "${EXTENSION_CONFIG_BASE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT IS_DIRECTORY "${EXTENSION_CONFIG_BASE_DIR}")
    message(FATAL_ERROR "Extension config directory does not exist: ${EXTENSION_CONFIG_BASE_DIR}")
endif()

if(DEFINED CORE_EXTENSIONS)
    message(DEPRECATION "CORE_EXTENSIONS is deprecated. Use BUILD_EXTENSIONS instead.")
    if(NOT DEFINED BUILD_EXTENSIONS)
        set(BUILD_EXTENSIONS ${CORE_EXTENSIONS})
    else()
        list(APPEND BUILD_EXTENSIONS ${CORE_EXTENSIONS})
    endif()
endif()

# Explicit project configurations take precedence over named defaults.
foreach(DUCKDB_EXTENSION_CONFIG IN LISTS DUCKDB_EXTENSION_CONFIGS)
    if (NOT "${DUCKDB_EXTENSION_CONFIG}" STREQUAL "")
        include("${DUCKDB_EXTENSION_CONFIG}")
    endif()
endforeach()

# Load extensions passed through cmake config var
foreach(EXT IN LISTS BUILD_EXTENSIONS)
    if("${EXT}" STREQUAL "jemalloc")
        message(WARNING "The 'jemalloc' allocator is no longer provided as an extension, use 'ENABLE_JEMALLOC=ON' to include jemalloc instead")
        set(ENABLE_JEMALLOC ON CACHE BOOL "Use jemalloc as the memory allocator for DuckDB" FORCE)
        # Backward-compat shim: downstream consumers call target_link_libraries(... ${ext}_extension).
        # We provide an empty INTERFACE target to make sure that doesn't fail.
        if(NOT TARGET jemalloc_extension)
            add_library(jemalloc_extension INTERFACE)
        endif()
        continue()
    endif()

    if(NOT "${EXT}" STREQUAL "")
        if (EXISTS "${EXTENSION_CONFIG_BASE_DIR}/${EXT}.cmake")
            # out-of-tree extension: load cmake file
            include("${EXTENSION_CONFIG_BASE_DIR}/${EXT}.cmake")
        else()
            # in-tree or non-existent extension: load it
            duckdb_extension_load(${EXT})
        endif()
        if(LINK_CORE_EXTENSIONS)
            duckdb_extension_statically_link(${EXT})
        endif()
    endif()
endforeach()

# Check if jemalloc is ignored, and if so disable it
list (FIND SKIP_EXTENSIONS "jemalloc" _index)
if (${_index} GREATER -1)
    message(WARNING "The 'jemalloc' allocator is no longer provided as an extension, use 'ENABLE_JEMALLOC=OFF' to disable jemalloc instead")
    set(ENABLE_JEMALLOC OFF CACHE BOOL "Use jemalloc as the memory allocator for DuckDB" FORCE)
endif()



# Local extension config
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/extension/extension_config_local.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/extension/extension_config_local.cmake)
endif()

# Load base extension config
include(${CMAKE_CURRENT_SOURCE_DIR}/extension/extension_config.cmake)




# Declares which of an extension's sources read EXT_VERSION_<NAME>. DuckDB defines it for the whole extension
# directory by default; this narrows it down to just the listed sources, so that a version change recompiles
# those instead of the entire extension and anything it vendors. Worth doing because the version defaults to a
# git commit hash - DuckDB's for in-tree extensions, the extension's own otherwise - and so changes constantly.
# Must be called from the extension's own CMakeLists before it declares any targets or subdirectories, as those
# inherit the directory-wide definition at the point they are created.
function(set_extension_version_sources EXT_NAME)
    string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
    if ("${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_EXT_VERSION}" STREQUAL "")
        return()
    endif()
    set(DEFINITION EXT_VERSION_${EXT_NAME_UPPERCASE}="${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_EXT_VERSION}")
    remove_definitions(-D${DEFINITION})
    if (NOT ARGN)
        return()
    endif()
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_DEFINITIONS ${DEFINITION})
endfunction()

# Add subdirectories for registered extensions
set(DUCKDB_ADDING_EXTENSIONS TRUE)
foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
    string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)

    if (NOT DEFINED DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_BUILD)
        set(DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_BUILD TRUE)
    endif()

    # Skip explicitly disabled extensions
    if (NOT ${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_BUILD} OR ${EXTENSION_CONFIG_BUILD})
        continue()
    endif()

    # Warning for trying to load vcpkg extensions without having VCPKG_BUILD SET
    if (EXISTS "${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PATH}/vcpkg.json" AND NOT DEFINED VCPKG_BUILD)
        message(WARNING "Extension '${EXT_NAME}' has a vcpkg.json, but build was not run with VCPKG. If build fails, check out VCPKG build instructions in 'duckdb/extension/README.md' or try manually installing the dependencies in ${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PATH}/vcpkg.json")
    endif()

    if (NOT "${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_EXT_VERSION}" STREQUAL "")
        add_definitions(-DEXT_VERSION_${EXT_NAME_UPPERCASE}="${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_EXT_VERSION}")
    endif()

    if (DEFINED DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PATH)
        # Provide the extension name to the extension's CMakeLists. C-API extensions built from the
        # extension-template-c template require EXTENSION_NAME to be defined; supplying it here lets
        # such an extension be built in-tree without a wrapper CMakeLists or hardcoding its own name.
        set(EXTENSION_NAME ${EXT_NAME})
        if(NOT "${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PREBUILT_PATH}" STREQUAL "")
            add_subdirectory(${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PATH} extension/${EXT_NAME} EXCLUDE_FROM_ALL)
        else()
            add_subdirectory(${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PATH} extension/${EXT_NAME})
        endif()
        duckdb_add_missing_extension_describe(${EXT_NAME})
        unset(EXTENSION_NAME)
    else()
        message(FATAL_ERROR "No path found for registered extension '${EXT_NAME}'")
    endif()

    if (NOT "${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_EXT_VERSION}" STREQUAL "")
        remove_definitions(-DEXT_VERSION_${EXT_NAME_UPPERCASE}="${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_EXT_VERSION}")
    endif()
endforeach()
unset(DUCKDB_ADDING_EXTENSIONS)

# Resolve what the DuckDB targets link once; each extension's SHOULD_LINK follows it, so everything that reads it agrees
duckdb_resolve_static_link(DUCKDB_STATICALLY_LINKED_CAPABILITIES DUCKDB_STATICALLY_LINKED_EXTENSIONS)
if(NOT ${BUILD_EXTENSIONS_ONLY})
    foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
        string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
        if(EXT_NAME IN_LIST DUCKDB_STATICALLY_LINKED_EXTENSIONS)
            set(DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_LINK TRUE)
        else()
            set(DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_LINK FALSE)
        endif()
    endforeach()
endif()

# Output the extensions that we linked into DuckDB for some nice build logs
set(DEFAULT_LINKED_EXTENSIONS "")
set(NONLINKED_EXTENSIONS "")
set(SKIPPED_EXTENSIONS "")
set(TEST_LOADED_EXTENSIONS "")
foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
    string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
    if (NOT ${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_BUILD})
        list(APPEND SKIPPED_EXTENSIONS ${EXT_NAME})
    elseif (${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_LINK})
        list(APPEND DEFAULT_LINKED_EXTENSIONS ${EXT_NAME})
    else()
        list(APPEND NONLINKED_EXTENSIONS ${EXT_NAME})
    endif()

    if (${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_LOAD_TESTS})
        list(APPEND TEST_LOADED_EXTENSIONS ${EXT_NAME})
    endif()
endforeach()

string(REPLACE ";"  ", " EXT_LIST_DEBUG_MESSAGE "${DUCKDB_STATICALLY_LINKED_EXTENSIONS}")
string(REPLACE ";"  ", " CAPABILITY_LIST_DEBUG_MESSAGE "${DUCKDB_STATICALLY_LINKED_CAPABILITIES}")
message(STATUS "Linked into DuckDB: extensions [${EXT_LIST_DEBUG_MESSAGE}], capabilities [${CAPABILITY_LIST_DEBUG_MESSAGE}]")
if(NOT "${NONLINKED_EXTENSIONS}" STREQUAL "")
    string(REPLACE ";"  ", " EXT_LIST_DEBUG_MESSAGE "${NONLINKED_EXTENSIONS}")
    message(STATUS "Extensions built but not linked: [${EXT_LIST_DEBUG_MESSAGE}]")
endif()
if(NOT "${SKIPPED_EXTENSIONS}" STREQUAL "")
    string(REPLACE ";"  ", " EXT_LIST_DEBUG_MESSAGE "${SKIPPED_EXTENSIONS}")
    message(STATUS "Extensions explicitly skipped: [${EXT_LIST_DEBUG_MESSAGE}]")
endif()
if(NOT "${TEST_LOADED_EXTENSIONS}" STREQUAL "")
    string(REPLACE ";"  ", " EXT_LIST_DEBUG_MESSAGE "${TEST_LOADED_EXTENSIONS}")
    message(STATUS "Tests loaded for extensions: [${EXT_LIST_DEBUG_MESSAGE}]")
endif()

# Special build where instead of building duckdb, we produce several artifact that require parsing the
# extension config, such as a merged vcpg.json file for extension dependencies.
if(${EXTENSION_CONFIG_BUILD})
    set(VCPKG_PATHS "")
    set(VCPKG_NAMES "")
    foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
        string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
        if (EXISTS "${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PATH}/vcpkg.json")
            list(APPEND VCPKG_NAMES ${EXT_NAME})
            list(APPEND VCPKG_PATHS ${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_PATH}/vcpkg.json)
        endif()
    endforeach()

    add_custom_target(
            duckdb_merge_vcpkg_manifests ALL
            COMMAND  ${Python3_EXECUTABLE} scripts/merge_vcpkg_deps.py ${VCPKG_PATHS} ${EXT_NAMES}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT Generates a shared vcpkg manifest from the individual extensions)
    string(REPLACE ";"  ", " VCPKG_NAMES_COMMAS "${VCPKG_NAMES}")
    Message(STATUS "Combined vcpkg manifest created from extensions: ${VCPKG_NAMES_COMMAS}")

    # Write linked extensions that will be built to extensions_linked.txt
    FILE(WRITE ${CMAKE_BINARY_DIR}/extensions.csv "")
    FILE(APPEND ${CMAKE_BINARY_DIR}/extensions.csv "name, version\r")
    foreach(EXT_NAME IN LISTS DUCKDB_EXTENSION_NAMES)
        string(TOUPPER ${EXT_NAME} EXT_NAME_UPPERCASE)
        if (${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_SHOULD_BUILD})
            FILE(APPEND ${CMAKE_BINARY_DIR}/extensions.csv "${EXT_NAME}, \"${DUCKDB_EXTENSION_${EXT_NAME_UPPERCASE}_EXT_VERSION}\"\r")
        endif()
    endforeach()

    return()
endif()
