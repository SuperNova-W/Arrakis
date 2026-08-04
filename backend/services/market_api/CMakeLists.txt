add_executable(market-api src/main.cpp src/live_market.cpp)
target_compile_features(market-api PRIVATE cxx_std_20)
target_include_directories(market-api PRIVATE include)
target_link_libraries(market-api PRIVATE arrakis_database_core arrakis_news_nlp_core arrakis_market_core Boost::json xgboost::xgboost)
set_target_properties(market-api PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(market-api PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

if(BUILD_TESTING)
    add_executable(market-api-live-market-tests tests/live_market_test.cpp src/live_market.cpp)
    target_compile_features(market-api-live-market-tests PRIVATE cxx_std_20)
    target_include_directories(market-api-live-market-tests PRIVATE include)
    target_compile_definitions(
        market-api-live-market-tests
        PRIVATE ARRAKIS_TEST_ETF_UNIVERSE="${PROJECT_SOURCE_DIR}/config/etf_universe.json"
    )
    target_link_libraries(
        market-api-live-market-tests
        PRIVATE arrakis_bar_aggregator_core arrakis_market_core Boost::json GTest::gtest_main
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            market-api-live-market-tests
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
    endif()
    add_test(NAME market-api-live-market-tests COMMAND market-api-live-market-tests)
endif()
