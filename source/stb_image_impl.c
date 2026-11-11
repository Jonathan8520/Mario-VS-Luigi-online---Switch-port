/* stb_image_impl.c -- the one translation unit that compiles stb_image
 * (source/third_party/stb_image.h, public domain / MIT, Sean Barrett), for the
 * loading screen's pictures. JPEG (baseline) and PNG only, from memory.
 * MIT -- part of mvlo_nx. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__aarch64__)
#define STBI_NEON
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop
