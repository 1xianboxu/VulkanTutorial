// ImageViewer Windows Shell integration
// - Explorer thumbnails: IThumbnailProvider
// - Explorer Preview Pane: IPreviewHandler

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <ocidl.h>
#include <shobjidl.h>
#include <thumbcache.h>

#include <cstdint>
#include <string>

// {587EF8B0-7D02-4492-A069-942A37B54297}
inline constexpr CLSID CLSID_ImageViewerThumbnail = {
    0x587EF8B0,
    0x7D02,
    0x4492,
    {0xA0, 0x69, 0x94, 0x2A, 0x37, 0xB5, 0x42, 0x97}};

// {3E2EDDE5-89EA-43EE-AB0C-9983D18CAF7D}
inline constexpr CLSID CLSID_ImageViewerPreview = {
    0x3E2EDDE5,
    0x89EA,
    0x43EE,
    {0xAB, 0x0C, 0x99, 0x83, 0xD1, 0x8C, 0xAF, 0x7D}};

#define CLSID_IMAGEVIEWER_THUMBNAIL_STR                                       \
  L"{587EF8B0-7D02-4492-A069-942A37B54297}"
#define CLSID_IMAGEVIEWER_PREVIEW_STR                                         \
  L"{3E2EDDE5-89EA-43EE-AB0C-9983D18CAF7D}"

// Official Shell handler registration subkeys.
#define IID_THUMBNAIL_HANDLER_STR L"{E357FCCD-A995-4576-B01F-234630154E96}"
#define IID_PREVIEW_HANDLER_STR L"{8895B1C6-B41F-4C1C-A562-0D564250836F}"

class ImageViewerThumbnailProvider final : public IThumbnailProvider,
                                           public IInitializeWithStream,
                                           public IInitializeWithFile {
public:
  ImageViewerThumbnailProvider();

  STDMETHOD(QueryInterface)(REFIID riid, void **ppvObject) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  STDMETHOD(Initialize)(IStream *stream, DWORD mode) override;
  STDMETHOD(Initialize)(PCWSTR filePath, DWORD mode) override;
  STDMETHOD(GetThumbnail)(UINT size, HBITMAP *bitmap,
                          WTS_ALPHATYPE *alphaType) override;

private:
  ~ImageViewerThumbnailProvider();

  LONG m_ref = 1;
  IStream *m_stream = nullptr;
  std::wstring m_filePath;
  bool m_initialized = false;
};

class ImageViewerPreviewHandler final : public IPreviewHandler,
                                        public IPreviewHandlerVisuals,
                                        public IInitializeWithStream,
                                        public IInitializeWithFile,
                                        public IObjectWithSite,
                                        public IOleWindow {
public:
  ImageViewerPreviewHandler();

  STDMETHOD(QueryInterface)(REFIID riid, void **ppvObject) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  STDMETHOD(Initialize)(IStream *stream, DWORD mode) override;
  STDMETHOD(Initialize)(PCWSTR filePath, DWORD mode) override;

  STDMETHOD(SetWindow)(HWND parent, const RECT *rect) override;
  STDMETHOD(SetRect)(const RECT *rect) override;
  STDMETHOD(DoPreview)() override;
  STDMETHOD(Unload)() override;
  STDMETHOD(SetFocus)() override;
  STDMETHOD(QueryFocus)(HWND *focusedWindow) override;
  STDMETHOD(TranslateAccelerator)(MSG *message) override;

  STDMETHOD(SetBackgroundColor)(COLORREF color) override;
  STDMETHOD(SetFont)(const LOGFONTW *font) override;
  STDMETHOD(SetTextColor)(COLORREF color) override;

  STDMETHOD(SetSite)(IUnknown *site) override;
  STDMETHOD(GetSite)(REFIID riid, void **site) override;

  STDMETHOD(GetWindow)(HWND *window) override;
  STDMETHOD(ContextSensitiveHelp)(BOOL enterMode) override;

private:
  ~ImageViewerPreviewHandler();

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam);
  HRESULT createPreviewWindow();
  HRESULT loadImage();
  void paint(HDC dc);
  void movePreviewWindow();
  void clearImage();

  LONG m_ref = 1;
  IStream *m_stream = nullptr;
  std::wstring m_filePath;
  bool m_initialized = false;

  IUnknown *m_site = nullptr;
  IPreviewHandlerFrame *m_frame = nullptr;
  HWND m_parent = nullptr;
  HWND m_window = nullptr;
  RECT m_rect{};

  HBITMAP m_bitmap = nullptr;
  int m_imageWidth = 0;
  int m_imageHeight = 0;
  COLORREF m_background = RGB(255, 255, 255);
  COLORREF m_textColor = RGB(90, 90, 90);
  LOGFONTW m_font{};
  bool m_hasFont = false;
};
