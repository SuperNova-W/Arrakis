set(ARRAKIS_MODEL_CORE_SOURCES
    src/dataset.cpp
    src/metrics.cpp
)
if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    list(APPEND ARRAKIS_MODEL_CORE_SOURCES src/sector_ml.cpp)
endif()

if(ARRAKIS_BUILD_TCN_TRAINER)
    find_path(MLX_INCLUDE_DIR mlx/mlx.h PATHS /opt/homebrew/opt/mlx/include /usr/local/include)
    find_library(MLX_LIBRARY NAMES mlx PATHS /opt/homebrew/opt/mlx/lib /usr/local/lib)
    if(NOT MLX_INCLUDE_DIR OR NOT MLX_LIBRARY)
        message(FATAL_ERROR "ARRAKIS_BUILD_TCN_TRAINER requires Apple MLX (brew install mlx-c)")
    endif()
    add_library(arrakis_tcn_core src/tcn.cpp)
    target_compile_features(arrakis_tcn_core PUBLIC cxx_std_20)
    target_include_directories(arrakis_tcn_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(arrakis_tcn_core SYSTEM PUBLIC ${MLX_INCLUDE_DIR})
    target_link_libraries(arrakis_tcn_core PUBLIC ${MLX_LIBRARY})
    set_target_properties(arrakis_tcn_core PROPERTIES BUILD_RPATH "/opt/homebrew/opt/mlx/lib")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis_tcn_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()
endif()
add_library(arrakis_model_core ${ARRAKIS_MODEL_CORE_SOURCES})

target_compile_features(arrakis_model_core PUBLIC cxx_std_20)
target_include_directories(arrakis_model_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    target_link_libraries(arrakis_model_core PUBLIC xgboost::xgboost OpenSSL::Crypto)
endif()

if(ARRAKIS_BUILD_TCN_TRAINER)
    add_executable(arrakis-train-tcn src/train_tcn.cpp)
    target_compile_features(arrakis-train-tcn PRIVATE cxx_std_20)
    target_include_directories(arrakis-train-tcn SYSTEM PRIVATE ${MLX_INCLUDE_DIR})
    target_link_libraries(arrakis-train-tcn PRIVATE arrakis_tcn_core arrakis_model_core OpenSSL::Crypto)
    set_target_properties(arrakis-train-tcn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" BUILD_RPATH "/opt/homebrew/opt/mlx/lib")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-train-tcn PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()
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

if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    add_executable(arrakis-train-xgboost src/train_xgboost.cpp)
    target_compile_features(arrakis-train-xgboost PRIVATE cxx_std_20)
    target_include_directories(
        arrakis-train-xgboost
        PRIVATE
            ${PROJECT_SOURCE_DIR}/services/news_nlp/include
    )
    target_link_libraries(arrakis-train-xgboost PRIVATE arrakis_model_core xgboost::xgboost)
    set_target_properties(arrakis-train-xgboost PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

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

    add_executable(arrakis-train-xgboost-ranker src/train_xgboost_ranker.cpp)
    target_compile_features(arrakis-train-xgboost-ranker PRIVATE cxx_std_20)
    target_link_libraries(arrakis-train-xgboost-ranker PRIVATE xgboost::xgboost)
    set_target_properties(arrakis-train-xgboost-ranker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis-train-xgboost-ranker
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()

    add_executable(arrakis-evaluate-volatility src/evaluate_volatility.cpp)
    target_compile_features(arrakis-evaluate-volatility PRIVATE cxx_std_20)
    set_target_properties(arrakis-evaluate-volatility PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis-evaluate-volatility
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()

    add_executable(arrakis-train-volatility-regression src/train_volatility_regression.cpp)
    target_compile_features(arrakis-train-volatility-regression PRIVATE cxx_std_20)
    target_include_directories(
        arrakis-train-volatility-regression
        PRIVATE
            ${PROJECT_SOURCE_DIR}/services/news_nlp/include
    )
    target_link_libraries(arrakis-train-volatility-regression PRIVATE xgboost::xgboost)
    set_target_properties(arrakis-train-volatility-regression PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis-train-volatility-regression
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()

    add_executable(arrakis-build-pooled-sector-dataset src/build_pooled_sector_dataset.cpp)
    target_compile_features(arrakis-build-pooled-sector-dataset PRIVATE cxx_std_20)
    set_target_properties(arrakis-build-pooled-sector-dataset PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis-build-pooled-sector-dataset
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()

    add_executable(arrakis-build-xlk-market-dataset src/build_xlk_market_dataset.cpp)
    target_compile_features(arrakis-build-xlk-market-dataset PRIVATE cxx_std_20)
    set_target_properties(arrakis-build-xlk-market-dataset PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-xlk-market-dataset PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-import-fnspid src/fnspid_import.cpp)
    target_compile_features(arrakis-import-fnspid PRIVATE cxx_std_20)
    target_link_libraries(arrakis-import-fnspid PRIVATE OpenSSL::Crypto arrakis_xlk_membership)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis-import-fnspid
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()

    add_executable(arrakis-import-fnspid-sectors src/fnspid_import_sectors.cpp)
    target_compile_features(arrakis-import-fnspid-sectors PRIVATE cxx_std_20)
    target_link_libraries(arrakis-import-fnspid-sectors PRIVATE OpenSSL::Crypto)
    set_target_properties(arrakis-import-fnspid-sectors PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            arrakis-import-fnspid-sectors
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Werror
        )
    endif()

    add_executable(arrakis-build-sec-filing-sector-events src/build_sec_filing_sector_events.cpp)
    target_compile_features(arrakis-build-sec-filing-sector-events PRIVATE cxx_std_20)
    target_link_libraries(arrakis-build-sec-filing-sector-events PRIVATE Boost::json OpenSSL::Crypto)
    set_target_properties(arrakis-build-sec-filing-sector-events PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-sec-filing-sector-events PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-sec-exhibit-manifest src/build_sec_exhibit_manifest.cpp)
    target_compile_features(arrakis-build-sec-exhibit-manifest PRIVATE cxx_std_20)
    set_target_properties(arrakis-build-sec-exhibit-manifest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-sec-exhibit-manifest PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-audit-sector-news-coverage src/audit_sector_news_coverage.cpp)
    target_compile_features(arrakis-audit-sector-news-coverage PRIVATE cxx_std_20)
    set_target_properties(arrakis-audit-sector-news-coverage PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-audit-sector-news-coverage PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-pooled-sector-news-control src/build_pooled_sector_news_control.cpp)
    target_compile_features(arrakis-build-pooled-sector-news-control PRIVATE cxx_std_20)
    set_target_properties(arrakis-build-pooled-sector-news-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-pooled-sector-news-control PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-pooled-sector-sec-control src/build_pooled_sector_sec_control.cpp)
    target_compile_features(arrakis-build-pooled-sector-sec-control PRIVATE cxx_std_20)
    set_target_properties(arrakis-build-pooled-sector-sec-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-pooled-sector-sec-control PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-pooled-sector-sec-finbert-control src/build_pooled_sector_sec_finbert_control.cpp)
    target_compile_features(arrakis-build-pooled-sector-sec-finbert-control PRIVATE cxx_std_20)
    set_target_properties(arrakis-build-pooled-sector-sec-finbert-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-pooled-sector-sec-finbert-control PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-pooled-sector-sec-fundamentals-control src/build_pooled_sector_sec_fundamentals_control.cpp)
    target_compile_features(arrakis-build-pooled-sector-sec-fundamentals-control PRIVATE cxx_std_20)
    target_link_libraries(arrakis-build-pooled-sector-sec-fundamentals-control PRIVATE Boost::json)
    set_target_properties(arrakis-build-pooled-sector-sec-fundamentals-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-pooled-sector-sec-fundamentals-control PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-pooled-sector-sec-insider-control src/build_pooled_sector_sec_insider_control.cpp)
    target_compile_features(arrakis-build-pooled-sector-sec-insider-control PRIVATE cxx_std_20)
    target_link_libraries(arrakis-build-pooled-sector-sec-insider-control PRIVATE Boost::json)
    set_target_properties(arrakis-build-pooled-sector-sec-insider-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-pooled-sector-sec-insider-control PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-pooled-sector-sec-embedding-control src/build_pooled_sector_sec_embedding_control.cpp)
    target_compile_features(arrakis-build-pooled-sector-sec-embedding-control PRIVATE cxx_std_20)
    set_target_properties(arrakis-build-pooled-sector-sec-embedding-control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-pooled-sector-sec-embedding-control PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-mrn-sector-articles src/build_mrn_sector_articles.cpp)
    target_compile_features(arrakis-build-mrn-sector-articles PRIVATE cxx_std_20)
    target_link_libraries(arrakis-build-mrn-sector-articles PRIVATE Boost::json OpenSSL::Crypto)
    set_target_properties(arrakis-build-mrn-sector-articles PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-mrn-sector-articles PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-yahoo-sector-articles src/build_yahoo_sector_articles.cpp)
    target_compile_features(arrakis-build-yahoo-sector-articles PRIVATE cxx_std_20)
    target_link_libraries(arrakis-build-yahoo-sector-articles PRIVATE OpenSSL::Crypto)
    set_target_properties(arrakis-build-yahoo-sector-articles PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-yahoo-sector-articles PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-build-luckycat-sector-articles src/build_luckycat_sector_articles.cpp)
    target_compile_features(arrakis-build-luckycat-sector-articles PRIVATE cxx_std_20)
    target_link_libraries(arrakis-build-luckycat-sector-articles PRIVATE Boost::json OpenSSL::Crypto)
    set_target_properties(arrakis-build-luckycat-sector-articles PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-build-luckycat-sector-articles PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-shift-news-panel src/shift_news_panel.cpp)
    target_compile_features(arrakis-shift-news-panel PRIVATE cxx_std_20)
    target_link_libraries(arrakis-shift-news-panel PRIVATE OpenSSL::Crypto)
    set_target_properties(arrakis-shift-news-panel PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-shift-news-panel PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-merge-sec-news-panels src/merge_sec_news_panels.cpp)
    target_compile_features(arrakis-merge-sec-news-panels PRIVATE cxx_std_20)
    set_target_properties(arrakis-merge-sec-news-panels PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-merge-sec-news-panels PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()

    add_executable(arrakis-sample-fnspid-timestamp-sources src/sample_fnspid_timestamp_sources.cpp)
    target_compile_features(arrakis-sample-fnspid-timestamp-sources PRIVATE cxx_std_20)
    set_target_properties(arrakis-sample-fnspid-timestamp-sources PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(arrakis-sample-fnspid-timestamp-sources PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()
endif()
