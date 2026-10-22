# berry-kiosk-admin: keeps the media library up to date, prepares the
# renditions and serves the web API and admin UI.
qt_add_library(berryadmin STATIC
    apiserver.h apiserver.cpp
    auth.h auth.cpp
    changewatcher.h changewatcher.cpp
    displaylink.h displaylink.cpp
    libraryworker.h libraryworker.cpp
)
target_include_directories(berryadmin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(berryadmin PUBLIC BERRY_KIOSK_VERSION="${PROJECT_VERSION}")
target_link_libraries(berryadmin PUBLIC berrycore Qt6::Core Qt6::Gui Qt6::Network Qt6::HttpServer)

# The web UI is compiled in, so there is nothing to install or to get out of
# sync. Plain ES modules, no build step: the Pi needs no Node.js.
file(GLOB_RECURSE berry_www_files RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS
    www/*.html www/*.js www/*.css www/*.svg www/*.png)
qt_add_resources(berryadmin "www"
    PREFIX "/"
    FILES ${berry_www_files}
)

qt_add_executable(berry-kiosk-admin main.cpp)
target_link_libraries(berry-kiosk-admin PRIVATE berryadmin)

install(TARGETS berry-kiosk-admin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
