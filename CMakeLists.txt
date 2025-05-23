cmake_minimum_required(VERSION 3.10)
project(cats-llvm)

find_package(LLVM REQUIRED CONFIG)

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})
if (${LLVM_VERSION_MAJOR} VERSION_GREATER_EQUAL 20)
    set(CMAKE_CXX_STANDARD 17)
elseif (${LLVM_VERSION_MAJOR} VERSION_GREATER_EQUAL 20)
    set(CMAKE_CXX_STANDARD 14)
endif ()

if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

add_subdirectory(passes)
add_subdirectory(runtime)
