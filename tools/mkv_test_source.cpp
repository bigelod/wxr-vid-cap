// Test-only MKV source. Reads an H.264 Annex B elementary stream, groups NAL units into access
// units, and writes them to Matroska with MkvWriter. Proves the local-recording container before
// the capture/encode pipeline exists. Not shipped.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "h264_es.h"
#include "mkv_writer.h"

namespace {

std::vector<std::uint8_t> ReadFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring input;
    std::wstring output;
    int fps = 30;
    int seconds = 5;
    int width = 640;
    int height = 480;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--file" && i + 1 < argc) {
            input = argv[++i];
        } else if (arg == L"--out" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == L"--fps" && i + 1 < argc) {
            fps = _wtoi(argv[++i]);
        } else if (arg == L"--seconds" && i + 1 < argc) {
            seconds = _wtoi(argv[++i]);
        } else if (arg == L"--width" && i + 1 < argc) {
            width = _wtoi(argv[++i]);
        } else if (arg == L"--height" && i + 1 < argc) {
            height = _wtoi(argv[++i]);
        } else {
            std::printf("unknown or incomplete option\n");
            return 2;
        }
    }

    if (input.empty() || output.empty() || fps <= 0 || seconds <= 0 || width <= 0 || height <= 0) {
        std::printf("usage: mkv_test_source --file <h264> --out <mkv> [--fps 30] [--seconds 5] "
                    "[--width 640] [--height 480]\n");
        return 2;
    }

    const std::vector<std::uint8_t> elementary = ReadFile(input);
    if (elementary.empty()) {
        std::printf("could not read %ls\n", input.c_str());
        return 1;
    }

    const std::vector<h264es::NalUnit> units = h264es::SplitNalUnits(elementary);
    const std::vector<h264es::AccessUnit> accessUnits = h264es::GroupAccessUnits(units);
    if (accessUnits.empty()) {
        std::printf("no access units found\n");
        return 1;
    }

    std::ofstream file(output, std::ios::binary);
    if (!file) {
        std::printf("could not create %ls\n", output.c_str());
        return 1;
    }

    MkvWriter writer(file, width, height, fps);
    const int64_t frameTicks = 90000 / fps;
    const int totalFrames = fps * seconds;

    for (int index = 0; index < totalFrames; ++index) {
        const h264es::AccessUnit& unit = accessUnits[static_cast<std::size_t>(index) % accessUnits.size()];
        const int64_t pts = static_cast<int64_t>(index) * frameTicks;
        if (!writer.WriteVideoFrame(unit.bytes.data(), unit.bytes.size(), pts, unit.keyframe)) {
            std::printf("write failed at frame %d\n", index);
            return 1;
        }
    }

    if (!writer.Finalize()) {
        std::printf("finalize failed\n");
        return 1;
    }

    std::printf("wrote %d frames (%zu access units, %.2f s) to %ls\n", totalFrames, accessUnits.size(),
                static_cast<double>(totalFrames) / fps, output.c_str());
    return 0;
}
