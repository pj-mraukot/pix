#include <stdio.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include "pix2_codec.h"
#include "pix_error.h"

using namespace std;

#define P2_PREDICTORS   5       // W, N, W+N-NW, W+NE-N, (W+N)/2
#define P2_ACT_BUCKETS  16      // local activity (sum of neighbour residuals)
#define P2_EG_BUCKETS   5       // size of the G residual of the same pixel (contexts of R and B)
#define P2_MAX_EXP      7       // |residual| <= 128 has exponent 0..7
#define P2_BIAS_LIMIT   64      // bias statistics are halved after this many samples
#define P2_BIAS_CLIP    16      // largest error sample used for the bias statistics
#define P2_CANDIDATES   9       // predicted colour, 2 colours remembered for the neighbourhood, neighbours
#define P2_HASH_BITS    20      // tables of colours remembered for a neighbourhood
#define P2_HASH_VALID   0x1000000u  // marks a used entry of these tables
#define P2_RECENT       16384   // recently used colours (move to front list)
#define P2_STAMPS       65536   // use timestamps of the recent list before they are renumbered (power of 2)
#define P2_INDEX_EXP    14      // recent list index + 1 <= 16384 has exponent 0..14
#define P2_MODE_CTX     16      // coding mode of W x coding mode of N
#define P2_CRC_SIZE     4
#define P2_FAST         4       // adaptation shift of the fast probability estimate
#define P2_SLOW         9       // adaptation shift of the slow probability estimate

// how a pixel is coded (written to the stream as a sequence of binary decisions)
#define MODE_PREDICTED  0       // the predicted colour
#define MODE_NEIGHBOUR  1       // another candidate: a neighbour or a colour remembered for the neighbourhood
#define MODE_RECENT     2       // colour from the recently used list
#define MODE_RESIDUAL   3       // residual of each channel

static const int32 channelOrder[3] = {1, 0, 2};    // G first, then R and B use its residual

// cost of coding a bit with probability (index + 0.5) / 4096, in 1/256 bits (encoder decisions and statistics)
static int32 costTable[4096];

// weight of a predictor from its error sum s (1..P2_MAX_ERRSUM): 2^32 / s^2
#define P2_MAX_ERRSUM   (6 * 255 + 1)
static int64_t weightTable[P2_MAX_ERRSUM + 1];

//----------------------------------------------------------------------------------------------------------------------
static void initTables()
{
    if (weightTable[1] != 0)
        return;
    for (int32 i = 0; i < 4096; i++)
        costTable[i] = (int32)(-log2((i + 0.5) / 4096.0) * 256.0 + 0.5);
    for (int64_t s = 1; s <= P2_MAX_ERRSUM; s++)
        weightTable[s] = ((int64_t)1 << 32) / (s * s);
}

// adaptive probability of a bit being 1, 16 bit precision; a fast and a slow adapting estimate are averaged
class BitModel
{
public:
    int32 fast;
    int32 slow;
    int32 count;

    BitModel() : fast(32768), slow(32768), count(0) {}

    int32 p() const
    {
        int32 v = (fast + slow) >> 1;
        return v < 64 ? 64 : (v > 65536 - 64 ? 65536 - 64 : v);
    }

    int32 cost(int32 bit) const
    {
        int32 q = p();
        return costTable[(bit ? q : 65536 - q) >> 4];
    }

    void update(int32 bit)
    {
        int32 target = bit ? 65536 : 0;
        fast += (target - fast) >> P2_FAST;
        if (count < (1 << P2_SLOW) - 2)
        {
            // the slow estimate starts as an average of all bits seen so far
            count++;
            slow += (target - slow) / (count + 1);
        }
        else
            slow += (target - slow) >> P2_SLOW;
    }
};

// range encoder (LZMA style carry handling)
class RangeEncoder
{
public:
    vector<byte> out;
    uint64_t low;
    dword range;
    byte cache;
    uint64_t cacheSize;

    RangeEncoder() : low(0), range(0xFFFFFFFFu), cache(0), cacheSize(1) {}

    void shiftLow()
    {
        if ((dword)low < 0xFF000000u || (low >> 32) != 0)
        {
            byte carry = (byte)(low >> 32);
            byte temp = cache;
            do
            {
                out.push_back((byte)(temp + carry));
                temp = 0xFF;
            } while (--cacheSize != 0);
            cache = (byte)(low >> 24);
        }
        cacheSize++;
        low = (low & 0x00FFFFFFu) << 8;
    }

    void encode(BitModel &m, int32 bit)
    {
        dword bound = (range >> 16) * (dword)m.p();
        if (bit)
            range = bound;
        else
        {
            low += bound;
            range -= bound;
        }
        m.update(bit);
        while (range < (1u << 24))
        {
            range <<= 8;
            shiftLow();
        }
    }

    void flush()
    {
        for (int32 i = 0; i < 5; i++)
            shiftLow();
    }
};

// range decoder; reading past the end of the data gives zero bytes
class RangeDecoder
{
public:
    const byte *data;
    size_t size;
    size_t pos;
    dword range;
    dword code;

    RangeDecoder(const byte *d, size_t s) : data(d), size(s), pos(0), range(0xFFFFFFFFu), code(0)
    {
        for (int32 i = 0; i < 5; i++)
            code = (code << 8) | next();
    }

    dword next()
    {
        dword b = pos < size ? data[pos] : 0;
        pos++;
        return b;
    }

    int32 decode(BitModel &m)
    {
        dword bound = (range >> 16) * (dword)m.p();
        int32 bit;
        if (code < bound)
        {
            range = bound;
            bit = 1;
        }
        else
        {
            code -= bound;
            range -= bound;
            bit = 0;
        }
        m.update(bit);
        while (range < (1u << 24))
        {
            range <<= 8;
            code = (code << 8) | next();
        }
        return bit;
    }
};

// the encoder and the decoder run the same code: bit() writes the given bit when encoding,
// and returns the decoded bit when decoding
class BitCoder
{
public:
    RangeEncoder *enc;
    RangeDecoder *dec;
    int64_t cost;       // encoder: total cost in 1/256 bits

    BitCoder(RangeEncoder *e, RangeDecoder *d) : enc(e), dec(d), cost(0) {}

    int32 bit(BitModel &m, int32 b)
    {
        if (dec)
            return dec->decode(m);
        cost += m.cost(b);
        enc->encode(m, b);
        return b;
    }
};

// models for coding a residual -128..127: zero flag, sign, exponent (unary), mantissa bits
struct ResidualModel
{
    BitModel zero;
    BitModel sign;
    BitModel exp[P2_MAX_EXP];
    BitModel mant[P2_MAX_EXP + 1][P2_MAX_EXP];
};

// models for coding a number 1..32767: exponent (unary), mantissa bits
struct NumberModel
{
    BitModel exp[P2_INDEX_EXP];
    BitModel mant[P2_INDEX_EXP + 1][P2_INDEX_EXP];
};

// running mean of the prediction error in one context
struct BiasStat
{
    int32 sum;
    int32 count;
};

// prediction of one channel of one pixel
struct ChannelPrediction
{
    int32 preds[P2_PREDICTORS];
    int32 spatial;      // blend of the predictors
    int32 corr;         // bias correction
    int32 biasCtx;
    int32 bucket;       // activity bucket
    int32 base;         // spatial prediction + G error (R and B), before bias correction
    int32 pred;         // final prediction 0..255
};

//----------------------------------------------------------------------------------------------------------------------
static int32 clampByte(int32 v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

//----------------------------------------------------------------------------------------------------------------------
static int32 absValue(int32 v)
{
    return v < 0 ? -v : v;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 wrapResidual(int32 v)
{
    v &= 255;
    return v >= 128 ? v - 256 : v;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 floorLog2(int32 n)
{
    int32 k = 0;
    while ((n >> (k + 1)) != 0)
        k++;
    return k;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 roundedDivide(int32 a, int32 b)
{
    // a / b rounded to nearest, b > 0, same result for negative a as for positive
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

//----------------------------------------------------------------------------------------------------------------------
static int32 activityBucket(int32 a)
{
    static const int32 limits[P2_ACT_BUCKETS - 1] = {0, 1, 2, 3, 4, 6, 8, 11, 15, 20, 28, 40, 56, 80, 112};
    int32 b = 0;
    while (b < P2_ACT_BUCKETS - 1 && a > limits[b])
        b++;
    return b;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 residualBucket(int32 e)
{
    int32 a = absValue(e);
    return a == 0 ? 0 : (a <= 1 ? 1 : (a <= 3 ? 2 : (a <= 7 ? 3 : 4)));
}

//----------------------------------------------------------------------------------------------------------------------
static int32 signOf(int32 v)
{
    return v < 0 ? 0 : (v == 0 ? 1 : 2);
}

//----------------------------------------------------------------------------------------------------------------------
static dword makeColour(int32 r, int32 g, int32 b)
{
    return ((dword)r << 16) | ((dword)g << 8) | (dword)b;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 channelOf(dword colour, int32 c)
{
    return (int32)((colour >> (16 - 8 * c)) & 255);
}

//----------------------------------------------------------------------------------------------------------------------
static int32 codeResidual(BitCoder &bc, ResidualModel &m, int32 e)
{
    // e is ignored when decoding
    if (bc.bit(m.zero, e == 0))
        return 0;
    int32 negative = bc.bit(m.sign, e < 0);
    int32 n = absValue(e);
    int32 kActual = n > 0 ? floorLog2(n) : 0;
    int32 k = 0;
    while (k < P2_MAX_EXP && bc.bit(m.exp[k], k < kActual))
        k++;
    int32 v = 1;
    for (int32 i = k - 1; i >= 0; i--)
        v = (v << 1) | bc.bit(m.mant[k][i], (n >> i) & 1);
    return negative ? -v : v;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 residualCost(const ResidualModel &m, int32 e)
{
    if (e == 0)
        return m.zero.cost(1);
    int32 n = absValue(e);
    int32 k = floorLog2(n);
    int32 cost = m.zero.cost(0) + m.sign.cost(e < 0);
    for (int32 i = 0; i < P2_MAX_EXP && i <= k; i++)
        cost += m.exp[i].cost(i < k);
    for (int32 i = k - 1; i >= 0; i--)
        cost += m.mant[k][i].cost((n >> i) & 1);
    return cost;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 codeNumber(BitCoder &bc, NumberModel &m, int32 n)
{
    // n >= 1 (ignored when decoding)
    int32 kActual = n > 0 ? floorLog2(n) : 0;
    int32 k = 0;
    while (k < P2_INDEX_EXP && bc.bit(m.exp[k], k < kActual))
        k++;
    int32 v = 1;
    for (int32 i = k - 1; i >= 0; i--)
        v = (v << 1) | bc.bit(m.mant[k][i], (n >> i) & 1);
    return v;
}

//----------------------------------------------------------------------------------------------------------------------
static int32 numberCost(const NumberModel &m, int32 n)
{
    int32 k = floorLog2(n);
    int32 cost = 0;
    for (int32 i = 0; i < P2_INDEX_EXP && i <= k; i++)
        cost += m.exp[i].cost(i < k);
    for (int32 i = k - 1; i >= 0; i--)
        cost += m.mant[k][i].cost((n >> i) & 1);
    return cost;
}

// per channel prediction: errors of the predictors in the previous and current row, bias statistics
class ChannelModel
{
public:
    vector<int32> predErr[2][3][P2_PREDICTORS];    // |value - predictor| per pixel, 2 rows, 2 pixels padding each side
    vector<int32> resErr[2][3];                    // |value - final prediction|
    vector<BiasStat> bias;
    vector<ResidualModel> models;

    ChannelModel(int32 width)
    {
        for (int32 r = 0; r < 2; r++)
            for (int32 c = 0; c < 3; c++)
            {
                for (int32 i = 0; i < P2_PREDICTORS; i++)
                    predErr[r][c][i].assign(width + 4, 0);
                resErr[r][c].assign(width + 4, 0);
            }
        BiasStat zero;
        zero.sum = 0;
        zero.count = 0;
        bias.assign(3 * P2_ACT_BUCKETS * 9, zero);
        models.resize(3 * P2_ACT_BUCKETS * P2_EG_BUCKETS);
    }

    ResidualModel &model(int32 c, const ChannelPrediction &cp, int32 eG)
    {
        int32 eb = c == 1 ? 0 : residualBucket(eG);
        return models[(c * P2_ACT_BUCKETS + cp.bucket) * P2_EG_BUCKETS + eb];
    }

    // spatial prediction of channel c of pixel (x,y), from already known pixels
    void predict(const Image &img, int32 x, int32 y, int32 c, ChannelPrediction &cp)
    {
        int32 w = img.width;
        const byte *row = &img.rgb[(size_t)y * w * 3 + c];
        const byte *up = y > 0 ? row - (size_t)w * 3 : row;
        int32 W = x > 0 ? row[(x - 1) * 3] : (y > 0 ? up[x * 3] : 0);
        int32 N = y > 0 ? up[x * 3] : W;
        int32 NW = x > 0 && y > 0 ? up[(x - 1) * 3] : N;
        int32 NE = y > 0 && x + 1 < w ? up[(x + 1) * 3] : N;

        cp.preds[0] = W;
        cp.preds[1] = N;
        cp.preds[2] = clampByte(W + N - NW);
        cp.preds[3] = clampByte(W + NE - N);
        cp.preds[4] = (W + N + 1) >> 1;

        // weights: predictors that were accurate around this pixel count more
        const int32 cur = y & 1;
        const int32 prev = cur ^ 1;
        int32 i0 = x + 2;
        int64_t sumW = 0;
        int64_t sumWP = 0;
        for (int32 i = 0; i < P2_PREDICTORS; i++)
        {
            const vector<int32> &ec = predErr[cur][c][i];
            const vector<int32> &ep = predErr[prev][c][i];
            int64_t weight = weightTable[2 * ec[i0 - 1] + 2 * ep[i0] + ep[i0 - 1] + ep[i0 + 1] + 1];
            sumW += weight;
            sumWP += weight * cp.preds[i];
        }
        cp.spatial = (int32)((sumWP + sumW / 2) / sumW);

        const vector<int32> &rc = resErr[cur][c];
        const vector<int32> &rp = resErr[prev][c];
        cp.bucket = activityBucket(rc[i0 - 1] + rp[i0] + rp[i0 - 1] + rp[i0 + 1]);

        cp.biasCtx = (c * P2_ACT_BUCKETS + cp.bucket) * 9 + signOf(W - NW) * 3 + signOf(N - NW);
        const BiasStat &b = bias[cp.biasCtx];
        cp.corr = b.count ? roundedDivide(b.sum, b.count) : 0;
        finish(c, cp, 0);
    }

    // final prediction; R and B are shifted by the error of G of the same pixel
    void finish(int32 c, ChannelPrediction &cp, int32 eG)
    {
        cp.base = c == 1 ? cp.spatial : clampByte(cp.spatial + eG);
        cp.pred = clampByte(cp.base + cp.corr);
    }

    // records the actual value of channel c of pixel (x,y)
    void update(int32 x, int32 y, int32 c, int32 v, const ChannelPrediction &cp)
    {
        const int32 cur = y & 1;
        int32 i0 = x + 2;
        for (int32 i = 0; i < P2_PREDICTORS; i++)
            predErr[cur][c][i][i0] = absValue(v - cp.preds[i]);
        resErr[cur][c][i0] = absValue(v - cp.pred);

        // samples are clipped so that single edges do not shift the mean of flat areas
        BiasStat &b = bias[cp.biasCtx];
        int32 e = v - cp.base;
        b.sum += e < -P2_BIAS_CLIP ? -P2_BIAS_CLIP : (e > P2_BIAS_CLIP ? P2_BIAS_CLIP : e);
        b.count++;
        if (b.count >= P2_BIAS_LIMIT)
        {
            b.sum = roundedDivide(b.sum, 2);
            b.count /= 2;
        }
    }
};

// recently used colours, most recent first (move to front list); every pixel moves its colour to the front.
// Positions are found with a Fenwick tree over the time of last use.
class RecentColours
{
public:
    vector<int32> tree;         // Fenwick tree, 1 for each timestamp that is the last use of a colour
    vector<byte> live;
    vector<dword> stampColour;
    hmap<dword, int32> stampOf; // colour -> timestamp of its last use
    int32 now;                  // next timestamp
    int32 oldest;               // no live timestamp is smaller
    int32 count;

    RecentColours() : tree(P2_STAMPS + 1, 0), live(P2_STAMPS, 0), stampColour(P2_STAMPS, 0), now(0), oldest(0),
        count(0) {}

    void add(int32 t, int32 d)
    {
        for (int32 i = t + 1; i <= P2_STAMPS; i += i & -i)
            tree[i] += d;
    }

    // number of live timestamps smaller than t
    int32 below(int32 t) const
    {
        int32 s = 0;
        for (int32 i = t; i > 0; i -= i & -i)
            s += tree[i];
        return s;
    }

    // position of colour, 0 = most recently used, -1 if not in the list
    int32 position(dword colour) const
    {
        hmap<dword, int32>::const_iterator it = stampOf.find(colour);
        if (it == stampOf.end())
            return -1;
        return count - 1 - below(it->second);
    }

    // colour at a position; false if there is none
    bool colourAt(int32 pos, dword &colour) const
    {
        if (pos < 0 || pos >= count)
            return false;
        int32 k = count - pos;          // k-th smallest live timestamp
        int32 at = 0;
        for (int32 step = P2_STAMPS; step > 0; step >>= 1)
            if (at + step <= P2_STAMPS && tree[at + step] < k)
            {
                at += step;
                k -= tree[at];
            }
        colour = stampColour[at];
        return true;
    }

    void use(dword colour)
    {
        if (now == P2_STAMPS)
            renumber();
        hmap<dword, int32>::iterator it = stampOf.find(colour);
        if (it != stampOf.end())
        {
            live[it->second] = 0;
            add(it->second, -1);
            it->second = now;
        }
        else
        {
            if (count == P2_RECENT)
                removeOldest();
            stampOf[colour] = now;
            count++;
        }
        live[now] = 1;
        stampColour[now] = colour;
        add(now, 1);
        now++;
    }

    void removeOldest()
    {
        while (!live[oldest])
            oldest++;
        live[oldest] = 0;
        add(oldest, -1);
        stampOf.erase(stampColour[oldest]);
        count--;
    }

    // gives the live colours timestamps 0..count-1, keeping their order
    void renumber()
    {
        vector<dword> order;
        for (int32 t = 0; t < now; t++)
            if (live[t])
                order.push_back(stampColour[t]);
        tree.assign(P2_STAMPS + 1, 0);
        live.assign(P2_STAMPS, 0);
        for (int32 t = 0; t < (int32)order.size(); t++)
        {
            live[t] = 1;
            stampColour[t] = order[t];
            stampOf[order[t]] = t;
            add(t, 1);
        }
        now = (int32)order.size();
        oldest = 0;
    }
};

// all state of algorithm 2, shared by encoder and decoder
class Pix2Coder
{
public:
    Image &img;
    BitCoder &bc;
    ChannelModel channels;
    RecentColours recent;
    vector<byte> modes[2];              // coding mode per pixel, previous and current row, index x + 1
    vector<word> hits[2];               // per pixel: bit i set if candidate i had the colour of the pixel

    vector<BitModel> candidateAny;      // [activity][prediction equals W, N][W == N, N == NE][modes]
    vector<BitModel> predictedFlags;    // [activity][prediction equals W, N][modes]
    vector<BitModel> neighbourFlags;    // [candidate 1..8][equal pattern][hit at W, N][modes]
    vector<dword> seen4;                // colour that followed neighbours W, N, NW, NE (with P2_HASH_VALID)
    vector<dword> seen2;                // colour that followed neighbours W, N
    dword hash4;
    dword hash2;
    vector<BitModel> recentFlags;       // [activity][modes]
    NumberModel recentIndex[4];         // [mode of W]

    // current pixel
    ChannelPrediction cp[3];
    dword cand[P2_CANDIDATES];
    int32 candId[P2_CANDIDATES];        // 0 = predicted colour, 1, 2 = remembered for neighbourhood,
                                        // 3.. = neighbour W, N, NE, NW, WW, NN
    int32 candCount;
    dword all[P2_CANDIDATES];           // candidate colours before removing duplicates (P2_HASH_VALID if missing)
    int32 hitCtx[P2_CANDIDATES];        // candidate i had the colour at W (bit 0), at N (bit 1)
    int32 candPos[P2_CANDIDATES];       // positions of the candidates in the recent list, ascending
    int32 candPosCount;                 // -1 until computed
    int32 eqPattern;
    int32 predEq;                       // predicted colour equals W (bit 0), N (bit 1)
    int32 modeW;
    int32 modeCtx;

    Pix2Coder(Image &image, BitCoder &coder) : img(image), bc(coder), channels(image.width), hash4(0), hash2(0),
        candCount(0), candPosCount(0), eqPattern(0), predEq(0), modeW(0), modeCtx(0)
    {
        modes[0].assign(image.width + 1, MODE_RESIDUAL);
        modes[1].assign(image.width + 1, MODE_RESIDUAL);
        hits[0].assign(image.width + 1, 0);
        hits[1].assign(image.width + 1, 0);
        candidateAny.resize(P2_ACT_BUCKETS * 4 * 4 * P2_MODE_CTX);
        predictedFlags.resize(P2_ACT_BUCKETS * 4 * P2_MODE_CTX);
        neighbourFlags.resize((P2_CANDIDATES - 1) * 64 * 4 * P2_MODE_CTX);
        seen4.assign((size_t)1 << P2_HASH_BITS, 0);
        seen2.assign((size_t)1 << P2_HASH_BITS, 0);
        recentFlags.resize(P2_ACT_BUCKETS * P2_MODE_CTX);
    }

    // predictions, candidate colours and contexts of pixel (x,y)
    void prepare(int32 x, int32 y)
    {
        for (int32 c = 0; c < 3; c++)
            channels.predict(img, x, y, c, cp[c]);
        dword predicted = makeColour(cp[0].pred, cp[1].pred, cp[2].pred);

        dword W = x > 0 ? img.getPixel(x - 1, y) : (y > 0 ? img.getPixel(x, y - 1) : 0);
        dword N = y > 0 ? img.getPixel(x, y - 1) : W;
        dword NE = y > 0 && x + 1 < img.width ? img.getPixel(x + 1, y - 1) : N;
        dword NW = x > 0 && y > 0 ? img.getPixel(x - 1, y - 1) : N;
        dword WW = x > 1 ? img.getPixel(x - 2, y) : W;
        dword NN = y > 1 ? img.getPixel(x, y - 2) : N;
        hash2 = ((W * 0x9E3779B1u) ^ (N * 0x85EBCA77u)) * 0xC2B2AE3Du >> (32 - P2_HASH_BITS);
        hash4 = ((W * 0x9E3779B1u) ^ (N * 0x85EBCA77u) ^ (NW * 0x27D4EB2Fu) ^ (NE * 0x165667B1u)) * 0xC2B2AE3Du
                >> (32 - P2_HASH_BITS);
        all[0] = predicted;
        all[1] = seen4[hash4] ^ P2_HASH_VALID;      // without the flag if valid, with it if missing
        all[2] = seen2[hash2] ^ P2_HASH_VALID;
        all[3] = W;
        all[4] = N;
        all[5] = NE;
        all[6] = NW;
        all[7] = WW;
        all[8] = NN;
        int32 hitsW = x > 0 ? hits[y & 1][x] : 0;
        int32 hitsN = y > 0 ? hits[(y & 1) ^ 1][x + 1] : 0;
        candCount = 0;
        for (int32 i = 0; i < P2_CANDIDATES; i++)
        {
            hitCtx[i] = ((hitsW >> i) & 1) | ((hitsN >> i) & 1) << 1;
            if (all[i] & P2_HASH_VALID)
                continue;
            bool seen = false;
            for (int32 j = 0; j < candCount; j++)
                if (cand[j] == all[i])
                    seen = true;
            if (!seen)
            {
                cand[candCount] = all[i];
                candId[candCount] = i;
                candCount++;
            }
        }

        candPosCount = -1;      // computed when needed

        predEq = (predicted == W) | (predicted == N) << 1;
        eqPattern = (W == N) | (W == NW) << 1 | (N == NE) << 2 | (N == NW) << 3 | (W == WW) << 4 | (N == NN) << 5;
        modeW = x > 0 ? modes[y & 1][x] : MODE_RESIDUAL;
        int32 modeN = y > 0 ? modes[(y & 1) ^ 1][x + 1] : MODE_RESIDUAL;
        modeCtx = modeW * 4 + modeN;
    }

    BitModel &anyFlag()
    {
        int32 eq = (eqPattern & 1) | ((eqPattern >> 1) & 2);
        return candidateAny[((cp[1].bucket * 4 + predEq) * 4 + eq) * P2_MODE_CTX + modeCtx];
    }

    BitModel &candidateFlag(int32 j)
    {
        if (candId[j] == 0)
            return predictedFlags[(cp[1].bucket * 4 + predEq) * P2_MODE_CTX + modeCtx];
        return neighbourFlags[(((candId[j] - 1) * 64 + eqPattern) * 4 + hitCtx[candId[j]]) * P2_MODE_CTX + modeCtx];
    }

    BitModel &recentFlag()
    {
        return recentFlags[cp[1].bucket * P2_MODE_CTX + modeCtx];
    }

    // positions of the candidates in the recent list, sorted (they are skipped when coding an index)
    void findCandidatePositions()
    {
        if (candPosCount >= 0)
            return;
        candPosCount = 0;
        for (int32 j = 0; j < candCount; j++)
        {
            int32 pos = recent.position(cand[j]);
            if (pos < 0)
                continue;
            int32 k = candPosCount++;
            while (k > 0 && candPos[k - 1] > pos)
            {
                candPos[k] = candPos[k - 1];
                k--;
            }
            candPos[k] = pos;
        }
    }

    // index in the recent list without the candidates, -1 if colour is not in the list
    int32 recentIndexOf(dword colour)
    {
        int32 pos = recent.position(colour);
        if (pos < 0)
            return -1;
        findCandidatePositions();
        int32 index = pos;
        for (int32 k = 0; k < candPosCount; k++)
            if (candPos[k] < pos)
                index--;
        return index;
    }

    bool recentColour(int32 index, dword &colour)
    {
        findCandidatePositions();
        int32 pos = index;
        for (int32 k = 0; k < candPosCount; k++)
            if (candPos[k] <= pos)
                pos++;
        return recent.colourAt(pos, colour);
    }

    // cost of coding the colour with residuals (encoder decision)
    int32 residualPathCost(dword colour)
    {
        int32 cost = 0;
        int32 eG = 0;
        for (int32 k = 0; k < 3; k++)
        {
            int32 c = channelOrder[k];
            ChannelPrediction p = cp[c];
            channels.finish(c, p, eG);
            int32 v = channelOf(colour, c);
            cost += residualCost(channels.model(c, p, eG), wrapResidual(v - p.pred));
            if (c == 1)
                eG = v - p.pred;
        }
        return cost;
    }

    // encoder: chooses how to code the colour of the current pixel
    int32 chooseMode(dword colour, int32 &choice)
    {
        // the cheapest of: a candidate, the recent list, residuals
        int32 residualCost = anyFlag().cost(0) + recentFlag().cost(0) + residualPathCost(colour);
        for (int32 j = 0; j < candCount; j++)
            if (cand[j] == colour)
            {
                int32 cost = anyFlag().cost(1);
                for (int32 i = 0; i < j; i++)
                    cost += candidateFlag(i).cost(0);
                if (j < candCount - 1)
                    cost += candidateFlag(j).cost(1);
                if (cost <= residualCost)
                {
                    choice = j;
                    return candId[j] == 0 ? MODE_PREDICTED : MODE_NEIGHBOUR;
                }
                return MODE_RESIDUAL;
            }
        int32 index = recentIndexOf(colour);
        if (index >= 0)
        {
            int32 cost = anyFlag().cost(0) + recentFlag().cost(1) + numberCost(recentIndex[modeW], index + 1);
            if (cost < residualCost)
            {
                choice = index;
                return MODE_RECENT;
            }
        }
        return MODE_RESIDUAL;
    }

    // codes pixel (x,y); when encoding, colour is its colour; returns false on invalid data
    bool codePixel(int32 x, int32 y, dword colour, Pix2Stats *stats)
    {
        prepare(x, y);
        int32 choice = 0;
        int32 mode = bc.enc ? chooseMode(colour, choice) : MODE_RESIDUAL;
        int64_t start = bc.cost;
        bool valid = true;

        if (bc.bit(anyFlag(), mode <= MODE_NEIGHBOUR))
        {
            // which candidate: one flag per candidate, the last one needs none
            int32 j = 0;
            while (j < candCount - 1 && !bc.bit(candidateFlag(j), choice == j))
                j++;
            colour = cand[j];
            mode = candId[j] == 0 ? MODE_PREDICTED : MODE_NEIGHBOUR;
        }
        else if (bc.bit(recentFlag(), mode == MODE_RECENT))
        {
            valid = recentColour(codeNumber(bc, recentIndex[modeW], choice + 1) - 1, colour);
            mode = MODE_RECENT;
        }
        else
            mode = MODE_RESIDUAL;
        if (stats)
        {
            stats->modeBits += (double)(bc.cost - start) / 256.0;
            stats->modes[mode]++;
        }

        // channels: residuals are coded only in residual mode, the models learn from every pixel
        byte *px = &img.rgb[((size_t)y * img.width + x) * 3];
        if (mode != MODE_RESIDUAL)
            img.setPixel(x, y, valid ? colour : 0);
        int32 eG = 0;
        for (int32 k = 0; k < 3; k++)
        {
            int32 c = channelOrder[k];
            channels.finish(c, cp[c], eG);
            if (mode == MODE_RESIDUAL)
            {
                start = bc.cost;
                int32 e = codeResidual(bc, channels.model(c, cp[c], eG),
                                       wrapResidual(channelOf(colour, c) - cp[c].pred));
                px[c] = (byte)((cp[c].pred + e) & 255);
                if (stats)
                    stats->channelBits[c] += (double)(bc.cost - start) / 256.0;
            }
            int32 v = px[c];
            channels.update(x, y, c, v, cp[c]);
            if (c == 1)
                eG = v - cp[c].pred;
        }

        modes[y & 1][x + 1] = (byte)mode;
        colour = img.getPixel(x, y);
        int32 hit = 0;
        for (int32 i = 0; i < P2_CANDIDATES; i++)
            if (all[i] == colour)
                hit |= 1 << i;
        hits[y & 1][x + 1] = (word)hit;
        recent.use(colour);
        seen4[hash4] = colour | P2_HASH_VALID;
        seen2[hash2] = colour | P2_HASH_VALID;
        return valid;
    }
};

//----------------------------------------------------------------------------------------------------------------------
static dword imageCrc(const Image &img)
{
    return (dword)crc32(crc32(0, NULL, 0), img.rgb.empty() ? NULL : &img.rgb[0], (uInt)img.rgb.size());
}

//----------------------------------------------------------------------------------------------------------------------
bool encodePix2Data(const Image &img, vector<byte> &data, Pix2Stats &stats)
{
    initTables();
    // the coder writes decoded pixels into its image like the decoder does; start from a copy
    Image work = img;
    RangeEncoder rc;
    BitCoder bc(&rc, NULL);
    Pix2Coder coder(work, bc);
    for (int32 y = 0; y < img.height; y++)
        for (int32 x = 0; x < img.width; x++)
            coder.codePixel(x, y, img.getPixel(x, y), &stats);
    rc.flush();
    data.swap(rc.out);
    dword crc = imageCrc(img);
    for (int32 i = 0; i < P2_CRC_SIZE; i++)
        data.push_back((byte)(crc >> (i * 8)));
    stats.pixels = (size_t)img.width * img.height;
    stats.dataBytes = data.size();
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool decodePix2Data(const byte *data, size_t size, Image &img)
{
    if (size < P2_CRC_SIZE)
    {
        setPixError("PIX algorithm 2 data is truncated");
        return false;
    }
    initTables();
    img.rgb.assign((size_t)img.width * img.height * 3, 0);
    RangeDecoder rd(data, size - P2_CRC_SIZE);
    BitCoder bc(NULL, &rd);
    Pix2Coder coder(img, bc);
    bool valid = true;
    for (int32 y = 0; y < img.height; y++)
        for (int32 x = 0; x < img.width; x++)
            if (!coder.codePixel(x, y, 0, NULL))
                valid = false;

    const byte *crcBytes = data + size - P2_CRC_SIZE;
    dword stored = (dword)crcBytes[0] | ((dword)crcBytes[1] << 8) | ((dword)crcBytes[2] << 16) |
                   ((dword)crcBytes[3] << 24);
    if (!valid || stored != imageCrc(img))
    {
        setPixError("PIX algorithm 2 data is damaged (checksum mismatch)");
        return false;
    }
    return true;
}
