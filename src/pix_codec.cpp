#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "pix_codec.h"
#include "pix_error.h"
#include "pix_report.h"

using namespace std;

static const size_t MAX_PIXELS = (size_t)1 << 28;

// last and next to last placed colours
struct ColourHistory
{
    dword last;
    dword nextToLast;

    ColourHistory() : last(0), nextToLast(0) {}

    void place(dword colour)
    {
        if (colour != last)
        {
            nextToLast = last;
            last = colour;
        }
    }
};

// difference between colour and its MED prediction (mod 256):
// dg = G - Pg, dr = (R - Pr) - dg, db = (B - Pb) - dg, each wrapped to -128..127
struct Residual
{
    int32 dg;
    int32 dr;
    int32 db;
};

//----------------------------------------------------------------------------------------------------------------------
static int32 paddedSize(int32 size)
{
    return (size + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
}

//----------------------------------------------------------------------------------------------------------------------
static bool checkDimensions(int32 width, int32 height)
{
    // any size is allowed: the encoder pads the image to whole 16x16 blocks
    if (width <= 0 || height <= 0 || width > PIX_MAX_DIMENSION || height > PIX_MAX_DIMENSION ||
        (size_t)width * (size_t)height > MAX_PIXELS ||
        (size_t)paddedSize(width) * (size_t)paddedSize(height) > MAX_PIXELS)
    {
        setPixError("Unsupported image size %dx%d", width, height);
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
static void padImage(const Image &src, Image &dst)
{
    // pads to whole blocks: the last pixel of each row is repeated to the right,
    // then the last row is repeated downwards
    dst.width = paddedSize(src.width);
    dst.height = paddedSize(src.height);
    dst.rgb.resize((size_t)dst.width * dst.height * 3);
    for (int32 y = 0; y < dst.height; y++)
    {
        int32 srcY = y < src.height ? y : src.height - 1;
        for (int32 x = 0; x < dst.width; x++)
            dst.setPixel(x, y, src.getPixel(x < src.width ? x : src.width - 1, srcY));
    }
}

//----------------------------------------------------------------------------------------------------------------------
static int32 nibblesNeeded(dword value)
{
    int32 n = 1;
    while (n < 8 && (value >> (n * 4)) != 0)
        n++;
    return n;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 medChannel(int32 left, int32 above, int32 aboveLeft)
{
    int32 lo = left < above ? left : above;
    int32 hi = left < above ? above : left;
    if (aboveLeft >= hi)
        return lo;
    if (aboveLeft <= lo)
        return hi;
    return left + above - aboveLeft;
}

//----------------------------------------------------------------------------------------------------------------------
static dword predictColour(const Image &img, int32 x, int32 y)
{
    // MED (median edge detector) predictor of JPEG-LS, applied per channel
    if (y == 0)
        return x == 0 ? 0 : img.getPixel(x - 1, 0);
    if (x == 0)
        return img.getPixel(0, y - 1);

    dword left = img.getPixel(x - 1, y);
    dword above = img.getPixel(x, y - 1);
    dword aboveLeft = img.getPixel(x - 1, y - 1);
    dword colour = 0;
    for (int32 shift = 16; shift >= 0; shift -= 8)
    {
        int32 v = medChannel((left >> shift) & 255, (above >> shift) & 255, (aboveLeft >> shift) & 255);
        colour |= (dword)v << shift;
    }
    return colour;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 wrapSigned(int32 v)
{
    v &= 255;
    return v >= 128 ? v - 256 : v;
}

//----------------------------------------------------------------------------------------------------------------------
static Residual computeResidual(dword colour, dword predicted)
{
    int32 r = wrapSigned((int32)((colour >> 16) & 255) - (int32)((predicted >> 16) & 255));
    int32 g = wrapSigned((int32)((colour >> 8) & 255) - (int32)((predicted >> 8) & 255));
    int32 b = wrapSigned((int32)(colour & 255) - (int32)(predicted & 255));
    Residual res;
    res.dg = g;
    res.dr = wrapSigned(r - g);
    res.db = wrapSigned(b - g);
    return res;
}

//----------------------------------------------------------------------------------------------------------------------
static dword applyResidual(dword predicted, int32 dg, int32 dr, int32 db)
{
    int32 r = (int32)((predicted >> 16) & 255) + dg + dr;
    int32 g = (int32)((predicted >> 8) & 255) + dg;
    int32 b = (int32)(predicted & 255) + dg + db;
    return ((dword)(r & 255) << 16) | ((dword)(g & 255) << 8) | (dword)(b & 255);
}

//----------------------------------------------------------------------------------------------------------------------
static bool residualFits(const Residual &res, int32 gLo, int32 gHi, int32 cLo, int32 cHi)
{
    return res.dg >= gLo && res.dg <= gHi && res.dr >= cLo && res.dr <= cHi && res.db >= cLo && res.db <= cHi;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 residualCost(const Residual &res)
{
    // nibbles needed by the shortest residual code, 0 if none fits
    if (res.dg == 0 && res.dr == 0 && res.db == 0)
        return 2;                                       // 02
    if (residualFits(res, -2, 1, -2, 1))
        return 3;                                       // 04..07 n
    if (residualFits(res, -8, 7, -8, 7))
        return 4;                                       // D G R B
    if (residualFits(res, -16, 15, -8, 7))
        return 5;                                       // 08..09 nnn
    if (residualFits(res, -128, 127, -16, 15))
        return 6;                                       // 0A..0D nnnn
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
static void encodeResidual(NibbleWriter &out, const Residual &res, int32 cost, EncodeStats &stats)
{
    dword v;
    switch (cost)
    {
    case 2:
        out.put(0);
        out.put(2);
        stats.medExact++;
        break;
    case 3:
        v = ((dword)(res.dg + 2) << 4) | ((dword)(res.dr + 2) << 2) | (dword)(res.db + 2);
        out.put(0);
        out.put(4 + (v >> 4));
        out.put(v & 15);
        stats.deltaSmall++;
        break;
    case 4:
        out.put(0xD);
        out.put((dword)res.dg & 15);
        out.put((dword)res.dr & 15);
        out.put((dword)res.db & 15);
        stats.codeD++;
        break;
    case 5:
        v = ((dword)(res.dg + 16) << 8) | ((dword)(res.dr + 8) << 4) | (dword)(res.db + 8);
        out.put(0);
        out.put(8 + (v >> 12));
        out.putN(v & 0xFFF, 3);
        stats.deltaMedium++;
        break;
    default:
        v = ((dword)(res.dg & 255) << 10) | ((dword)(res.dr + 16) << 5) | (dword)(res.db + 16);
        out.put(0);
        out.put(0xA + (v >> 16));
        out.putN(v & 0xFFFF, 4);
        stats.deltaWide++;
        break;
    }
}

//----------------------------------------------------------------------------------------------------------------------
static void putLittleEndian(byte *p, dword v, int32 bytes)
{
    for (int32 i = 0; i < bytes; i++)
        p[i] = (byte)(v >> (i * 8));
}

//----------------------------------------------------------------------------------------------------------------------
static dword getLittleEndian(const byte *p, int32 bytes)
{
    dword v = 0;
    for (int32 i = bytes - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 selectCache(const hmap<dword, int32> &blockColours, const hmap<dword, vector<int32> > &colourBlocks,
                         int32 blockNo, vector<int32> &score, vector<int32> &touched)
{
    // score[dist] = how many colours of the block are in the final cache1 of block (blockNo - 1 - dist)
    touched.clear();
    for (hmap<dword, int32>::const_iterator it = blockColours.begin(); it != blockColours.end(); ++it)
    {
        hmap<dword, vector<int32> >::const_iterator cb = colourBlocks.find(it->first);
        if (cb == colourBlocks.end())
            continue;
        const vector<int32> &blocks = cb->second;
        for (int32 k = (int32)blocks.size() - 1; k >= 0; k--)
        {
            int32 dist = blockNo - 1 - blocks[k];
            if (dist >= CACHE_SEARCH_WINDOW)
                break;
            if (score[dist] == 0)
                touched.push_back(dist);
            score[dist]++;
        }
    }

    int32 bestDist = -1;
    int32 bestScore = 0;
    for (size_t t = 0; t < touched.size(); t++)
    {
        int32 dist = touched[t];
        if (score[dist] > bestScore || (score[dist] == bestScore && dist < bestDist))
        {
            bestScore = score[dist];
            bestDist = dist;
        }
        score[dist] = 0;
    }
    return bestDist;
}

//----------------------------------------------------------------------------------------------------------------------
static void encodeRun(NibbleWriter &out, int32 len, EncodeStats &stats)
{
    // len is at most BLOCK_PIXELS (256), which fits in one 4XX code (max 274)
    if (len <= 18)
    {
        out.put(3);
        out.put(len - 3);
        stats.run3++;
    }
    else
    {
        out.put(4);
        out.putN(len - 19, 2);
        stats.run4++;
    }
    stats.runPixels += len;
}

//----------------------------------------------------------------------------------------------------------------------
static void encodeMedRun(NibbleWriter &out, int32 len, EncodeStats &stats)
{
    // len is 2..BLOCK_PIXELS (256), which fits in one 0E NN code (max 273)
    if (len <= 17)
    {
        out.put(0);
        out.put(3);
        out.put(len - 2);
    }
    else
    {
        out.put(0);
        out.put(0xE);
        out.putN(len - 18, 2);
    }
    stats.medRun++;
    stats.medRunPixels += len;
}

//----------------------------------------------------------------------------------------------------------------------
static const char *residualMnemonic(int32 cost)
{
    switch (cost)
    {
    case 2:
        return "EXACT";
    case 3:
        return "SMALL";
    case 4:
        return "DELTA";
    case 5:
        return "MEDIUM";
    default:
        return "WIDE";
    }
}

//----------------------------------------------------------------------------------------------------------------------
static void addFreqNoted(Cache1 &cache, dword colour, dword count, char *note, size_t size)
{
    // same as Cache1::addFreq, also describes the change for the report
    int32 slot = cache.find(colour);
    if (slot < 0)
    {
        snprintf(note, size, "not in cache1, no freq update");
        return;
    }
    dword old = cache.slots[slot].freq;
    cache.slots[slot].freq += count;
    snprintf(note, size, "cache1 slot %d freq %u -> %u", slot, old, cache.slots[slot].freq);
}

//----------------------------------------------------------------------------------------------------------------------
static void encodePixel(dword c, dword predicted, int32 remaining, Cache1 &cache, Cache2 &cache2,
                        NibbleWriter &out, EncodeStats &stats, PixReport *report, const Image &img, int32 x, int32 y)
{
    // cheapest code first; residual codes compress better with zlib than cache indexes,
    // so a colour is only inserted into a cache when no residual code fits
    size_t start = out.count;
    if (y > 0 && c == img.getPixel(x, y - 1))
    {
        out.put(0xF);
        stats.above++;
        if (report)
            report->code(out, start, "ABOVE", x, y, 1, c, "= pixel above");
        return;
    }

    Residual res = computeResidual(c, predicted);
    int32 cost = residualCost(res);
    if (cost == 2)
    {
        encodeResidual(out, res, cost, stats);
        if (report)
            report->code(out, start, "EXACT", x, y, 1, c, "= MED prediction");
        return;
    }

    int32 slot = cache.find(c);
    if (slot >= 0)
    {
        out.put(5 + slot / 16);
        out.put(slot % 16);
        cache.slots[slot].freq++;
        stats.cache1++;
        if (report)
            report->code(out, start, "C1", x, y, 1, c, "cache1 slot %d, freq %u -> %u", slot,
                         cache.slots[slot].freq - 1, cache.slots[slot].freq);
        return;
    }

    // cache2 costs 4 nibbles: it wins over 5 and 6 nibble residual codes
    int32 slot2 = cache2.find(c);
    if (cost != 0 && (slot2 < 0 || cost <= 4))
    {
        encodeResidual(out, res, cost, stats);
        if (report)
            report->code(out, start, residualMnemonic(cost), x, y, 1, c, "pred #%06X  dG=%+d dR=%+d dB=%+d",
                         predicted, res.dg, res.dr, res.db);
        return;
    }

    char why[80];
    if (cost == 0)
        snprintf(why, sizeof(why), "no residual code fits (pred #%06X dG=%+d dR=%+d dB=%+d)",
                 predicted, res.dg, res.dr, res.db);
    else
        snprintf(why, sizeof(why), "residual code would need %d nibbles", cost);

    if (slot2 >= 0)
    {
        out.put(0xA + slot2 / 4096);
        out.putN(slot2 % 4096, 3);
        stats.cache2++;
        if (report)
            report->code(out, start, "C2", x, y, 1, c, "cache2 slot %d; %s", slot2, why);
        return;
    }

    if (remaining >= C_MIN_COUNT)
    {
        out.put(0xC);
        out.putN(c, 6);
        Cache1Eviction ev;
        int32 inserted = cache.insert(c, cache2, &ev);
        stats.codeC++;
        if (report)
        {
            char evicted[64] = "";
            if (ev.evicted && ev.cache2Slot >= 0)
                snprintf(evicted, sizeof(evicted), ", replaces #%06X -> cache2 slot %d", ev.colour, ev.cache2Slot);
            else if (ev.evicted)
                snprintf(evicted, sizeof(evicted), ", replaces #%06X (already in cache2)", ev.colour);
            report->code(out, start, "NEW", x, y, 1, c, "-> cache1 slot %d%s (%d pixels of this colour left in block); %s",
                         inserted, evicted, remaining, why);
        }
    }
    else
    {
        out.put(0xE);
        out.putN(c, 6);
        int32 inserted = cache2.insert(c);
        stats.codeE++;
        if (report)
            report->code(out, start, "NEW", x, y, 1, c, "-> cache2 slot %d (%d pixel%s of this colour left in block); %s",
                         inserted, remaining, remaining == 1 ? "" : "s", why);
    }
}

//----------------------------------------------------------------------------------------------------------------------
static void encodeBlock(const Image &img, int32 bx, int32 by, const dword *px, hmap<dword, int32> &remaining,
                        Cache1 &cache, Cache2 &cache2, ColourHistory &hist, NibbleWriter &out,
                        EncodeStats &stats, PixReport *report)
{
    dword predicted[BLOCK_PIXELS];
    for (int32 i = 0; i < BLOCK_PIXELS; i++)
        predicted[i] = predictColour(img, bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE);

    char note[80];
    int32 i = 0;
    while (i < BLOCK_PIXELS)
    {
        dword c = px[i];
        int32 x = bx + i % BLOCK_SIZE;
        int32 y = by + i / BLOCK_SIZE;
        size_t start = out.count;

        int32 lastRun = 0;
        if (c == hist.last)
        {
            lastRun = 1;
            while (i + lastRun < BLOCK_PIXELS && px[i + lastRun] == c)
                lastRun++;
        }
        int32 medRun = 0;
        while (i + medRun < BLOCK_PIXELS && px[i + medRun] == predicted[i + medRun])
            medRun++;

        if (medRun >= 2 && medRun > lastRun)
        {
            encodeMedRun(out, medRun, stats);
            if (report)
                report->code(out, start, "MEDRUN", x, y, medRun, c, "each pixel = its MED prediction:");
            for (int32 k = 0; k < medRun; k++)
            {
                if (report)
                    report->pixel(bx + (i + k) % BLOCK_SIZE, by + (i + k) / BLOCK_SIZE, px[i + k], "MED prediction");
                hist.place(px[i + k]);
                remaining[px[i + k]]--;
            }
            i += medRun;
            continue;
        }

        if (lastRun > 0)
        {
            if (lastRun >= 3)
            {
                encodeRun(out, lastRun, stats);
                addFreqNoted(cache, c, lastRun, note, sizeof(note));
                if (report)
                    report->code(out, start, "RUN", x, y, lastRun, c, "last colour %d times; %s", lastRun, note);
            }
            else
            {
                for (int32 k = 0; k < lastRun; k++)
                {
                    start = out.count;
                    out.put(1);
                    addFreqNoted(cache, c, 1, note, sizeof(note));
                    if (report)
                        report->code(out, start, "LAST", bx + (i + k) % BLOCK_SIZE, by + (i + k) / BLOCK_SIZE, 1, c,
                                     "last colour; %s", note);
                }
                stats.last += lastRun;
            }
            remaining[c] -= lastRun;
            i += lastRun;
            continue;
        }

        if (c == hist.nextToLast)
        {
            out.put(2);
            addFreqNoted(cache, c, 1, note, sizeof(note));
            stats.nextToLast++;
            if (report)
                report->code(out, start, "NEXT", x, y, 1, c, "next to last colour (swaps history); %s", note);
        }
        else
            encodePixel(c, predicted[i], remaining[c], cache, cache2, out, stats, report, img, x, y);
        hist.place(c);
        remaining[c]--;
        i++;
    }
}

//----------------------------------------------------------------------------------------------------------------------
bool encodePixStream(const Image &img, vector<byte> &stream, EncodeStats &stats, PixReport *report)
{
    if (!checkDimensions(img.width, img.height))
        return false;
    if (img.width % BLOCK_SIZE != 0 || img.height % BLOCK_SIZE != 0)
    {
        Image padded;
        padImage(img, padded);
        if (report)
            report->comment("image padded from %d x %d to %d x %d (last column and last row repeated)",
                            img.width, img.height, padded.width, padded.height);
        return encodePixStream(padded, stream, stats, report);
    }

    NibbleWriter out;
    Cache1 cache;
    Cache2 cache2;
    ColourHistory hist;
    vector<Cache1> finalCaches(CACHE_SEARCH_WINDOW);   // ring buffer of final cache1 of previous blocks
    hmap<dword, vector<int32> > colourBlocks;           // colour -> blocks whose final cache1 contains it
    hmap<dword, int32> remaining;                       // colour -> pixels left in current block
    vector<int32> score(CACHE_SEARCH_WINDOW, 0);
    vector<int32> touched;
    dword px[BLOCK_PIXELS];
    int32 blockNo = 0;

    for (int32 by = 0; by < img.height; by += BLOCK_SIZE)
    {
        for (int32 bx = 0; bx < img.width; bx += BLOCK_SIZE)
        {
            remaining.clear();
            for (int32 i = 0; i < BLOCK_PIXELS; i++)
            {
                px[i] = img.getPixel(bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE);
                remaining[px[i]]++;
            }

            if (report)
                report->block(blockNo, bx, by);
            int32 dist = selectCache(remaining, colourBlocks, blockNo, score, touched);
            if (dist >= 0)
            {
                size_t start = out.count;
                int32 n = nibblesNeeded((dword)dist);
                out.put(0);
                out.put(1);
                out.put(n);
                out.putN((dword)dist, n);
                cache.copyFrom(finalCaches[(blockNo - 1 - dist) % CACHE_SEARCH_WINDOW]);
                stats.cacheSelect++;
                if (report)
                    report->code(out, start, "SELECT", -1, 0, 0, 0,
                                 "cache1 <- final cache1 of block %d (distance %d), freq / 4", blockNo - 1 - dist, dist);
            }
            else
            {
                cache.clear();
                if (report)
                    report->comment("no cache1 selected (no earlier cache has colours of this block): cache1 is empty");
            }

            encodeBlock(img, bx, by, px, remaining, cache, cache2, hist, out, stats, report);

            finalCaches[blockNo % CACHE_SEARCH_WINDOW] = cache;
            for (int32 i = 0; i < CACHE1_SIZE; i++)
                if (cache.slots[i].used)
                    colourBlocks[cache.slots[i].colour].push_back(blockNo);
            blockNo++;
        }
    }

    size_t endStart = out.count;
    out.put(0);
    out.put(0);
    if (report)
    {
        report->comment("end of image");
        report->code(out, endStart, "END", -1, 0, 0, 0, "end of stream");
    }
    stats.nibbles = out.count;
    stream.swap(out.data);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
static bool decodeError(const char *msg, size_t nibblePos)
{
    setPixError("Decode error: %s (at nibble %zu)", msg, nibblePos);
    return false;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 signedNibble(dword n)
{
    return n >= 8 ? (int32)n - 16 : (int32)n;
}

//----------------------------------------------------------------------------------------------------------------------
bool decodePixStream(const vector<byte> &stream, Image &img)
{
    if (!checkDimensions(img.width, img.height))
        return false;
    if (img.width % BLOCK_SIZE != 0 || img.height % BLOCK_SIZE != 0)
    {
        // the stream holds the padded image: decode it, then keep only the original area
        Image padded;
        padded.width = paddedSize(img.width);
        padded.height = paddedSize(img.height);
        if (!decodePixStream(stream, padded))
            return false;
        img.rgb.resize((size_t)img.width * img.height * 3);
        for (int32 y = 0; y < img.height; y++)
            memcpy(&img.rgb[(size_t)y * img.width * 3], &padded.rgb[(size_t)y * padded.width * 3],
                   (size_t)img.width * 3);
        return true;
    }
    img.rgb.assign((size_t)img.width * img.height * 3, 0);

    NibbleReader in(stream.empty() ? NULL : &stream[0], stream.size());
    Cache1 cache;
    Cache2 cache2;
    ColourHistory hist;
    vector<Cache1> finalCaches(CACHE_SEARCH_WINDOW);
    int32 blockNo = 0;

    for (int32 by = 0; by < img.height; by += BLOCK_SIZE)
    {
        for (int32 bx = 0; bx < img.width; bx += BLOCK_SIZE)
        {
            if (in.peek() == 0 && in.peekAt(1) == 1)
            {
                in.getN(2);
                dword n = in.get();
                if (n < 1 || n > 4)
                    return decodeError("invalid cache selection length", in.pos);
                dword dist = in.getN((int32)n);
                if (in.error)
                    return decodeError("unexpected end of stream", in.pos);
                if (dist >= CACHE_SEARCH_WINDOW || (int32)dist >= blockNo)
                    return decodeError("invalid cache selection", in.pos);
                cache.copyFrom(finalCaches[(blockNo - 1 - (int32)dist) % CACHE_SEARCH_WINDOW]);
            }
            else
                cache.clear();

            int32 i = 0;
            while (i < BLOCK_PIXELS)
            {
                dword op = in.get();
                dword c = 0;
                int32 count = 1;
                bool medRun = false;
                int32 slot;
                dword v;
                switch (op)
                {
                case 0:
                    op = in.get();
                    switch (op)
                    {
                    case 2:
                        c = predictColour(img, bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE);
                        break;
                    case 3:
                        count = (int32)in.get() + 2;
                        medRun = true;
                        break;
                    case 4:
                    case 5:
                    case 6:
                    case 7:
                        v = ((op - 4) << 4) | in.get();
                        c = applyResidual(predictColour(img, bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE),
                                          (int32)(v >> 4) - 2, (int32)((v >> 2) & 3) - 2, (int32)(v & 3) - 2);
                        break;
                    case 8:
                    case 9:
                        v = ((op - 8) << 12) | in.getN(3);
                        c = applyResidual(predictColour(img, bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE),
                                          (int32)(v >> 8) - 16, (int32)((v >> 4) & 15) - 8, (int32)(v & 15) - 8);
                        break;
                    case 0xA:
                    case 0xB:
                    case 0xC:
                    case 0xD:
                        v = ((op - 0xA) << 16) | in.getN(4);
                        c = applyResidual(predictColour(img, bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE),
                                          (int32)(v >> 10), (int32)((v >> 5) & 31) - 16, (int32)(v & 31) - 16);
                        break;
                    case 0xE:
                        count = (int32)in.getN(2) + 18;
                        medRun = true;
                        break;
                    default:
                        if (in.error)
                            return decodeError("unexpected end of stream", in.pos);
                        return decodeError("invalid 0X code", in.pos);
                    }
                    break;
                case 1:
                    c = hist.last;
                    cache.addFreq(c, 1);
                    break;
                case 2:
                    c = hist.nextToLast;
                    cache.addFreq(c, 1);
                    break;
                case 3:
                    count = (int32)in.get() + 3;
                    c = hist.last;
                    cache.addFreq(c, count);
                    break;
                case 4:
                    count = (int32)in.getN(2) + 19;
                    c = hist.last;
                    cache.addFreq(c, count);
                    break;
                case 5:
                case 6:
                case 7:
                case 8:
                case 9:
                    slot = (int32)(op - 5) * 16 + (int32)in.get();
                    if (!cache.slots[slot].used)
                        return decodeError("empty cache1 slot", in.pos);
                    c = cache.slots[slot].colour;
                    cache.slots[slot].freq++;
                    break;
                case 0xA:
                case 0xB:
                    slot = (int32)(op - 0xA) * 4096 + (int32)in.getN(3);
                    if (!cache2.used[slot])
                        return decodeError("empty cache2 slot", in.pos);
                    c = cache2.colours[slot];
                    break;
                case 0xC:
                    c = in.getN(6);
                    cache.insert(c, cache2);
                    break;
                case 0xD:
                    v = in.getN(3);
                    c = applyResidual(predictColour(img, bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE),
                                      signedNibble(v >> 8), signedNibble((v >> 4) & 15), signedNibble(v & 15));
                    break;
                case 0xE:
                    c = in.getN(6);
                    cache2.insert(c);
                    break;
                case 0xF:
                    if (by + i / BLOCK_SIZE == 0)
                        return decodeError("no pixel above in the top row", in.pos);
                    c = img.getPixel(bx + i % BLOCK_SIZE, by + i / BLOCK_SIZE - 1);
                    break;
                default:
                    if (in.error)
                        return decodeError("unexpected end of stream", in.pos);
                    return decodeError("invalid code", in.pos);
                }
                if (in.error)
                    return decodeError("unexpected end of stream", in.pos);
                if (i + count > BLOCK_PIXELS)
                    return decodeError("run crosses block boundary", in.pos);

                for (int32 k = 0; k < count; k++, i++)
                {
                    int32 x = bx + i % BLOCK_SIZE;
                    int32 y = by + i / BLOCK_SIZE;
                    // MED run: each pixel is predicted from already decoded pixels
                    if (medRun)
                        c = predictColour(img, x, y);
                    img.setPixel(x, y, c);
                    if (medRun)
                        hist.place(c);
                }
                if (!medRun)
                    hist.place(c);
            }

            finalCaches[blockNo % CACHE_SEARCH_WINDOW] = cache;
            blockNo++;
        }
    }

    dword end1 = in.get();
    dword end2 = in.get();
    if (in.error || end1 != 0 || end2 != 0)
        return decodeError("missing finish code", in.pos);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
static void writeHeader(vector<byte> &file, int32 algorithm, const Image &img, dword packedSize, dword unpackedSize)
{
    file[0] = 'P';
    file[1] = 'I';
    file[2] = 'X';
    file[3] = 0;
    file[4] = 'A';                                  // algorithm as text: "A1", "A2"
    file[5] = (byte)('0' + algorithm);
    putLittleEndian(&file[6], (dword)img.width, 3);
    putLittleEndian(&file[9], (dword)img.height, 3);
    putLittleEndian(&file[12], packedSize, 4);      // size of the data after the header
    putLittleEndian(&file[16], unpackedSize, 4);
}

//----------------------------------------------------------------------------------------------------------------------
bool encodePixFile(const Image &img, vector<byte> &file, EncodeStats &stats, PixReport *report)
{
    vector<byte> stream;
    if (!encodePixStream(img, stream, stats, report))
        return false;

    uLongf compSize = compressBound((uLong)stream.size());
    file.assign(PIX_HEADER_SIZE + compSize, 0);
    int r = compress2(&file[PIX_HEADER_SIZE], &compSize, &stream[0], (uLong)stream.size(), Z_BEST_COMPRESSION);
    if (r != Z_OK)
    {
        setPixError("zlib compression failed (%d)", r);
        return false;
    }
    file.resize(PIX_HEADER_SIZE + compSize);
    // unpacked size: nibble stream
    writeHeader(file, 1, img, (dword)compSize, (dword)stream.size());
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool encodePix2File(const Image &img, vector<byte> &file, Pix2Stats &stats)
{
    if (!checkDimensions(img.width, img.height))
        return false;
    vector<byte> data;
    if (!encodePix2Data(img, data, stats))
        return false;
    file.assign(PIX_HEADER_SIZE, 0);
    file.insert(file.end(), data.begin(), data.end());
    // unpacked size: raw RGB data
    writeHeader(file, 2, img, (dword)data.size(), (dword)img.rgb.size());
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool decodePixFile(const vector<byte> &file, Image &img)
{
    if (file.size() < PIX_HEADER_SIZE || memcmp(&file[0], "PIX\0", 4) != 0)
    {
        setPixError("Not a PIX file");
        return false;
    }
    if (file[4] != 'A' || file[5] < '1' || file[5] > '9')
    {
        setPixError("Invalid algorithm in PIX header (bytes 0x%02X 0x%02X, expected \"A1\" or \"A2\")",
                    file[4], file[5]);
        return false;
    }
    int32 algorithm = file[5] - '0';
    if (algorithm > 2)
    {
        setPixError("Unsupported PIX algorithm %d", algorithm);
        return false;
    }
    img.width = (int32)getLittleEndian(&file[6], 3);
    img.height = (int32)getLittleEndian(&file[9], 3);
    if (!checkDimensions(img.width, img.height))
        return false;
    dword packedSize = getLittleEndian(&file[12], 4);
    if (packedSize != file.size() - PIX_HEADER_SIZE)
    {
        setPixError("PIX data is damaged or truncated (file has %zu bytes of data, header says %u)",
                    file.size() - PIX_HEADER_SIZE, packedSize);
        return false;
    }

    dword unpackedSize = getLittleEndian(&file[16], 4);
    if (algorithm == 2)
    {
        if (unpackedSize != (size_t)img.width * img.height * 3)
        {
            setPixError("Invalid unpacked size in PIX header");
            return false;
        }
        return decodePix2Data(&file[0] + PIX_HEADER_SIZE, packedSize, img);
    }
    // algorithm 1; longest possible stream: 7 nibbles per pixel, 7 per cache selection, 2 for finish
    dword streamSize = unpackedSize;
    size_t pixels = (size_t)paddedSize(img.width) * paddedSize(img.height);
    size_t maxNibbles = pixels * 7 + (pixels / BLOCK_PIXELS) * 7 + 2;
    if (streamSize == 0 || streamSize > (maxNibbles + 1) / 2)
    {
        setPixError("Invalid unpacked size in PIX header");
        return false;
    }

    vector<byte> stream(streamSize);
    uLongf destLen = streamSize;
    int r = uncompress(&stream[0], &destLen, &file[0] + PIX_HEADER_SIZE, (uLong)packedSize);
    if (r != Z_OK || destLen != streamSize)
    {
        setPixError("PIX data is damaged or truncated (zlib error %d)", r);
        return false;
    }
    return decodePixStream(stream, img);
}
