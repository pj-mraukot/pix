#ifndef FILE_DIALOG_H
#define FILE_DIALOG_H

#include <windows.h>
#include <string>
#include "../src/types.h"

using namespace std;

// a file type in a dialog's type list
struct FileType
{
    wstring name;       // "PNG images (*.png)"
    wstring patterns;   // "*.png" or "*.jpg;*.jpeg"; the first one is the default extension when saving
};

// Asks for a file to open; folder is where the dialog starts, and becomes the chosen file's folder.
// False if cancelled.
bool askOpenFile(HWND owner, const wstring &title, const vector<FileType> &types, wstring &folder, wstring &path);

// Asks for a file to save. type: the type selected at the start, then the one chosen (index into types).
// Choosing a type changes the extension of the name typed so far, e.g. photo.png -> photo.jpg. A name without
// a known extension gets the chosen type's one. False if cancelled.
bool askSaveFile(HWND owner, const wstring &title, const vector<FileType> &types, const wstring &suggestedName,
                 int32 &type, wstring &folder, wstring &path);

// the extensions of a type, lower case without "*.": "*.jpg;*.jpeg" -> jpg, jpeg
vector<wstring> extensionsOf(const FileType &type);

#endif
