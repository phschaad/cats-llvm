add_library(CatsRuntime SHARED
    cats_runtime.cpp
)

target_include_directories(CatsRuntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(CatsRuntime PUBLIC
    cxx_std_11
)

# Set visibility for LLVM ABI compatibility
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(CatsRuntime PRIVATE
        -fvisibility=hidden
    )
endif()

set_target_properties(CatsRuntime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "CatsRuntime"
)
