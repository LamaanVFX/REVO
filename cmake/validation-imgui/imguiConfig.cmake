# Local validation adapter for the pinned upstream ImGui sources.
if(NOT TARGET imgui::imgui)
  add_library(revo_validation_imgui STATIC
    "${REVO_IMGUI_SOURCE}/imgui.cpp"
    "${REVO_IMGUI_SOURCE}/imgui_draw.cpp"
    "${REVO_IMGUI_SOURCE}/imgui_tables.cpp"
    "${REVO_IMGUI_SOURCE}/imgui_widgets.cpp"
    "${REVO_IMGUI_SOURCE}/backends/imgui_impl_dx11.cpp"
    "${REVO_IMGUI_SOURCE}/backends/imgui_impl_win32.cpp")
  target_include_directories(revo_validation_imgui PUBLIC
    "${REVO_IMGUI_SOURCE}" "${REVO_IMGUI_SOURCE}/backends")
  target_link_libraries(revo_validation_imgui PUBLIC d3dcompiler dwmapi)
  add_library(imgui::imgui ALIAS revo_validation_imgui)
endif()
