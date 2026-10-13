#ifndef PIX_ERROR_H
#define PIX_ERROR_H

// last error message of the image loaders and the PIX codec
void setPixError(const char *format, ...);
const char *getPixError();

#endif
