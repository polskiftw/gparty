#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdupeJpegImage {
  int width;
  int height;
  uint8_t *pixels;
  char error[256];
} GdupeJpegImage;

int gdupe_decode_jpeg_rgb(const uint8_t *data, size_t size,
                          uint64_t max_pixels, GdupeJpegImage *output);
void gdupe_free_jpeg_image(GdupeJpegImage *image);

#ifdef __cplusplus
}
#endif
