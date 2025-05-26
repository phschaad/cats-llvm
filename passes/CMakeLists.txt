add_library(CatsPass MODULE
    catspass.cpp
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
