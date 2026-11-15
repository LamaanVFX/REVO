add_library(revo_toasts MODULE
  revo-toasts.cpp
)

target_include_directories(revo_toasts PRIVATE
  "\${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(revo_toasts PRIVATE
  _WIN32_WINNT=0x0A00
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

target_link_libraries(revo_toasts PRIVATE
  OBS::libobs
  gdiplus
)

if(MSVC)
  target_compile_options(revo_toasts PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
else()
  target_compile_options(revo_toasts PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(revo_toasts PROPERTIES
  PREFIX ""
  OUTPUT_NAME "revo-toasts"
  RUNTIME_OUTPUT_DIRECTORY "\${CMAKE_BINARY_DIR}/obs-plugins/64bit"
  LIBRARY_OUTPUT_DIRECTORY "\${CMAKE_BINARY_DIR}/obs-plugins/64bit"
  ARCHIVE_OUTPUT_DIRECTORY "\${CMAKE_BINARY_DIR}/obs-plugins/64bit"
)

add_custom_command(TARGET revo_toasts POST_BUILD
  COMMAND "\${CMAKE_COMMAND}" -E make_directory
          "\${CMAKE_BINARY_DIR}/data/obs-plugins/revo-toasts/locale"
  COMMAND "\${CMAKE_COMMAND}" -E copy_if_different
          "\${CMAKE_CURRENT_SOURCE_DIR}/locale/en-US.ini"
          "\${CMAKE_BINARY_DIR}/data/obs-plugins/revo-toasts/locale/en-US.ini"
)
