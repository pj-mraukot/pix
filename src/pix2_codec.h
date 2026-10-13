#ifndef PIX2_CODEC_H
#define PIX2_CODEC_H

// PIX algorithm 2: per channel prediction (blend of several predictors) and adaptive binary
// arithmetic coding of the residuals

#include "types.h"

// statistics of the algorithm 2 encoder
struct Pix2Stats
{
    size_t pixels;
    size_t dataBytes;           // coded data including CRC
    size_t modes[4];            // pixels coded as: predicted colour, neighbour colour, recent colour, residuals
    double modeBits;            // estimated bits spent on choosing how each pixel is coded
    double channelBits[3];      // estimated bits spent on R, G, B residuals

    Pix2Stats() : pixels(0), dataBytes(0), modeBits(0)
    {
        modes[0] = modes[1] = modes[2] = modes[3] = 0;
        channelBits[0] = channelBits[1] = channelBits[2] = 0;
    }
};

// encodes image into algorithm 2 data (arithmetic coded pixels followed by CRC32 of the RGB data)
bool encodePix2Data(const Image &img, vector<byte> &data, Pix2Stats &stats);

// decodes algorithm 2 data into image (width and height must be set)
bool decodePix2Data(const byte *data, size_t size, Image &img);

#endif
