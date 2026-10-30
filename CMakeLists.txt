cmake_minimum_required(VERSION 3.16)

project(EastapeHFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(EHFS_BUILD_TESTS "Build and register engine tests" ON)

add_subdirectory(engine)
add_subdirectory(tools/ehfs-cli)

if(EHFS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
