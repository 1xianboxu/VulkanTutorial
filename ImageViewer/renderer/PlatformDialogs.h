#pragma once

// 平台打开对话框（Windows 实现，其他平台返回空串）
// ownerWindow：Windows 下为 HWND（可由 glfwGetWin32Window 获得），以 void* 传递避免头文件依赖 windows.h

#include <string>

// 图片文件选择框（过滤 psd/jpg/png/bmp/tga/gif/hdr 等）；取消返回空串
std::wstring openFileDialog(void *ownerWindow);

// 文件夹选择框（IFileOpenDialog + FOS_PICKFOLDERS）；取消返回空串
std::wstring openFolderDialog(void *ownerWindow);

// PNG 保存对话框；suggestedName 可传完整建议路径。取消返回空串。
std::wstring savePngDialog(void *ownerWindow,
                           const std::wstring &suggestedName);

// 使用 Windows Imaging Component 写入 RGBA8 PNG，支持中文路径。
bool saveRgbaPng(const std::wstring &path, const unsigned char *rgba,
                 int width, int height);

// Photoshop ABR / ImageViewer 画笔预设导入，以及 .ivbrush 导出对话框。
std::wstring openBrushPresetDialog(void *ownerWindow);
std::wstring saveBrushPresetDialog(void *ownerWindow,
                                   const std::wstring &suggestedName);
