add_library(ircore STATIC
  analysis.cc
  color.cc
  demosaic.cc
  dng_reader.cc
  dng_writer.cc
  error.cc
  image.cc
  ljpeg.cc
  options.cc
  preview.cc
  processing.cc
  tiff.cc
  version.cc
)

target_include_directories(ircore
  PUBLIC "${PROJECT_SOURCE_DIR}/include"
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(ircore PRIVATE Threads::Threads)
target_compile_definitions(ircore PRIVATE SPECTRAL_LAB_VERSION="${PROJECT_VERSION}")
target_compile_features(ircore PUBLIC cxx_std_23)
set_target_properties(ircore PROPERTIES CXX_EXTENSIONS OFF)
enable_strict_warnings(ircore)
