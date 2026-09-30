add_library(test_support STATIC support/synthetic.cc)
target_include_directories(test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test_support PUBLIC ircore)
enable_strict_warnings(test_support)

# Core tests may use internal headers to test building blocks directly.
add_executable(ircore_test
  analysis_test.cc
  color_test.cc
  demosaic_test.cc
  dng_test.cc
  ljpeg_test.cc
  normalization_test.cc
  processing_test.cc
  tiff_test.cc
)
target_include_directories(ircore_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ircore_test PRIVATE ircore test_support GTest::gtest_main)
enable_strict_warnings(ircore_test)
gtest_discover_tests(ircore_test)

add_executable(ircli_test cli_test.cc)
target_include_directories(ircli_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ircli_test PRIVATE ircli_core ircore test_support GTest::gtest_main)
enable_strict_warnings(ircli_test)
gtest_discover_tests(ircli_test)
