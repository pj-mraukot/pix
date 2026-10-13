#ifndef PIX_API_H
#define PIX_API_H

// Interface to the PIX codec for C++17 code (e.g. the Qt viewer).
// It does not include types.h: in C++17 its 'byte' typedef clashes with std::byte
// because of 'using namespace std'.

#include <stddef.h>
#include <vector>

using namespace std;

struct PixEncodeInfo
{
    size_t streamBytes;     // algorithm 1: uncompressed nibble stream size; algorithm 2: coded data size
    size_t fileBytes;       // size of complete .pix file
};

// encodes 24 bit RGB pixels (rows top-down, 'stride' bytes per row) into .pix file content
// with PIX algorithm 1 or 2
bool pixEncodeRgb(const unsigned char *rgb, int width, int height, int stride,
                  vector<unsigned char> &file, PixEncodeInfo &info, int algorithm = 1);

// decodes .pix file content into 24 bit RGB pixels (rows top-down, width * 3 bytes per row)
bool pixDecodeRgb(const vector<unsigned char> &file, vector<unsigned char> &rgb, int &width, int &height);

// message describing the last failure
const char *pixLastError();

#endif
