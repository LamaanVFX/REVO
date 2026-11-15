cmake_minimum_required(VERSION 3.24)
project(REVOToastNotifications LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OBS_ROOT "" CACHE PATH "OBS Studio 33.0.1 Windows SDK/runtime root")
if(NOT EXISTS "${OBS_ROOT}/bin/64bit/obs.dll" OR
   NOT EXISTS "${OBS_ROOT}/include/libobs/obs.h" OR
   NOT EXISTS "${OBS_ROOT}/lib/libobs.dll.a")
  message(FATAL_ERROR "OBS_ROOT must contain bin/64bit/obs.dll, include/libobs and lib/libobs.dll.a")
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/obsconfig.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/obs-include/obsconfig.h"
  @ONLY
)

add_library(OBS::libobs SHARED IMPORTED)
set_target_properties(OBS::libobs PROPERTIES
  IMPORTED_IMPLIB "${OBS_ROOT}/lib/libobs.dll.a"
  IMPORTED_LOCATION "${OBS_ROOT}/bin/64bit/obs.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${OBS_ROOT}/include/libobs;${CMAKE_CURRENT_BINARY_DIR}/obs-include"
)

set(REVO_TOASTS_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/dist/revo-toasts" CACHE PATH
    "Directory where the OBS 33 plugin DLL is written")
set(REVO_TOASTS_LOCALE_DIR "${REVO_TOASTS_PLUGIN_OUTPUT_DIR}/data/locale" CACHE PATH
    "Directory where the OBS 33 plugin locale is written")

add_subdirectory(".." "${CMAKE_CURRENT_BINARY_DIR}/plugin")
