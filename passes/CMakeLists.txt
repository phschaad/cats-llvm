add_library(CatsPass MODULE
    cats_passes.cpp
    cats_pass_utils.cpp
    allocation_tracker.cpp
    load_store_tracker.cpp
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
