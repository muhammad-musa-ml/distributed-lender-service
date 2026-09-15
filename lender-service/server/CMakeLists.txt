# Build for the C++ Lender gRPC server.
#
# WHY CMAKE HERE AND PLAIN MAKE IN ../lowlat. The sibling project's Makefile explains its
# own choice: four self-contained translation units with no dependency beyond libstdc++
# and pthreads, so a build system would have been the heaviest file in the project. That
# reasoning inverts here. This target links grpc++, protobuf, arrow, parquet, libcurl and
# libmysqlclient, and it has to run protoc AND the gRPC codegen plugin as build steps with
# correct dependency tracking. That is exactly the job CMake does better than a hand-rolled
# Makefile, so the same trade-off lands on the other side.
#
#   cmake -S . -B build && cmake --build build -j
#   ./build/lender_tests        # unit tests, no cluster required
#   ./build/lender_server       # needs mysql + hdfs reachable

cmake_minimum_required(VERSION 3.22)
project(lender_server CXX)

set(CMAKE_CXX_STANDARD 20)          # NOT 17: arrow/buffer.h uses std::span (see
set(CMAKE_CXX_STANDARD_REQUIRED ON) # PREREQUISITES-VERIFIED.md, fact 1).
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The same warning set the sibling lowlat/ project builds clean under. Applied ONLY to our
# own sources -- protoc and grpc_cpp_plugin emit code that does not survive -Wconversion,
# and silencing our warnings to accommodate generated code would defeat the point.
set(LENDER_WARNINGS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP   REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(ARROW    REQUIRED IMPORTED_TARGET arrow)
pkg_check_modules(PARQUET  REQUIRED IMPORTED_TARGET parquet)
pkg_check_modules(CURLPP   REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(MYSQLCL  REQUIRED IMPORTED_TARGET mysqlclient)

find_program(PROTOC_EXE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXE grpc_cpp_plugin REQUIRED)

# ---------------------------------------------------------------------------------------
# Generated protobuf / gRPC stubs.
#
# The proto is the project's, not ours -- it lives at p4/lender.proto and is the contract
# the Python client in p4/client.py already speaks. Overridable so the server image can
# copy it next to the sources instead of reaching two directories up.
# ---------------------------------------------------------------------------------------
set(LENDER_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/../lender.proto"
    CACHE FILEPATH "Path to lender.proto")
if(NOT EXISTS "${LENDER_PROTO}")
  message(FATAL_ERROR "lender.proto not found at ${LENDER_PROTO} -- pass -DLENDER_PROTO=<path>")
endif()
get_filename_component(LENDER_PROTO_DIR "${LENDER_PROTO}" DIRECTORY)

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")

set(GEN_SRCS
    "${GEN_DIR}/lender.pb.cc"
    "${GEN_DIR}/lender.pb.h"
    "${GEN_DIR}/lender.grpc.pb.cc"
    "${GEN_DIR}/lender.grpc.pb.h")

add_custom_command(
    OUTPUT ${GEN_SRCS}
    COMMAND "${PROTOC_EXE}"
            --proto_path "${LENDER_PROTO_DIR}"
            --cpp_out "${GEN_DIR}"
            --grpc_out "${GEN_DIR}"
            # No quotes around the plugin path: this is ONE argument token, and under
            # VERBATIM any quotes written here are passed through to protoc as literal
            # characters -- protoc then looks for a file whose name begins with '"' and
            # reports it as 'program not found', which reads like a missing plugin.
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXE}
            "${LENDER_PROTO}"
    DEPENDS "${LENDER_PROTO}"
    COMMENT "protoc + grpc_cpp_plugin -> ${GEN_DIR}"
    VERBATIM)

add_library(lender_proto STATIC "${GEN_DIR}/lender.pb.cc" "${GEN_DIR}/lender.grpc.pb.cc")
target_include_directories(lender_proto PUBLIC "${GEN_DIR}")
target_link_libraries(lender_proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)
set_target_properties(lender_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------------------
# Our code. Split into a library so the unit tests link the same objects the server runs.
# ---------------------------------------------------------------------------------------
add_library(lender_core STATIC
    src/config.cpp
    src/webhdfs.cpp
    src/block_locations.cpp
    src/parquet_util.cpp
    src/mysql_source.cpp
    src/cache_policy.cpp
    src/lender_service.cpp)
target_include_directories(lender_core PUBLIC src)
target_compile_options(lender_core PRIVATE ${LENDER_WARNINGS})
target_link_libraries(lender_core PUBLIC
    lender_proto
    PkgConfig::ARROW
    PkgConfig::PARQUET
    PkgConfig::CURLPP
    PkgConfig::MYSQLCL)

add_executable(lender_server src/main.cpp)
target_compile_options(lender_server PRIVATE ${LENDER_WARNINGS})
target_link_libraries(lender_server PRIVATE lender_core)

# A native client, so the end-to-end check needs no Python image and no second codegen path.
add_executable(lender_client src/client_main.cpp)
target_compile_options(lender_client PRIVATE ${LENDER_WARNINGS})
target_link_libraries(lender_client PRIVATE lender_proto)

add_executable(lender_tests
    tests/test_main.cpp
    tests/test_block_locations.cpp
    tests/test_parquet_util.cpp
    tests/test_cache_policy.cpp
    tests/test_config.cpp)
target_compile_options(lender_tests PRIVATE ${LENDER_WARNINGS})
target_link_libraries(lender_tests PRIVATE lender_core)

enable_testing()
add_test(NAME unit COMMAND lender_tests)
