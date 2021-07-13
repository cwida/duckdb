.PHONY: all opt unit clean debug release release_expanded test unittest allunit docs doxygen format sqlite imdb

all: release
opt: release
unit: unittest
imdb: third_party/imdb/data

GENERATOR=
FORCE_COLOR=
WARNINGS_AS_ERRORS=
DISABLE_UNITY_FLAG=
DISABLE_SANITIZER_FLAG=
ifeq ($(GEN),ninja)
	GENERATOR=-G "Ninja"
	FORCE_COLOR=-DFORCE_COLORED_OUTPUT=1
endif
ifeq (${TREAT_WARNINGS_AS_ERRORS}, 1)
	WARNINGS_AS_ERRORS=-DTREAT_WARNINGS_AS_ERRORS=1
endif
ifeq (${DISABLE_UNITY}, 1)
	DISABLE_UNITY_FLAG=-DDISABLE_UNITY=1
endif
ifeq (${DISABLE_SANITIZER}, 1)
	DISABLE_SANITIZER_FLAG=-DENABLE_SANITIZER=FALSE -DENABLE_UBSAN=0
endif
ifeq (${DISABLE_UBSAN}, 1)
	DISABLE_SANITIZER_FLAG=-DENABLE_UBSAN=0
endif
ifeq (${DISABLE_VPTR_SANITIZER}, 1)
	DISABLE_SANITIZER_FLAG:=${DISABLE_SANITIZER_FLAG} -DDISABLE_VPTR_SANITIZER=1
endif
ifeq (${FORCE_SANITIZER}, 1)
	DISABLE_SANITIZER_FLAG:=${DISABLE_SANITIZER_FLAG} -DFORCE_SANITIZER=1
endif
ifeq (${THREADSAN}, 1)
	DISABLE_SANITIZER_FLAG:=${DISABLE_SANITIZER_FLAG} -DENABLE_THREAD_SANITIZER=1
endif
ifeq (${STATIC_LIBCPP}, 1)
	STATIC_LIBCPP=-DSTATIC_LIBCPP=TRUE
endif
EXTENSIONS=-DBUILD_PARQUET_EXTENSION=TRUE
ifeq (${BUILD_BENCHMARK}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_BENCHMARKS=1
endif
ifeq (${BUILD_ICU}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_ICU_EXTENSION=1
endif
ifeq (${BUILD_TPCH}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_TPCH_EXTENSION=1
endif
ifeq (${BUILD_TPCDS}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_TPCDS_EXTENSION=1
endif
ifeq (${BUILD_FTS}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_FTS_EXTENSION=1
endif
ifeq (${BUILD_VISUALIZER}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_VISUALIZER_EXTENSION=1
endif
ifeq (${BUILD_HTTPFS}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_HTTPFS_EXTENSION=1
endif
ifeq (${BUILD_SQLSMITH}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_SQLSMITH=1
endif
ifeq (${BUILD_TPCE}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_TPCE=1
endif
ifeq (${BUILD_JDBC}, 1)
	EXTENSIONS:=${EXTENSIONS} -DJDBC_DRIVER=1
endif
ifeq (${BUILD_ODBC}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_ODBC_DRIVER=1
endif
ifeq (${BUILD_PYTHON}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_PYTHON=1 -DBUILD_FTS_EXTENSION=1 -DBUILD_TPCH_EXTENSION=1 -DBUILD_VISUALIZER_EXTENSION=1 -DBUILD_TPCDS_EXTENSION=1
endif
ifeq (${BUILD_R}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_R=1
endif
ifeq (${BUILD_REST}, 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_REST=1
endif
ifneq ($(TIDY_THREADS),)
	TIDY_THREAD_PARAMETER := -j ${TIDY_THREADS}
endif
ifeq ($(BUILD_ARROW_ABI_TEST), 1)
	EXTENSIONS:=${EXTENSIONS} -DBUILD_ARROW_ABI_TEST=1
endif
ifneq ("${FORCE_QUERY_LOG}a", "a")
	EXTENSIONS:=${EXTENSIONS} -DFORCE_QUERY_LOG=${FORCE_QUERY_LOG}
endif

clean:
	rm -rf build

debug:
	mkdir -p build/debug && \
	cd build/debug && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${DISABLE_UNITY_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${EXTENSIONS} -DCMAKE_BUILD_TYPE=Debug ../.. && \
	cmake --build .

release_expanded:
	mkdir -p build/release_expanded && \
	cd build/release_expanded && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${DISABLE_UNITY_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${EXTENSIONS} -DCMAKE_BUILD_TYPE=Release ../.. && \
	cmake --build .

cldebug:
	mkdir -p build/cldebug && \
	cd build/cldebug && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${DISABLE_UNITY_FLAG} ${EXTENSIONS} -DBUILD_PYTHON=1 -DBUILD_R=1 -DENABLE_SANITIZER=0 -DENABLE_UBSAN=0 -DCMAKE_BUILD_TYPE=Debug ../.. && \
	cmake --build .

clreldebug:
	mkdir -p build/clreldebug && \
	cd build/clreldebug && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${DISABLE_UNITY_FLAG} ${STATIC_LIBCPP} ${EXTENSIONS} -DBUILD_PYTHON=1 -DBUILD_R=1 -DBUILD_FTS_EXTENSION=1 -DENABLE_SANITIZER=0 -DENABLE_UBSAN=0 -DCMAKE_BUILD_TYPE=RelWithDebInfo ../.. && \
	cmake --build .

unittest: debug
	build/debug/test/unittest
	build/debug/tools/sqlite3_api_wrapper/test_sqlite3_api_wrapper

unittestci:
	build/debug/test/unittest
	build/debug/tools/sqlite3_api_wrapper/test_sqlite3_api_wrapper

allunit: release_expanded # uses release build because otherwise allunit takes forever
	build/release_expanded/test/unittest "*"

docs:
	mkdir -p build/docs && \
	doxygen Doxyfile

doxygen: docs
	open build/docs/html/index.html

release:
	mkdir -p build/release && \
	cd build/release && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${DISABLE_UNITY_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${EXTENSIONS} -DCMAKE_BUILD_TYPE=Release ../.. && \
	cmake --build .

reldebug:
	mkdir -p build/reldebug && \
	cd build/reldebug && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${DISABLE_UNITY_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${EXTENSIONS} -DCMAKE_BUILD_TYPE=RelWithDebInfo ../.. && \
	cmake --build .

relassert:
	mkdir -p build/relassert && \
	cd build/relassert && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${WARNINGS_AS_ERRORS} ${DISABLE_UNITY_FLAG} ${DISABLE_SANITIZER_FLAG} ${STATIC_LIBCPP} ${EXTENSIONS} -DFORCE_ASSERT=1 -DCMAKE_BUILD_TYPE=RelWithDebInfo ../.. && \
	cmake --build .

amaldebug:
	mkdir -p build/amaldebug && \
	python scripts/amalgamation.py && \
	cd build/amaldebug && \
	cmake $(GENERATOR) $(FORCE_COLOR) ${STATIC_LIBCPP} ${EXTENSIONS} -DAMALGAMATION_BUILD=1 -DCMAKE_BUILD_TYPE=Debug ../.. && \
	cmake --build .

tidy-check:
	mkdir -p build/tidy && \
	cd build/tidy && \
	cmake -DCLANG_TIDY=1 -DDISABLE_UNITY=1 -DBUILD_ODBC_DRIVER=TRUE -DBUILD_PARQUET_EXTENSION=TRUE -DBUILD_PYTHON_PKG=TRUE -DBUILD_SHELL=0 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../.. && \
	python3 ../../scripts/run-clang-tidy.py -quiet ${TIDY_THREAD_PARAMETER}

tidy-fix:
	mkdir -p build/tidy && \
	cd build/tidy && \
	cmake -DCLANG_TIDY=1 -DDISABLE_UNITY=1 -DBUILD_PARQUET_EXTENSION=TRUE -DBUILD_SHELL=0 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../.. && \
	python3 ../../scripts/run-clang-tidy.py -fix

test_compile: # test compilation of individual cpp files
	python scripts/amalgamation.py --compile

format-check:
	python3 scripts/format.py --all --check

format-check-silent:
	python3 scripts/format.py --all --check --silent

format-fix:
	python3 scripts/format.py --all --fix --noconfirm

format-head:
	python3 scripts/format.py HEAD --fix --noconfirm

format-master:
	python3 scripts/format.py master --fix --noconfirm

third_party/sqllogictest:
	git clone --depth=1 https://github.com/cwida/sqllogictest.git third_party/sqllogictest

third_party/imdb/data:
	wget -i "http://download.duckdb.org/imdb/list.txt" -P third_party/imdb/data

sqlite: release_expanded | third_party/sqllogictest
	git --git-dir third_party/sqllogictest/.git pull
	./build/release_expanded/test/unittest "[sqlitelogic]"

sqlsmith: debug
	./build/debug/third_party/sqlsmith/sqlsmith --duckdb=:memory:
