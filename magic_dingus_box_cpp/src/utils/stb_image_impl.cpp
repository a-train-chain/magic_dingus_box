// The ONE translation unit that generates stb_image's implementation.
//
// This lived at the top of ui/renderer.cpp — the largest TU in the
// project — so every renderer edit recompiled all of stb_image along
// with it (felt most on the Pi 4B's make -j2). Every other consumer
// (renderer.cpp, artwork_cache.cpp) includes stb_image.h in
// declaration-only mode and links against the symbols produced here.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
