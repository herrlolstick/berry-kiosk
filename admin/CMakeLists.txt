# berry-kiosk-admin: keeps the media library up to date and prepares the
# renditions. Milestone 4 adds the web API to this process.
qt_add_executable(berry-kiosk-admin
    main.cpp
    changewatcher.h changewatcher.cpp
    libraryworker.h libraryworker.cpp
)

target_compile_definitions(berry-kiosk-admin PRIVATE BERRY_KIOSK_VERSION="${PROJECT_VERSION}")
target_link_libraries(berry-kiosk-admin PRIVATE berrycore Qt6::Core)

install(TARGETS berry-kiosk-admin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
