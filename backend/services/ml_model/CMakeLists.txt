set(ARRAKIS_MODEL_CORE_SOURCES
    src/dataset.cpp
    src/metrics.cpp
)
if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    list(APPEND ARRAKIS_MODEL_CORE_SOURCES src/sector_ml.cpp)
endif()
add_library(arrakis_model_core ${ARRAKIS_MODEL_CORE_SOURCES})

target_compile_features(arrakis_model_core PUBLIC cxx_std_20)
target_include_directories(arrakis_model_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(ARRAKIS_BUILD_XGBOOST_TRAINER)
    target_link_libraries(arrakis_model_core PUBLIC xgboost::xgboost OpenSSL::Crypto)
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

    add_executable(arrakis-import-fnspid src/fnspid_import.cpp)
    target_compile_features(arrakis-import-fnspid PRIVATE cxx_std_20)
    target_link_libraries(arrakis-import-fnspid PRIVATE OpenSSL::Crypto)
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
endif()
