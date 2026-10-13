#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <math.h>
#include <wchar.h>
#include "main_window.h"
#include "../src/pix_license.h"

using namespace std;

static const wchar_t *CLASS_NAME = L"PixViewerWindow";
static const wchar_t *APP_NAME = L"PIX Viewer";

static const double ZOOM_STEP = 1.25;
static const double ZOOM_MIN = 0.02;
static const double ZOOM_MAX = 32.0;

// save dialog file types; the PIX type chosen there selects the algorithm
#define TYPE_PIX1 0
#define TYPE_PIX2 1
#define TYPE_PNG  2

#define STATUS_MESSAGE 0
#define STATUS_INFO    1
#define STATUS_ZOOM    2

// status bar tips of menu items
struct MenuTip
{
    int32 id;
    const wchar_t *tip;
};

static const MenuTip menuTips[] = {
    {IDM_OPEN, L"Open a PIX file or an image (PNG, JPEG, BMP, GIF, TIFF, ...)"},
    {IDM_SAVE_AS, L"Save the image as PIX, PNG, BMP or JPEG (chosen by file extension)"},
    {IDM_QUIT, L"Close the viewer"},
    {IDM_TO_PIX, L"Choose a PNG, JPEG, BMP, ... image and save it as PIX"},
    {IDM_FROM_PIX, L"Choose a PIX file and save it as PNG, BMP or JPEG"},
    {IDM_ALGORITHM1, L"Save PIX files with algorithm 1"},
    {IDM_ALGORITHM2, L"Save PIX files with algorithm 2 (smaller files)"},
    {IDM_ZOOM_IN, L"Magnify the image"},
    {IDM_ZOOM_OUT, L"Shrink the image"},
    {IDM_ACTUAL_SIZE, L"Show the image at 100%"},
    {IDM_FIT, L"Shrink large images to fit the window"},
    {IDM_ABOUT, L"About PIX Viewer"},
};

// shows the wait cursor while it exists
struct WaitCursor
{
    HCURSOR previous;

    WaitCursor() { previous = SetCursor(LoadCursor(NULL, IDC_WAIT)); }
    ~WaitCursor() { SetCursor(previous); }
};

// measures elapsed time
struct Stopwatch
{
    LARGE_INTEGER start;

    Stopwatch() { QueryPerformanceCounter(&start); }

    double seconds() const
    {
        LARGE_INTEGER now;
        LARGE_INTEGER frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
    }
};

//----------------------------------------------------------------------------------------------------------------------
static wstring formatSize(long long bytes)
{
    wchar_t buf[64];
    if (bytes < 1024)
        swprintf(buf, 64, L"%d bytes", (int)bytes);
    else if (bytes < 1024 * 1024)
        swprintf(buf, 64, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        swprintf(buf, 64, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else
        swprintf(buf, 64, L"%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

//----------------------------------------------------------------------------------------------------------------------
static wstring percentOfRaw(long long bytes, const Picture &pic)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f%%", 100.0 * (double)bytes / ((double)pic.width * pic.height * 3));
    return buf;
}

//----------------------------------------------------------------------------------------------------------------------
static FileType fileType(const wchar_t *name, const wchar_t *patterns)
{
    FileType type;
    type.name = name;
    type.patterns = patterns;
    return type;
}

//----------------------------------------------------------------------------------------------------------------------
MainWindow::MainWindow() : hwnd(NULL), accelerators(NULL), instance(NULL), status(NULL), menu(NULL), currentFileSize(0),
                           algorithm(2), fit(true)
{
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::registerClass(HINSTANCE instance)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1));
    if (!wc.hIcon)
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassExW(&wc) != 0 && ImageView::registerClass(instance);
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::create(HINSTANCE inst, int32 show)
{
    instance = inst;
    wchar_t dir[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, dir) > 0)
        lastDir = dir;
    openPatterns = readablePatterns();

    // 1000 x 750 at 96 dpi
    HDC screen = GetDC(NULL);
    int32 dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, CLASS_NAME, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1000, dpi, 96), MulDiv(750, dpi, 96),
                           NULL, NULL, instance, this);
    if (!hwnd)
        return false;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
LRESULT CALLBACK MainWindow::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainWindow *window = (MainWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        window = (MainWindow *)cs->lpCreateParams;
        window->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)window);
    }
    if (window)
        return window->handle(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::createMenu()
{
    menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE_AS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_QUIT, L"&Quit\tCtrl+Q");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");

    HMENU convert = CreatePopupMenu();
    AppendMenuW(convert, MF_STRING, IDM_TO_PIX, L"Image to &PIX...\tCtrl+E");
    AppendMenuW(convert, MF_STRING, IDM_FROM_PIX, L"PIX to &Image...\tCtrl+D");
    AppendMenuW(convert, MF_SEPARATOR, 0, NULL);
    HMENU algorithms = CreatePopupMenu();
    AppendMenuW(algorithms, MF_STRING, IDM_ALGORITHM1, L"Algorithm &1: nibble codes + zlib");
    AppendMenuW(algorithms, MF_STRING, IDM_ALGORITHM2, L"Algorithm &2: prediction + arithmetic coding");
    AppendMenuW(convert, MF_POPUP, (UINT_PTR)algorithms, L"PIX &Algorithm");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)convert, L"&Convert");

    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING, IDM_ZOOM_IN, L"Zoom &In\tCtrl++");
    AppendMenuW(viewMenu, MF_STRING, IDM_ZOOM_OUT, L"Zoom &Out\tCtrl+-");
    AppendMenuW(viewMenu, MF_STRING, IDM_ACTUAL_SIZE, L"&Actual Size\tCtrl+0");
    AppendMenuW(viewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(viewMenu, MF_STRING, IDM_FIT, L"&Fit to Window\tCtrl+F");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)viewMenu, L"&View");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L"&Help");
    SetMenu(hwnd, menu);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::createAccelerators()
{
    ACCEL keys[] = {
        {FCONTROL | FVIRTKEY, 'O', IDM_OPEN},
        {FCONTROL | FSHIFT | FVIRTKEY, 'S', IDM_SAVE_AS},
        {FCONTROL | FVIRTKEY, 'Q', IDM_QUIT},
        {FCONTROL | FVIRTKEY, 'E', IDM_TO_PIX},
        {FCONTROL | FVIRTKEY, 'D', IDM_FROM_PIX},
        {FCONTROL | FVIRTKEY, VK_OEM_PLUS, IDM_ZOOM_IN},       // Ctrl+= and Ctrl++ on the main keyboard
        {FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PLUS, IDM_ZOOM_IN},
        {FCONTROL | FVIRTKEY, VK_ADD, IDM_ZOOM_IN},
        {FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_ZOOM_OUT},
        {FCONTROL | FVIRTKEY, VK_SUBTRACT, IDM_ZOOM_OUT},
        {FCONTROL | FVIRTKEY, '0', IDM_ACTUAL_SIZE},
        {FCONTROL | FVIRTKEY, VK_NUMPAD0, IDM_ACTUAL_SIZE},
        {FCONTROL | FVIRTKEY, 'F', IDM_FIT},
    };
    accelerators = CreateAcceleratorTableW(keys, (int)(sizeof(keys) / sizeof(keys[0])));
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::layout()
{
    RECT client;
    GetClientRect(hwnd, &client);
    SendMessageW(status, WM_SIZE, 0, 0);
    RECT bar;
    GetWindowRect(status, &bar);
    int32 barHeight = bar.bottom - bar.top;
    MoveWindow(view.hwnd, 0, 0, client.right, client.bottom - barHeight > 0 ? client.bottom - barHeight : 0, TRUE);

    // the information and zoom parts are as wide as their text, the message gets the rest
    HDC dc = GetDC(status);
    HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(status, WM_GETFONT, 0, 0));
    int32 widths[2] = {0, 0};
    for (int32 part = STATUS_INFO; part <= STATUS_ZOOM; part++)
    {
        int32 length = (int32)LOWORD(SendMessageW(status, SB_GETTEXTLENGTHW, part, 0));
        wstring text((size_t)length + 1, L'\0');
        SendMessageW(status, SB_GETTEXTW, part, (LPARAM)&text[0]);
        SIZE size = {0, 0};
        if (length > 0)
            GetTextExtentPoint32W(dc, text.c_str(), length, &size);
        widths[part - 1] = length > 0 ? size.cx + 24 : 0;
    }
    SelectObject(dc, old);
    ReleaseDC(status, dc);
    int32 grip = GetSystemMetrics(SM_CXVSCROLL);
    int32 edges[3];
    edges[2] = -1;
    edges[1] = client.right - grip - widths[1];
    edges[0] = edges[1] - widths[0];
    if (edges[0] < 0)
        edges[0] = 0;
    if (edges[1] < edges[0])
        edges[1] = edges[0];
    SendMessageW(status, SB_SETPARTS, 3, (LPARAM)edges);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::setStatusText(int32 part, const wstring &text)
{
    SendMessageW(status, SB_SETTEXTW, (WPARAM)part, (LPARAM)text.c_str());
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::showMessage(const wstring &text)
{
    message = text;
    setStatusText(STATUS_MESSAGE, message);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::updateActions()
{
    bool has = !picture.empty();
    EnableMenuItem(menu, IDM_SAVE_AS, MF_BYCOMMAND | (has ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_ZOOM_IN, MF_BYCOMMAND | (has && view.zoom() < ZOOM_MAX ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_ZOOM_OUT, MF_BYCOMMAND | (has && view.zoom() > ZOOM_MIN ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_ACTUAL_SIZE, MF_BYCOMMAND | (has ? MF_ENABLED : MF_GRAYED));
    CheckMenuRadioItem(menu, IDM_ALGORITHM1, IDM_ALGORITHM2, algorithm == 2 ? IDM_ALGORITHM2 : IDM_ALGORITHM1,
                       MF_BYCOMMAND);
    CheckMenuItem(menu, IDM_FIT, MF_BYCOMMAND | (fit ? MF_CHECKED : MF_UNCHECKED));
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::updateStatus()
{
    if (picture.empty())
    {
        setStatusText(STATUS_INFO, L"");
        setStatusText(STATUS_ZOOM, L"");
        layout();
        return;
    }
    wchar_t buf[512];
    swprintf(buf, 512, L"%ls   %d x %d", fileNameOf(currentPath).c_str(), picture.width, picture.height);
    wstring info = buf;
    if (currentFileSize > 0)
        info += L"   " + formatSize(currentFileSize) + L" (" + percentOfRaw(currentFileSize, picture) + L" of raw RGB)";
    setStatusText(STATUS_INFO, info);
    swprintf(buf, 512, L"%d%%", (int)floor(view.zoom() * 100 + 0.5));
    setStatusText(STATUS_ZOOM, buf);
    layout();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::setZoom(double zoom)
{
    if (zoom < ZOOM_MIN)
        zoom = ZOOM_MIN;
    if (zoom > ZOOM_MAX)
        zoom = ZOOM_MAX;
    view.setZoom(zoom);
    updateActions();
    updateStatus();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::applyFit()
{
    if (!view.hasPicture())
        return;
    SIZE area = view.fullSize();
    double zoom = (double)area.cx / view.pictureWidth();
    double zoomY = (double)area.cy / view.pictureHeight();
    if (zoomY < zoom)
        zoom = zoomY;
    setZoom(zoom < 1.0 ? zoom : 1.0);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::showPicture(const wstring &path)
{
    currentPath = path;
    currentFileSize = fileSizeOf(path);
    view.setPicture(&picture);
    if (fit)
        applyFit();
    else
        setZoom(view.zoom());
    SetWindowTextW(hwnd, (fileNameOf(path) + L" - " + APP_NAME).c_str());
    updateActions();
    updateStatus();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::showError(const wstring &title, const wstring &text)
{
    MessageBoxW(hwnd, text.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::loadFile(const wstring &path)
{
    Picture loaded;
    wstring error;
    bool pix = isPixFile(path);
    double seconds = 0;
    {
        WaitCursor wait;
        Stopwatch watch;
        if (!readPicture(path, loaded, error))
        {
            showError(L"Open", L"Cannot open " + path + L":\n" + error);
            return false;
        }
        seconds = watch.seconds();
    }
    picture.pixels.swap(loaded.pixels);
    picture.width = loaded.width;
    picture.height = loaded.height;
    picture.hasAlpha = loaded.hasAlpha;
    lastDir = folderOf(path);
    if (pix)
    {
        wchar_t buf[64];
        swprintf(buf, 64, L"Decoded PIX in %.3f s", seconds);
        showMessage(buf);
    }
    else
        showMessage(L"");
    showPicture(path);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::savePix(const wstring &path, wstring &error)
{
    double seconds = 0;
    {
        WaitCursor wait;
        Stopwatch watch;
        if (!writePix(path, picture, algorithm, error))
            return false;
        seconds = watch.seconds();
    }
    long long size = fileSizeOf(path);
    wchar_t buf[512];
    swprintf(buf, 512, L"Saved %ls: %ls (%ls of raw RGB), algorithm %d, encoded in %.3f s", fileNameOf(path).c_str(),
             formatSize(size).c_str(), percentOfRaw(size, picture).c_str(), algorithm, seconds);
    showMessage(buf);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::saveImage(const wstring &path, wstring &error)
{
    int32 format = formatOfPath(path);
    if (format == FORMAT_PIX)
        return savePix(path, error);
    if (format == FORMAT_NONE)
    {
        error = L"Unknown file type: use .pix, .png, .bmp or .jpg.";
        return false;
    }
    {
        WaitCursor wait;
        if (!writeImage(path, format, picture, error))
            return false;
    }
    showMessage(L"Saved " + fileNameOf(path) + L": " + formatSize(fileSizeOf(path)));
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
vector<FileType> MainWindow::openTypes() const
{
    vector<FileType> types;
    types.push_back(fileType(L"Images", openPatterns.c_str()));
    types.push_back(fileType(L"PIX images (*.pix)", L"*.pix"));
    types.push_back(fileType(L"All files (*.*)", L"*.*"));
    return types;
}

//----------------------------------------------------------------------------------------------------------------------
vector<FileType> MainWindow::pixTypes() const
{
    vector<FileType> types;
    types.push_back(fileType(L"PIX algorithm 1 (*.pix)", L"*.pix"));
    types.push_back(fileType(L"PIX algorithm 2, smaller files (*.pix)", L"*.pix"));
    return types;
}

//----------------------------------------------------------------------------------------------------------------------
vector<FileType> MainWindow::imageTypes() const
{
    vector<FileType> types;
    types.push_back(fileType(L"PNG images (*.png)", L"*.png"));
    types.push_back(fileType(L"BMP images (*.bmp)", L"*.bmp"));
    types.push_back(fileType(L"JPEG images (*.jpg, *.jpeg)", L"*.jpg;*.jpeg"));
    return types;
}

//----------------------------------------------------------------------------------------------------------------------
vector<FileType> MainWindow::saveTypes() const
{
    vector<FileType> types = pixTypes();
    vector<FileType> images = imageTypes();
    for (size_t i = 0; i < images.size(); i++)
        types.push_back(images[i]);
    return types;
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::askSource(const wstring &title, const vector<FileType> &types, wstring &path)
{
    return askOpenFile(hwnd, title, types, lastDir, path);
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::askTarget(const wstring &title, const vector<FileType> &types, const wstring &suggestedName,
                           int32 &type, wstring &path)
{
    return askSaveFile(hwnd, title, types, suggestedName, type, lastDir, path);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::open()
{
    wstring path;
    if (askSource(L"Open Image", openTypes(), path))
        loadFile(path);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::saveAs()
{
    if (picture.empty())
        return;
    bool pix = isPixFile(currentPath);
    wstring suggested = baseNameOf(currentPath) + (pix ? L".png" : L".pix");
    // a PIX file starts with the PIX type of the current algorithm selected
    int32 type = pix ? TYPE_PNG : (algorithm == 2 ? TYPE_PIX2 : TYPE_PIX1);
    wstring path;
    if (!askTarget(L"Save Image As", saveTypes(), suggested, type, path))
        return;
    if (type == TYPE_PIX1 || type == TYPE_PIX2)
        algorithm = type == TYPE_PIX2 ? 2 : 1;
    updateActions();
    wstring error;
    if (!saveImage(path, error))
        showError(L"Save As", L"Cannot save " + path + L":\n" + error);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::imageToPix()
{
    wstring source;
    if (!askSource(L"Image to PIX - choose image", openTypes(), source) || !loadFile(source))
        return;
    if (isPixFile(source))
    {
        showError(L"Image to PIX", L"This file is already a PIX image.");
        return;
    }
    int32 type = algorithm == 2 ? TYPE_PIX2 : TYPE_PIX1;
    wstring target;
    if (!askTarget(L"Image to PIX - save as", pixTypes(), baseNameOf(source) + L".pix", type, target))
        return;
    algorithm = type == TYPE_PIX2 ? 2 : 1;
    updateActions();
    if (extensionOf(target) != L"pix")
        target += L".pix";
    wstring error;
    if (!savePix(target, error))
        showError(L"Image to PIX", L"Cannot save " + target + L":\n" + error);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::pixToImage()
{
    vector<FileType> types;
    types.push_back(fileType(L"PIX images (*.pix)", L"*.pix"));
    types.push_back(fileType(L"All files (*.*)", L"*.*"));
    wstring source;
    if (!askSource(L"PIX to Image - choose PIX file", types, source))
        return;
    if (!isPixFile(source))
    {
        showError(L"PIX to Image", source + L" is not a PIX file.");
        return;
    }
    if (!loadFile(source))
        return;
    int32 type = 0;
    wstring target;
    if (!askTarget(L"PIX to Image - save as", imageTypes(), baseNameOf(source) + L".png", type, target))
        return;
    wstring error;
    if (formatOfPath(target) == FORMAT_PIX)
        error = L"Choose PNG, BMP or JPEG (use Save As to write PIX files).";
    if (!error.empty() || !saveImage(target, error))
        showError(L"PIX to Image", L"Cannot save " + target + L":\n" + error);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::about()
{
    wstring text = L"PIX Viewer for Windows\n\n"
                   L"Views PIX images and the image formats Windows can read (PNG, JPEG, BMP, GIF, TIFF, ...), "
                   L"and converts images to and from PIX.\n\n"
                   L"PIX is a lossless 24 bit RGB format with two algorithms:\n"
                   L"1: 16x16 blocks of nibble codes (colour caches, runs, MED predicted residuals), compressed with "
                   L"zlib\n"
                   L"2: per pixel prediction and adaptive binary arithmetic coding (smaller files, slower)\n\n";
    // the BSD 2-clause license asks binaries to carry the copyright notice and the license
    text += widen(PIX_COPYRIGHT) + L"\n\n" + widen(PIX_LICENSE);
    MessageBoxW(hwnd, text.c_str(), L"About PIX Viewer", MB_OK | MB_ICONINFORMATION);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::command(int32 id)
{
    switch (id)
    {
    case IDM_OPEN:
        open();
        break;
    case IDM_SAVE_AS:
        saveAs();
        break;
    case IDM_QUIT:
        DestroyWindow(hwnd);
        break;
    case IDM_TO_PIX:
        imageToPix();
        break;
    case IDM_FROM_PIX:
        pixToImage();
        break;
    case IDM_ALGORITHM1:
    case IDM_ALGORITHM2:
        algorithm = id == IDM_ALGORITHM2 ? 2 : 1;
        updateActions();
        break;
    case IDM_ZOOM_IN:
        if (!picture.empty())
        {
            fit = false;
            setZoom(view.zoom() * ZOOM_STEP);
        }
        break;
    case IDM_ZOOM_OUT:
        if (!picture.empty())
        {
            fit = false;
            setZoom(view.zoom() / ZOOM_STEP);
        }
        break;
    case IDM_ACTUAL_SIZE:
        if (!picture.empty())
        {
            fit = false;
            setZoom(1.0);
        }
        break;
    case IDM_FIT:
        fit = !fit;
        updateActions();
        if (fit)
            applyFit();
        break;
    case IDM_ABOUT:
        about();
        break;
    default:
        break;
    }
}

//----------------------------------------------------------------------------------------------------------------------
LRESULT MainWindow::handle(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        createMenu();
        createAccelerators();
        status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                                 NULL, instance, NULL);
        if (!status || !view.create(hwnd, instance, IDM_ZOOM_IN, IDM_ZOOM_OUT))
            return -1;
        DragAcceptFiles(hwnd, TRUE);
        updateActions();
        showMessage(L"Open an image (Ctrl+O) or drop a file here");
        return 0;

    case WM_SIZE:
        if (status && view.hwnd)
        {
            layout();
            if (fit)
                applyFit();
        }
        return 0;

    case WM_SETFOCUS:
        if (view.hwnd)
            SetFocus(view.hwnd);
        return 0;

    case WM_COMMAND:
        command(LOWORD(wParam));
        return 0;

    case WM_MENUSELECT:
    {
        // a menu item's tip in the status bar while the menu is open
        int32 id = LOWORD(wParam);
        const wchar_t *tip = L"";
        if (!(HIWORD(wParam) & MF_POPUP))
            for (size_t i = 0; i < sizeof(menuTips) / sizeof(menuTips[0]); i++)
                if (menuTips[i].id == id)
                    tip = menuTips[i].tip;
        setStatusText(STATUS_MESSAGE, tip);
        return 0;
    }

    case WM_EXITMENULOOP:
        setStatusText(STATUS_MESSAGE, message);
        return 0;

    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wParam;
        UINT length = DragQueryFileW(drop, 0, NULL, 0);
        wstring path((size_t)length + 1, L'\0');
        DragQueryFileW(drop, 0, &path[0], length + 1);
        path.resize(length);
        DragFinish(drop);
        if (!path.empty())
            loadFile(path);
        return 0;
    }

    case WM_DESTROY:
        if (accelerators)
            DestroyAcceleratorTable(accelerators);
        accelerators = NULL;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
