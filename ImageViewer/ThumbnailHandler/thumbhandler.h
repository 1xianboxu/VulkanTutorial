// ImageViewer Thumbnail Handler
// COM 类：实现 IThumbnailProvider + IInitializeWithStream
// Shell 在文件资源管理器中显示缩略图时加载此 DLL（进程内，跑在
// ThumbnailHost.exe）

#pragma once

// windows.h 必须最先（thumbcache.h / shobjidl.h 等 SDK 头依赖完整的 Windows
// 类型）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// 标准库（头文件里用到的容器和字符串）
#include <cstdint>
#include <string>
#include <vector>

// Windows COM + Shell 接口
#include <objbase.h>
#include <shobjidl.h>   // IInitializeWithStream, IInitializeWithFile
#include <thumbcache.h> // IThumbnailProvider, WTSAT_*, WTS_FLAGS
#include <unknwn.h>

// 我们的 CLSID（稳定 GUID）
// 生成: [guid]::NewGuid().ToString('B').ToUpper()
static const CLSID CLSID_ImageViewerThumbnail = {
    0x587EF8B0,
    0x7D02,
    0x4492,
    {0xA0, 0x69, 0x94, 0x2A, 0x37, 0xB5, 0x42, 0x97}};
// GUID 字符串形式，注册时用
// {587EF8B0-7D02-4492-A069-942A37B54297}

// Shell 找到缩略图 Handler 的 shellex GUID（对所有图片格式通用）
// HKCR\.png\shellex\{E357FCCD-A995-4578-B878-6D1F3C60A6A8} = {our CLSID}
static const GUID THUMBNAIL_SHELLEX_GUID = {
    0xE357FCCD,
    0xA995,
    0x4578,
    {0xB8, 0x78, 0x6D, 0x1F, 0x3C, 0x60, 0xA6, 0xA8}};
#define IID_THUMBNAIL_SHELLEX_GUID L"{E357FCCD-A995-4578-B878-6D1F3C60A6A8}"

class ImageViewerThumbnailProvider : public IThumbnailProvider,
                                     public IInitializeWithStream,
                                     public IInitializeWithFile {
public:
  // IUnknown
  STDMETHOD(QueryInterface)(REFIID riid, void **ppvObject) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  // IInitializeWithStream（shell 优先给 stream）
  STDMETHOD(Initialize)(IStream *pStream, DWORD grfMode) override;

  // IInitializeWithFile（shell 有时直接给文件路径）
  STDMETHOD(Initialize)(PCWSTR pszFilePath, DWORD grfMode) override;

  // IThumbnailProvider
  STDMETHOD(GetThumbnail)(UINT cx, HBITMAP *phbmp,
                          WTS_ALPHATYPE *pdwAlpha) override;

  // COM 工厂需要 public 构造
  ImageViewerThumbnailProvider() = default;
  ~ImageViewerThumbnailProvider() = default;

private:
  // 读取文件到内存并返回解码后的 RGBA8 缓冲区（stbi_load）
  // 成功返回 true，像素存入 pixels，宽高存入 w/h
  bool decodeFromStream(IStream *pStream, std::vector<uint8_t> &pixels, int &w,
                        int &h);
  bool decodeFromFile(PCWSTR pszFilePath, std::vector<uint8_t> &pixels, int &w,
                      int &h);

  LONG m_ref = 1;
  // 存储 IStream（如果 shell 用 IInitializeWithStream 初始化）
  IStream *m_pStream = nullptr;
  // 或者存文件路径（如果 shell 用 IInitializeWithFile 初始化）
  std::wstring m_filePath;
  bool m_initialized = false;
};
