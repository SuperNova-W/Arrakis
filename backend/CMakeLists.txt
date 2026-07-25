cmake_minimum_required(VERSION 3.18)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(
    ArrakisBackend
    VERSION 0.1.0
    DESCRIPTION "C++ model training and market-signal backend for Arrakis"
    LANGUAGES CXX
)

include(CTest)

option(ARRAKIS_BUILD_XGBOOST_TRAINER "Build the XGBoost baseline trainer" ON)
option(ARRAKIS_BUILD_MARKET_STREAM "Build the Finnhub market-data WebSocket client" ON)

add_library(arrakis_model_core
    src/dataset.cpp
    src/metrics.cpp
)

target_compile_features(arrakis_model_core PUBLIC cxx_std_20)
target_include_directories(
    arrakis_model_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        arrakis_model_core
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -Werror
    )
endif()

if(ARRAKIS_BUILD_MARKET_STREAM)
    find_package(Boost 1.75 REQUIRED COMPONENTS json)
    find_package(OpenSSL REQUIRED)

    add_library(arrakis_market_core src/finnhub_message.cpp)
    target_compile_features(arrakis_market_core PUBLIC cxx_std_20)
    target_include_directories(
        arrakis_market_core
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(arrakis_market_core PUBLIC Boost::json)

    add_executable(arrakis-market-stream src/market_stream.cpp)
    target_compile_features(arrakis-market-stream PRIVATE cxx_std_20)
    target_link_libraries(
        arrakis-market-stream
        PRIVATE
            arrakis_market_core
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis_market_core
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
        target_compile_options(
            arrakis-market-stream
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()
endif()

if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    find_package(xgboost CONFIG REQUIRED)

    add_executable(arrakis-train-xgboost src/train_xgboost.cpp)
    target_compile_features(arrakis-train-xgboost PRIVATE cxx_std_20)
    target_link_libraries(
        arrakis-train-xgboost
        PRIVATE
            arrakis_model_core
            xgboost::xgboost
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis-train-xgboost
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()
endif()

if(BUILD_TESTING)
    add_executable(arrakis_model_core_tests tests/model_core_test.cpp)
    target_compile_features(arrakis_model_core_tests PRIVATE cxx_std_20)
    target_link_libraries(arrakis_model_core_tests PRIVATE arrakis_model_core)
    target_compile_definitions(
        arrakis_model_core_tests
        PRIVATE
            ARRAKIS_SAMPLE_DATASET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/sample_features.csv"
    )
    add_test(NAME arrakis_model_core_tests COMMAND arrakis_model_core_tests)

    if(ARRAKIS_BUILD_MARKET_STREAM)
        add_executable(arrakis_market_core_tests tests/market_core_test.cpp)
        target_compile_features(arrakis_market_core_tests PRIVATE cxx_std_20)
        target_link_libraries(arrakis_market_core_tests PRIVATE arrakis_market_core)
        add_test(NAME arrakis_market_core_tests COMMAND arrakis_market_core_tests)
    endif()
endif()
