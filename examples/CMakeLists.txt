cmake_minimum_required(VERSION 3.10)
project(cats_examples CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set absolute paths for runtime and pass plugins
if (NOT DEFINED CATS_RUNTIME_DIR)
    set(CATS_RUNTIME_DIR "${CMAKE_INSTALL_PREFIX}/lib")
endif()
set(CATS_RUNTIME_LIB "${CATS_RUNTIME_DIR}/libCatsRuntime.so")
if (NOT EXISTS "${CATS_RUNTIME_LIB}")
    message(FATAL_ERROR "CATS_RUNTIME_LIB does not exist. Please check the path: ${CATS_RUNTIME_LIB}")
endif()
if (NOT DEFINED PASS_PLUGIN_DIR)
    set (PASS_PLUGIN_DIR "${CMAKE_INSTALL_PREFIX}/lib")
endif()
set(PASS_PLUGIN_SO "${PASS_PLUGIN_DIR}/libCatsPass.so")
if (NOT EXISTS "${PASS_PLUGIN_SO}")
    message(FATAL_ERROR "PASS_PLUGIN_SO does not exist. Please check the path: ${PASS_PLUGIN_SO}")
endif()

# List of example targets
set(EXAMPLES gemm)

foreach(example ${EXAMPLES})
    set(src_file "${CMAKE_CURRENT_SOURCE_DIR}/${example}.cpp")
    set(bc_file "${CMAKE_CURRENT_BINARY_DIR}/${example}.bc")
    set(ll_file "${CMAKE_CURRENT_BINARY_DIR}/${example}.ll")
    set(bin_file "${CMAKE_CURRENT_BINARY_DIR}/${example}")

    # Step 1: Compile to LLVM bitcode
    add_custom_command(
        OUTPUT ${bc_file}
        COMMAND clang++ -Wall -Wextra -O3 -g -c -emit-llvm -o ${bc_file} ${src_file}
        DEPENDS ${src_file}
        COMMENT "Compiling ${src_file} to LLVM bitcode"
    )

    # Step 2: Run opt with pass plugins
    add_custom_command(
        OUTPUT ${ll_file}
        COMMAND opt -S -load-pass-plugin=${PASS_PLUGIN_SO} -passes=cats-allocation-tracker,cats-load-store-tracker,cats-function-scope-tracker -o ${ll_file} ${bc_file}
        DEPENDS ${bc_file} ${PASS_PLUGIN_SO}
        COMMENT "Running opt passes on ${bc_file}"
    )

    # Step 3: Link final executable
    add_custom_command(
        OUTPUT ${bin_file}
        COMMAND clang++ -g -o ${bin_file} ${ll_file} -L${CATS_RUNTIME_DIR} -lCatsRuntime -lpthread -ldl
        DEPENDS ${ll_file} ${CATS_RUNTIME_LIB}
        COMMENT "Linking ${bin_file}"
    )

    add_custom_target(${example}_build ALL DEPENDS ${bin_file})
endforeach()

add_custom_target(clean-examples
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Cleaning example build directory"
)
