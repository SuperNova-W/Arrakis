add_library(arrakis_prediction_service_core
    src/config.cpp
    src/prediction_service.cpp
    src/serialization.cpp
)
target_compile_features(arrakis_prediction_service_core PUBLIC cxx_std_20)
target_include_directories(arrakis_prediction_service_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR})
target_link_libraries(arrakis_prediction_service_core PUBLIC arrakis_model_core arrakis_market_core)

add_executable(prediction-service src/main.cpp)
target_compile_features(prediction-service PRIVATE cxx_std_20)
target_link_libraries(prediction-service PRIVATE arrakis_prediction_service_core arrakis_market_core)
set_target_properties(prediction-service PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(arrakis_prediction_service_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_compile_options(prediction-service PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

if(BUILD_TESTING)
    add_executable(prediction_service_tests tests/prediction_service_test.cpp)
    target_compile_features(prediction_service_tests PRIVATE cxx_std_20)
    target_link_libraries(prediction_service_tests PRIVATE arrakis_prediction_service_core GTest::gtest_main)
    add_test(NAME prediction_service_tests COMMAND prediction_service_tests)
endif()
