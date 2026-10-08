// ImageViewer Shell extension: Explorer thumbnails + Preview Pane.

#include "thumbhandler.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include <shlobj.h>
#include <shlwapi.h>

namespace fs = std::filesystem;

namespace {

HINSTANCE g_module = nullptr;
std::atomic_long g_objects{0};
std::atomic_long g_serverLocks{0};

constexpr size_t MAX_INPUT_BYTES = 512u * 1024u * 1024u;
constexpr int MAX_PREVIEW_EDGE = 4096;
constexpr wchar_t PREVIEW_WINDOW_CLASS[] =
    L"ImageViewer.PreviewWindow.3E2EDDE5";
constexpr wchar_t PREVHOST_APPID[] =
    L"{6D2B5079-2F0B-48DD-AB7F-97CEC514D30B}";

const wchar_t *const SUPPORTED_EXTENSIONS[] = {
    L".png", L".jpg", L".jpeg", L".bmp", L".tga", L".gif",
    L".hdr", L".psd", L".pic",  L".ppm", L".pgm"};

bool readStream(IStream *stream, std::vector<uint8_t> &bytes) {
  if (!stream)
    return false;

  LARGE_INTEGER zero{};
  if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
    return false;

  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
    return false;
  const ULONGLONG length = stat.cbSize.QuadPart;
  if (length == 0 || length > MAX_INPUT_BYTES)
    return false;

  try {
    bytes.resize(static_cast<size_t>(length));
  } catch (...) {
    return false;
  }

  size_t total = 0;
  while (total < bytes.size()) {
    const ULONG request = static_cast<ULONG>(
        (std::min)(bytes.size() - total, static_cast<size_t>(1u << 20)));
    ULONG read = 0;
    if (FAILED(stream->Read(bytes.data() + total, request, &read)) || read == 0)
      return false;
    total += read;
  }
  return true;
}

bool readFile(PCWSTR filePath, std::vector<uint8_t> &bytes) {
  if (!filePath || !*filePath)
    return false;
  FILE *file = _wfopen(filePath, L"rb");
  if (!file)
    return false;

  bool ok = false;
  if (_fseeki64(file, 0, SEEK_END) == 0) {
    const __int64 length = _ftelli64(file);
    if (length > 0 && static_cast<unsigned long long>(length) <= MAX_INPUT_BYTES &&
        _fseeki64(file, 0, SEEK_SET) == 0) {
      try {
        bytes.resize(static_cast<size_t>(length));
        ok = fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
      } catch (...) {
        ok = false;
      }
    }
  }
  fclose(file);
  return ok;
}

bool decodeImage(IStream *stream, const std::wstring &filePath,
                 std::vector<uint8_t> &pixels, int &width, int &height) {
  std::vector<uint8_t> bytes;
  const bool readOk = stream ? readStream(stream, bytes)
                             : readFile(filePath.c_str(), bytes);
  if (!readOk || bytes.empty() || bytes.size() > INT_MAX)
    return false;

  int channels = 0;
  stbi_uc *decoded = stbi_load_from_memory(
      bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels,
      STBI_rgb_alpha);
  if (!decoded || width <= 0 || height <= 0)
    return false;

  try {
    pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
  } catch (...) {
    stbi_image_free(decoded);
    return false;
  }
  stbi_image_free(decoded);
  return true;
}

void downscaleAlphaAware(const uint8_t *src, int srcWidth, int srcHeight,
                         uint8_t *dst, int dstWidth, int dstHeight) {
  const float xScale = static_cast<float>(srcWidth) / dstWidth;
  const float yScale = static_cast<float>(srcHeight) / dstHeight;

  for (int y = 0; y < dstHeight; ++y) {
    int y0 = static_cast<int>(y * yScale);
    int y1 = static_cast<int>((y + 1) * yScale);
    y1 = (std::min)(y1, srcHeight);
    if (y1 <= y0)
      y1 = y0 + 1;

    for (int x = 0; x < dstWidth; ++x) {
      int x0 = static_cast<int>(x * xScale);
      int x1 = static_cast<int>((x + 1) * xScale);
      x1 = (std::min)(x1, srcWidth);
      if (x1 <= x0)
        x1 = x0 + 1;

      uint64_t redPremul = 0, greenPremul = 0, bluePremul = 0, alpha = 0;
      uint32_t count = 0;
      for (int sy = y0; sy < y1; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          const uint8_t *p = src +
                             (static_cast<size_t>(sy) * srcWidth + sx) * 4;
          redPremul += static_cast<uint32_t>(p[0]) * p[3];
          greenPremul += static_cast<uint32_t>(p[1]) * p[3];
          bluePremul += static_cast<uint32_t>(p[2]) * p[3];
          alpha += p[3];
          ++count;
        }
      }

      uint8_t *out =
          dst + (static_cast<size_t>(y) * dstWidth + x) * 4;
      out[0] = alpha ? static_cast<uint8_t>(redPremul / alpha) : 0;
      out[1] = alpha ? static_cast<uint8_t>(greenPremul / alpha) : 0;
      out[2] = alpha ? static_cast<uint8_t>(bluePremul / alpha) : 0;
      out[3] = static_cast<uint8_t>(alpha / count);
    }
  }
}

bool fitImage(std::vector<uint8_t> &pixels, int &width, int &height,
              int maxEdge, bool allowUpscale) {
  if (width <= 0 || height <= 0 || maxEdge <= 0)
    return false;
  float scale = static_cast<float>(maxEdge) / (std::max)(width, height);
  if (!allowUpscale)
    scale = (std::min)(scale, 1.0f);
  const int outWidth = (std::max)(1, static_cast<int>(std::lround(width * scale)));
  const int outHeight =
      (std::max)(1, static_cast<int>(std::lround(height * scale)));
  if (outWidth == width && outHeight == height)
    return true;

  try {
    std::vector<uint8_t> resized(static_cast<size_t>(outWidth) * outHeight * 4);
    downscaleAlphaAware(pixels.data(), width, height, resized.data(), outWidth,
                        outHeight);
    pixels.swap(resized);
  } catch (...) {
    return false;
  }
  width = outWidth;
  height = outHeight;
  return true;
}

HBITMAP createPremultipliedBitmap(const std::vector<uint8_t> &rgba, int width,
                                  int height) {
  if (rgba.size() < static_cast<size_t>(width) * height * 4)
    return nullptr;

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void *bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits,
                                    nullptr, 0);
  if (!bitmap || !bits) {
    if (bitmap)
      DeleteObject(bitmap);
    return nullptr;
  }

  auto *out = static_cast<uint8_t *>(bits);
  const size_t pixelCount = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < pixelCount; ++i) {
    const uint8_t a = rgba[i * 4 + 3];
    out[i * 4 + 0] = static_cast<uint8_t>(rgba[i * 4 + 2] * a / 255);
    out[i * 4 + 1] = static_cast<uint8_t>(rgba[i * 4 + 1] * a / 255);
    out[i * 4 + 2] = static_cast<uint8_t>(rgba[i * 4 + 0] * a / 255);
    out[i * 4 + 3] = a;
  }
  return bitmap;
}

bool setRegistryString(HKEY root, const std::wstring &subkey,
                       const wchar_t *name, const std::wstring &value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS)
    return false;
  const LONG result = RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool setRegistryDword(HKEY root, const std::wstring &subkey,
                      const wchar_t *name, DWORD value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS)
    return false;
  const LONG result = RegSetValueExW(
      key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value),
      sizeof(value));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

void deleteRegistryTree(HKEY root, const std::wstring &subkey) {
  RegDeleteTreeW(root, subkey.c_str());
}

void deleteRegistryValue(HKEY root, const std::wstring &subkey,
                         const wchar_t *name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE, &key) ==
      ERROR_SUCCESS) {
    RegDeleteValueW(key, name);
    RegCloseKey(key);
  }
}

std::wstring queryCurrentProgId(const wchar_t *extension) {
  wchar_t value[512]{};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CLASSES_ROOT, extension, nullptr, RRF_RT_REG_SZ,
                   nullptr, value, &size) == ERROR_SUCCESS && value[0])
    return value;
  return {};
}

std::wstring modulePath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(g_module, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  return length > 0 && length < buffer.size()
             ? std::wstring(buffer.data(), length)
             : std::wstring();
}

class ImageViewerClassFactory final : public IClassFactory {
public:
  explicit ImageViewerClassFactory(REFCLSID clsid) : m_clsid(clsid) {
    ++g_objects;
  }

  STDMETHOD(QueryInterface)(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
      *object = static_cast<IClassFactory *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_ref); }

  STDMETHOD_(ULONG, Release)() override {
    const ULONG ref = InterlockedDecrement(&m_ref);
    if (ref == 0)
      delete this;
    return ref;
  }

  STDMETHOD(CreateInstance)(IUnknown *outer, REFIID riid,
                            void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (outer)
      return CLASS_E_NOAGGREGATION;

    IUnknown *instance = nullptr;
    if (IsEqualCLSID(m_clsid, CLSID_ImageViewerThumbnail)) {
      instance = static_cast<IThumbnailProvider *>(
          new (std::nothrow) ImageViewerThumbnailProvider());
    } else if (IsEqualCLSID(m_clsid, CLSID_ImageViewerPreview)) {
      instance = static_cast<IPreviewHandler *>(
          new (std::nothrow) ImageViewerPreviewHandler());
    }
    if (!instance)
      return E_OUTOFMEMORY;

    const HRESULT result = instance->QueryInterface(riid, object);
    instance->Release();
    return result;
  }

  STDMETHOD(LockServer)(BOOL lock) override {
    if (lock)
      ++g_serverLocks;
    else
      --g_serverLocks;
    return S_OK;
  }

private:
  ~ImageViewerClassFactory() { --g_objects; }
  LONG m_ref = 1;
  CLSID m_clsid{};
};

} // namespace

// ---------------------------------------------------------------------------
// Thumbnail provider
// ---------------------------------------------------------------------------

ImageViewerThumbnailProvider::ImageViewerThumbnailProvider() { ++g_objects; }

ImageViewerThumbnailProvider::~ImageViewerThumbnailProvider() {
  if (m_stream)
    m_stream->Release();
  --g_objects;
}

STDMETHODIMP ImageViewerThumbnailProvider::QueryInterface(REFIID riid,
                                                          void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_IThumbnailProvider))
    *object = static_cast<IThumbnailProvider *>(this);
  else if (IsEqualIID(riid, IID_IInitializeWithStream))
    *object = static_cast<IInitializeWithStream *>(this);
  else if (IsEqualIID(riid, IID_IInitializeWithFile))
    *object = static_cast<IInitializeWithFile *>(this);
  else
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) ImageViewerThumbnailProvider::AddRef() {
  return InterlockedIncrement(&m_ref);
}

STDMETHODIMP_(ULONG) ImageViewerThumbnailProvider::Release() {
  const ULONG ref = InterlockedDecrement(&m_ref);
  if (ref == 0)
    delete this;
  return ref;
}

STDMETHODIMP ImageViewerThumbnailProvider::Initialize(IStream *stream,
                                                      DWORD) {
  if (!stream)
    return E_INVALIDARG;
  if (m_initialized)
    return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
  stream->AddRef();
  m_stream = stream;
  m_initialized = true;
  return S_OK;
}

STDMETHODIMP ImageViewerThumbnailProvider::Initialize(PCWSTR filePath,
                                                      DWORD) {
  if (!filePath || !*filePath)
    return E_INVALIDARG;
  if (m_initialized)
    return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
  m_filePath = filePath;
  m_initialized = true;
  return S_OK;
}

STDMETHODIMP ImageViewerThumbnailProvider::GetThumbnail(
    UINT requestedSize, HBITMAP *bitmap, WTS_ALPHATYPE *alphaType) {
  if (!bitmap || !alphaType)
    return E_POINTER;
  *bitmap = nullptr;
  *alphaType = WTSAT_UNKNOWN;
  if (!m_initialized || requestedSize == 0)
    return E_UNEXPECTED;

  std::vector<uint8_t> pixels;
  int width = 0, height = 0;
  if (!decodeImage(m_stream, m_filePath, pixels, width, height) ||
      !fitImage(pixels, width, height, static_cast<int>(requestedSize), false))
    return E_FAIL;

  *bitmap = createPremultipliedBitmap(pixels, width, height);
  if (!*bitmap)
    return E_OUTOFMEMORY;
  *alphaType = WTSAT_ARGB;
  return S_OK;
}

// ---------------------------------------------------------------------------
// Preview handler
// ---------------------------------------------------------------------------

ImageViewerPreviewHandler::ImageViewerPreviewHandler() { ++g_objects; }

ImageViewerPreviewHandler::~ImageViewerPreviewHandler() {
  Unload();
  SetSite(nullptr);
  --g_objects;
}

STDMETHODIMP ImageViewerPreviewHandler::QueryInterface(REFIID riid,
                                                       void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPreviewHandler))
    *object = static_cast<IPreviewHandler *>(this);
  else if (IsEqualIID(riid, IID_IPreviewHandlerVisuals))
    *object = static_cast<IPreviewHandlerVisuals *>(this);
  else if (IsEqualIID(riid, IID_IInitializeWithStream))
    *object = static_cast<IInitializeWithStream *>(this);
  else if (IsEqualIID(riid, IID_IInitializeWithFile))
    *object = static_cast<IInitializeWithFile *>(this);
  else if (IsEqualIID(riid, IID_IObjectWithSite))
    *object = static_cast<IObjectWithSite *>(this);
  else if (IsEqualIID(riid, IID_IOleWindow))
    *object = static_cast<IOleWindow *>(this);
  else
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) ImageViewerPreviewHandler::AddRef() {
  return InterlockedIncrement(&m_ref);
}

STDMETHODIMP_(ULONG) ImageViewerPreviewHandler::Release() {
  const ULONG ref = InterlockedDecrement(&m_ref);
  if (ref == 0)
    delete this;
  return ref;
}

STDMETHODIMP ImageViewerPreviewHandler::Initialize(IStream *stream, DWORD) {
  if (!stream)
    return E_INVALIDARG;
  if (m_initialized)
    return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
  stream->AddRef();
  m_stream = stream;
  m_initialized = true;
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::Initialize(PCWSTR filePath, DWORD) {
  if (!filePath || !*filePath)
    return E_INVALIDARG;
  if (m_initialized)
    return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
  m_filePath = filePath;
  m_initialized = true;
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::SetWindow(HWND parent,
                                                  const RECT *rect) {
  if (!parent || !rect)
    return E_INVALIDARG;
  m_parent = parent;
  m_rect = *rect;
  if (m_window) {
    SetParent(m_window, m_parent);
    movePreviewWindow();
  }
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::SetRect(const RECT *rect) {
  if (!rect)
    return E_INVALIDARG;
  m_rect = *rect;
  movePreviewWindow();
  return S_OK;
}

HRESULT ImageViewerPreviewHandler::createPreviewWindow() {
  if (m_window)
    return S_OK;
  if (!m_parent)
    return E_UNEXPECTED;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = g_module;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = PREVIEW_WINDOW_CLASS;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return HRESULT_FROM_WIN32(GetLastError());

  m_window = CreateWindowExW(
      0, PREVIEW_WINDOW_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
      m_rect.left, m_rect.top, m_rect.right - m_rect.left,
      m_rect.bottom - m_rect.top, m_parent, nullptr, g_module, this);
  return m_window ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT ImageViewerPreviewHandler::loadImage() {
  clearImage();
  std::vector<uint8_t> pixels;
  if (!decodeImage(m_stream, m_filePath, pixels, m_imageWidth, m_imageHeight))
    return E_FAIL;
  if ((std::max)(m_imageWidth, m_imageHeight) > MAX_PREVIEW_EDGE &&
      !fitImage(pixels, m_imageWidth, m_imageHeight, MAX_PREVIEW_EDGE, false))
    return E_OUTOFMEMORY;

  m_bitmap = createPremultipliedBitmap(pixels, m_imageWidth, m_imageHeight);
  return m_bitmap ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP ImageViewerPreviewHandler::DoPreview() {
  if (!m_initialized)
    return E_UNEXPECTED;
  HRESULT result = createPreviewWindow();
  if (FAILED(result))
    return result;
  result = loadImage();
  InvalidateRect(m_window, nullptr, TRUE);
  return result;
}

void ImageViewerPreviewHandler::clearImage() {
  if (m_bitmap) {
    DeleteObject(m_bitmap);
    m_bitmap = nullptr;
  }
  m_imageWidth = m_imageHeight = 0;
}

STDMETHODIMP ImageViewerPreviewHandler::Unload() {
  clearImage();
  if (m_window) {
    DestroyWindow(m_window);
    m_window = nullptr;
  }
  if (m_stream) {
    m_stream->Release();
    m_stream = nullptr;
  }
  m_filePath.clear();
  m_initialized = false;
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::SetFocus() {
  if (!m_window)
    return S_FALSE;
  ::SetFocus(m_window);
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::QueryFocus(HWND *focusedWindow) {
  if (!focusedWindow)
    return E_POINTER;
  *focusedWindow = ::GetFocus();
  return *focusedWindow ? S_OK : S_FALSE;
}

STDMETHODIMP ImageViewerPreviewHandler::TranslateAccelerator(MSG *message) {
  if (!message)
    return E_INVALIDARG;
  return m_frame ? m_frame->TranslateAccelerator(message) : S_FALSE;
}

STDMETHODIMP ImageViewerPreviewHandler::SetBackgroundColor(COLORREF color) {
  m_background = color;
  if (m_window)
    InvalidateRect(m_window, nullptr, TRUE);
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::SetFont(const LOGFONTW *font) {
  if (!font)
    return E_INVALIDARG;
  m_font = *font;
  m_hasFont = true;
  if (m_window)
    InvalidateRect(m_window, nullptr, TRUE);
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::SetTextColor(COLORREF color) {
  m_textColor = color;
  if (m_window)
    InvalidateRect(m_window, nullptr, TRUE);
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::SetSite(IUnknown *site) {
  if (m_frame) {
    m_frame->Release();
    m_frame = nullptr;
  }
  if (m_site) {
    m_site->Release();
    m_site = nullptr;
  }
  if (site) {
    site->AddRef();
    m_site = site;
    site->QueryInterface(IID_PPV_ARGS(&m_frame));
  }
  return S_OK;
}

STDMETHODIMP ImageViewerPreviewHandler::GetSite(REFIID riid, void **site) {
  if (!site)
    return E_POINTER;
  *site = nullptr;
  return m_site ? m_site->QueryInterface(riid, site) : E_FAIL;
}

STDMETHODIMP ImageViewerPreviewHandler::GetWindow(HWND *window) {
  if (!window)
    return E_POINTER;
  *window = m_window;
  return m_window ? S_OK : E_FAIL;
}

STDMETHODIMP ImageViewerPreviewHandler::ContextSensitiveHelp(BOOL) {
  return E_NOTIMPL;
}

void ImageViewerPreviewHandler::movePreviewWindow() {
  if (m_window)
    MoveWindow(m_window, m_rect.left, m_rect.top, m_rect.right - m_rect.left,
               m_rect.bottom - m_rect.top, TRUE);
}

void ImageViewerPreviewHandler::paint(HDC dc) {
  RECT client{};
  GetClientRect(m_window, &client);
  HBRUSH background = CreateSolidBrush(m_background);
  FillRect(dc, &client, background);
  DeleteObject(background);

  if (!m_bitmap || m_imageWidth <= 0 || m_imageHeight <= 0)
    return;

  constexpr int margin = 16;
  constexpr int metadataHeight = 32;
  const int availableWidth =
      (std::max)(1, static_cast<int>(client.right) - margin * 2);
  const int availableHeight =
      (std::max)(1, static_cast<int>(client.bottom) - margin * 2 -
                        metadataHeight);
  const float scale =
      (std::min)(static_cast<float>(availableWidth) / m_imageWidth,
                 static_cast<float>(availableHeight) / m_imageHeight);
  const int drawWidth = (std::max)(1, static_cast<int>(m_imageWidth * scale));
  const int drawHeight =
      (std::max)(1, static_cast<int>(m_imageHeight * scale));
  const int x = (client.right - drawWidth) / 2;
  const int y = margin + (availableHeight - drawHeight) / 2;

  HDC memory = CreateCompatibleDC(dc);
  HGDIOBJ previous = SelectObject(memory, m_bitmap);
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  AlphaBlend(dc, x, y, drawWidth, drawHeight, memory, 0, 0, m_imageWidth,
             m_imageHeight, blend);
  SelectObject(memory, previous);
  DeleteDC(memory);

  HPEN border = CreatePen(PS_SOLID, 1, RGB(205, 210, 218));
  HGDIOBJ oldPen = SelectObject(dc, border);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, x, y, x + drawWidth, y + drawHeight);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(border);

  wchar_t metadata[96];
  swprintf_s(metadata, L"%d × %d px", m_imageWidth, m_imageHeight);
  RECT textRect{margin, client.bottom - metadataHeight,
                client.right - margin, client.bottom - 4};
  SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, m_textColor);
  HFONT createdFont = m_hasFont ? CreateFontIndirectW(&m_font) : nullptr;
  HGDIOBJ oldFont =
      SelectObject(dc, createdFont ? createdFont : GetStockObject(DEFAULT_GUI_FONT));
  DrawTextW(dc, metadata, -1, &textRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(dc, oldFont);
  if (createdFont)
    DeleteObject(createdFont);
}

LRESULT CALLBACK ImageViewerPreviewHandler::WindowProc(HWND hwnd, UINT message,
                                                       WPARAM wParam,
                                                       LPARAM lParam) {
  auto *self = reinterpret_cast<ImageViewerPreviewHandler *>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
    self = static_cast<ImageViewerPreviewHandler *>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(self));
  }
  if (message == WM_ERASEBKGND)
    return 1;
  if (message == WM_PAINT && self) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    self->paint(dc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// COM exports + per-user registration
// ---------------------------------------------------------------------------

extern "C" STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid,
                                    void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (!IsEqualCLSID(clsid, CLSID_ImageViewerThumbnail) &&
      !IsEqualCLSID(clsid, CLSID_ImageViewerPreview))
    return CLASS_E_CLASSNOTAVAILABLE;

  auto *factory = new (std::nothrow) ImageViewerClassFactory(clsid);
  if (!factory)
    return E_OUTOFMEMORY;
  const HRESULT result = factory->QueryInterface(riid, object);
  factory->Release();
  return result;
}

extern "C" STDAPI DllCanUnloadNow() {
  return g_objects.load() == 0 && g_serverLocks.load() == 0 ? S_OK : S_FALSE;
}

extern "C" STDAPI DllRegisterServer() {
  try {
    const std::wstring dll = modulePath();
    if (dll.empty())
      return E_FAIL;
    const std::wstring exe = (fs::path(dll).parent_path() / L"ImageViewer.exe")
                                 .wstring();
    const std::wstring command = L"\"" + exe + L"\" \"%1\"";
    const std::wstring icon = exe + L",0";
    bool ok = true;

    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring thumbClsid =
        classes + L"CLSID\\" CLSID_IMAGEVIEWER_THUMBNAIL_STR;
    const std::wstring previewClsid =
        classes + L"CLSID\\" CLSID_IMAGEVIEWER_PREVIEW_STR;
    ok &= setRegistryString(HKEY_CURRENT_USER, thumbClsid, nullptr,
                            L"ImageViewer Thumbnail Provider");
    ok &= setRegistryString(HKEY_CURRENT_USER, thumbClsid + L"\\InprocServer32",
                            nullptr, dll);
    ok &= setRegistryString(HKEY_CURRENT_USER, thumbClsid + L"\\InprocServer32",
                            L"ThreadingModel", L"Apartment");
    ok &= setRegistryString(HKEY_CURRENT_USER, previewClsid, nullptr,
                            L"ImageViewer Preview Handler");
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            previewClsid + L"\\InprocServer32", nullptr, dll);
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            previewClsid + L"\\InprocServer32",
                            L"ThreadingModel", L"Apartment");
    ok &= setRegistryString(HKEY_CURRENT_USER, previewClsid, L"AppID",
                            PREVHOST_APPID);
    ok &= setRegistryString(HKEY_CURRENT_USER, previewClsid, L"DisplayName",
                            L"ImageViewer Preview Handler");
    ok &= setRegistryString(HKEY_CURRENT_USER, previewClsid, L"Icon", icon);
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            previewClsid + L"\\InprocServer32", L"ProgID",
                            L"ImageViewer.Image");
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            previewClsid + L"\\InprocServer32",
                            L"VersionIndependentProgID", L"ImageViewer.Image");

    ok &= setRegistryString(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
        CLSID_IMAGEVIEWER_PREVIEW_STR, L"ImageViewer Preview Handler");

    for (const wchar_t *extension : SUPPORTED_EXTENSIONS) {
      const std::wstring direct = classes + extension + L"\\ShellEx\\";
      const std::wstring system =
          classes + L"SystemFileAssociations\\" + extension + L"\\ShellEx\\";
      ok &= setRegistryString(HKEY_CURRENT_USER,
                              direct + IID_THUMBNAIL_HANDLER_STR, nullptr,
                              CLSID_IMAGEVIEWER_THUMBNAIL_STR);
      ok &= setRegistryString(HKEY_CURRENT_USER,
                              direct + IID_PREVIEW_HANDLER_STR, nullptr,
                              CLSID_IMAGEVIEWER_PREVIEW_STR);
      ok &= setRegistryString(HKEY_CURRENT_USER,
                              system + IID_THUMBNAIL_HANDLER_STR, nullptr,
                              CLSID_IMAGEVIEWER_THUMBNAIL_STR);
      ok &= setRegistryString(HKEY_CURRENT_USER,
                              system + IID_PREVIEW_HANDLER_STR, nullptr,
                              CLSID_IMAGEVIEWER_PREVIEW_STR);
      const std::wstring currentProgId = queryCurrentProgId(extension);
      if (!currentProgId.empty()) {
        const std::wstring progIdShell =
            classes + currentProgId + L"\\ShellEx\\";
        ok &= setRegistryString(HKEY_CURRENT_USER,
                                progIdShell + IID_THUMBNAIL_HANDLER_STR,
                                nullptr, CLSID_IMAGEVIEWER_THUMBNAIL_STR);
        ok &= setRegistryString(HKEY_CURRENT_USER,
                                progIdShell + IID_PREVIEW_HANDLER_STR, nullptr,
                                CLSID_IMAGEVIEWER_PREVIEW_STR);
      }
    }

    const std::wstring app = classes + L"Applications\\ImageViewer.exe";
    ok &= setRegistryString(HKEY_CURRENT_USER, app, L"FriendlyAppName",
                            L"ImageViewer");
    ok &= setRegistryString(HKEY_CURRENT_USER, app, L"ApplicationIcon", icon);
    ok &= setRegistryString(HKEY_CURRENT_USER, app + L"\\DefaultIcon", nullptr,
                            icon);
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            app + L"\\shell\\open\\command", nullptr, command);
    for (const wchar_t *extension : SUPPORTED_EXTENSIONS) {
      ok &= setRegistryString(HKEY_CURRENT_USER, app + L"\\SupportedTypes",
                              extension, L"");
      ok &= setRegistryString(HKEY_CURRENT_USER,
                              app + L"\\Capabilities\\FileAssociations",
                              extension, L"ImageViewer.Image");
    }
    ok &= setRegistryString(HKEY_CURRENT_USER, app + L"\\Capabilities",
                            L"ApplicationName", L"ImageViewer");
    ok &= setRegistryString(HKEY_CURRENT_USER, app + L"\\Capabilities",
                            L"ApplicationDescription",
                            L"Professional GPU-accelerated image viewer");

    const std::wstring progId = classes + L"ImageViewer.Image";
    ok &= setRegistryString(HKEY_CURRENT_USER, progId, nullptr,
                            L"ImageViewer Image");
    ok &= setRegistryString(HKEY_CURRENT_USER, progId + L"\\DefaultIcon", nullptr,
                            icon);
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            progId + L"\\shell\\open\\command", nullptr,
                            command);
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            progId + L"\\ShellEx\\" IID_THUMBNAIL_HANDLER_STR,
                            nullptr, CLSID_IMAGEVIEWER_THUMBNAIL_STR);
    ok &= setRegistryString(HKEY_CURRENT_USER,
                            progId + L"\\ShellEx\\" IID_PREVIEW_HANDLER_STR,
                            nullptr, CLSID_IMAGEVIEWER_PREVIEW_STR);
    ok &= setRegistryDword(HKEY_CURRENT_USER, progId, L"Treatment", 2);
    ok &= setRegistryString(
        HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"ImageViewer",
        L"Software\\Classes\\Applications\\ImageViewer.exe\\Capabilities");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? S_OK : HRESULT_FROM_WIN32(ERROR_CANTWRITE);
  } catch (...) {
    return E_FAIL;
  }
}

extern "C" STDAPI DllUnregisterServer() {
  const std::wstring classes = L"Software\\Classes\\";
  deleteRegistryTree(HKEY_CURRENT_USER,
                     classes + L"CLSID\\" CLSID_IMAGEVIEWER_THUMBNAIL_STR);
  deleteRegistryTree(HKEY_CURRENT_USER,
                     classes + L"CLSID\\" CLSID_IMAGEVIEWER_PREVIEW_STR);
  deleteRegistryValue(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
      CLSID_IMAGEVIEWER_PREVIEW_STR);

  for (const wchar_t *extension : SUPPORTED_EXTENSIONS) {
    const std::wstring direct = classes + extension + L"\\ShellEx\\";
    const std::wstring system =
        classes + L"SystemFileAssociations\\" + extension + L"\\ShellEx\\";
    deleteRegistryTree(HKEY_CURRENT_USER, direct + IID_THUMBNAIL_HANDLER_STR);
    deleteRegistryTree(HKEY_CURRENT_USER, direct + IID_PREVIEW_HANDLER_STR);
    deleteRegistryTree(HKEY_CURRENT_USER, system + IID_THUMBNAIL_HANDLER_STR);
    deleteRegistryTree(HKEY_CURRENT_USER, system + IID_PREVIEW_HANDLER_STR);
    const std::wstring currentProgId = queryCurrentProgId(extension);
    if (!currentProgId.empty()) {
      const std::wstring progIdShell =
          classes + currentProgId + L"\\ShellEx\\";
      deleteRegistryTree(HKEY_CURRENT_USER,
                         progIdShell + IID_THUMBNAIL_HANDLER_STR);
      deleteRegistryTree(HKEY_CURRENT_USER,
                         progIdShell + IID_PREVIEW_HANDLER_STR);
    }
  }

  deleteRegistryTree(HKEY_CURRENT_USER,
                     classes + L"Applications\\ImageViewer.exe");
  deleteRegistryTree(HKEY_CURRENT_USER, classes + L"ImageViewer.Image");
  deleteRegistryValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications",
                      L"ImageViewer");
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
