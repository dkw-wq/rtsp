#pragma once

#include <array>
#include <cstdint>

namespace rtsp::rendering {

using Glyph = std::array<uint8_t, 7>;

Glyph glyphFor(char ch);

} // namespace rtsp::rendering
