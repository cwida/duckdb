add_library_unity(test_filter OBJECT bloom_filter_lookup.cpp filter_cache.cpp
                  prefix_range_filter_lookup.cpp)
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_filter>
    PARENT_SCOPE)
