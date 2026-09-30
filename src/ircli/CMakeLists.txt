add_library(ircli_core STATIC command.cc)

target_include_directories(ircli_core
  PUBLIC "${PROJECT_SOURCE_DIR}/include"
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(ircli_core PRIVATE ircore)
target_compile_features(ircli_core PUBLIC cxx_std_23)
set_target_properties(ircli_core PROPERTIES CXX_EXTENSIONS OFF)
enable_strict_warnings(ircli_core)

add_executable(spectrallab main.cc)
target_link_libraries(spectrallab PRIVATE ircli_core)
set_target_properties(spectrallab PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
enable_strict_warnings(spectrallab)
