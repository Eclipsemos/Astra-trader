cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0167 NEW)

project(astra_trader VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(astra_core
    src/audit_writer.cpp
    src/binance_public_feed.cpp
    src/exchange_clock.cpp
    src/feature_builder.cpp
    src/laya_gateway.cpp
    src/ledger.cpp
    src/model_worker.cpp
    src/paper_exchange.cpp
    src/risk.cpp
    src/status_server.cpp
)
add_library(astra::core ALIAS astra_core)
target_compile_features(astra_core PUBLIC cxx_std_23)
target_include_directories(astra_core PUBLIC include)
target_compile_definitions(astra_core PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(astra_core PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

function(astra_strict target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror
        )
        if(CMAKE_CXX_FLAGS MATCHES "fsanitize=thread")
            # GCC 14 diagnoses Boost.Asio's required fences under TSan; retain the
            # instrumentation while allowing that toolchain warning to be emitted.
            target_compile_options(${target} PRIVATE -Wno-error=tsan)
        endif()
    endif()
endfunction()

astra_strict(astra_core)

add_executable(astra_trader apps/astra_trader/main.cpp)
target_link_libraries(astra_trader PRIVATE astra::core)
astra_strict(astra_trader)

add_executable(astra_replay apps/astra_replay/main.cpp)
target_link_libraries(astra_replay PRIVATE astra::core)
astra_strict(astra_replay)

if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_executable(astra_core_tests
        tests/audit_writer_tests.cpp
        tests/exchange_clock_tests.cpp
        tests/ledger_tests.cpp
        tests/laya_gateway_tests.cpp
        tests/model_worker_tests.cpp
        tests/paper_exchange_tests.cpp
        tests/risk_tests.cpp
        tests/status_server_tests.cpp
    )
    target_link_libraries(astra_core_tests PRIVATE astra::core Catch2::Catch2WithMain)
    astra_strict(astra_core_tests)
    include(Catch)
    catch_discover_tests(astra_core_tests)
    add_test(
        NAME astra_replay_fixture
        COMMAND astra_replay
            --input ${CMAKE_SOURCE_DIR}/tests/fixtures/replay_sample.csv
            --output ${CMAKE_CURRENT_BINARY_DIR}/replay_fixture.json
            --model baseline
            --decision-every 1
    )
endif()
