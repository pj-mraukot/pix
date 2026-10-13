#include <stdio.h>
#include <string.h>
#include <time.h>
#include "types.h"
#include "image_io.h"
#include "pix_codec.h"
#include "pix_error.h"
#include "pix_report.h"

using namespace std;

//----------------------------------------------------------------------------------------------------------------------
static void printUsage()
{
    printf("Usage:\n");
    printf("  pix e [-a 1|2] <input.png|jpg|bmp> <output.pix>   encode image with algorithm 1 (default) or 2\n");
    printf("  pix d <input.pix> <output.bmp>                    decode image (any algorithm)\n");
    printf("  pix t [-a 1|2] <input.png|jpg|bmp>                round trip test (encode, decode, compare)\n");
    printf("  pix r <input.png|jpg|bmp> <output.pix>            encode image with algorithm 1 and write a report\n");
    printf("                                                    of every code to <input>.txt\n");
}

//----------------------------------------------------------------------------------------------------------------------
static double percent(size_t part, size_t whole)
{
    return whole ? 100.0 * (double)part / (double)whole : 0.0;
}

//----------------------------------------------------------------------------------------------------------------------
static void printStats(const Image &img, const EncodeStats &stats, size_t streamBytes, size_t fileBytes)
{
    size_t rawBytes = (size_t)img.width * img.height * 3;
    size_t pixels = (size_t)img.width * img.height;

    printf("Image:        %d x %d (%zu pixels)\n", img.width, img.height, pixels);
    printf("Raw size:     %zu bytes\n", rawBytes);
    printf("Stream size:  %zu bytes (%zu nibbles, %.2f%% of raw)\n",
           streamBytes, stats.nibbles, percent(streamBytes, rawBytes));
    printf("File size:    %zu bytes (%.2f%% of raw, %.3f bits/pixel)\n",
           fileBytes, percent(fileBytes, rawBytes), 8.0 * (double)fileBytes / (double)pixels);
    printf("Codes:\n");
    printf("  01  cache select  %10zu\n", stats.cacheSelect);
    printf("  1   last          %10zu\n", stats.last);
    printf("  2   next to last  %10zu\n", stats.nextToLast);
    printf("  3X  run           %10zu\n", stats.run3);
    printf("  4XX run           %10zu\n", stats.run4);
    printf("      run pixels    %10zu\n", stats.runPixels);
    printf("  F   above         %10zu\n", stats.above);
    printf("  5-9 cache1        %10zu\n", stats.cache1);
    printf("  A,B cache2        %10zu\n", stats.cache2);
    printf("  C   -> cache1     %10zu\n", stats.codeC);
    printf("  02  MED exact     %10zu\n", stats.medExact);
    printf("  03,0E MED run     %10zu\n", stats.medRun);
    printf("      run pixels    %10zu\n", stats.medRunPixels);
    printf("  04-07 small delta %10zu\n", stats.deltaSmall);
    printf("  D   delta         %10zu\n", stats.codeD);
    printf("  08,09 medium      %10zu\n", stats.deltaMedium);
    printf("  0A-0D wide        %10zu\n", stats.deltaWide);
    printf("  E   -> cache2     %10zu\n", stats.codeE);
}

//----------------------------------------------------------------------------------------------------------------------
static void printStats2(const Image &img, const Pix2Stats &stats, size_t fileBytes)
{
    size_t rawBytes = (size_t)img.width * img.height * 3;
    size_t pixels = (size_t)img.width * img.height;
    double residualBits = stats.channelBits[0] + stats.channelBits[1] + stats.channelBits[2];

    printf("Image:        %d x %d (%zu pixels)\n", img.width, img.height, pixels);
    printf("Raw size:     %zu bytes\n", rawBytes);
    printf("Algorithm:    2\n");
    printf("File size:    %zu bytes (%.2f%% of raw, %.3f bits/pixel)\n",
           fileBytes, percent(fileBytes, rawBytes), 8.0 * (double)fileBytes / (double)pixels);
    printf("Pixels coded as:\n");
    printf("  predicted colour  %10zu\n", stats.modes[0]);
    printf("  other candidate   %10zu   (neighbour, or colour remembered for the neighbourhood)\n", stats.modes[1]);
    printf("  recent colour     %10zu\n", stats.modes[2]);
    printf("  residuals         %10zu\n", stats.modes[3]);
    printf("Bytes spent on (estimated):\n");
    printf("  choosing modes    %10.0f\n", stats.modeBits / 8);
    printf("  residuals R G B   %10.0f   (%.0f %.0f %.0f)\n", residualBits / 8, stats.channelBits[0] / 8,
           stats.channelBits[1] / 8, stats.channelBits[2] / 8);
}

//----------------------------------------------------------------------------------------------------------------------
static bool encodeImage(const Image &img, int32 algorithm, vector<byte> &file, EncodeStats &stats, Pix2Stats &stats2)
{
    if (algorithm == 2)
        return encodePix2File(img, file, stats2);
    return encodePixFile(img, file, stats);
}

//----------------------------------------------------------------------------------------------------------------------
static void printEncodeStats(const Image &img, int32 algorithm, const EncodeStats &stats, const Pix2Stats &stats2,
                             size_t fileBytes)
{
    if (algorithm == 2)
        printStats2(img, stats2, fileBytes);
    else
        printStats(img, stats, (stats.nibbles + 1) / 2, fileBytes);
}

//----------------------------------------------------------------------------------------------------------------------
static int reportError()
{
    fprintf(stderr, "%s\n", getPixError());
    return 1;
}

//----------------------------------------------------------------------------------------------------------------------
static int encodeCommand(const char *inPath, const char *outPath, int32 algorithm)
{
    Image img;
    if (!loadImage(inPath, img))
        return reportError();

    clock_t start = clock();
    vector<byte> file;
    EncodeStats stats;
    Pix2Stats stats2;
    if (!encodeImage(img, algorithm, file, stats, stats2))
        return reportError();
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;

    if (!writeFile(outPath, file))
        return reportError();
    printEncodeStats(img, algorithm, stats, stats2, file.size());
    printf("Encode time:  %.3f s\n", seconds);
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
static int decodeCommand(const char *inPath, const char *outPath)
{
    vector<byte> file;
    if (!readFile(inPath, file))
        return reportError();

    clock_t start = clock();
    Image img;
    if (!decodePixFile(file, img))
        return reportError();
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;

    if (!saveBmp(outPath, img))
        return reportError();
    printf("Decoded %d x %d in %.3f s\n", img.width, img.height, seconds);
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
static int testCommand(const char *inPath, int32 algorithm)
{
    Image img;
    if (!loadImage(inPath, img))
        return reportError();

    clock_t start = clock();
    vector<byte> file;
    EncodeStats stats;
    Pix2Stats stats2;
    if (!encodeImage(img, algorithm, file, stats, stats2))
        return reportError();
    double encodeSeconds = (double)(clock() - start) / CLOCKS_PER_SEC;

    start = clock();
    Image decoded;
    if (!decodePixFile(file, decoded))
        return reportError();
    double decodeSeconds = (double)(clock() - start) / CLOCKS_PER_SEC;

    printEncodeStats(img, algorithm, stats, stats2, file.size());
    printf("Encode time:  %.3f s\n", encodeSeconds);
    printf("Decode time:  %.3f s\n", decodeSeconds);

    if (decoded.width != img.width || decoded.height != img.height || decoded.rgb != img.rgb)
    {
        printf("Round trip:   FAILED (decoded image differs)\n");
        return 1;
    }
    printf("Round trip:   OK\n");
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
static void reportPath(const char *inPath, char *out, size_t size)
{
    // input path with its extension replaced by .txt
    snprintf(out, size, "%s", inPath);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash))
        *dot = 0;
    size_t len = strlen(out);
    snprintf(out + len, size - len, ".txt");
}

//----------------------------------------------------------------------------------------------------------------------
static int reportCommand(const char *inPath, const char *outPath)
{
    char txtPath[4096];
    reportPath(inPath, txtPath, sizeof(txtPath));
    if (strcmp(txtPath, inPath) == 0)
    {
        fprintf(stderr, "Report file would overwrite the input file: %s\n", inPath);
        return 1;
    }

    Image img;
    if (!loadImage(inPath, img))
        return reportError();

    FILE *f = fopen(txtPath, "w");
    if (!f)
    {
        fprintf(stderr, "Cannot create report file: %s\n", txtPath);
        return 1;
    }
    PixReport report(f);
    report.header(inPath, img);

    vector<byte> file;
    EncodeStats stats;
    bool ok = encodePixFile(img, file, stats, &report);
    if (ok)
        report.summary(stats, file.size(), (size_t)img.width * img.height * 3);
    bool written = !ferror(f);
    if (fclose(f) != 0)
        written = false;
    if (!ok)
        return reportError();
    if (!written)
    {
        fprintf(stderr, "Error writing report file: %s\n", txtPath);
        return 1;
    }

    if (!writeFile(outPath, file))
        return reportError();
    printStats(img, stats, (stats.nibbles + 1) / 2, file.size());
    printf("Report:       %s\n", txtPath);
    return 0;
}

//----------------------------------------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    // optional "-a 1" or "-a 2" after the command selects the algorithm (e and t)
    int32 algorithm = 1;
    bool algorithmGiven = false;
    if (argc >= 4 && strcmp(argv[2], "-a") == 0)
    {
        if (strcmp(argv[3], "1") != 0 && strcmp(argv[3], "2") != 0)
        {
            fprintf(stderr, "Unknown algorithm: %s (use 1 or 2)\n", argv[3]);
            return 1;
        }
        algorithm = argv[3][0] - '0';
        algorithmGiven = true;
        for (int i = 2; i + 2 < argc; i++)
            argv[i] = argv[i + 2];
        argc -= 2;
    }

    if (argc == 4 && strcmp(argv[1], "e") == 0)
        return encodeCommand(argv[2], argv[3], algorithm);
    if (argc == 3 && strcmp(argv[1], "t") == 0)
        return testCommand(argv[2], algorithm);
    if (argc == 4 && strcmp(argv[1], "d") == 0 && !algorithmGiven)
        return decodeCommand(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "r") == 0)
    {
        if (algorithm != 1)
        {
            fprintf(stderr, "The report lists algorithm 1 codes only\n");
            return 1;
        }
        return reportCommand(argv[2], argv[3]);
    }
    printUsage();
    return 1;
}
