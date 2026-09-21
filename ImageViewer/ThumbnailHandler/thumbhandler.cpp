// ImageViewer Thumbnail Handler —— COM 接口实现
// Shell 进程内加载，不能依赖 Vulkan/GLFW/ImGui，只用 stb_image + GDI

// IMPORTANT: windows.h 必须在 thumbhandler.h 之前（thumbcache.h 等 SDK
// 头依赖它）
#include <shlwapi.h>
#include <windows.h>

#include "thumbhandler.h"

// ---- stb_image ----
// 一个编译单元定义 STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cstdint>
#include <vector>

// ============================================================================
// 工具：box 滤波降采样（和 main.cpp 里的 downscaleBox 逻辑相同）
// 输入 RGBA8 (src, sw, sh) → 输出 RGBA8 (dst, dw, dh)
// ============================================================================
static void downscaleBox(const uint8_t *src, int sw, int sh, uint8_t *dst,
                         int dw, int dh) {
  float xscale = (float)sw / dw;
  float yscale = (float)sh / dh;
  for (int y = 0; y < dh; y++) {
    int y0 = (int)(y * yscale), y1 = (int)((y + 1) * yscale);
    y1 = (std::min)(y1, sh); // 括号防 Windows min 宏
    if (y1 <= y0)
      y1 = y0 + 1;
    for (int x = 0; x < dw; x++) {
      int x0 = (int)(x * xscale), x1 = (int)((x + 1) * xscale);
      x1 = (std::min)(x1, sw); // 括号防 Windows min 宏
      if (x1 <= x0)
        x1 = x0 + 1;

      uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
      for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
          const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
          r += p[0];
          g += p[1];
          b += p[2];
          a += p[3];
          cnt++;
        }
      }
      uint8_t *d = dst + ((size_t)y * dw + x) * 4;
      d[0] = (uint8_t)(r / cnt);
      d[1] = (uint8_t)(g / cnt);
      d[2] = (uint8_t)(b / cnt);
      d[3] = (uint8_t)(a / cnt);
    }
  }
}

// ============================================================================
// COM 实现
// ============================================================================

STDMETHODIMP_(ULONG) ImageViewerThumbnailProvider::AddRef() { return ++m_ref; }
STDMETHODIMP_(ULONG) ImageViewerThumbnailProvider::Release() {
  LONG r = --m_ref;
  if (r == 0)
    delete this;
  return r;
}

STDMETHODIMP ImageViewerThumbnailProvider::QueryInterface(REFIID riid,
                                                          void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = nullptr;
  if (IsEqualGUID(riid, IID_IUnknown) ||
      IsEqualGUID(riid, IID_IThumbnailProvider)) {
    *ppv = static_cast<IThumbnailProvider *>(this);
  } else if (IsEqualGUID(riid, IID_IInitializeWithStream)) {
    *ppv = static_cast<IInitializeWithStream *>(this);
  } else if (IsEqualGUID(riid, IID_IInitializeWithFile)) {
    *ppv = static_cast<IInitializeWithFile *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

// --- IInitializeWithStream ---
STDMETHODIMP ImageViewerThumbnailProvider::Initialize(IStream *pStream,
                                                      DWORD /*grfMode*/) {
  if (!pStream)
    return E_INVALIDARG;
  // 不 AddRef：shell 会在调用 GetThumbnail 期间保持 IStream 有效
  m_pStream = pStream;
  m_filePath.clear();
  m_initialized = true;
  return S_OK;
}

// --- IInitializeWithFile ---
STDMETHODIMP ImageViewerThumbnailProvider::Initialize(PCWSTR pszFilePath,
                                                      DWORD /*grfMode*/) {
  if (!pszFilePath)
    return E_INVALIDARG;
  m_filePath = pszFilePath;
  if (m_pStream) {
    m_pStream = nullptr;
  }
  m_initialized = true;
  return S_OK;
}

// --- IThumbnailProvider::GetThumbnail ---
STDMETHODIMP
ImageViewerThumbnailProvider::GetThumbnail(UINT cx, HBITMAP *phbmp,
                                           WTS_ALPHATYPE *pdwAlpha) {

  if (!phbmp || !pdwAlpha)
    return E_POINTER;
  *phbmp = nullptr;
  *pdwAlpha = WTSAT_RGB;

  if (!m_initialized)
    return E_FAIL;

  std::vector<uint8_t> pixels;
  int w = 0, h = 0;

  if (m_pStream) {
    if (!decodeFromStream(m_pStream, pixels, w, h))
      return E_FAIL;
  } else if (!m_filePath.empty()) {
    if (!decodeFromFile(m_filePath.c_str(), pixels, w, h))
      return E_FAIL;
  } else {
    return E_FAIL;
  }

  if (w <= 0 || h <= 0 || pixels.empty())
    return E_FAIL;

  // 计算输出尺寸：保持 aspect ratio，适配 cx（最大边）
  UINT tw = cx, th = cx;
  if (w >= h)
    th = (UINT)(cx * (float)h / w);
  else
    tw = (UINT)(cx * (float)w / h);
  if (tw < 1)
    tw = 1;
  if (th < 1)
    th = 1;

  if ((int)tw != w || (int)th != h) {
    std::vector<uint8_t> out(tw * th * 4);
    downscaleBox(pixels.data(), w, h, out.data(), (int)tw, (int)th);
    pixels = std::move(out);
    w = (int)tw;
    h = (int)th;
  }

  // 创建 HBITMAP（DIBSection）：BGRA 格式，GDI 要求
  // stb 给的是 RGBA8 → 转成 BGRA8
  const size_t sz = (size_t)w * h * 4;
  std::vector<uint8_t> bgra(sz);
  for (size_t i = 0; i < sz; i += 4) {
    bgra[i + 0] = pixels[i + 2]; // B
    bgra[i + 1] = pixels[i + 1]; // G
    bgra[i + 2] = pixels[i + 0]; // R
    bgra[i + 3] = pixels[i + 3]; // A
  }

  BITMAPINFOHEADER bih = {};
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = w;
  bih.biHeight = -h; // 负值 = 顶部到底部（GDI 默认是倒序的）
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;

  void *bits = nullptr;
  HDC hdc = GetDC(nullptr);
  HBITMAP hbmp = CreateDIBSection(hdc, (BITMAPINFO *)&bih, DIB_RGB_COLORS,
                                  &bits, nullptr, 0);
  if (hbmp && bits) {
    memcpy(bits, bgra.data(), sz);
    *phbmp = hbmp;
    *pdwAlpha = WTSAT_ARGB; // 有 alpha 通道
  } else if (hbmp) {
    DeleteObject(hbmp);
  }
  ReleaseDC(nullptr, hdc);

  return *phbmp ? S_OK : E_FAIL;
}

// ============================================================================
// 解码辅助
// ============================================================================
bool ImageViewerThumbnailProvider::decodeFromStream(
    IStream *pStream, std::vector<uint8_t> &outPixels, int &w, int &h) {
  // 1. 获取流大小
  LARGE_INTEGER li;
  ULARGE_INTEGER uli;
  if (FAILED(pStream->Seek(li /*offset*/ = {0}, STREAM_SEEK_END, &uli)))
    return false;
  LARGE_INTEGER zero = {0};
  if (FAILED(pStream->Seek(zero, STREAM_SEEK_SET, nullptr)))
    return false;

  const size_t size = (size_t)uli.QuadPart;
  if (size == 0 || size > 256u * 1024u * 1024u)
    return false; // 256MB 上限

  // 2. 读入内存
  std::vector<uint8_t> buf(size);
  ULONG read = 0;
  if (FAILED(pStream->Read(buf.data(), (ULONG)size, &read)) || read == 0)
    return false;

  // 3. stb 解码
  int channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(buf.data(), (int)read, &w, &h,
                                          &channels, STBI_rgb_alpha);
  if (!pixels)
    return false;

  outPixels.assign(pixels, pixels + (size_t)w * h * 4);
  stbi_image_free(pixels);
  return true;
}

bool ImageViewerThumbnailProvider::decodeFromFile(
    PCWSTR pszFilePath, std::vector<uint8_t> &outPixels, int &w, int &h) {

  // 用 _wfopen 避免 CreateFileW/HANDLE 的类型问题（编译器级联错误）
  FILE *fp = _wfopen(pszFilePath, L"rb");
  if (!fp)
    return false;

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }
  long fsize = ftell(fp);
  if (fsize <= 0 || fsize > 256 * 1024 * 1024) {
    fclose(fp);
    return false;
  }
  rewind(fp);

  std::vector<uint8_t> buf((size_t)fsize);
  size_t rd = fread(buf.data(), 1, (size_t)fsize, fp);
  fclose(fp);
  if (rd == 0)
    return false;

  int channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(buf.data(), (int)rd, &w, &h,
                                          &channels, STBI_rgb_alpha);
  if (!pixels)
    return false;
  outPixels.assign(pixels, pixels + (size_t)w * h * 4);
  stbi_image_free(pixels);
  return true;
}

// ============================================================================
// DLL 导出
// ============================================================================

// 外部创建函数（shell 通过 DllGetClassObject 调）
static HRESULT CreateThumbnail(REFCLSID /*clsid*/, REFIID riid, LPVOID *ppv) {
  auto *p = new (std::nothrow) ImageViewerThumbnailProvider();
  if (!p)
    return E_OUTOFMEMORY;
  HRESULT hr = p->QueryInterface(riid, ppv);
  p->Release(); // 构造时 refcount = 1，QueryInterface 里会 AddRef 返回给 shell
  return hr;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = nullptr;
  if (!IsEqualCLSID(rclsid, CLSID_ImageViewerThumbnail))
    return CLASS_E_CLASSNOTAVAILABLE;

  // CLSID 匹配 → 直接创建实例并返回请求的接口
  return CreateThumbnail(rclsid, riid, ppv);
}

STDAPI DllRegisterServer() {
  // 实际注册由 PowerShell 脚本完成，这里只占位
  // 脚本用 HKCU 写注册表，不需要管理员权限
  return S_OK;
}

STDAPI DllUnregisterServer() { return S_OK; }

STDAPI DllCanUnloadNow() {
  // 简单策略：永远不卸载（shell 会自己在空闲时释放 thumbnail host 进程）
  return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE /*hinstDLL*/, DWORD reason, LPVOID /*reserved*/) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls((HMODULE) nullptr); // 简化：我们没用到
  }
  return TRUE;
}
