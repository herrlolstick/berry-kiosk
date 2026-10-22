qt_add_executable(berry-kiosk-display
    main.cpp
    kioskcontroller.h kioskcontroller.cpp
    mediaimageprovider.h mediaimageprovider.cpp
    mediasource.h mediasource.cpp
    metadatareader.h metadatareader.cpp
)

set(berry_qml_files
    qml/Main.qml
    qml/LayoutView.qml
    qml/SlideshowRegion.qml
    qml/DebugOverlay.qml
    qml/WidgetPanel.qml
    qml/ClockWidget.qml
    qml/ImageInfoWidget.qml
    qml/QuoteWidget.qml
)
# Serve the QML files from the module root instead of a qml/ sub folder
foreach(file IN LISTS berry_qml_files)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()

qt_add_qml_module(berry-kiosk-display
    URI BerryKiosk
    VERSION 1.0
    QML_FILES ${berry_qml_files}
)

target_compile_definitions(berry-kiosk-display PRIVATE BERRY_KIOSK_VERSION="${PROJECT_VERSION}")
target_link_libraries(berry-kiosk-display PRIVATE berrycore Qt6::Gui Qt6::Quick)

install(TARGETS berry-kiosk-display RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
