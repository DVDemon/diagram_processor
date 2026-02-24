cmake_minimum_required(VERSION 3.16)
project(poco_template_server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(Poco REQUIRED COMPONENTS Net NetSSL Foundation Util Prometheus JSON)
find_package(ZLIB REQUIRED)

add_executable(poco_template_server
    src/main.cpp
)

target_include_directories(poco_template_server PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(poco_template_server
    PRIVATE
        Poco::Net
        Poco::NetSSL
        Poco::Foundation
        Poco::Util
        Poco::Prometheus
        Poco::JSON
        ZLIB::ZLIB
)

install(TARGETS poco_template_server DESTINATION bin)

# Unit tests with Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(parser_tests test/parser_test.cpp)
target_include_directories(parser_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(parser_tests PRIVATE TEST_FIXTURES_PATH="${CMAKE_SOURCE_DIR}/test/fixtures")
target_link_libraries(parser_tests
    PRIVATE
        GTest::gtest_main
        Poco::Foundation
        Poco::JSON
        ZLIB::ZLIB
)
add_test(NAME ParserTests COMMAND parser_tests)
