#include "jpeg_decode.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

typedef struct JpegJumpState {
  jmp_buf *jump;
  char *message;
  size_t message_size;
} JpegJumpState;

static void set_error(GdupeJpegImage *output, const char *message) {
  if (!output)
    return;
  snprintf(output->error, sizeof(output->error), "%s", message);
}

static void jpeg_failure(j_common_ptr context) {
  JpegJumpState *state = (JpegJumpState *)context->client_data;
  char message[JMSG_LENGTH_MAX];
  context->err->format_message(context, message);
  if (state && state->message && state->message_size > 0)
    snprintf(state->message, state->message_size, "%s", message);
  if (!state || !state->jump)
    abort();
  longjmp(*state->jump, 1);
}

int gdupe_decode_jpeg_rgb(const uint8_t *data, size_t size,
                          uint64_t max_pixels, GdupeJpegImage *output) {
  if (!output)
    return 0;
  memset(output, 0, sizeof(*output));
  if (!data || size == 0) {
    set_error(output, "JPEG input is empty");
    return 0;
  }
  if (size > ULONG_MAX) {
    set_error(output, "JPEG input exceeds libjpeg's memory-source limit");
    return 0;
  }

  struct jpeg_decompress_struct decoder;
  struct jpeg_error_mgr error_manager;
  jmp_buf jump;
  JpegJumpState jump_state = {&jump, output->error, sizeof(output->error)};

  memset(&decoder, 0, sizeof(decoder));
  decoder.err = jpeg_std_error(&error_manager);
  error_manager.error_exit = jpeg_failure;
  decoder.client_data = &jump_state;

  if (setjmp(jump) != 0) {
    jpeg_destroy_decompress(&decoder);
    free(output->pixels);
    output->pixels = NULL;
    output->width = 0;
    output->height = 0;
    return 0;
  }

  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, data, (unsigned long)size);
  if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK) {
    set_error(output, "JPEG stream does not contain an image header");
    jpeg_destroy_decompress(&decoder);
    return 0;
  }

  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  if (decoder.output_width == 0 || decoder.output_height == 0 ||
      decoder.output_components != 3) {
    set_error(output, "JPEG decoder returned invalid RGB dimensions");
    jpeg_destroy_decompress(&decoder);
    return 0;
  }

  const uint64_t pixel_count =
      (uint64_t)decoder.output_width * (uint64_t)decoder.output_height;
  if (pixel_count > max_pixels || pixel_count > SIZE_MAX / 3U ||
      decoder.output_width > INT_MAX || decoder.output_height > INT_MAX) {
    set_error(output, "JPEG dimensions exceed gdupe's safety limit");
    jpeg_destroy_decompress(&decoder);
    return 0;
  }

  const size_t byte_count = (size_t)pixel_count * 3U;
  output->pixels = (uint8_t *)malloc(byte_count);
  if (!output->pixels) {
    set_error(output, "JPEG RGB buffer allocation failed");
    jpeg_destroy_decompress(&decoder);
    return 0;
  }
  output->width = (int)decoder.output_width;
  output->height = (int)decoder.output_height;

  while (decoder.output_scanline < decoder.output_height) {
    JSAMPROW row = output->pixels +
                   (size_t)decoder.output_scanline * decoder.output_width * 3U;
    if (jpeg_read_scanlines(&decoder, &row, 1) != 1) {
      set_error(output, "JPEG decoder suspended while reading image data");
      jpeg_destroy_decompress(&decoder);
      free(output->pixels);
      output->pixels = NULL;
      output->width = 0;
      output->height = 0;
      return 0;
    }
  }

  if (!jpeg_finish_decompress(&decoder)) {
    set_error(output, "JPEG decoder did not finish the image");
    jpeg_destroy_decompress(&decoder);
    free(output->pixels);
    output->pixels = NULL;
    output->width = 0;
    output->height = 0;
    return 0;
  }
  jpeg_destroy_decompress(&decoder);
  return 1;
}

void gdupe_free_jpeg_image(GdupeJpegImage *image) {
  if (!image)
    return;
  free(image->pixels);
  image->pixels = NULL;
  image->width = 0;
  image->height = 0;
}
