cmake_minimum_required(VERSION 3.10)
project(ha_duckdb)

# Paths to MariaDB source and build trees - override these at cmake time:
#   cmake -DMARIADB_SOURCE_DIR=... -DMARIADB_BUILD_DIR=...
set(MARIADB_SOURCE_DIR "/home/erik/shared/mariadb/mariadb-11.8.3-git"
    CACHE PATH "Path to MariaDB source tree")
set(MARIADB_BUILD_DIR  "${MARIADB_SOURCE_DIR}/build"
    CACHE PATH "Path to MariaDB build tree (where cmake was run)")

set(DUCKDB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib"
    CACHE PATH "Path to directory containing libduckdb.so and duckdb.hpp")

# Validate required paths
foreach(p MARIADB_SOURCE_DIR MARIADB_BUILD_DIR DUCKDB_DIR)
  if(NOT EXISTS ${${p}})
    message(FATAL_ERROR "${p} not found: ${${p}}")
  endif()
endforeach()

set(CMAKE_CXX_STANDARD 11)

# Match flags from MariaDB's own example storage engine build
add_definitions(
  -DHAVE_CONFIG_H
  -DMYSQL_DYNAMIC_PLUGIN
  -D_FILE_OFFSET_BITS=64
  -D_GLIBCXX_USE_CXX11_ABI=0   # Match libduckdb.so which uses old ABI
)

include_directories(
  ${MARIADB_SOURCE_DIR}/wsrep-lib/include
  ${MARIADB_SOURCE_DIR}/wsrep-lib/wsrep-API/v26
  ${MARIADB_BUILD_DIR}/include          # generated: my_config.h, mysql_version.h
  ${MARIADB_SOURCE_DIR}/include/providers
  ${MARIADB_SOURCE_DIR}/include
  ${MARIADB_SOURCE_DIR}/sql
  ${MARIADB_BUILD_DIR}/sql
  ${DUCKDB_DIR}                         # duckdb.hpp, duckdb.h
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
  -fPIC -fstack-protector --param=ssp-buffer-size=4 \
  -O2 -g -DNDEBUG -fno-omit-frame-pointer -D_FORTIFY_SOURCE=2 -DDBUG_OFF \
  -Wall -Wenum-compare -Wextra -Wformat-security -Wmissing-braces \
  -Wno-format-truncation -Wno-init-self -Wno-nonnull-compare \
  -Wno-unused-parameter -Wnon-virtual-dtor -Woverloaded-virtual \
  -Wvla -Wwrite-strings")

add_library(ha_duckdb MODULE ha_duckdb.cc)

target_link_libraries(ha_duckdb ${DUCKDB_DIR}/libduckdb.so)

# Show where the .so ends up
message(STATUS "Plugin will be built at: ${CMAKE_CURRENT_BINARY_DIR}/ha_duckdb.so")
message(STATUS "MariaDB source: ${MARIADB_SOURCE_DIR}")
message(STATUS "MariaDB build:  ${MARIADB_BUILD_DIR}")
message(STATUS "DuckDB dir:     ${DUCKDB_DIR}")
