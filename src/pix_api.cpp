#include <string.h>
#include "types.h"
#include "pix_api.h"
#include "pix_codec.h"
#include "pix_error.h"

using namespace std;

//----------------------------------------------------------------------------------------------------------------------
bool pixEncodeRgb(const unsigned char *rgb, int width, int height, int stride,
                  vector<unsigned char> &file, PixEncodeInfo &info, int algorithm)
{
    Image img;
    img.width = width;
    img.height = height;
    img.rgb.resize((size_t)width * height * 3);
    for (int32 y = 0; y < height; y++)
        memcpy(&img.rgb[(size_t)y * width * 3], rgb + (size_t)y * stride, (size_t)width * 3);

    if (algorithm == 2)
    {
        Pix2Stats stats2;
        if (!encodePix2File(img, file, stats2))
            return false;
        info.streamBytes = stats2.dataBytes;
        info.fileBytes = file.size();
        return true;
    }
    EncodeStats stats;
    if (!encodePixFile(img, file, stats))
        return false;
    info.streamBytes = (stats.nibbles + 1) / 2;
    info.fileBytes = file.size();
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool pixDecodeRgb(const vector<unsigned char> &file, vector<unsigned char> &rgb, int &width, int &height)
{
    Image img;
    if (!decodePixFile(file, img))
        return false;
    width = img.width;
    height = img.height;
    rgb.swap(img.rgb);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
const char *pixLastError()
{
    return getPixError();
}
