set(OBS_ROOT "" CACHE PATH "Pinned minimal OBS 32.1.2 SDK/runtime from tools/prepare-obs.ps1")
if(NOT EXISTS "${OBS_ROOT}/pin.json")
  message(FATAL_ERROR "Prepare OBS with tools/prepare-obs.ps1 and set OBS_ROOT.")
endif()
file(READ "${OBS_ROOT}/pin.json" obs_pin)
string(JSON obs_version GET "${obs_pin}" version)
if(NOT obs_version STREQUAL "32.1.2")
  message(FATAL_ERROR "Only the pinned OBS 32.1.2 runtime is supported.")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/obsconfig.h.in" "${CMAKE_CURRENT_BINARY_DIR}/obs-include/obsconfig.h" @ONLY)
add_library(OBS::libobs SHARED IMPORTED)
set_target_properties(OBS::libobs PROPERTIES
  IMPORTED_IMPLIB "${OBS_ROOT}/lib/libobs.dll.a"
  IMPORTED_LOCATION "${OBS_ROOT}/bin/64bit/obs.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${OBS_ROOT}/include/libobs;${CMAKE_CURRENT_BINARY_DIR}/obs-include")
target_compile_definitions(revo_core PUBLIC REVO_USE_LIBOBS)
add_library(revo_media STATIC
  src/obs/obs_engine.cpp src/core/error.cpp src/core/logger.cpp
  src/graphics/d3d_device.cpp src/platform/hotkeys.cpp src/platform/user_paths.cpp
  src/platform/win32_window.cpp src/ui/imgui_host.cpp src/ui/main_panel.cpp)
target_include_directories(revo_media PUBLIC include)
target_compile_definitions(revo_media PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN WINRT_LEAN_AND_MEAN UNICODE _UNICODE NOMINMAX)
target_link_libraries(revo_media PUBLIC revo_core PRIVATE OBS::libobs imgui::imgui
  d3d11 dxgi dwmapi gdi32 imm32 ole32 shell32 uuid user32 windowscodecs windowsapp runtimeobject psapi)
revo_project_warnings(revo_media)
if(REVO_ENABLE_TEST_HOOKS)
  target_compile_definitions(revo_media PUBLIC REVO_ENABLE_TEST_HOOKS)
endif()
add_executable(revo_app WIN32 src/app/application_obs.cpp src/main.cpp)
set(REVO_ICON_RESOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/revo.ico")
configure_file(packaging/revo.rc.in "${CMAKE_CURRENT_BINARY_DIR}/revo.rc" @ONLY)
target_sources(revo_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/revo.rc")
set_target_properties(revo_app PROPERTIES OUTPUT_NAME REVO RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/64bit")
target_link_libraries(revo_app PRIVATE revo_media imgui::imgui)
if(MINGW)
  target_link_options(revo_app PRIVATE -municode)
endif()
revo_project_warnings(revo_app)
if(REVO_BUILD_TESTS)
  add_executable(revo_output_reservation_tests tests/output_reservation_tests.cpp)
  target_include_directories(revo_output_reservation_tests PRIVATE include)
  target_compile_definitions(revo_output_reservation_tests PRIVATE UNICODE _UNICODE NOMINMAX)
  revo_project_warnings(revo_output_reservation_tests)
  add_test(NAME revo_output_reservation_tests COMMAND revo_output_reservation_tests)
  set_tests_properties(revo_output_reservation_tests PROPERTIES TIMEOUT 30)
  add_executable(revo_event_driven_tabs_tests tests/event_driven_tabs_tests.cpp)
  target_include_directories(revo_event_driven_tabs_tests PRIVATE include)
  target_link_libraries(revo_event_driven_tabs_tests PRIVATE imgui::imgui user32 imm32)
  revo_project_warnings(revo_event_driven_tabs_tests)
  add_test(NAME revo_event_driven_tabs_tests COMMAND revo_event_driven_tabs_tests)
  set_tests_properties(revo_event_driven_tabs_tests PROPERTIES TIMEOUT 30)
endif()
add_custom_command(TARGET revo_app POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${OBS_ROOT}/bin" "${CMAKE_CURRENT_BINARY_DIR}/bin"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${OBS_ROOT}/obs-plugins" "${CMAKE_CURRENT_BINARY_DIR}/obs-plugins"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${OBS_ROOT}/data" "${CMAKE_CURRENT_BINARY_DIR}/data"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${OBS_ROOT}/licenses" "${CMAKE_CURRENT_BINARY_DIR}/licenses"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OBS_ROOT}/pin.json" "${CMAKE_CURRENT_BINARY_DIR}/obs-pin.json"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:revo_app>/fonts")
