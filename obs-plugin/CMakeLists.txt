set(REVO_TOASTS_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/obs-plugins/64bit" CACHE PATH "Directory where revo-toasts.dll is written")
set(REVO_TOASTS_LOCALE_DIR "${CMAKE_BINARY_DIR}/data/obs-plugins/revo-toasts/locale" CACHE PATH "Directory where the plugin locale is written")

add_library(revo_toasts MODULE
  revo-toasts.cpp
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
  RUNTIME_OUTPUT_DIRECTORY "${REVO_TOASTS_PLUGIN_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${REVO_TOASTS_PLUGIN_OUTPUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${REVO_TOASTS_PLUGIN_OUTPUT_DIR}"
)

add_custom_command(TARGET revo_toasts POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${REVO_TOASTS_LOCALE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/locale/en-US.ini"
          "${REVO_TOASTS_LOCALE_DIR}/en-US.ini"
)
