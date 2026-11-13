#pragma once

#include <cstdint>
#include <vector>

// Converts a top-down BGRA frame to I420 (BT.601 limited range), which is what the H.264 encoder
// consumes. Width and height must be even; returns false otherwise.
bool BgraToI420(const std::uint8_t* bgra, int width, int height, std::vector<std::uint8_t>& i420);
