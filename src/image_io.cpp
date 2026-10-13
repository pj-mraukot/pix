#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <png.h>
#include <jpeglib.h>
#include "image_io.h"
#include "pix_error.h"

using namespace std;

static const size_t MAX_PIXELS = (size_t)1 << 28;

//----------------------------------------------------------------------------------------------------------------------
static dword getDword(const byte *p)
{
    return (dword)p[0] | ((dword)p[1] << 8) | ((dword)p[2] << 16) | ((dword)p[3] << 24);
}

//----------------------------------------------------------------------------------------------------------------------
static word getWord(const byte *p)
{
    return (word)(p[0] | (p[1] << 8));
}

//----------------------------------------------------------------------------------------------------------------------
static void putDword(byte *p, dword v)
{
    p[0] = (byte)v;
    p[1] = (byte)(v >> 8);
    p[2] = (byte)(v >> 16);
    p[3] = (byte)(v >> 24);
}

//----------------------------------------------------------------------------------------------------------------------
static void putWord(byte *p, word v)
{
    p[0] = (byte)v;
    p[1] = (byte)(v >> 8);
}

//----------------------------------------------------------------------------------------------------------------------
bool readFile(const char *path, vector<byte> &data)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        setPixError("Cannot open file: %s", path);
        return false;
    }
    data.clear();
    byte buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok)
        setPixError("Error reading file: %s", path);
    return ok;
}

//----------------------------------------------------------------------------------------------------------------------
bool writeFile(const char *path, const vector<byte> &data)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        setPixError("Cannot create file: %s", path);
        return false;
    }
    bool ok = data.empty() || fwrite(&data[0], 1, data.size(), f) == data.size();
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        setPixError("Error writing file: %s", path);
    return ok;
}

//----------------------------------------------------------------------------------------------------------------------
static bool checkSize(int32 width, int32 height)
{
    if (width <= 0 || height <= 0 || (size_t)width * (size_t)height > MAX_PIXELS)
    {
        setPixError("Unsupported image size %dx%d", width, height);
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
static bool loadPng(const char *path, Image &img)
{
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path))
    {
        setPixError("PNG error: %s", image.message);
        return false;
    }
    if (!checkSize((int32)image.width, (int32)image.height))
    {
        png_image_free(&image);
        return false;
    }
    image.format = PNG_FORMAT_RGB;
    img.width = (int32)image.width;
    img.height = (int32)image.height;
    img.rgb.resize(PNG_IMAGE_SIZE(image));

    // transparent pixels are composed onto white background
    png_color background;
    background.red = 255;
    background.green = 255;
    background.blue = 255;
    if (!png_image_finish_read(&image, &background, &img.rgb[0], 0, NULL))
    {
        setPixError("PNG error: %s", image.message);
        png_image_free(&image);
        return false;
    }
    return true;
}

struct JpegErrorMgr
{
    jpeg_error_mgr pub;
    jmp_buf jump;
};

//----------------------------------------------------------------------------------------------------------------------
static void jpegErrorExit(j_common_ptr cinfo)
{
    JpegErrorMgr *err = (JpegErrorMgr *)cinfo->err;
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    setPixError("JPEG error: %s", msg);
    longjmp(err->jump, 1);
}

//----------------------------------------------------------------------------------------------------------------------
static bool loadJpeg(const char *path, Image &img)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        setPixError("Cannot open file: %s", path);
        return false;
    }

    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;
    if (setjmp(jerr.jump))
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    if (!checkSize((int32)cinfo.output_width, (int32)cinfo.output_height))
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return false;
    }
    img.width = (int32)cinfo.output_width;
    img.height = (int32)cinfo.output_height;
    img.rgb.resize((size_t)img.width * img.height * 3);

    while (cinfo.output_scanline < cinfo.output_height)
    {
        JSAMPROW row = &img.rgb[(size_t)cinfo.output_scanline * img.width * 3];
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
static bool loadBmp(const vector<byte> &data, Image &img)
{
    if (data.size() < 54)
    {
        setPixError("BMP error: file too short");
        return false;
    }
    dword offBits = getDword(&data[10]);
    dword infoSize = getDword(&data[14]);
    int32 width = (int32)getDword(&data[18]);
    int32 height = (int32)getDword(&data[22]);
    word bitCount = getWord(&data[28]);
    dword compression = getDword(&data[30]);

    if (infoSize < 40)
    {
        setPixError("BMP error: unsupported header type");
        return false;
    }
    if (bitCount != 24 && bitCount != 32)
    {
        setPixError("BMP error: only 24 and 32 bit images are supported (got %d bit)", bitCount);
        return false;
    }
    // 0 = BI_RGB, 3 = BI_BITFIELDS (assumed standard BGRA masks)
    if (compression != 0 && !(compression == 3 && bitCount == 32))
    {
        setPixError("BMP error: compressed images are not supported");
        return false;
    }

    bool topDown = height < 0;
    if (topDown)
        height = -height;
    if (!checkSize(width, height))
        return false;

    size_t bytesPerPixel = bitCount / 8;
    size_t stride = ((size_t)width * bitCount + 31) / 32 * 4;
    if (offBits > data.size() || stride * height > data.size() - offBits)
    {
        setPixError("BMP error: file truncated");
        return false;
    }

    img.width = width;
    img.height = height;
    img.rgb.resize((size_t)width * height * 3);
    for (int32 y = 0; y < height; y++)
    {
        int32 srcY = topDown ? y : height - 1 - y;
        const byte *src = &data[offBits + stride * srcY];
        byte *dst = &img.rgb[(size_t)y * width * 3];
        for (int32 x = 0; x < width; x++)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            src += bytesPerPixel;
            dst += 3;
        }
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool loadImage(const char *path, Image &img)
{
    vector<byte> data;
    if (!readFile(path, data))
        return false;

    if (data.size() >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return loadPng(path, img);
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return loadJpeg(path, img);
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M')
        return loadBmp(data, img);

    setPixError("Unknown image format: %s (supported: PNG, JPG, BMP)", path);
    return false;
}

//----------------------------------------------------------------------------------------------------------------------
bool saveBmp(const char *path, const Image &img)
{
    size_t stride = ((size_t)img.width * 3 + 3) / 4 * 4;
    size_t imageSize = stride * img.height;
    vector<byte> data(54 + imageSize, 0);

    data[0] = 'B';
    data[1] = 'M';
    putDword(&data[2], (dword)data.size());
    putDword(&data[10], 54);
    putDword(&data[14], 40);
    putDword(&data[18], (dword)img.width);
    putDword(&data[22], (dword)img.height);
    putWord(&data[26], 1);
    putWord(&data[28], 24);
    putDword(&data[30], 0);
    putDword(&data[34], (dword)imageSize);
    putDword(&data[38], 2835);  // 72 DPI
    putDword(&data[42], 2835);

    for (int32 y = 0; y < img.height; y++)
    {
        const byte *src = &img.rgb[(size_t)y * img.width * 3];
        byte *dst = &data[54 + stride * (img.height - 1 - y)];
        for (int32 x = 0; x < img.width; x++)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            src += 3;
            dst += 3;
        }
    }
    return writeFile(path, data);
}
