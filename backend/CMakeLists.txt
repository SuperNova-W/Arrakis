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
option(ARRAKIS_BUILD_MARKET_API "Build the market REST/WebSocket API and XGBoost inference runtime" ON)
option(ARRAKIS_BUILD_ML_RUNTIME "Build deferred feature and prediction services" ON)

if(ARRAKIS_BUILD_XGBOOST_TRAINER OR (ARRAKIS_BUILD_MARKET_STREAM AND ARRAKIS_BUILD_MARKET_API))
    find_package(xgboost CONFIG REQUIRED)
endif()

find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)
set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
endif()

add_subdirectory(services/ml_model)
add_subdirectory(services/news_nlp)

if(ARRAKIS_BUILD_MARKET_STREAM)
    set(PROTO_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/event_metadata.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/trade_event.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/market_bar.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/late_trade_event.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/dead_letter_event.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/feature_event.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/feature_error_event.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/model_prediction.proto
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/news_article.proto
    )

    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
    set_source_files_properties(${PROTO_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-nullability-extension")

    add_library(arrakis_market_core
        libs/market/src/finnhub_message.cpp
        src/historical_data.cpp
        libs/market/src/normalization.cpp
        libs/serialization/src/serialization.cpp
        libs/serialization/src/news_serialization.cpp
        src/kafka.cpp
        src/config.cpp
        src/metrics_runtime.cpp
        ${PROTO_SRCS}
    )
    target_compile_features(arrakis_market_core PUBLIC cxx_std_20)
    target_include_directories(
        arrakis_market_core
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/libs/market/include
            ${CMAKE_CURRENT_SOURCE_DIR}/libs/serialization/include
            ${CMAKE_CURRENT_SOURCE_DIR}/services/bar_aggregator/include
            ${CMAKE_CURRENT_BINARY_DIR}
    )
    find_path(RDKAFKA_INCLUDE_DIR librdkafka/rdkafka.h REQUIRED)
    find_library(RDKAFKA_LIBRARY rdkafka REQUIRED)
    target_include_directories(arrakis_market_core PUBLIC ${RDKAFKA_INCLUDE_DIR})
    target_link_libraries(arrakis_market_core PUBLIC Boost::json protobuf::libprotobuf OpenSSL::SSL OpenSSL::Crypto ${RDKAFKA_LIBRARY})

    add_library(arrakis_streaming_core
        libs/market/src/finnhub_message.cpp
        libs/market/src/normalization.cpp
        libs/serialization/src/serialization.cpp
        src/config.cpp
        src/metrics_runtime.cpp
        ${PROTO_SRCS}
    )
    target_compile_features(arrakis_streaming_core PUBLIC cxx_std_20)
    target_include_directories(
        arrakis_streaming_core
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/libs/market/include
            ${CMAKE_CURRENT_SOURCE_DIR}/libs/serialization/include
            ${CMAKE_CURRENT_SOURCE_DIR}/services/bar_aggregator/include
            ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(arrakis_streaming_core PUBLIC Boost::json protobuf::libprotobuf OpenSSL::Crypto)

    add_subdirectory(services/bar_aggregator)
    if(APPLE AND NOT PostgreSQL_ROOT AND EXISTS "/opt/homebrew/opt/libpq")
        set(PostgreSQL_ROOT "/opt/homebrew/opt/libpq")
    endif()
    find_package(PostgreSQL REQUIRED)
    add_library(arrakis_database_core
        libs/database/src/postgres.cpp
    )
    target_compile_features(arrakis_database_core PUBLIC cxx_std_20)
    target_include_directories(arrakis_database_core
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/services/bar_aggregator/include
    )
    target_link_libraries(arrakis_database_core PUBLIC arrakis_bar_aggregator_core PostgreSQL::PostgreSQL)
    target_compile_options(arrakis_database_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_link_libraries(bar-aggregator PRIVATE arrakis_database_core)
    if(ARRAKIS_BUILD_MARKET_API)
        add_subdirectory(services/market_api)
    endif()
    if(ARRAKIS_BUILD_ML_RUNTIME)
        add_subdirectory(services/feature_engine)
        add_subdirectory(services/prediction_service)
    endif()

    add_executable(market-ingestion src/market_stream.cpp)
    target_compile_features(market-ingestion PRIVATE cxx_std_20)
    target_link_libraries(
        market-ingestion
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

    add_executable(news-ingestion src/news_stream.cpp)
    target_compile_features(news-ingestion PRIVATE cxx_std_20)
    target_link_libraries(news-ingestion PRIVATE arrakis_market_core OpenSSL::Crypto)

    add_executable(news-enricher src/news_enricher.cpp)
    target_compile_features(news-enricher PRIVATE cxx_std_20)
    target_link_libraries(news-enricher PRIVATE arrakis_database_core arrakis_news_nlp_core arrakis_market_core)

    add_executable(arrakis-kafka-benchmark benchmarks/kafka_pipeline_benchmark.cpp)
    target_compile_features(arrakis-kafka-benchmark PRIVATE cxx_std_20)
    target_link_libraries(arrakis-kafka-benchmark PRIVATE arrakis_market_core)

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
            market-ingestion
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
    endif()
endif()

if(BUILD_TESTING AND ARRAKIS_BUILD_XGBOOST_TRAINER)
    add_executable(arrakis_model_core_tests tests/model_core_test.cpp)
    target_compile_features(arrakis_model_core_tests PRIVATE cxx_std_20)
    target_link_libraries(arrakis_model_core_tests PRIVATE arrakis_model_core)
    target_compile_definitions(
        arrakis_model_core_tests
        PRIVATE
            ARRAKIS_SAMPLE_DATASET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/services/ml_model/data/sample_features.csv"
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
        target_link_libraries(arrakis_streaming_tests PRIVATE arrakis_streaming_core arrakis_bar_aggregator_core GTest::gtest_main)
        add_test(NAME arrakis_streaming_tests COMMAND arrakis_streaming_tests)
        add_executable(arrakis_news_nlp_tests tests/news_nlp_test.cpp)
        target_compile_features(arrakis_news_nlp_tests PRIVATE cxx_std_20)
        target_link_libraries(arrakis_news_nlp_tests PRIVATE arrakis_news_nlp_core GTest::gtest_main)
        add_test(NAME arrakis_news_nlp_tests COMMAND arrakis_news_nlp_tests)
        add_executable(arrakis_finbert_smoke_test tests/finbert_smoke_test.cpp)
        target_compile_features(arrakis_finbert_smoke_test PRIVATE cxx_std_20)
        target_link_libraries(arrakis_finbert_smoke_test PRIVATE arrakis_news_nlp_core GTest::gtest_main)
        add_test(NAME arrakis_finbert_smoke_test COMMAND arrakis_finbert_smoke_test)
    endif()
endif()
