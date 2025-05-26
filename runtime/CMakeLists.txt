add_library(CatsRuntime SHARED
    cats_runtime.cpp
)

option(CATS_RUNTIME_INSTALL "Install CatsRuntime library" ON)


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

if (CATS_RUNTIME_INSTALL)
    install(TARGETS CatsRuntime
        EXPORT CatsRuntimeTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cats_runtime.h
        DESTINATION include
    )
endif()

set_target_properties(CatsRuntime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "CatsRuntime"
)
