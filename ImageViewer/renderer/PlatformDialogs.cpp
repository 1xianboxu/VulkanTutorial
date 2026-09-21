#include "PlatformDialogs.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>
#include <shobjidl.h>

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

#else

std::wstring openFileDialog(void *) { return L""; }
std::wstring openFolderDialog(void *) { return L""; }

#endif
