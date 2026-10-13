#ifndef PIX_REPORT_H
#define PIX_REPORT_H

#include <stdio.h>
#include "types.h"
#include "pix_codec.h"

// text listing of the encoder output: one line per code, like a debugger listing
class PixReport
{
public:
    explicit PixReport(FILE *file) : f(file) {}

    void header(const char *inputName, const Image &img);
    void block(int32 blockNo, int32 bx, int32 by);
    void comment(const char *format, ...);

    // one code: nibble address, nibbles, mnemonic, first pixel (x < 0: none), pixel count, colour, explanation
    void code(const NibbleWriter &out, size_t start, const char *mnemonic, int32 x, int32 y, int32 count,
              dword colour, const char *format, ...);

    // continuation line for one pixel of a multi-pixel code
    void pixel(int32 x, int32 y, dword colour, const char *format, ...);

    void summary(const EncodeStats &stats, size_t fileBytes, size_t rawBytes);

private:
    FILE *f;
};

#endif
