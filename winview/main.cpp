#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include "main_window.h"

using namespace std;

// wine_get_dos_file_name: exported by Wine's kernel32 only
typedef wchar_t *(CDECL *DosFileNameFunction)(const char *unixPath);

//----------------------------------------------------------------------------------------------------------------------
static wstring windowsPath(const wstring &path)
{
    // under Wine a Linux path ("/home/...") can be given on the command line: convert it to a Windows path
    if (path.empty() || path[0] != L'/')
        return path;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    void *address = kernel ? (void *)GetProcAddress(kernel, "wine_get_dos_file_name") : NULL;
    if (!address)
        return path;
    DosFileNameFunction dosFileName = (DosFileNameFunction)address;
    int32 n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, NULL, 0, NULL, NULL);
    string unixPath((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &unixPath[0], n, NULL, NULL);
    wchar_t *converted = dosFileName(unixPath.c_str());
    if (!converted)
        return path;
    wstring result = converted;
    HeapFree(GetProcessHeap(), 0, converted);
    return result;
}

//----------------------------------------------------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    INITCOMMONCONTROLSEX controls;
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return 1;

    wstring error;
    if (!pictureStartup(error))
        MessageBoxW(NULL, (error + L"\nOnly PIX files can be opened.").c_str(), L"PIX Viewer", MB_OK | MB_ICONWARNING);

    MainWindow window;
    if (!MainWindow::registerClass(instance) || !window.create(instance, show))
    {
        MessageBoxW(NULL, L"Cannot create the main window.", L"PIX Viewer", MB_OK | MB_ICONERROR);
        pictureShutdown();
        CoUninitialize();
        return 1;
    }

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        if (argc > 1)
            window.loadFile(windowsPath(argv[1]));
        LocalFree(argv);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (!window.accelerators || !TranslateAcceleratorW(window.hwnd, window.accelerators, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    pictureShutdown();
    CoUninitialize();
    return (int)msg.wParam;
}
