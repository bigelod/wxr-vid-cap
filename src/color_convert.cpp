#include "color_convert.h"

namespace {

std::uint8_t Clamp(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

}  // namespace

bool BgraToI420(const std::uint8_t* bgra, int width, int height, std::vector<std::uint8_t>& i420)
{
    if (bgra == nullptr || width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
        return false;
    }

    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    const std::size_t lumaSize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t chromaSize = static_cast<std::size_t>(chromaWidth) * static_cast<std::size_t>(chromaHeight);
    i420.resize(lumaSize + chromaSize * 2);

    std::uint8_t* yPlane = i420.data();
    std::uint8_t* uPlane = yPlane + lumaSize;
    std::uint8_t* vPlane = uPlane + chromaSize;

    for (int y = 0; y < height; y += 2) {
        const std::uint8_t* row0 = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        const std::uint8_t* row1 = row0 + static_cast<std::size_t>(width) * 4;

        for (int x = 0; x < width; x += 2) {
            int uSum = 0;
            int vSum = 0;

            const std::uint8_t* pixels[4] = {
                row0 + static_cast<std::size_t>(x) * 4,
                row0 + static_cast<std::size_t>(x + 1) * 4,
                row1 + static_cast<std::size_t>(x) * 4,
                row1 + static_cast<std::size_t>(x + 1) * 4,
            };

            for (int i = 0; i < 4; ++i) {
                const int b = pixels[i][0];
                const int g = pixels[i][1];
                const int r = pixels[i][2];

                const int luma = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

                const int px = x + (i & 1);
                const int py = y + (i >> 1);
                yPlane[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(px)] = Clamp(luma);

                uSum += Clamp(u);
                vSum += Clamp(v);
            }

            const std::size_t chromaIndex =
                static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(chromaWidth) +
                static_cast<std::size_t>(x / 2);
            uPlane[chromaIndex] = static_cast<std::uint8_t>(uSum / 4);
            vPlane[chromaIndex] = static_cast<std::uint8_t>(vSum / 4);
        }
    }

    return true;
}
