cmake_minimum_required(VERSION 3.21)

project(berry-kiosk
    VERSION 1.0.0
    DESCRIPTION "Bilderrahmen und Kiosk-Anzeige für den Raspberry Pi"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BERRY_BUILD_TESTS "Build the unit tests" ON)

# Qt 6.5 is the oldest version with qt_add_qml_module/loadFromModule as used here.
# Raspberry Pi OS based on Debian 13 (trixie) ships Qt 6.8.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Quick Sql)
# The admin web API needs the QHttpServer API of Qt 6.8 (Debian trixie: 6.8.2)
find_package(Qt6 6.8 REQUIRED COMPONENTS HttpServer)
qt_standard_project_setup(REQUIRES 6.5)

# Exiv2 reads EXIF/IPTC/XMP. Debian ships a CMake package, fall back to pkg-config.
find_package(exiv2 CONFIG QUIET)
if(TARGET Exiv2::exiv2lib)
    set(BERRY_EXIV2_TARGET Exiv2::exiv2lib)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EXIV2 REQUIRED IMPORTED_TARGET exiv2>=0.28)
    set(BERRY_EXIV2_TARGET PkgConfig::EXIV2)
endif()

add_subdirectory(core)
add_subdirectory(display)
add_subdirectory(admin)

if(BERRY_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

# Installation and the Debian package (cpack -G DEB). The units and the udev
# rule go below the prefix, which for the package is /usr.
set(BERRY_UNIT_DIR "lib/systemd/system" CACHE STRING "Where systemd units are installed, relative to the prefix")
foreach(unit berry-kiosk-display berry-kiosk-admin)
    configure_file(packaging/systemd/${unit}.service.in ${CMAKE_BINARY_DIR}/packaging/${unit}.service @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/packaging/${unit}.service DESTINATION ${BERRY_UNIT_DIR})
endforeach()
install(FILES packaging/udev/99-berry-kiosk-hotplug.rules DESTINATION lib/udev/rules.d)
install(FILES packaging/tmpfiles/berry-kiosk.conf DESTINATION lib/tmpfiles.d)
install(FILES config.example.json DESTINATION ${CMAKE_INSTALL_DATADIR}/berry-kiosk)

set(CPACK_PACKAGE_NAME berry-kiosk)
set(CPACK_PACKAGE_VENDOR herrlolstick)
set(CPACK_PACKAGE_CONTACT "herrlolstick <125745158+herrlolstick@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Photo frame and kiosk display for the Raspberry Pi")
set(CPACK_PACKAGE_DESCRIPTION "Shows nature photography full screen with widgets (clock, image info, quotes), grid layouts, 4K with Full HD fallback, a schedule and a web UI for uploads and layouts.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/herrlolstick/berry-kiosk")
set(CPACK_GENERATOR DEB)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION misc)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Plugins and QML modules are loaded at runtime, so dpkg-shlibdeps cannot see them
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qml6-module-qtquick, qml6-module-qtquick-window, qml6-module-qtqml-workerscript, qt6-qpa-plugins, qt6-image-formats-plugins, libqt6sql6-sqlite, fonts-dejavu-core, passwd")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "v4l-utils")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "ddcutil")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    ${CMAKE_SOURCE_DIR}/packaging/debian/postinst
    ${CMAKE_SOURCE_DIR}/packaging/debian/prerm
    ${CMAKE_SOURCE_DIR}/packaging/debian/postrm)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
include(CPack)
