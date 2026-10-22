# Exiv2 and QtGui only to write test images with metadata
qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE berrycore Qt6::Gui Qt6::Test ${BERRY_EXIV2_TARGET})
add_test(NAME tst_core COMMAND tst_core)

qt_add_executable(tst_library tst_library.cpp)
target_link_libraries(tst_library PRIVATE berrycore Qt6::Gui Qt6::Sql Qt6::Test ${BERRY_EXIV2_TARGET})
add_test(NAME tst_library COMMAND tst_library)

# Built from the admin sources directly, without the web server parts
qt_add_executable(tst_admin tst_admin.cpp ../admin/auth.cpp ../admin/displaylink.cpp)
target_include_directories(tst_admin PRIVATE ${CMAKE_SOURCE_DIR}/admin)
target_link_libraries(tst_admin PRIVATE berrycore Qt6::Network Qt6::Test)
add_test(NAME tst_admin COMMAND tst_admin)

# The real HTTP API on a random local port
qt_add_executable(tst_api tst_api.cpp)
target_link_libraries(tst_api PRIVATE berryadmin Qt6::Network Qt6::Test ${BERRY_EXIV2_TARGET})
add_test(NAME tst_api COMMAND tst_api)
