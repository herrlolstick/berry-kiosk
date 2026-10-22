qt_add_executable(tst_core tst_core.cpp)
# Exiv2 and QtGui only to write test images with metadata
target_link_libraries(tst_core PRIVATE berrycore Qt6::Gui Qt6::Test ${BERRY_EXIV2_TARGET})
add_test(NAME tst_core COMMAND tst_core)
