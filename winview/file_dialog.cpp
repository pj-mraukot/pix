#include <windows.h>
#include <shobjidl.h>
#include <wchar.h>
#include "file_dialog.h"
#include "picture.h"

using namespace std;

//----------------------------------------------------------------------------------------------------------------------
vector<wstring> extensionsOf(const FileType &type)
{
    vector<wstring> result;
    size_t start = 0;
    while (start < type.patterns.size())
    {
        size_t end = type.patterns.find(L';', start);
        if (end == wstring::npos)
            end = type.patterns.size();
        wstring pattern = type.patterns.substr(start, end - start);
        if (pattern.size() > 2 && pattern[0] == L'*' && pattern[1] == L'.' && pattern != L"*.*")
        {
            wstring ext = pattern.substr(2);
            for (size_t i = 0; i < ext.size(); i++)
                ext[i] = (wchar_t)towlower(ext[i]);
            result.push_back(ext);
        }
        start = end + 1;
    }
    return result;
}

//----------------------------------------------------------------------------------------------------------------------
static bool hasExtension(const vector<wstring> &extensions, const wstring &ext)
{
    for (size_t i = 0; i < extensions.size(); i++)
        if (extensions[i] == ext)
            return true;
    return false;
}

// Keeps the file name's extension in step with the file type chosen in a save dialog.
class SaveTypeEvents : public IFileDialogEvents
{
public:
    SaveTypeEvents(const vector<FileType> *t) : refs(1), types(t) {}
    virtual ~SaveTypeEvents() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object)
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IFileDialogEvents))
        {
            *object = (IFileDialogEvents *)this;
            AddRef();
            return S_OK;
        }
        *object = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() { return ++refs; }

    ULONG STDMETHODCALLTYPE Release()
    {
        ULONG n = --refs;
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *dialog)
    {
        // the name as typed: the dialog may still add the selected type's extension to it
        wchar_t *name = NULL;
        if (SUCCEEDED(dialog->GetFileName(&name)) && name)
        {
            typedName = name;
            CoTaskMemFree(name);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog *, IShellItem *, FDE_SHAREVIOLATION_RESPONSE *) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog *, IShellItem *, FDE_OVERWRITE_RESPONSE *) { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *dialog)
    {
        UINT index = 0;
        if (FAILED(dialog->GetFileTypeIndex(&index)) || index < 1 || index > types->size())
            return S_OK;
        vector<wstring> extensions = extensionsOf((*types)[index - 1]);
        if (extensions.empty())
            return S_OK;
        dialog->SetDefaultExtension(extensions[0].c_str());
        wchar_t *name = NULL;
        if (FAILED(dialog->GetFileName(&name)) || !name)
            return S_OK;
        wstring current = name;
        CoTaskMemFree(name);
        // photo.png -> photo.jpg; photo.jpeg stays when JPEG is chosen
        if (current.empty() || hasExtension(extensions, extensionOf(current)))
            return S_OK;
        wstring base = extensionOf(current).empty() ? current : current.substr(0, current.rfind(L'.'));
        dialog->SetFileName((base + L"." + extensions[0]).c_str());
        return S_OK;
    }

    wstring typedName;

private:
    ULONG refs;
    const vector<FileType> *types;
};

//----------------------------------------------------------------------------------------------------------------------
static void setStartFolder(IFileDialog *dialog, const wstring &folder)
{
    if (folder.empty())
        return;
    IShellItem *item = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), NULL, IID_IShellItem, (void **)&item)))
    {
        dialog->SetFolder(item);
        item->Release();
    }
}

//----------------------------------------------------------------------------------------------------------------------
static bool resultPath(IFileDialog *dialog, wstring &path)
{
    IShellItem *item = NULL;
    if (FAILED(dialog->GetResult(&item)))
        return false;
    wchar_t *name = NULL;
    HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
    item->Release();
    if (FAILED(hr) || !name)
        return false;
    path = name;
    CoTaskMemFree(name);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
static void setTypes(IFileDialog *dialog, const vector<FileType> &types)
{
    vector<COMDLG_FILTERSPEC> specs(types.size());
    for (size_t i = 0; i < types.size(); i++)
    {
        specs[i].pszName = types[i].name.c_str();
        specs[i].pszSpec = types[i].patterns.c_str();
    }
    if (!specs.empty())
        dialog->SetFileTypes((UINT)specs.size(), &specs[0]);
}

//----------------------------------------------------------------------------------------------------------------------
bool askOpenFile(HWND owner, const wstring &title, const vector<FileType> &types, wstring &folder, wstring &path)
{
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void **)&dialog)))
        return false;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.c_str());
    setTypes(dialog, types);
    dialog->SetFileTypeIndex(1);
    setStartFolder(dialog, folder);
    bool ok = SUCCEEDED(dialog->Show(owner)) && resultPath(dialog, path);
    dialog->Release();
    if (ok)
        folder = folderOf(path);
    return ok;
}

//----------------------------------------------------------------------------------------------------------------------
bool askSaveFile(HWND owner, const wstring &title, const vector<FileType> &types, const wstring &suggestedName,
                 int32 &type, wstring &folder, wstring &path)
{
    if (types.empty())
        return false;
    if (type < 0 || type >= (int32)types.size())
        type = 0;
    IFileSaveDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, (void **)&dialog)))
        return false;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.c_str());
    setTypes(dialog, types);
    dialog->SetFileTypeIndex((UINT)type + 1);
    vector<wstring> extensions = extensionsOf(types[type]);
    if (!extensions.empty())
        dialog->SetDefaultExtension(extensions[0].c_str());
    setStartFolder(dialog, folder);
    dialog->SetFileName(suggestedName.c_str());

    SaveTypeEvents *events = new SaveTypeEvents(&types);
    DWORD cookie = 0;
    bool advised = SUCCEEDED(dialog->Advise(events, &cookie));
    bool ok = SUCCEEDED(dialog->Show(owner)) && resultPath(dialog, path);
    UINT index = 0;
    if (ok && SUCCEEDED(dialog->GetFileTypeIndex(&index)) && index >= 1 && index <= types.size())
        type = (int32)index - 1;
    wstring typed = fileNameOf(events->typedName);
    if (advised)
        dialog->Unadvise(cookie);
    events->Release();
    dialog->Release();
    if (!ok)
        return false;

    // A typed extension of another known type wins, as in the Qt viewer: "photo.bmp" with PNG selected is saved
    // as a BMP file. Some dialogs (Wine's) add the selected type's extension anyway, giving photo.bmp.png:
    // take that one off again. The dialog checked the longer name for overwriting, so ask here.
    if (formatOfPath(typed) != FORMAT_NONE)
    {
        wstring folder = folderOf(path);
        wstring wanted = folder + (folder.empty() || folder[folder.size() - 1] == L'\\' ? L"" : L"\\") + typed;
        if (path.size() > wanted.size() && path[wanted.size()] == L'.' &&
            _wcsnicmp(path.c_str(), wanted.c_str(), wanted.size()) == 0 &&
            path.find_first_of(L"\\/", wanted.size()) == wstring::npos)
        {
            path = wanted;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                wstring question = fileNameOf(path) + L" already exists.\nDo you want to replace it?";
                if (MessageBoxW(owner, question.c_str(), title.c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                    return false;
            }
        }
    }

    // a name without an extension the viewer knows gets the chosen type's one
    extensions = extensionsOf(types[type]);
    if (!extensions.empty() && formatOfPath(path) == FORMAT_NONE)
        path += L"." + extensions[0];
    folder = folderOf(path);
    return true;
}
