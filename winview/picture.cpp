#include <windows.h>
#include <wincodec.h>
#include <string.h>
#include <wchar.h>
#include <set>
#include "picture.h"
#include "../src/pix_api.h"

using namespace std;

// the PIX decoder's limit; WIC images above it could not be saved as PIX and would need gigabytes
static const unsigned long long MAX_PIXELS = (unsigned long long)1 << 28;

static IWICImagingFactory *factory = NULL;

//----------------------------------------------------------------------------------------------------------------------
static void release(IUnknown *object)
{
    if (object)
        object->Release();
}

//----------------------------------------------------------------------------------------------------------------------
wstring widen(const char *text)
{
    int32 n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (n <= 1)
        return wstring();
    wstring result((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], n);
    result.resize((size_t)n - 1);
    return result;
}

//----------------------------------------------------------------------------------------------------------------------
wstring errorText(HRESULT hr)
{
    // WIC errors have no system message text
    switch (hr)
    {
    case WINCODEC_ERR_COMPONENTNOTFOUND:
    case WINCODEC_ERR_UNKNOWNIMAGEFORMAT:
        return L"Unknown or unsupported image format.";
    case WINCODEC_ERR_BADHEADER:
    case WINCODEC_ERR_BADIMAGE:
    case WINCODEC_ERR_BADSTREAMDATA:
    case WINCODEC_ERR_STREAMREAD:
        return L"The image file is damaged.";
    case WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT:
        return L"Unsupported pixel format.";
    default:
        break;
    }
    wchar_t *text = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)hr, 0, (wchar_t *)&text, 0, NULL);
    wstring result;
    if (n > 0 && text)
    {
        result = text;
        while (!result.empty() && (result[result.size() - 1] == L'\n' || result[result.size() - 1] == L'\r'))
            result.resize(result.size() - 1);
    }
    if (text)
        LocalFree(text);
    if (result.empty())
    {
        wchar_t buf[64];
        swprintf(buf, 64, L"Error 0x%08X.", (unsigned)hr);
        result = buf;
    }
    return result;
}

//----------------------------------------------------------------------------------------------------------------------
bool pictureStartup(wstring &error)
{
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                  (void **)&factory);
    if (FAILED(hr))
    {
        error = L"Windows Imaging Component is not available: " + errorText(hr);
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
void pictureShutdown()
{
    release(factory);
    factory = NULL;
}

//----------------------------------------------------------------------------------------------------------------------
wstring extensionOf(const wstring &path)
{
    size_t dot = path.rfind(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == wstring::npos || (slash != wstring::npos && dot < slash))
        return wstring();
    wstring ext = path.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); i++)
        ext[i] = (wchar_t)towlower(ext[i]);
    return ext;
}

//----------------------------------------------------------------------------------------------------------------------
wstring fileNameOf(const wstring &path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == wstring::npos ? path : path.substr(slash + 1);
}

//----------------------------------------------------------------------------------------------------------------------
wstring baseNameOf(const wstring &path)
{
    wstring name = fileNameOf(path);
    size_t dot = name.rfind(L'.');
    return (dot == wstring::npos || dot == 0) ? name : name.substr(0, dot);
}

//----------------------------------------------------------------------------------------------------------------------
wstring folderOf(const wstring &path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == wstring::npos)
        return wstring();
    if (slash == 2 && path[1] == L':')
        return path.substr(0, 3);   // "C:\"
    return path.substr(0, slash);
}

//----------------------------------------------------------------------------------------------------------------------
int32 formatOfPath(const wstring &path)
{
    wstring ext = extensionOf(path);
    if (ext == L"pix")
        return FORMAT_PIX;
    if (ext == L"png")
        return FORMAT_PNG;
    if (ext == L"bmp" || ext == L"dib")
        return FORMAT_BMP;
    if (ext == L"jpg" || ext == L"jpeg" || ext == L"jpe" || ext == L"jfif")
        return FORMAT_JPEG;
    return FORMAT_NONE;
}

//----------------------------------------------------------------------------------------------------------------------
bool readFileBytes(const wstring &path, vector<byte> &data, wstring &error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = errorText(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 0x7FFFFFFF)
    {
        CloseHandle(file);
        error = L"The file is too large.";
        return false;
    }
    data.resize((size_t)size.QuadPart);
    size_t done = 0;
    while (done < data.size())
    {
        DWORD chunk = (DWORD)(data.size() - done > (1u << 24) ? (1u << 24) : data.size() - done);
        DWORD got = 0;
        if (!ReadFile(file, &data[done], chunk, &got, NULL) || got == 0)
        {
            error = errorText(HRESULT_FROM_WIN32(GetLastError()));
            CloseHandle(file);
            return false;
        }
        done += got;
    }
    CloseHandle(file);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool writeFileBytes(const wstring &path, const vector<byte> &data, wstring &error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = errorText(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size())
    {
        DWORD chunk = (DWORD)(data.size() - done > (1u << 24) ? (1u << 24) : data.size() - done);
        DWORD put = 0;
        ok = WriteFile(file, &data[done], chunk, &put, NULL) && put == chunk;
        done += put;
    }
    if (!ok)
        error = errorText(HRESULT_FROM_WIN32(GetLastError()));
    if (!CloseHandle(file) && ok)
    {
        error = errorText(HRESULT_FROM_WIN32(GetLastError()));
        ok = false;
    }
    if (!ok)
        DeleteFileW(path.c_str());
    return ok;
}

//----------------------------------------------------------------------------------------------------------------------
long long fileSizeOf(const wstring &path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    return ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
}

//----------------------------------------------------------------------------------------------------------------------
bool isPixFile(const wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    byte magic[4];
    DWORD got = 0;
    bool ok = ReadFile(file, magic, 4, &got, NULL) && got == 4;
    CloseHandle(file);
    return ok && magic[0] == 'P' && magic[1] == 'I' && magic[2] == 'X' && magic[3] == 0;
}

//----------------------------------------------------------------------------------------------------------------------
dword onWhite(dword colour)
{
    dword a = colour >> 24;
    if (a == 255)
        return colour;
    dword result = 0xFF000000u;
    for (int32 shift = 0; shift < 24; shift += 8)
    {
        dword c = (colour >> shift) & 255;
        result |= ((c * a + 255 * (255 - a) + 127) / 255) << shift;
    }
    return result;
}

//----------------------------------------------------------------------------------------------------------------------
bool readPix(const wstring &path, Picture &pic, wstring &error)
{
    vector<byte> data;
    if (!readFileBytes(path, data, error))
        return false;
    vector<unsigned char> rgb;
    int width = 0;
    int height = 0;
    if (!pixDecodeRgb(data, rgb, width, height))
    {
        error = widen(pixLastError());
        return false;
    }
    pic.width = width;
    pic.height = height;
    pic.hasAlpha = false;
    pic.pixels.resize((size_t)width * height);
    const unsigned char *p = rgb.empty() ? NULL : &rgb[0];
    for (size_t i = 0; i < pic.pixels.size(); i++, p += 3)
        pic.pixels[i] = 0xFF000000u | ((dword)p[0] << 16) | ((dword)p[1] << 8) | (dword)p[2];
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool writePix(const wstring &path, const Picture &pic, int32 algorithm, wstring &error)
{
    vector<unsigned char> rgb((size_t)pic.width * pic.height * 3);
    unsigned char *p = rgb.empty() ? NULL : &rgb[0];
    for (size_t i = 0; i < pic.pixels.size(); i++, p += 3)
    {
        dword c = onWhite(pic.pixels[i]);
        p[0] = (unsigned char)(c >> 16);
        p[1] = (unsigned char)(c >> 8);
        p[2] = (unsigned char)c;
    }
    vector<unsigned char> file;
    PixEncodeInfo info;
    if (!pixEncodeRgb(rgb.empty() ? NULL : &rgb[0], pic.width, pic.height, pic.width * 3, file, info, algorithm))
    {
        error = widen(pixLastError());
        return false;
    }
    return writeFileBytes(path, file, error);
}

//----------------------------------------------------------------------------------------------------------------------
static WICBitmapTransformOptions orientationTransform(IWICBitmapFrameDecode *frame)
{
    // EXIF orientation of JPEG (and TIFF) images: how the camera was held
    IWICMetadataQueryReader *reader = NULL;
    if (FAILED(frame->GetMetadataQueryReader(&reader)))
        return WICBitmapTransformRotate0;
    int32 orientation = 1;
    const wchar_t *queries[2] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    for (int32 i = 0; i < 2 && orientation == 1; i++)
    {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->GetMetadataByName(queries[i], &value)) && value.vt == VT_UI2)
            orientation = value.uiVal;
        PropVariantClear(&value);
    }
    reader->Release();
    switch (orientation)
    {
    case 2:
        return WICBitmapTransformFlipHorizontal;
    case 3:
        return WICBitmapTransformRotate180;
    case 4:
        return WICBitmapTransformFlipVertical;
    case 5:
        return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6:
        return WICBitmapTransformRotate90;
    case 7:
        return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8:
        return WICBitmapTransformRotate270;
    default:
        return WICBitmapTransformRotate0;
    }
}

//----------------------------------------------------------------------------------------------------------------------
static bool readWic(const wstring &path, Picture &pic, wstring &error)
{
    if (!factory)
    {
        error = L"Windows Imaging Component is not available.";
        return false;
    }
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapFlipRotator *rotator = NULL;
    IWICBitmapSource *converted = NULL;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    IWICBitmapSource *source = frame;
    if (SUCCEEDED(hr))
    {
        WICBitmapTransformOptions transform = orientationTransform(frame);
        if (transform != WICBitmapTransformRotate0 && SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) &&
            SUCCEEDED(rotator->Initialize(frame, transform)))
            source = rotator;
        hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, source, &converted);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr))
        hr = converted->GetSize(&width, &height);
    if (SUCCEEDED(hr) && (width == 0 || height == 0 || (unsigned long long)width * height > MAX_PIXELS))
    {
        release(converted);
        release(rotator);
        release(frame);
        release(decoder);
        wchar_t buf[96];
        swprintf(buf, 96, L"The image is too large (%u x %u pixels).", width, height);
        error = buf;
        return false;
    }
    if (SUCCEEDED(hr))
    {
        pic.width = (int32)width;
        pic.height = (int32)height;
        pic.pixels.resize((size_t)width * height);
        hr = converted->CopyPixels(NULL, width * 4, width * height * 4, (BYTE *)&pic.pixels[0]);
    }
    release(converted);
    release(rotator);
    release(frame);
    release(decoder);
    if (FAILED(hr))
    {
        pic = Picture();
        error = errorText(hr);
        return false;
    }
    pic.hasAlpha = false;
    for (size_t i = 0; i < pic.pixels.size() && !pic.hasAlpha; i++)
        pic.hasAlpha = (pic.pixels[i] >> 24) != 255;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool readPicture(const wstring &path, Picture &pic, wstring &error)
{
    if (isPixFile(path))
        return readPix(path, pic, error);
    return readWic(path, pic, error);
}

//----------------------------------------------------------------------------------------------------------------------
bool writeImage(const wstring &path, int32 format, const Picture &pic, wstring &error)
{
    if (format == FORMAT_PIX)
    {
        error = L"Internal error: PIX files are written by writePix.";
        return false;
    }
    if (!factory)
    {
        error = L"Windows Imaging Component is not available.";
        return false;
    }
    GUID container = GUID_ContainerFormatPng;
    if (format == FORMAT_BMP)
        container = GUID_ContainerFormatBmp;
    else if (format == FORMAT_JPEG)
        container = GUID_ContainerFormatJpeg;

    // only PNG keeps transparency: other formats get the image put on white
    bool keepAlpha = format == FORMAT_PNG && pic.hasAlpha;
    vector<dword> flat;
    const dword *pixels = &pic.pixels[0];
    if (pic.hasAlpha && !keepAlpha)
    {
        flat.resize(pic.pixels.size());
        for (size_t i = 0; i < flat.size(); i++)
            flat[i] = onWhite(pic.pixels[i]);
        pixels = &flat[0];
    }
    UINT width = (UINT)pic.width;
    UINT height = (UINT)pic.height;

    IWICBitmap *bitmap = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    HRESULT hr = factory->CreateBitmapFromMemory(width, height,
                                                 keepAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppBGR,
                                                 width * 4, width * height * 4, (BYTE *)pixels, &bitmap);
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    bool created = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
        hr = factory->CreateEncoder(container, NULL, &encoder);
    if (SUCCEEDED(hr))
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr))
        hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr) && format == FORMAT_JPEG && options)
    {
        PROPBAG2 option;
        memset(&option, 0, sizeof(option));
        option.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.95f;
        options->Write(1, &option, &value);    // the default quality is used if this fails
    }
    if (SUCCEEDED(hr))
        hr = frame->Initialize(options);
    if (SUCCEEDED(hr))
        hr = frame->SetSize(width, height);
    if (SUCCEEDED(hr))
    {
        // the encoder may pick another format; WriteSource converts to it
        WICPixelFormatGUID pixelFormat = keepAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        hr = frame->SetPixelFormat(&pixelFormat);
    }
    if (SUCCEEDED(hr))
        hr = frame->WriteSource(bitmap, NULL);
    if (SUCCEEDED(hr))
        hr = frame->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();
    release(options);
    release(frame);
    release(encoder);
    release(stream);
    release(bitmap);
    if (FAILED(hr))
    {
        if (created)
            DeleteFileW(path.c_str());
        error = errorText(hr);
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
wstring readablePatterns()
{
    // file extensions of every WIC decoder, e.g. ".bmp,.dib,.rle" -> "*.bmp;*.dib;*.rle"
    set<wstring> seen;
    wstring patterns = L"*.pix";
    seen.insert(L".pix");
    IEnumUnknown *components = NULL;
    if (factory && SUCCEEDED(factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &components)))
    {
        IUnknown *item = NULL;
        ULONG fetched = 0;
        while (components->Next(1, &item, &fetched) == S_OK && item)
        {
            IWICBitmapDecoderInfo *info = NULL;
            if (SUCCEEDED(item->QueryInterface(IID_IWICBitmapDecoderInfo, (void **)&info)))
            {
                UINT length = 0;
                if (SUCCEEDED(info->GetFileExtensions(0, NULL, &length)) && length > 1)
                {
                    wstring list((size_t)length, L'\0');
                    if (SUCCEEDED(info->GetFileExtensions(length, &list[0], &length)))
                    {
                        list.resize(wcslen(list.c_str()));
                        size_t start = 0;
                        while (start < list.size())
                        {
                            size_t end = list.find(L',', start);
                            if (end == wstring::npos)
                                end = list.size();
                            wstring ext = list.substr(start, end - start);
                            for (size_t i = 0; i < ext.size(); i++)
                                ext[i] = (wchar_t)towlower(ext[i]);
                            if (ext.size() > 1 && ext[0] == L'.' && seen.insert(ext).second)
                                patterns += L";*" + ext;
                            start = end + 1;
                        }
                    }
                }
                info->Release();
            }
            item->Release();
            item = NULL;
        }
        components->Release();
    }
    if (seen.size() == 1)
        patterns += L";*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico";
    return patterns;
}
