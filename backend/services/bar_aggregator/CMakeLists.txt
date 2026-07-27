add_library(arrakis_bar_aggregator_core src/aggregation.cpp)
target_compile_features(arrakis_bar_aggregator_core PUBLIC cxx_std_20)
target_include_directories(
    arrakis_bar_aggregator_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/libs/market/include
)

add_executable(bar-aggregator src/main.cpp)
target_compile_features(bar-aggregator PRIVATE cxx_std_20)
target_link_libraries(bar-aggregator PRIVATE arrakis_bar_aggregator_core arrakis_market_core)
set_target_properties(bar-aggregator PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        bar-aggregator
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -Werror
    )
    target_compile_options(arrakis_bar_aggregator_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()
