cmake_minimum_required(VERSION 3.8)

project(ImageViewer)
set(CMAKE_CXX_STANDARD 17)

# Vulkan SDK 未注册到环境变量时，在常见安装位置回退查找（取版本号最大的一个）
if(NOT DEFINED ENV{VULKAN_SDK})
  file(GLOB VULKAN_SDK_ROOTS LIST_DIRECTORIES true
    "C:/VulkanSDK/*" "D:/VulkanSDK/*" "E:/VulkanSDK/*" "E:/Software/VulkanSDK/*")
  if(VULKAN_SDK_ROOTS)
    list(SORT VULKAN_SDK_ROOTS)
    list(REVERSE VULKAN_SDK_ROOTS)
    list(GET VULKAN_SDK_ROOTS 0 VULKAN_SDK_LOCAL)
    set(ENV{VULKAN_SDK} "${VULKAN_SDK_LOCAL}")
    message(STATUS "VULKAN_SDK env not set, falling back to ${VULKAN_SDK_LOCAL}")
  endif()
endif()

find_package(Vulkan REQUIRED)

# glslangValidator：编译 GLSL -> SPIR-V，输出到构建目录下 shaders/（与可执行文件同目录）
add_executable(glslang::validator IMPORTED)
find_program(GLSLANG_VALIDATOR "glslangValidator" HINTS $ENV{VULKAN_SDK}/bin REQUIRED)
set_property(TARGET glslang::validator PROPERTY IMPORTED_LOCATION "${GLSLANG_VALIDATOR}")

set(SHADERS_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADERS_DIR})

add_custom_command(
  OUTPUT ${SHADERS_DIR}/frag.spv ${SHADERS_DIR}/vert.spv
  COMMAND glslang::validator
  ARGS --target-env vulkan1.0 ${CMAKE_SOURCE_DIR}/shaders/viewer.vert ${CMAKE_SOURCE_DIR}/shaders/viewer.frag --quiet
  WORKING_DIRECTORY ${SHADERS_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/viewer.vert ${CMAKE_SOURCE_DIR}/shaders/viewer.frag
  COMMENT "Compiling graphics shaders"
  VERBATIM)

add_custom_command(
  OUTPUT ${SHADERS_DIR}/comp.spv
  COMMAND glslang::validator
  ARGS --target-env vulkan1.0 ${CMAKE_SOURCE_DIR}/shaders/viewer.comp --quiet
  WORKING_DIRECTORY ${SHADERS_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/viewer.comp
  COMMENT "Compiling compute shader"
  VERBATIM)

add_custom_target(shaders DEPENDS
  ${SHADERS_DIR}/frag.spv
  ${SHADERS_DIR}/vert.spv
  ${SHADERS_DIR}/comp.spv)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_executable(ImageViewer
  main.cpp
  renderer/VulkanImageRenderer.cpp
  renderer/PlatformDialogs.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)

set_target_properties(ImageViewer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  CXX_STANDARD 17)

target_include_directories(ImageViewer PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${CMAKE_SOURCE_DIR}/third_party/stb)

target_link_libraries(ImageViewer
  Vulkan::Vulkan
  ${CMAKE_SOURCE_DIR}/lib/glfw3.lib)
add_dependencies(ImageViewer shaders)

if(WIN32)
  # /utf-8：源码含中文/Unicode 字面量；comdlg32/ole32/gdi32 用于打开对话框与 GDI
  target_compile_options(ImageViewer PRIVATE /utf-8)
  target_link_libraries(ImageViewer comdlg32 ole32 gdi32)
endif()

# Windows Shell 缩略图 Handler（COM DLL，与 exe 同目录）
if(WIN32)
  add_subdirectory(ThumbnailHandler)
endif()
