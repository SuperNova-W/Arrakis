add_library(arrakis_feature_engine_core
    src/feature_engine.cpp
    src/config.cpp
    src/serialization.cpp
)
target_compile_features(arrakis_feature_engine_core PUBLIC cxx_std_20)
target_include_directories(
    arrakis_feature_engine_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/libs/market/include
        ${PROJECT_SOURCE_DIR}/libs/serialization/include
        ${PROJECT_BINARY_DIR}
)
target_link_libraries(arrakis_feature_engine_core PUBLIC arrakis_market_core arrakis_bar_aggregator_core)

add_executable(feature-engine src/main.cpp)
target_compile_features(feature-engine PRIVATE cxx_std_20)
target_link_libraries(feature-engine PRIVATE arrakis_feature_engine_core arrakis_market_core arrakis_bar_aggregator_core)
set_target_properties(feature-engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(arrakis_feature_engine_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_compile_options(feature-engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

if(BUILD_TESTING)
    add_executable(feature_engine_tests tests/feature_engine_test.cpp)
    target_compile_features(feature_engine_tests PRIVATE cxx_std_20)
    target_link_libraries(feature_engine_tests PRIVATE arrakis_feature_engine_core GTest::gtest_main)
    add_test(NAME feature_engine_tests COMMAND feature_engine_tests)
endif()
