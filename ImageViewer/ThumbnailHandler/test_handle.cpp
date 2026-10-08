#include "thumbhandler.h"

#include <iostream>

namespace {

template <typename T> void release(T *&value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

int fail(const char *message, HRESULT result = E_FAIL) {
  std::cerr << message << " (HRESULT=0x" << std::hex
            << static_cast<unsigned long>(result) << ")\n";
  return 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 2)
    return fail("usage: ImageViewerShellTest <image>");

  HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(result))
    return fail("CoInitializeEx failed", result);

  IInitializeWithFile *thumbnailInit = nullptr;
  result = CoCreateInstance(CLSID_ImageViewerThumbnail, nullptr,
                            CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&thumbnailInit));
  if (FAILED(result)) {
    CoUninitialize();
    return fail("thumbnail activation failed", result);
  }
  result = thumbnailInit->Initialize(argv[1], STGM_READ);
  IThumbnailProvider *thumbnail = nullptr;
  if (SUCCEEDED(result))
    result = thumbnailInit->QueryInterface(IID_PPV_ARGS(&thumbnail));
  HBITMAP bitmap = nullptr;
  WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
  if (SUCCEEDED(result))
    result = thumbnail->GetThumbnail(256, &bitmap, &alpha);
  BITMAP details{};
  if (SUCCEEDED(result) && bitmap)
    GetObjectW(bitmap, sizeof(details), &details);
  if (bitmap)
    DeleteObject(bitmap);
  release(thumbnail);
  release(thumbnailInit);
  if (FAILED(result) || details.bmWidth <= 0 || details.bmHeight <= 0 ||
      details.bmWidth > 256 || details.bmHeight > 256) {
    CoUninitialize();
    return fail("thumbnail generation failed", result);
  }

  IInitializeWithFile *previewInit = nullptr;
  result = CoCreateInstance(CLSID_ImageViewerPreview, nullptr,
                            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&previewInit));
  if (FAILED(result)) {
    CoUninitialize();
    return fail("preview activation failed", result);
  }
  result = previewInit->Initialize(argv[1], STGM_READ);
  IPreviewHandler *preview = nullptr;
  if (SUCCEEDED(result))
    result = previewInit->QueryInterface(IID_PPV_ARGS(&preview));

  HWND host = CreateWindowExW(0, L"STATIC", L"ImageViewer Shell Test",
                              WS_OVERLAPPED, 0, 0, 480, 640, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  RECT rect{0, 0, 480, 640};
  if (SUCCEEDED(result) && host)
    result = preview->SetWindow(host, &rect);
  if (SUCCEEDED(result))
    result = preview->DoPreview();
  IOleWindow *previewWindow = nullptr;
  HWND child = nullptr;
  if (SUCCEEDED(result))
    result = preview->QueryInterface(IID_PPV_ARGS(&previewWindow));
  if (SUCCEEDED(result))
    result = previewWindow->GetWindow(&child);
  if (preview)
    preview->Unload();
  release(previewWindow);
  release(preview);
  release(previewInit);
  if (host)
    DestroyWindow(host);

  CoUninitialize();
  if (FAILED(result) || !child)
    return fail("preview generation failed", result);

  std::cout << "thumbnail=" << details.bmWidth << "x" << details.bmHeight
            << ", alpha=" << static_cast<int>(alpha)
            << "; preview=ok\n";
  return 0;
}
