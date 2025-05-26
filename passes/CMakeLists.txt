add_library(CatsPass MODULE
    cats_pass_utils.cpp
    allocation_tracker.cpp
    load_store_tracker.cpp
    function_scope_tracker.cpp
    cats_passes.cpp
)

option(CATS_PASSES_INSTALL "Install CatsPass library" ON)

if (CATS_PASSES_INSTALL)
    install(TARGETS CatsPass
        EXPORT CatsPassTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()
