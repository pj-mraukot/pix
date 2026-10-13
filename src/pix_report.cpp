#include <stdio.h>
#include <stdarg.h>
#include "pix_report.h"

using namespace std;

//----------------------------------------------------------------------------------------------------------------------
static void formatNibbles(const NibbleWriter &out, size_t start, char *buf, size_t size)
{
    // opcode (1 nibble, or 2 when it starts with 0), then the arguments as one group;
    // cache selection 01 L DIST shows L separately
    size_t end = out.count;
    size_t opLen = out.nibbleAt(start) == 0 ? 2 : 1;
    size_t pos = 0;
    for (size_t i = start; i < end && pos + 3 < size; i++)
    {
        bool gap = i == start + opLen || (opLen == 2 && out.nibbleAt(start + 1) == 1 && i == start + 3);
        if (gap)
            buf[pos++] = ' ';
        buf[pos++] = "0123456789ABCDEF"[out.nibbleAt(i)];
    }
    buf[pos] = 0;
}

//----------------------------------------------------------------------------------------------------------------------
void PixReport::header(const char *inputName, const Image &img)
{
    fprintf(f, "PIX encoding report\n");
    fprintf(f, "input: %s, %d x %d pixels, %d x %d blocks of %dx%d\n\n", inputName, img.width, img.height,
            img.width / BLOCK_SIZE, img.height / BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
    fprintf(f, "columns: nibble address in the uncompressed stream (hex, byte = address / 2), nibbles (hex),\n");
    fprintf(f, "         code, first pixel (x,y) and pixel count, colour #RRGGBB, explanation\n\n");
    fprintf(f, "codes (nibbles):\n");
    fprintf(f, "  SELECT  01 L DIST   cache1 copied from block (current - 1 - DIST), freq / 4, at block start\n");
    fprintf(f, "  LAST    1           last colour\n");
    fprintf(f, "  NEXT    2           next to last colour\n");
    fprintf(f, "  RUN     3X, 4XX     last colour 3..18, 19..274 times\n");
    fprintf(f, "  ABOVE   F           colour of the pixel above\n");
    fprintf(f, "  C1      5X..9X      colour from cache1 slot 0..79\n");
    fprintf(f, "  C2      AXXX, BXXX  colour from cache2 slot 0..8191\n");
    fprintf(f, "  EXACT   02          colour = MED prediction\n");
    fprintf(f, "  MEDRUN  03N, 0ENN   2..17, 18..273 pixels, each = its MED prediction\n");
    fprintf(f, "  SMALL   0X N        MED prediction + residual -2..1        (X = 4..7)\n");
    fprintf(f, "  DELTA   D GRB       MED prediction + residual -8..7\n");
    fprintf(f, "  MEDIUM  0X NNN      MED prediction + residual G -16..15, R,B -8..7   (X = 8,9)\n");
    fprintf(f, "  WIDE    0X NNNN     MED prediction + residual G any, R,B -16..15     (X = A..D)\n");
    fprintf(f, "  NEW     C RRGGBB    literal colour, inserted into cache1 (the colour it replaces goes to cache2)\n");
    fprintf(f, "  NEW     E RRGGBB    literal colour, inserted into cache2\n");
    fprintf(f, "  END     00          end of stream\n");
    fprintf(f, "residuals: dG = G - pred.G, dR = (R - pred.R) - dG, dB = (B - pred.B) - dG\n");
}

//----------------------------------------------------------------------------------------------------------------------
void PixReport::block(int32 blockNo, int32 bx, int32 by)
{
    fprintf(f, "\n; ---- block %d at (%d,%d) ----\n", blockNo, bx, by);
}

//----------------------------------------------------------------------------------------------------------------------
void PixReport::comment(const char *format, ...)
{
    fprintf(f, "; ");
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fprintf(f, "\n");
}

//----------------------------------------------------------------------------------------------------------------------
void PixReport::code(const NibbleWriter &out, size_t start, const char *mnemonic, int32 x, int32 y, int32 count,
                     dword colour, const char *format, ...)
{
    char nibbles[40];
    formatNibbles(out, start, nibbles, sizeof(nibbles));
    fprintf(f, "%08zX  %-10s %-7s", start, nibbles, mnemonic);
    if (x >= 0)
    {
        char where[64];
        if (count > 1)
        {
            // pixels of a code are consecutive inside the block (row by row)
            int32 bx = x / BLOCK_SIZE * BLOCK_SIZE;
            int32 i = (y % BLOCK_SIZE) * BLOCK_SIZE + (x - bx) + count - 1;
            snprintf(where, sizeof(where), "(%d,%d)-(%d,%d) x%d", x, y, bx + i % BLOCK_SIZE,
                     y / BLOCK_SIZE * BLOCK_SIZE + i / BLOCK_SIZE, count);
        }
        else
            snprintf(where, sizeof(where), "(%d,%d)", x, y);
        fprintf(f, "%-25s #%06X  ", where, colour);
    }
    else
        fprintf(f, "%-25s %-7s  ", "", "");

    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fprintf(f, "\n");
}

//----------------------------------------------------------------------------------------------------------------------
void PixReport::pixel(int32 x, int32 y, dword colour, const char *format, ...)
{
    char where[64];
    snprintf(where, sizeof(where), "(%d,%d)", x, y);
    fprintf(f, "%-8s  %-10s %-7s%-25s #%06X  ", "", "", "", where, colour);
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fprintf(f, "\n");
}

//----------------------------------------------------------------------------------------------------------------------
void PixReport::summary(const EncodeStats &stats, size_t fileBytes, size_t rawBytes)
{
    fprintf(f, "\n; ---- summary ----\n");
    fprintf(f, "stream:  %zu nibbles (%zu bytes)\n", stats.nibbles, (stats.nibbles + 1) / 2);
    fprintf(f, "file:    %zu bytes after zlib (%.2f%% of raw RGB %zu bytes)\n", fileBytes,
            rawBytes ? 100.0 * (double)fileBytes / (double)rawBytes : 0.0, rawBytes);
    fprintf(f, "codes:   SELECT %zu  LAST %zu  NEXT %zu  RUN %zu (%zu pixels)  ABOVE %zu  C1 %zu  C2 %zu\n",
            stats.cacheSelect, stats.last, stats.nextToLast, stats.run3 + stats.run4, stats.runPixels,
            stats.above, stats.cache1, stats.cache2);
    fprintf(f, "         EXACT %zu  MEDRUN %zu (%zu pixels)  SMALL %zu  DELTA %zu  MEDIUM %zu  WIDE %zu\n",
            stats.medExact, stats.medRun, stats.medRunPixels, stats.deltaSmall, stats.codeD,
            stats.deltaMedium, stats.deltaWide);
    fprintf(f, "         NEW C %zu  NEW E %zu\n", stats.codeC, stats.codeE);
}
