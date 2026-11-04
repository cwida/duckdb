set(SQLITE_TEST_RUNNER_SOURCES
    result_helper.cpp sqllogic_command.cpp sqllogic_test_runner.cpp
    sqllogic_parser.cpp sqllogic_test_logger.cpp sqllogic_test_executor.cpp)

add_library_unity(test_sqlite_runner STATIC ${SQLITE_TEST_RUNNER_SOURCES})
