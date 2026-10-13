#ifndef IMAGE_IO_H
#define IMAGE_IO_H

#include "types.h"

// loads PNG, JPG or BMP (detected by file content) into a 24 bit RGB bitmap
bool loadImage(const char *path, Image &img);

// saves 24 bit uncompressed BMP
bool saveBmp(const char *path, const Image &img);

// reads whole file into memory
bool readFile(const char *path, vector<byte> &data);

// writes whole buffer to file
bool writeFile(const char *path, const vector<byte> &data);

#endif
