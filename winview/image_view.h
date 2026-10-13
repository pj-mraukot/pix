#ifndef IMAGE_VIEW_H
#define IMAGE_VIEW_H

#include <windows.h>
#include "picture.h"

using namespace std;

// Shows a picture at a zoom factor in a child window with scroll bars. A picture smaller than the window is centred.
// Scrolling: scroll bars, mouse wheel (Shift: sideways), arrow keys, Page Up/Down, Home/End, dragging with the left
// button. Ctrl + wheel sends the parent IDM_ZOOM_IN / IDM_ZOOM_OUT commands.
class ImageView
{
public:
    HWND hwnd;

    ImageView();

    static bool registerClass(HINSTANCE instance);
    bool create(HWND parent, HINSTANCE instance, int32 zoomInCommand, int32 zoomOutCommand);

    // shows pic (kept by the caller while shown); clear() shows nothing
    void setPicture(const Picture *pic);
    void clear();
    // keeps the point in the middle of the window where it is
    void setZoom(double zoom);

    bool hasPicture() const { return pixels != NULL; }
    int32 pictureWidth() const { return width; }
    int32 pictureHeight() const { return height; }
    double zoom() const { return zoomFactor; }
    // the window size without scroll bars: the room a fitted picture has
    SIZE fullSize() const;

private:
    const dword *pixels;    // the picture's pixels, or 'flat' for a picture with transparency
    vector<dword> flat;     // the picture put on white, as it is saved in PIX, BMP and JPEG files
    int32 width;
    int32 height;
    double zoomFactor;
    int32 scrollX;
    int32 scrollY;
    bool dragging;
    POINT dragStart;
    int32 dragScrollX;
    int32 dragScrollY;
    bool updatingBars;
    int32 zoomInCommand;
    int32 zoomOutCommand;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);

    int32 zoomedWidth() const;
    int32 zoomedHeight() const;
    bool scrollable() const;
    void origin(const RECT &client, int32 &x, int32 &y) const;
    void paint();
    void updateScrollBars();
    void scrollTo(int32 x, int32 y);
    void onScroll(int32 bar, int32 code);
};

#endif
