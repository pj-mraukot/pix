#ifndef PIX_CODEC_H
#define PIX_CODEC_H

#include "types.h"
#include "pix2_codec.h"

#define BLOCK_SIZE          16
#define BLOCK_PIXELS        (BLOCK_SIZE * BLOCK_SIZE)
#define CACHE1_SIZE         80
#define CACHE2_SIZE         8192
#define CACHE_SEARCH_WINDOW 4096    // how many previous blocks' cache1 can be selected
#define C_MIN_COUNT         3       // no residual code fits and colour occurs >= 3 times in rest of block -> C, else E
#define PIX_HEADER_SIZE     20      // magic 4, algorithm 2, width 3, height 3, packed size 4, unpacked size 4 bytes
#define PIX_MAX_DIMENSION   0xFFFFFF    // width and height are stored in 24 bits

// global cache of 8192 colours, filled round robin (codes E and colours removed from cache1)
class Cache2
{
public:
    vector<dword> colours;
    vector<bool> used;
    hmap<dword, int32> index;  // colour -> slot
    int32 slotIndex;

    Cache2() : colours(CACHE2_SIZE, 0), used(CACHE2_SIZE, false), slotIndex(0) {}

    int32 find(dword colour) const
    {
        hmap<dword, int32>::const_iterator it = index.find(colour);
        return it == index.end() ? -1 : it->second;
    }

    // returns the slot the colour was written to
    int32 insert(dword colour)
    {
        int32 slot = slotIndex;
        if (used[slot])
        {
            // a damaged stream may hold a colour twice: keep the index of the newer copy
            hmap<dword, int32>::iterator it = index.find(colours[slot]);
            if (it != index.end() && it->second == slot)
                index.erase(it);
        }
        colours[slot] = colour;
        used[slot] = true;
        index[colour] = slot;
        slotIndex = (slotIndex + 1) % CACHE2_SIZE;
        return slot;
    }
};

// what happened to the colour removed from cache1 by an insertion
struct Cache1Eviction
{
    bool evicted;       // a used slot was overwritten
    dword colour;       // its colour
    int32 cache2Slot;   // where it went in cache2, -1 if it was already there
};

struct Cache1Slot
{
    dword colour;
    dword freq;
    bool used;
};

// cache of 80 recently used colours, one per 16x16 block
class Cache1
{
public:
    Cache1Slot slots[CACHE1_SIZE];
    int32 slotIndex;

    Cache1() { clear(); }

    void clear()
    {
        for (int32 i = 0; i < CACHE1_SIZE; i++)
        {
            slots[i].colour = 0;
            slots[i].freq = 0;
            slots[i].used = false;
        }
        slotIndex = 0;
    }

    // copies selected cache of a previous block, frequencies divided by 4
    void copyFrom(const Cache1 &src)
    {
        for (int32 i = 0; i < CACHE1_SIZE; i++)
        {
            slots[i] = src.slots[i];
            slots[i].freq /= 4;
        }
        slotIndex = 0;
    }

    int32 find(dword colour) const
    {
        for (int32 i = 0; i < CACHE1_SIZE; i++)
            if (slots[i].used && slots[i].colour == colour)
                return i;
        return -1;
    }

    // adds count to frequency if the colour is in the cache
    void addFreq(dword colour, dword count)
    {
        int32 i = find(colour);
        if (i >= 0)
            slots[i].freq += count;
    }

    // inserts into slot with smallest freq among slotIndex..79 (unused slots first);
    // a colour removed from the slot is first inserted into cache2, unless cache2 already has it
    int32 insert(dword colour, Cache2 &cache2, Cache1Eviction *eviction = NULL)
    {
        int32 best = slotIndex;
        for (int32 i = slotIndex; i < CACHE1_SIZE; i++)
        {
            if (!slots[i].used)
            {
                best = i;
                break;
            }
            if (slots[i].freq < slots[best].freq)
                best = i;
        }
        Cache1Eviction ev;
        ev.evicted = slots[best].used;
        ev.colour = slots[best].colour;
        ev.cache2Slot = -1;
        if (ev.evicted && cache2.find(ev.colour) < 0)
            ev.cache2Slot = cache2.insert(ev.colour);
        if (eviction)
            *eviction = ev;
        slots[best].colour = colour;
        slots[best].freq = 1;
        slots[best].used = true;
        slotIndex = (slotIndex + 1) % CACHE1_SIZE;
        return best;
    }
};

class NibbleWriter
{
public:
    vector<byte> data;
    bool half;
    size_t count;

    NibbleWriter() : half(false), count(0) {}

    void put(dword nibble)
    {
        if (!half)
            data.push_back((byte)((nibble & 15) << 4));
        else
            data.back() |= (byte)(nibble & 15);
        half = !half;
        count++;
    }

    dword nibbleAt(size_t pos) const
    {
        byte b = data[pos / 2];
        return (pos & 1) ? (b & 15) : (b >> 4);
    }

    // puts value as 'nibbles' nibbles, most significant first
    void putN(dword value, int32 nibbles)
    {
        for (int32 i = nibbles - 1; i >= 0; i--)
            put(value >> (i * 4));
    }
};

class NibbleReader
{
public:
    const byte *data;
    size_t size;    // in bytes
    size_t pos;     // in nibbles
    bool error;

    NibbleReader(const byte *d, size_t s) : data(d), size(s), pos(0), error(false) {}

    dword get()
    {
        if (pos >= size * 2)
        {
            error = true;
            return 0;
        }
        byte b = data[pos / 2];
        dword n = (pos & 1) ? (b & 15) : (b >> 4);
        pos++;
        return n;
    }

    dword peek()
    {
        return peekAt(0);
    }

    dword peekAt(size_t offset)
    {
        size_t p = pos + offset;
        if (p >= size * 2)
            return 0;
        byte b = data[p / 2];
        return (p & 1) ? (b & 15) : (b >> 4);
    }

    dword getN(int32 nibbles)
    {
        dword v = 0;
        for (int32 i = 0; i < nibbles; i++)
            v = (v << 4) | get();
        return v;
    }
};

// statistics of the encoder: how many times each code was used
struct EncodeStats
{
    size_t cacheSelect;
    size_t last;
    size_t nextToLast;
    size_t run3;
    size_t run4;
    size_t runPixels;
    size_t cache1;
    size_t cache2;
    size_t medExact;
    size_t medRun;
    size_t medRunPixels;
    size_t deltaSmall;
    size_t deltaMedium;
    size_t deltaWide;
    size_t codeC;
    size_t codeD;
    size_t codeE;
    size_t above;
    size_t nibbles;

    EncodeStats() : cacheSelect(0), last(0), nextToLast(0), run3(0), run4(0), runPixels(0),
                    cache1(0), cache2(0), medExact(0), medRun(0), medRunPixels(0),
                    deltaSmall(0), deltaMedium(0), deltaWide(0), codeC(0), codeD(0), codeE(0), above(0), nibbles(0) {}
};

class PixReport;

// encodes image into uncompressed nibble stream, optionally writing a listing of every code to report
bool encodePixStream(const Image &img, vector<byte> &stream, EncodeStats &stats, PixReport *report = NULL);

// decodes uncompressed nibble stream into image (width and height must be set);
// any size works, the stream holds the image padded to whole 16x16 blocks
bool decodePixStream(const vector<byte> &stream, Image &img);

// encodes image with algorithm 1 and builds complete .pix file content (header + zlib compressed stream)
bool encodePixFile(const Image &img, vector<byte> &file, EncodeStats &stats, PixReport *report = NULL);

// encodes image with algorithm 2 and builds complete .pix file content (header + algorithm 2 data)
bool encodePix2File(const Image &img, vector<byte> &file, Pix2Stats &stats);

// decodes complete .pix file content (any algorithm)
bool decodePixFile(const vector<byte> &file, Image &img);

#endif
