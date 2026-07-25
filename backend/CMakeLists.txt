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

if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    find_package(xgboost CONFIG REQUIRED)
endif()

find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(GTest REQUIRED)

add_library(arrakis_model_core
    src/dataset.cpp
    src/metrics.cpp
    src/sector_ml.cpp
)

target_compile_features(arrakis_model_core PUBLIC cxx_std_20)
target_include_directories(
    arrakis_model_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    target_link_libraries(arrakis_model_core PUBLIC xgboost::xgboost)
endif()

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
    set(PROTO_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/event_metadata.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/trade_event.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/market_bar.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/late_trade_event.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/dead_letter_event.proto
    )

    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
    set_source_files_properties(${PROTO_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-nullability-extension")

    add_library(arrakis_market_core
        src/finnhub_message.cpp
        src/historical_data.cpp
        src/normalization.cpp
        src/serialization.cpp
        src/kafka.cpp
        ${PROTO_SRCS}
    )
    target_compile_features(arrakis_market_core PUBLIC cxx_std_20)
    target_include_directories(
        arrakis_market_core
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}
    )
    find_path(RDKAFKA_INCLUDE_DIR librdkafka/rdkafka.h REQUIRED)
    find_library(RDKAFKA_LIBRARY rdkafka REQUIRED)
    target_include_directories(arrakis_market_core PUBLIC ${RDKAFKA_INCLUDE_DIR})
    target_link_libraries(arrakis_market_core PUBLIC Boost::json protobuf::libprotobuf absl::log absl::log_internal_check_op OpenSSL::SSL OpenSSL::Crypto ${RDKAFKA_LIBRARY})

    add_library(arrakis_streaming_core
        src/finnhub_message.cpp
        src/normalization.cpp
        src/serialization.cpp
        ${PROTO_SRCS}
    )
    target_compile_features(arrakis_streaming_core PUBLIC cxx_std_20)
    target_include_directories(
        arrakis_streaming_core
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(arrakis_streaming_core PUBLIC Boost::json protobuf::libprotobuf absl::log absl::log_internal_check_op)

    add_executable(bar-aggregator src/bar_aggregator.cpp)
    target_compile_features(bar-aggregator PRIVATE cxx_std_20)
    target_link_libraries(bar-aggregator PRIVATE arrakis_market_core)

    add_executable(arrakis-market-stream src/market_stream.cpp)
    target_compile_features(arrakis-market-stream PRIVATE cxx_std_20)
    target_link_libraries(
        arrakis-market-stream
        PRIVATE
            arrakis_market_core
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    add_executable(fetch-historical-data src/fetch_historical_data.cpp)
    target_compile_features(fetch-historical-data PRIVATE cxx_std_20)
    target_link_libraries(
        fetch-historical-data
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
            arrakis_streaming_core
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
        target_compile_options(
            fetch-historical-data
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
        target_compile_options(bar-aggregator PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()
endif()

if(ARRAKIS_BUILD_XGBOOST_TRAINER)
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

        add_executable(arrakis_historical_data_tests tests/historical_data_test.cpp)
        target_compile_features(arrakis_historical_data_tests PRIVATE cxx_std_20)
        target_link_libraries(arrakis_historical_data_tests PRIVATE arrakis_market_core)
        add_test(NAME arrakis_historical_data_tests COMMAND arrakis_historical_data_tests)

        add_executable(arrakis_streaming_tests tests/streaming_core_test.cpp)
        target_compile_features(arrakis_streaming_tests PRIVATE cxx_std_20)
        target_link_libraries(arrakis_streaming_tests PRIVATE arrakis_streaming_core GTest::gtest_main)
        add_test(NAME arrakis_streaming_tests COMMAND arrakis_streaming_tests)
    endif()
endif()
