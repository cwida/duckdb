set(SQLITE_TEST_RUNNER_SOURCES
    result_helper.cpp
    sqllogic_command.cpp
    sqllogic_test_runner.cpp
    sqllogic_parser.cpp
    test_sqllogictest.cpp
    sqllogic_test_logger.cpp
    catch_test_reporter.cpp)

add_library_unity(test_sqlite OBJECT ${SQLITE_TEST_RUNNER_SOURCES})
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_sqlite>
    PARENT_SCOPE)
