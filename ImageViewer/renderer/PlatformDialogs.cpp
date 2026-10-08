#include "PlatformDialogs.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>
#include <cstdio>
#include <shobjidl.h>
#include <vector>
#include <wincodec.h>

std::wstring openFileDialog(void *ownerWindow)
{
    wchar_t file[32768] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)ownerWindow;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"图片文件\0*.psd;*.jpg;*.jpeg;*.png;*.bmp;*.tga;*.gif;*.hdr;*.pic;*.ppm;*.pgm\0"
                      L"所有文件\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn))
        return file;
    return L"";
}

std::wstring openFolderDialog(void *ownerWindow)
{
    std::wstring result;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInit = SUCCEEDED(hr);

    IFileOpenDialog *dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileOpenDialog, (void **)&dlg)))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

        if (SUCCEEDED(dlg->Show((HWND)ownerWindow)))
        {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                wchar_t *path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (comInit)
        CoUninitialize();
    return result;
}

std::wstring savePngDialog(void *ownerWindow,
                           const std::wstring &suggestedName)
{
    wchar_t file[32768] = L"";
    if (!suggestedName.empty())
        wcsncpy_s(file, suggestedName.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)ownerWindow;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"PNG 图像\0*.png\0所有文件\0*.*\0";
    ofn.lpstrDefExt = L"png";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn))
        return file;
    return L"";
}

bool saveRgbaPng(const std::wstring &path, const unsigned char *rgba,
                 int width, int height)
{
    if (path.empty() || rgba == nullptr || width <= 0 || height <= 0)
        return false;

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                              COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(initHr);
    IWICImagingFactory *factory = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    IPropertyBag2 *properties = nullptr;
    const wchar_t *stage = L"CoCreateInstance";

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        stage = L"CreateStream";
        hr = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr)) {
        stage = L"InitializeFromFilename";
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(hr)) {
        stage = L"CreateEncoder";
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
        stage = L"EncoderInitialize";
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
        stage = L"CreateNewFrame";
        hr = encoder->CreateNewFrame(&frame, &properties);
    }
    if (SUCCEEDED(hr)) {
        stage = L"FrameInitialize";
        hr = frame->Initialize(properties);
    }
    if (SUCCEEDED(hr)) {
        stage = L"SetSize";
        hr = frame->SetSize((UINT)width, (UINT)height);
    }

    // Windows PNG encoder accepts 32bppBGRA on all supported versions.
    // The viewer keeps pixels as RGBA, so swap R/B once at the file boundary.
    std::vector<BYTE> bgra((size_t)width * height * 4);
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) {
        stage = L"SetPixelFormat";
        hr = frame->SetPixelFormat(&format);
    }
    if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
        hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) {
        stage = L"WritePixels";
        const UINT stride = (UINT)width * 4;
        const UINT bytes = stride * (UINT)height;
        hr = frame->WritePixels((UINT)height, stride, bytes, bgra.data());
    }
    if (SUCCEEDED(hr)) {
        stage = L"FrameCommit";
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        stage = L"EncoderCommit";
        hr = encoder->Commit();
    }

    if (FAILED(hr))
        fwprintf(stderr, L"WIC PNG save failed at %ls: 0x%08lX\n", stage,
                 (unsigned long)hr);

    if (properties) properties->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (uninit) CoUninitialize();
    return SUCCEEDED(hr);
}

std::wstring openBrushPresetDialog(void *ownerWindow)
{
    wchar_t file[32768] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)ownerWindow;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"画笔文件（Photoshop / ImageViewer）\0*.abr;*.ivbrush\0"
                      L"Photoshop 画笔\0*.abr\0"
                      L"ImageViewer 画笔预设\0*.ivbrush\0"
                      L"所有文件\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? file : L"";
}

std::wstring saveBrushPresetDialog(void *ownerWindow,
                                   const std::wstring &suggestedName)
{
    wchar_t file[32768] = L"";
    if (!suggestedName.empty())
        wcsncpy_s(file, suggestedName.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)ownerWindow;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"ImageViewer 画笔预设\0*.ivbrush\0所有文件\0*.*\0";
    ofn.lpstrDefExt = L"ivbrush";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? file : L"";
}

#else

std::wstring openFileDialog(void *) { return L""; }
std::wstring openFolderDialog(void *) { return L""; }
std::wstring savePngDialog(void *, const std::wstring &) { return L""; }
bool saveRgbaPng(const std::wstring &, const unsigned char *, int, int)
{
    return false;
}
std::wstring openBrushPresetDialog(void *) { return L""; }
std::wstring saveBrushPresetDialog(void *, const std::wstring &) { return L""; }

#endif
