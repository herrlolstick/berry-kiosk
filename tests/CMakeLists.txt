# Exiv2 and QtGui only to write test images with metadata
qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE berrycore Qt6::Gui Qt6::Test ${BERRY_EXIV2_TARGET})
add_test(NAME tst_core COMMAND tst_core)

qt_add_executable(tst_library tst_library.cpp)
target_link_libraries(tst_library PRIVATE berrycore Qt6::Gui Qt6::Sql Qt6::Test ${BERRY_EXIV2_TARGET})
add_test(NAME tst_library COMMAND tst_library)
