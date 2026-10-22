# libberrycore: everything the display and (later) the admin server share.
# Needs no display: QtGui is used for image files only, so it works in the
# headless admin service and in tests.
qt_add_library(berrycore STATIC
    config.h config.cpp
    deviceinfo.h deviceinfo.cpp
    displayprotocol.h displayprotocol.cpp
    imagemetadata.h imagemetadata.cpp
    imagescanner.h imagescanner.cpp
    library.h library.cpp
    mediafiles.h mediafiles.cpp
    renditions.h renditions.cpp
    rendertier.h rendertier.cpp
    schedule.h schedule.cpp
    sdnotify.h sdnotify.cpp
    state.h state.cpp
)

target_include_directories(berrycore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(berrycore
    PUBLIC Qt6::Core
    PRIVATE Qt6::Gui Qt6::Sql ${BERRY_EXIV2_TARGET})
