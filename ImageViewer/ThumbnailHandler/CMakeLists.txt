# ThumbnailHandler —— Windows Shell Thumbnail Provider COM DLL
# 仅 stb_image + GDI，不依赖 Vulkan/GLFW/ImGui
# 输出到 ${CMAKE_BINARY_DIR}（与 ImageViewer.exe 同目录）

set(THUMBNAIL_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(ImageViewerThumbnail SHARED
  ${THUMBNAIL_DIR}/thumbhandler.cpp
  ${THUMBNAIL_DIR}/thumbhandler.h
  ${THUMBNAIL_DIR}/thumbhandler.def)

set_target_properties(ImageViewerThumbnail PROPERTIES
  OUTPUT_NAME "ImageViewerThumbnail"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  PREFIX ""
  CXX_STANDARD 17)

target_include_directories(ImageViewerThumbnail PRIVATE
  ${THUMBNAIL_DIR}
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/third_party/stb)

# 系统库：ole32(shell COM), shell32(thumbcache), user32, gdi32
target_link_libraries(ImageViewerThumbnail PRIVATE
  ole32 shell32 user32 gdi32 shlwapi advapi32 msimg32)

if(WIN32)
  target_compile_options(ImageViewerThumbnail PRIVATE /utf-8 /D_CRT_SECURE_NO_WARNINGS)

  add_executable(ImageViewerShellTest EXCLUDE_FROM_ALL
    ${THUMBNAIL_DIR}/test_handle.cpp)
  target_include_directories(ImageViewerShellTest PRIVATE ${THUMBNAIL_DIR})
  target_link_libraries(ImageViewerShellTest PRIVATE ole32 user32 gdi32)
  target_compile_options(ImageViewerShellTest PRIVATE /utf-8)
endif()
