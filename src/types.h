#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <vector>
#include <unordered_map>

using namespace std;

#define hmap unordered_map

typedef unsigned char byte;
typedef unsigned short word;
typedef uint32_t dword;
typedef int32_t  int32;

// 24 bit RGB bitmap, rows top-down, 3 bytes per pixel (R,G,B)
struct Image
{
    int32 width;
    int32 height;
    vector<byte> rgb;

    Image() : width(0), height(0) {}

    dword getPixel(int32 x, int32 y) const
    {
        const byte *p = &rgb[((size_t)y * width + x) * 3];
        return ((dword)p[0] << 16) | ((dword)p[1] << 8) | (dword)p[2];
    }

    void setPixel(int32 x, int32 y, dword colour)
    {
        byte *p = &rgb[((size_t)y * width + x) * 3];
        p[0] = (byte)(colour >> 16);
        p[1] = (byte)(colour >> 8);
        p[2] = (byte)colour;
    }
};

#endif
