#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <windows.h>
#include <string>
#include "picture.h"
#include "image_view.h"
#include "file_dialog.h"

using namespace std;

// menu commands
#define IDM_OPEN        101
#define IDM_SAVE_AS     102
#define IDM_QUIT        103
#define IDM_TO_PIX      201
#define IDM_FROM_PIX    202
#define IDM_ALGORITHM1  203
#define IDM_ALGORITHM2  204
#define IDM_ZOOM_IN     301
#define IDM_ZOOM_OUT    302
#define IDM_ACTUAL_SIZE 303
#define IDM_FIT         304
#define IDM_ABOUT       401

// The viewer's main window: menu, image view and status bar (message, file information, zoom).
class MainWindow
{
public:
    HWND hwnd;
    HACCEL accelerators;

    MainWindow();

    static bool registerClass(HINSTANCE instance);
    bool create(HINSTANCE instance, int32 show);

    // opens a PIX file or any image WIC reads and shows it
    bool loadFile(const wstring &path);

private:
    HINSTANCE instance;
    HWND status;
    ImageView view;
    HMENU menu;
    Picture picture;
    wstring currentPath;
    long long currentFileSize;
    wstring lastDir;
    wstring message;        // status bar message (a menu item's tip replaces it while the menu is open)
    wstring openPatterns;
    int32 algorithm;        // PIX algorithm used for saving, 1 or 2
    bool fit;               // shrink large pictures to the window

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);
    void command(int32 id);

    void createMenu();
    void createAccelerators();
    void layout();
    void updateActions();
    void updateStatus();
    void showMessage(const wstring &text);
    void setStatusText(int32 part, const wstring &text);
    void setZoom(double zoom);
    void applyFit();
    void showPicture(const wstring &path);
    void showError(const wstring &title, const wstring &text);

    bool savePix(const wstring &path, wstring &error);
    bool saveImage(const wstring &path, wstring &error);

    vector<FileType> openTypes() const;
    vector<FileType> saveTypes() const;
    vector<FileType> pixTypes() const;
    vector<FileType> imageTypes() const;
    bool askSource(const wstring &title, const vector<FileType> &types, wstring &path);
    bool askTarget(const wstring &title, const vector<FileType> &types, const wstring &suggestedName, int32 &type,
                   wstring &path);

    void open();
    void saveAs();
    void imageToPix();
    void pixToImage();
    void about();
};

#endif
