cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME aproql)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flag
add_compile_options(-O2)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/aproql_extension.cpp
    src/sampler.cpp
    src/algorithms.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# Standalone benchmark executable
add_executable(aproql_benchmark
    src/benchmark.cpp
    src/sampler.cpp
    src/algorithms.cpp
)
target_include_directories(aproql_benchmark PRIVATE src/include)
target_link_libraries(aproql_benchmark duckdb)
target_compile_features(aproql_benchmark PRIVATE cxx_std_17)
