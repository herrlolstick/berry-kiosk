qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE berrycore Qt6::Test)
add_test(NAME tst_core COMMAND tst_core)
