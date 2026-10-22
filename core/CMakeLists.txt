# libberrycore: everything the display and (later) the admin server share.
# Depends on QtCore only so it stays usable in headless tools and tests.
qt_add_library(berrycore STATIC
    config.h config.cpp
    deviceinfo.h deviceinfo.cpp
    imagescanner.h imagescanner.cpp
    rendertier.h rendertier.cpp
)

target_include_directories(berrycore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(berrycore PUBLIC Qt6::Core)
