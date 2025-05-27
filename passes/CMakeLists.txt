file(GLOB_RECURSE CATS_PASS_SOURCES "*.cpp")
add_library(CatsPass MODULE ${CATS_PASS_SOURCES})

option(CATS_PASSES_INSTALL "Install CatsPass library" ON)

if (CATS_PASSES_INSTALL)
    install(TARGETS CatsPass
        EXPORT CatsPassTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()
