#ifndef PICTURE_H
#define PICTURE_H

// Images of the Windows viewer: PIX files through the codec (src/pix_api.h), every other format through the
// Windows Imaging Component (WIC), which is part of Windows (PNG, JPEG, BMP, GIF, TIFF, ICO, ...)

#include <windows.h>
#include <string>
#include "../src/types.h"

using namespace std;

// 32 bit pixels, 0xAARRGGBB (bytes B, G, R, A in memory, as GDI and WIC use them), rows top-down
struct Picture
{
    int32 width;
    int32 height;
    vector<dword> pixels;
    bool hasAlpha;      // some pixel is not fully opaque

    Picture() : width(0), height(0), hasAlpha(false) {}

    bool empty() const { return pixels.empty(); }
};

// file formats the viewer saves
#define FORMAT_NONE 0
#define FORMAT_PIX  1
#define FORMAT_PNG  2
#define FORMAT_BMP  3
#define FORMAT_JPEG 4

// creates the WIC factory (COM must be initialised)
bool pictureStartup(wstring &error);
void pictureShutdown();

// reads a PIX file or any image WIC can decode (the first frame; JPEG orientation is applied)
bool readPicture(const wstring &path, Picture &pic, wstring &error);
bool readPix(const wstring &path, Picture &pic, wstring &error);

// encodes as PIX (algorithm 1 or 2); transparent pixels are put on white, as in the command line tool
bool writePix(const wstring &path, const Picture &pic, int32 algorithm, wstring &error);
// saves as PNG (keeps transparency), BMP or JPEG (quality 95)
bool writeImage(const wstring &path, int32 format, const Picture &pic, wstring &error);

// the colour of a pixel put on a white background
dword onWhite(dword colour);

// "*.png;*.jpg;..." for every format WIC can read, including installed codecs
wstring readablePatterns();

// format chosen by a file name's extension, FORMAT_NONE if unknown
int32 formatOfPath(const wstring &path);

bool isPixFile(const wstring &path);
bool readFileBytes(const wstring &path, vector<byte> &data, wstring &error);
bool writeFileBytes(const wstring &path, const vector<byte> &data, wstring &error);
long long fileSizeOf(const wstring &path);

// lower case extension without the dot, "" if none
wstring extensionOf(const wstring &path);
// file name without the folder
wstring fileNameOf(const wstring &path);
// file name without the folder and the extension
wstring baseNameOf(const wstring &path);
// the folder, without a trailing backslash
wstring folderOf(const wstring &path);

wstring widen(const char *text);
// text of a Windows / COM error code
wstring errorText(HRESULT hr);

#endif
