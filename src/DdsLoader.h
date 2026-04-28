#pragma once
#include <cstdint>
#include <string>

// Minimal DDS loader that covers Skyrim's BC1/BC3/BC5/BC7 and uncompressed
// BGRA8 formats.  Returns the OpenGL texture object ID on success, or 0 on
// failure.  Caller is responsible for glDeleteTextures when done.
unsigned int DdsLoadGLTexture(const std::string& path);

// Load from an in-memory DDS buffer (e.g. bytes extracted from a BSA).
unsigned int DdsLoadGLTextureFromBuffer(const uint8_t* data, size_t size);
