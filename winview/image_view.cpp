#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include "image_view.h"

using namespace std;

static const wchar_t *CLASS_NAME = L"PixImageView";
static const int32 LINE_STEP = 40;      // pixels scrolled by an arrow key or scroll bar arrow

//----------------------------------------------------------------------------------------------------------------------
ImageView::ImageView() : hwnd(NULL), pixels(NULL), width(0), height(0), zoomFactor(1.0), scrollX(0), scrollY(0),
                         dragging(false), dragScrollX(0), dragScrollY(0), updatingBars(false), zoomInCommand(0),
                         zoomOutCommand(0)
{
    dragStart.x = 0;
    dragStart.y = 0;
}

//----------------------------------------------------------------------------------------------------------------------
bool ImageView::registerClass(HINSTANCE instance)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;    // painted in WM_PAINT
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassExW(&wc) != 0;
}

//----------------------------------------------------------------------------------------------------------------------
bool ImageView::create(HWND parent, HINSTANCE instance, int32 zoomIn, int32 zoomOut)
{
    zoomInCommand = zoomIn;
    zoomOutCommand = zoomOut;
    hwnd = CreateWindowExW(0, CLASS_NAME, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | WS_CLIPSIBLINGS,
                           0, 0, 0, 0, parent, NULL, instance, this);
    if (!hwnd)
        return false;
    updateScrollBars();
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
LRESULT CALLBACK ImageView::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ImageView *view = (ImageView *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        view = (ImageView *)cs->lpCreateParams;
        view->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)view);
    }
    if (view)
        return view->handle(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::setPicture(const Picture *pic)
{
    if (!pic || pic->empty())
    {
        clear();
        return;
    }
    width = pic->width;
    height = pic->height;
    flat.clear();
    if (pic->hasAlpha)
    {
        flat.resize(pic->pixels.size());
        for (size_t i = 0; i < flat.size(); i++)
            flat[i] = onWhite(pic->pixels[i]);
        pixels = &flat[0];
    }
    else
        pixels = &pic->pixels[0];
    scrollX = 0;
    scrollY = 0;
    updateScrollBars();
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::clear()
{
    pixels = NULL;
    flat.clear();
    width = 0;
    height = 0;
    scrollX = 0;
    scrollY = 0;
    updateScrollBars();
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::setZoom(double zoom)
{
    RECT client;
    GetClientRect(hwnd, &client);
    int32 oldWidth = zoomedWidth();
    int32 oldHeight = zoomedHeight();
    // the fraction of the picture in the middle of the window
    double fx = oldWidth > client.right ? (scrollX + client.right / 2.0) / oldWidth : 0.5;
    double fy = oldHeight > client.bottom ? (scrollY + client.bottom / 2.0) / oldHeight : 0.5;
    zoomFactor = zoom;
    scrollX = (int32)floor(fx * zoomedWidth() - client.right / 2.0 + 0.5);
    scrollY = (int32)floor(fy * zoomedHeight() - client.bottom / 2.0 + 0.5);
    updateScrollBars();
}

//----------------------------------------------------------------------------------------------------------------------
SIZE ImageView::fullSize() const
{
    // the view has no border, so its window rectangle is the client area plus the scroll bars
    RECT r;
    GetWindowRect(hwnd, &r);
    SIZE size;
    size.cx = r.right - r.left;
    size.cy = r.bottom - r.top;
    return size;
}

//----------------------------------------------------------------------------------------------------------------------
int32 ImageView::zoomedWidth() const
{
    if (!pixels)
        return 0;
    double w = floor(width * zoomFactor + 0.5);
    return w < 1 ? 1 : (int32)w;
}

//----------------------------------------------------------------------------------------------------------------------
int32 ImageView::zoomedHeight() const
{
    if (!pixels)
        return 0;
    double h = floor(height * zoomFactor + 0.5);
    return h < 1 ? 1 : (int32)h;
}

//----------------------------------------------------------------------------------------------------------------------
bool ImageView::scrollable() const
{
    RECT client;
    GetClientRect(hwnd, &client);
    return pixels && (zoomedWidth() > client.right || zoomedHeight() > client.bottom);
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::origin(const RECT &client, int32 &x, int32 &y) const
{
    // top left corner of the zoomed picture in the window
    int32 zw = zoomedWidth();
    int32 zh = zoomedHeight();
    x = zw < client.right ? (client.right - zw) / 2 : -scrollX;
    y = zh < client.bottom ? (client.bottom - zh) / 2 : -scrollY;
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::updateScrollBars()
{
    if (updatingBars || !hwnd)
        return;
    updatingBars = true;
    // showing or hiding one scroll bar changes the room for the other: repeat until the client area stays the same
    for (int32 pass = 0; pass < 3; pass++)
    {
        RECT client;
        GetClientRect(hwnd, &client);
        int32 zw = zoomedWidth();
        int32 zh = zoomedHeight();
        int32 maxX = zw > client.right ? zw - client.right : 0;
        int32 maxY = zh > client.bottom ? zh - client.bottom : 0;
        scrollX = scrollX < 0 ? 0 : (scrollX > maxX ? maxX : scrollX);
        scrollY = scrollY < 0 ? 0 : (scrollY > maxY ? maxY : scrollY);

        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = zw > 0 ? zw - 1 : 0;
        si.nPage = (UINT)client.right;
        si.nPos = scrollX;
        SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);
        si.nMax = zh > 0 ? zh - 1 : 0;
        si.nPage = (UINT)client.bottom;
        si.nPos = scrollY;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

        RECT after;
        GetClientRect(hwnd, &after);
        if (after.right == client.right && after.bottom == client.bottom)
            break;
    }
    updatingBars = false;
    InvalidateRect(hwnd, NULL, FALSE);
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::scrollTo(int32 x, int32 y)
{
    if (x == scrollX && y == scrollY)
        return;
    scrollX = x;
    scrollY = y;
    updateScrollBars();
    UpdateWindow(hwnd);
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::onScroll(int32 bar, int32 code)
{
    RECT client;
    GetClientRect(hwnd, &client);
    int32 page = bar == SB_HORZ ? client.right : client.bottom;
    int32 pos = bar == SB_HORZ ? scrollX : scrollY;
    switch (code)
    {
    case SB_LINEUP:
        pos -= LINE_STEP;
        break;
    case SB_LINEDOWN:
        pos += LINE_STEP;
        break;
    case SB_PAGEUP:
        pos -= page;
        break;
    case SB_PAGEDOWN:
        pos += page;
        break;
    case SB_TOP:
        pos = 0;
        break;
    case SB_BOTTOM:
        pos = 0x3FFFFFFF;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
    {
        // the 32 bit position (the message only carries 16 bits)
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd, bar, &si);
        pos = si.nTrackPos;
        break;
    }
    default:
        return;
    }
    if (bar == SB_HORZ)
        scrollTo(pos, scrollY);
    else
        scrollTo(scrollX, pos);
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::paint()
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    if (pixels)
    {
        int32 ox = 0;
        int32 oy = 0;
        origin(client, ox, oy);
        int32 zw = zoomedWidth();
        int32 zh = zoomedHeight();
        RECT image = {ox, oy, ox + zw, oy + zh};
        RECT visible;
        if (IntersectRect(&visible, &image, &ps.rcPaint))
        {
            // only the picture pixels that cover the visible part are stretched; picture pixel i starts at
            // ox + round(i * zw / width)
            double sx = (double)zw / width;
            double sy = (double)zh / height;
            int32 x0 = (int32)floor((visible.left - ox) / sx);
            int32 x1 = (int32)ceil((visible.right - ox) / sx);
            int32 y0 = (int32)floor((visible.top - oy) / sy);
            int32 y1 = (int32)ceil((visible.bottom - oy) / sy);
            x0 = x0 < 0 ? 0 : x0;
            y0 = y0 < 0 ? 0 : y0;
            x1 = x1 > width ? width : x1;
            y1 = y1 > height ? height : y1;
            int32 dx0 = ox + (int32)floor(x0 * sx + 0.5);
            int32 dx1 = ox + (int32)floor(x1 * sx + 0.5);
            int32 dy0 = oy + (int32)floor(y0 * sy + 0.5);
            int32 dy1 = oy + (int32)floor(y1 * sy + 0.5);

            // a top-down DIB of rows y0..y1-1 (StretchDIBits counts source rows of top-down DIBs unreliably)
            BITMAPINFO bi;
            ZeroMemory(&bi, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -(y1 - y0);
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            // smooth when shrinking, sharp pixels when magnifying
            SetStretchBltMode(dc, zoomFactor < 1.0 ? HALFTONE : COLORONCOLOR);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchDIBits(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, x0, 0, x1 - x0, y1 - y0,
                          pixels + (size_t)y0 * width, &bi, DIB_RGB_COLORS, SRCCOPY);
            ExcludeClipRect(dc, image.left, image.top, image.right, image.bottom);
        }
    }
    FillRect(dc, &ps.rcPaint, GetSysColorBrush(COLOR_APPWORKSPACE));
    EndPaint(hwnd, &ps);
}

//----------------------------------------------------------------------------------------------------------------------
LRESULT ImageView::handle(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
        paint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        updateScrollBars();
        return 0;

    case WM_HSCROLL:
        onScroll(SB_HORZ, LOWORD(wParam));
        return 0;

    case WM_VSCROLL:
        onScroll(SB_VERT, LOWORD(wParam));
        return 0;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    {
        int32 delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int32 keys = GET_KEYSTATE_WPARAM(wParam);
        if (msg == WM_MOUSEWHEEL && (keys & MK_CONTROL))
        {
            SendMessageW(GetParent(hwnd), WM_COMMAND, (WPARAM)(delta > 0 ? zoomInCommand : zoomOutCommand), 0);
            return 0;
        }
        int32 step = MulDiv(delta, 3 * LINE_STEP, WHEEL_DELTA);
        if (msg == WM_MOUSEHWHEEL)
            scrollTo(scrollX + step, scrollY);
        else if (keys & MK_SHIFT)
            scrollTo(scrollX - step, scrollY);
        else
            scrollTo(scrollX, scrollY - step);
        return msg == WM_MOUSEHWHEEL ? TRUE : 0;
    }

    case WM_KEYDOWN:
    {
        RECT client;
        GetClientRect(hwnd, &client);
        switch (wParam)
        {
        case VK_LEFT:
            scrollTo(scrollX - LINE_STEP, scrollY);
            return 0;
        case VK_RIGHT:
            scrollTo(scrollX + LINE_STEP, scrollY);
            return 0;
        case VK_UP:
            scrollTo(scrollX, scrollY - LINE_STEP);
            return 0;
        case VK_DOWN:
            scrollTo(scrollX, scrollY + LINE_STEP);
            return 0;
        case VK_PRIOR:
            scrollTo(scrollX, scrollY - client.bottom);
            return 0;
        case VK_NEXT:
            scrollTo(scrollX, scrollY + client.bottom);
            return 0;
        case VK_HOME:
            scrollTo(0, 0);
            return 0;
        case VK_END:
            scrollTo(0x3FFFFFFF, 0x3FFFFFFF);
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (scrollable())
        {
            dragging = true;
            dragStart.x = GET_X_LPARAM(lParam);
            dragStart.y = GET_Y_LPARAM(lParam);
            dragScrollX = scrollX;
            dragScrollY = scrollY;
            SetCapture(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (dragging)
            scrollTo(dragScrollX - (GET_X_LPARAM(lParam) - dragStart.x),
                     dragScrollY - (GET_Y_LPARAM(lParam) - dragStart.y));
        return 0;

    case WM_LBUTTONUP:
        if (dragging)
            ReleaseCapture();
        return 0;

    case WM_CAPTURECHANGED:
        dragging = false;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursor(NULL, scrollable() ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
