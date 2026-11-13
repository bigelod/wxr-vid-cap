// Test-only cast source. Reads an H.264 Annex B elementary stream, groups NAL units into access
// units, muxes them with TsMuxer, and sends the result over UDP. Exists to prove the cast path
// end to end on Windows before any real capture/encode pipeline exists. Not shipped.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "ts_muxer.h"
#include "udp_sender.h"

namespace {

struct NalUnit {
    const std::uint8_t* data = nullptr;  // payload after the start code
    std::size_t size = 0;
};

struct AccessUnit {
    std::vector<std::uint8_t> bytes;  // Annex B, start codes included
    bool keyframe = false;
    bool hasVcl = false;
};

bool IsVcl(std::uint8_t nalType)
{
    return nalType >= 1 && nalType <= 5;
}

// Returns true when the slice's first_mb_in_slice is 0, i.e. it starts a picture.
bool StartsPicture(const NalUnit& nal)
{
    if (nal.size < 2) {
        return false;
    }

    std::uint8_t rbsp[64] = {};
    std::size_t count = 0;
    for (std::size_t i = 1; i < nal.size && count < sizeof(rbsp); ++i) {
        if (i >= 3 && nal.data[i] == 0x03 && nal.data[i - 1] == 0x00 && nal.data[i - 2] == 0x00) {
            continue;  // emulation prevention byte
        }
        rbsp[count++] = nal.data[i];
    }
    if (count == 0) {
        return false;
    }

    std::size_t bit = 0;
    int leadingZeros = 0;
    while (bit < count * 8 && ((rbsp[bit / 8] >> (7 - (bit % 8))) & 1) == 0) {
        ++leadingZeros;
        ++bit;
        if (leadingZeros > 31) {
            return false;
        }
    }
    if (bit >= count * 8) {
        return false;
    }
    ++bit;  // consume the terminating 1

    std::uint32_t value = 0;
    for (int i = 0; i < leadingZeros; ++i) {
        if (bit >= count * 8) {
            return false;
        }
        value = (value << 1) | ((rbsp[bit / 8] >> (7 - (bit % 8))) & 1);
        ++bit;
    }
    return value == 0;
}

std::size_t FindStartCode(const std::vector<std::uint8_t>& data, std::size_t from)
{
    for (std::size_t i = from; i + 3 <= data.size(); ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            return i;
        }
    }
    return data.size();
}

std::vector<NalUnit> SplitNalUnits(const std::vector<std::uint8_t>& data)
{
    std::vector<NalUnit> units;
    std::size_t position = 0;

    while (position < data.size()) {
        const std::size_t start = FindStartCode(data, position);
        if (start >= data.size()) {
            break;
        }
        std::size_t payload = start + 3;
        const std::size_t next = FindStartCode(data, payload);
        if (payload < next) {
            units.push_back({data.data() + payload, next - payload});
        }
        position = next;
    }
    return units;
}

std::vector<AccessUnit> GroupAccessUnits(const std::vector<NalUnit>& units)
{
    std::vector<AccessUnit> result;
    AccessUnit current;

    for (const NalUnit& unit : units) {
        if (unit.size == 0) {
            continue;
        }
        const std::uint8_t type = unit.data[0] & 0x1F;
        const bool vcl = IsVcl(type);

        bool startNew = false;
        if (current.hasVcl) {
            if (type == 9) {  // access unit delimiter
                startNew = true;
            } else if (vcl) {
                startNew = StartsPicture(unit);
            } else {
                startNew = true;  // parameter sets / SEI after a picture begin the next one
            }
        }

        if (startNew && !current.bytes.empty()) {
            result.push_back(std::move(current));
            current = AccessUnit();
        }

        static const std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
        current.bytes.insert(current.bytes.end(), kStartCode, kStartCode + 4);
        current.bytes.insert(current.bytes.end(), unit.data, unit.data + unit.size);

        if (vcl) {
            current.hasVcl = true;
            if (type == 5) {
                current.keyframe = true;
            }
        }
    }

    if (!current.bytes.empty()) {
        result.push_back(std::move(current));
    }
    return result;
}

std::vector<std::uint8_t> ReadFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

class UdpTsSink final : public TsSink {
public:
    UdpTsSink(UdpSender& sender, std::vector<std::uint8_t>& captured)
        : sender_(sender), captured_(captured)
    {
        buffer_.reserve(TsMuxer::kPacketSize * UdpSender::kPacketsPerDatagram);
    }

    void OnTsPacket(const std::uint8_t* packet) override
    {
        captured_.insert(captured_.end(), packet, packet + TsMuxer::kPacketSize);
        buffer_.insert(buffer_.end(), packet, packet + TsMuxer::kPacketSize);
        if (buffer_.size() >= TsMuxer::kPacketSize * UdpSender::kPacketsPerDatagram) {
            Flush();
        }
    }

    void Flush()
    {
        if (buffer_.empty()) {
            return;
        }
        const std::size_t packets = buffer_.size() / TsMuxer::kPacketSize;
        sender_.SendTsPackets(buffer_.data(), packets);
        buffer_.clear();
    }

private:
    UdpSender& sender_;
    std::vector<std::uint8_t>& captured_;
    std::vector<std::uint8_t> buffer_;
};

struct Options {
    std::wstring input;
    std::wstring audio;
    std::wstring output;
    std::wstring target = L"127.0.0.1";
    unsigned short port = 1234;
    int fps = 30;
    int seconds = 5;
};

bool ParseOptions(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto next = [&](std::wstring& value) {
            if (i + 1 >= argc) {
                return false;
            }
            value = argv[++i];
            return true;
        };

        if (arg == L"--file") {
            if (!next(options.input)) {
                return false;
            }
        } else if (arg == L"--audio") {
            if (!next(options.audio)) {
                return false;
            }
        } else if (arg == L"--out") {
            if (!next(options.output)) {
                return false;
            }
        } else if (arg == L"--target") {
            if (!next(options.target)) {
                return false;
            }
        } else if (arg == L"--port") {
            options.port = static_cast<unsigned short>(_wtoi(argv[++i]));
        } else if (arg == L"--fps") {
            options.fps = _wtoi(argv[++i]);
        } else if (arg == L"--seconds") {
            options.seconds = _wtoi(argv[++i]);
        } else {
            std::printf("unknown option\n");
            return false;
        }
    }

    return !options.input.empty() && options.port != 0 && options.fps > 0 && options.seconds > 0;
}

// Splits an ADTS byte stream into whole frames. Returns false on a malformed header.
bool ParseAdtsFrames(const std::vector<std::uint8_t>& data, std::vector<std::vector<std::uint8_t>>& frames,
                     int& sampleRate)
{
    static const int kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                         22050, 16000, 12000, 11025, 8000,  7350};

    std::size_t position = 0;
    while (position + 7 <= data.size()) {
        if (data[position] != 0xFF || (data[position + 1] & 0xF6) != 0xF0) {
            return false;
        }
        const std::size_t length = (static_cast<std::size_t>(data[position + 3] & 0x03) << 11) |
                                   (static_cast<std::size_t>(data[position + 4]) << 3) |
                                   (static_cast<std::size_t>(data[position + 5]) >> 5);
        if (length < 7 || position + length > data.size()) {
            return false;
        }
        frames.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(position),
                            data.begin() + static_cast<std::ptrdiff_t>(position + length));
        position += length;
    }

    if (frames.empty() || position != data.size()) {
        return false;
    }

    const int index = (data[2] >> 2) & 0x0F;
    sampleRate = (index < 13) ? kSampleRates[index] : 48000;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::printf("usage: cast_test_source --file <h264> [--audio <aac>] [--target 127.0.0.1] "
                    "[--port 1234] [--fps 30] [--seconds 5] [--out capture.ts]\n");
        return 2;
    }

    const std::vector<std::uint8_t> elementary = ReadFile(options.input);
    if (elementary.empty()) {
        std::printf("could not read %ls\n", options.input.c_str());
        return 1;
    }

    const std::vector<NalUnit> units = SplitNalUnits(elementary);
    const std::vector<AccessUnit> accessUnits = GroupAccessUnits(units);
    if (accessUnits.empty()) {
        std::printf("no access units found\n");
        return 1;
    }

    std::printf("access units: %zu (first keyframe: %s)\n", accessUnits.size(),
                accessUnits.front().keyframe ? "yes" : "no");

    std::vector<std::vector<std::uint8_t>> audioFrames;
    int audioSampleRate = 48000;
    if (!options.audio.empty()) {
        const std::vector<std::uint8_t> audioData = ReadFile(options.audio);
        if (audioData.empty()) {
            std::printf("could not read %ls\n", options.audio.c_str());
            return 1;
        }
        if (!ParseAdtsFrames(audioData, audioFrames, audioSampleRate)) {
            std::printf("audio is not a valid ADTS stream\n");
            return 1;
        }
        std::printf("audio frames: %zu at %d Hz\n", audioFrames.size(), audioSampleRate);
    }

    UdpSender sender;
    std::wstring error;
    if (!sender.Open(options.target, options.port, error)) {
        std::printf("sender failed: %ls\n", error.c_str());
        return 1;
    }

    std::vector<std::uint8_t> captured;
    UdpTsSink sink(sender, captured);
    TsMuxer muxer(sink);

    const int64_t frameTicks = 90000 / options.fps;
    const auto start = std::chrono::steady_clock::now();
    int frameIndex = 0;
    int audioIndex = 0;
    const int totalFrames = options.fps * options.seconds;
    const int64_t audioFrameTicks = audioFrames.empty()
                                        ? 0
                                        : (static_cast<int64_t>(1024) * 90000) / audioSampleRate;

    while (frameIndex < totalFrames) {
        const AccessUnit& unit = accessUnits[static_cast<std::size_t>(frameIndex) % accessUnits.size()];
        const int64_t pts = static_cast<int64_t>(frameIndex) * frameTicks;
        muxer.WriteVideo(unit.bytes.data(), unit.bytes.size(), pts, unit.keyframe);

        // Send audio up to (not past) the current video timestamp. Timestamps keep advancing even
        // though the fixture loops, so VLC sees a continuous clock.
        while (!audioFrames.empty()) {
            const int64_t audioPts = static_cast<int64_t>(audioIndex) * audioFrameTicks;
            if (audioPts > pts) {
                break;
            }
            const std::vector<std::uint8_t>& frame =
                audioFrames[static_cast<std::size_t>(audioIndex) % audioFrames.size()];
            muxer.WriteAudio(frame.data(), frame.size(), audioPts);
            ++audioIndex;
        }

        sink.Flush();

        ++frameIndex;
        const auto deadline = start + std::chrono::microseconds(static_cast<long long>(frameIndex) * 1000000 /
                                                               options.fps);
        std::this_thread::sleep_until(deadline);
    }

    std::printf("sent %d video frames, %d audio frames, %zu TS bytes\n", frameIndex, audioIndex,
                captured.size());

    if (!options.output.empty()) {
        std::ofstream file(options.output, std::ios::binary);
        file.write(reinterpret_cast<const char*>(captured.data()), static_cast<std::streamsize>(captured.size()));
    }

    sender.Close();
    return 0;
}
