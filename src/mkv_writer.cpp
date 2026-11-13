#include "mkv_writer.h"

#include <cstring>
#include <vector>

namespace {

struct NalView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

void WriteId(std::ostream& out, std::uint32_t id)
{
    if (id <= 0xFF) {
        out.put(static_cast<char>(id));
    } else if (id <= 0xFFFF) {
        out.put(static_cast<char>((id >> 8) & 0xFF));
        out.put(static_cast<char>(id & 0xFF));
    } else if (id <= 0xFFFFFF) {
        out.put(static_cast<char>((id >> 16) & 0xFF));
        out.put(static_cast<char>((id >> 8) & 0xFF));
        out.put(static_cast<char>(id & 0xFF));
    } else {
        out.put(static_cast<char>((id >> 24) & 0xFF));
        out.put(static_cast<char>((id >> 16) & 0xFF));
        out.put(static_cast<char>((id >> 8) & 0xFF));
        out.put(static_cast<char>(id & 0xFF));
    }
}

void WriteSize(std::ostream& out, std::uint64_t size)
{
    int length = 1;
    while (length < 8 && size >= ((1ull << (7 * length)) - 1)) {
        ++length;
    }
    for (int i = length - 1; i >= 0; --i) {
        std::uint8_t byte = static_cast<std::uint8_t>((size >> (8 * i)) & 0xFF);
        if (i == length - 1) {
            byte = static_cast<std::uint8_t>(byte | (1 << (8 - length)));
        }
        out.put(static_cast<char>(byte));
    }
}

std::string MakeElement(std::uint32_t id, const std::string& payload)
{
    std::string element;
    element.reserve(payload.size() + 12);

    // IDs are small and fixed, so render them through a temporary stream-free helper.
    auto appendId = [&element](std::uint32_t value) {
        if (value <= 0xFF) {
            element.push_back(static_cast<char>(value));
        } else if (value <= 0xFFFF) {
            element.push_back(static_cast<char>((value >> 8) & 0xFF));
            element.push_back(static_cast<char>(value & 0xFF));
        } else if (value <= 0xFFFFFF) {
            element.push_back(static_cast<char>((value >> 16) & 0xFF));
            element.push_back(static_cast<char>((value >> 8) & 0xFF));
            element.push_back(static_cast<char>(value & 0xFF));
        } else {
            element.push_back(static_cast<char>((value >> 24) & 0xFF));
            element.push_back(static_cast<char>((value >> 16) & 0xFF));
            element.push_back(static_cast<char>((value >> 8) & 0xFF));
            element.push_back(static_cast<char>(value & 0xFF));
        }
    };

    auto appendSize = [&element](std::uint64_t size) {
        int length = 1;
        while (length < 8 && size >= ((1ull << (7 * length)) - 1)) {
            ++length;
        }
        for (int i = length - 1; i >= 0; --i) {
            std::uint8_t byte = static_cast<std::uint8_t>((size >> (8 * i)) & 0xFF);
            if (i == length - 1) {
                byte = static_cast<std::uint8_t>(byte | (1 << (8 - length)));
            }
            element.push_back(static_cast<char>(byte));
        }
    };

    appendId(id);
    appendSize(payload.size());
    element.append(payload);
    return element;
}

std::string MakeUInt(std::uint32_t id, std::uint64_t value)
{
    int length = 1;
    for (int i = 7; i >= 0; --i) {
        if (((value >> (8 * i)) & 0xFF) != 0) {
            length = i + 1;
            break;
        }
    }

    std::string payload;
    payload.reserve(static_cast<std::size_t>(length));
    for (int i = length - 1; i >= 0; --i) {
        payload.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
    return MakeElement(id, payload);
}

std::string MakeString(std::uint32_t id, const char* text)
{
    return MakeElement(id, std::string(text));
}

std::string MakeFloat(std::uint32_t id, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::string payload;
    payload.push_back(static_cast<char>((bits >> 24) & 0xFF));
    payload.push_back(static_cast<char>((bits >> 16) & 0xFF));
    payload.push_back(static_cast<char>((bits >> 8) & 0xFF));
    payload.push_back(static_cast<char>(bits & 0xFF));
    return MakeElement(id, payload);
}

std::vector<NalView> ParseAnnexB(const std::uint8_t* data, std::size_t size)
{
    std::vector<NalView> units;

    std::size_t position = 0;
    while (position + 3 <= size) {
        if (!(data[position] == 0x00 && data[position + 1] == 0x00 && data[position + 2] == 0x01)) {
            ++position;
            continue;
        }

        const std::size_t payload = position + 3;
        std::size_t next = payload;
        while (next + 3 <= size) {
            if (data[next] == 0x00 && data[next + 1] == 0x00 && data[next + 2] == 0x01) {
                break;
            }
            ++next;
        }
        const std::size_t end = (next + 3 <= size) ? next : size;

        std::size_t trimmed = end;
        while (trimmed > payload && data[trimmed - 1] == 0x00) {
            --trimmed;  // trailing_zero_8bits between NAL units
        }
        if (trimmed > payload) {
            units.push_back({data + payload, trimmed - payload});
        }

        position = (next + 3 <= size) ? next : size;
    }

    return units;
}

std::string BuildAvcC(const std::vector<std::uint8_t>& sps, const std::vector<std::uint8_t>& pps)
{
    std::string config;
    config.push_back(1);  // configurationVersion
    config.push_back(static_cast<char>(sps[1]));
    config.push_back(static_cast<char>(sps[2]));
    config.push_back(static_cast<char>(sps[3]));
    config.push_back(static_cast<char>(0xFF));  // reserved + lengthSizeMinusOne = 3
    config.push_back(static_cast<char>(0xE1));  // reserved + one SPS
    config.push_back(static_cast<char>((sps.size() >> 8) & 0xFF));
    config.push_back(static_cast<char>(sps.size() & 0xFF));
    config.append(reinterpret_cast<const char*>(sps.data()), sps.size());
    config.push_back(1);  // one PPS
    config.push_back(static_cast<char>((pps.size() >> 8) & 0xFF));
    config.push_back(static_cast<char>(pps.size() & 0xFF));
    config.append(reinterpret_cast<const char*>(pps.data()), pps.size());

    // High profiles (100/110/122/144) require the chroma/bit-depth extension in the record.
    const std::uint8_t profile = sps[1];
    if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
        config.push_back(static_cast<char>(0xFC | 0x01));  // reserved + chroma_format 4:2:0
        config.push_back(static_cast<char>(0xF8 | 0x00));  // bit_depth_luma_minus8
        config.push_back(static_cast<char>(0xF8 | 0x00));  // bit_depth_chroma_minus8
        config.push_back(0);                               // numOfSequenceParameterSetExt
    }
    return config;
}

std::int64_t PtsToMs(std::int64_t pts90k)
{
    return (pts90k * 1000) / 90000;
}

}  // namespace

MkvWriter::MkvWriter(std::ostream& out, int width, int height, int fps, int audioSampleRate, int audioChannels,
                     const std::vector<std::uint8_t>& audioCodecPrivate)
    : out_(out),
      width_(width),
      height_(height),
      fps_(fps > 0 ? fps : 30),
      audioSampleRate_(audioSampleRate),
      audioChannels_(audioChannels),
      audioCodecPrivate_(audioCodecPrivate)
{
}

bool MkvWriter::WriteVideoFrame(const std::uint8_t* annexB, std::size_t size, std::int64_t pts90k, bool keyframe)
{
    if (!ok_ || annexB == nullptr || size == 0) {
        return false;
    }

    if (!headerWritten_ && !EnsureHeader(annexB, size)) {
        ok_ = false;
        return false;
    }

    const std::vector<NalView> units = ParseAnnexB(annexB, size);
    if (units.empty()) {
        ok_ = false;
        return false;
    }

    // Matroska stores AVC as length-prefixed NAL units, not Annex B start codes. Parameter sets
    // live in CodecPrivate, so drop them from the block stream.
    std::string frame;
    for (const NalView& unit : units) {
        const std::uint8_t type = unit.data[0] & 0x1F;
        if (type == 7 || type == 8) {
            continue;
        }
        const std::uint32_t length = static_cast<std::uint32_t>(unit.size);
        frame.push_back(static_cast<char>((length >> 24) & 0xFF));
        frame.push_back(static_cast<char>((length >> 16) & 0xFF));
        frame.push_back(static_cast<char>((length >> 8) & 0xFF));
        frame.push_back(static_cast<char>(length & 0xFF));
        frame.append(reinterpret_cast<const char*>(unit.data), unit.size);
    }
    if (frame.empty()) {
        ok_ = false;
        return false;
    }

    const std::int64_t ptsMs = PtsToMs(pts90k);
    const bool needNewCluster = !clusterOpen_ || keyframe || ptsMs < clusterTimeMs_ ||
                                (ptsMs - clusterTimeMs_) > 1000 || (ptsMs - clusterTimeMs_) > 32767;
    if (needNewCluster) {
        FlushCluster();
        clusterTimeMs_ = ptsMs;
        clusterOpen_ = true;
        cluster_ = MakeUInt(0xE7, static_cast<std::uint64_t>(ptsMs));  // Cluster Timecode
    }

    const std::int64_t relative = ptsMs - clusterTimeMs_;
    std::string block;
    block.push_back(static_cast<char>(0x81));  // track number 1 as a VINT
    block.push_back(static_cast<char>((static_cast<std::uint16_t>(relative) >> 8) & 0xFF));
    block.push_back(static_cast<char>(static_cast<std::uint16_t>(relative) & 0xFF));
    block.push_back(static_cast<char>(keyframe ? 0x80 : 0x00));
    block.append(frame);

    cluster_.append(MakeElement(0xA3, block));  // SimpleBlock
    lastPtsMs_ = ptsMs;
    return ok_;
}

bool MkvWriter::WriteAudioFrame(const std::uint8_t* data, std::size_t size, std::int64_t pts90k)
{
    if (!ok_ || data == nullptr || size == 0) {
        return false;
    }
    if (!headerWritten_ || audioChannels_ <= 0) {
        ok_ = false;
        return false;
    }

    const std::int64_t ptsMs = PtsToMs(pts90k);
    const bool needNewCluster = !clusterOpen_ || ptsMs < clusterTimeMs_ ||
                                (ptsMs - clusterTimeMs_) > 1000 || (ptsMs - clusterTimeMs_) > 32767;
    if (needNewCluster) {
        FlushCluster();
        clusterTimeMs_ = ptsMs;
        clusterOpen_ = true;
        cluster_ = MakeUInt(0xE7, static_cast<std::uint64_t>(ptsMs));
    }

    const std::int64_t relative = ptsMs - clusterTimeMs_;
    std::string block;
    block.push_back(static_cast<char>(0x82));  // track number 2 as a VINT
    block.push_back(static_cast<char>((static_cast<std::uint16_t>(relative) >> 8) & 0xFF));
    block.push_back(static_cast<char>(static_cast<std::uint16_t>(relative) & 0xFF));
    block.push_back(static_cast<char>(0x80));  // Opus frames are independently decodable
    block.append(reinterpret_cast<const char*>(data), size);

    cluster_.append(MakeElement(0xA3, block));
    return ok_;
}

bool MkvWriter::Finalize()
{
    FlushCluster();
    out_.flush();
    ok_ = ok_ && out_.good();
    return ok_;
}

bool MkvWriter::EnsureHeader(const std::uint8_t* annexB, std::size_t size)
{
    const std::vector<NalView> units = ParseAnnexB(annexB, size);

    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
    for (const NalView& unit : units) {
        if (unit.size < 4) {
            continue;
        }
        const std::uint8_t type = unit.data[0] & 0x1F;
        if (type == 7 && sps.empty()) {
            sps.assign(unit.data, unit.data + unit.size);
        } else if (type == 8 && pps.empty()) {
            pps.assign(unit.data, unit.data + unit.size);
        }
    }

    if (sps.size() < 4 || pps.empty()) {
        return false;  // cannot describe the track without parameter sets
    }

    std::string ebml;
    ebml += MakeUInt(0x4286, 1);  // EBMLVersion
    ebml += MakeUInt(0x42F7, 1);  // EBMLReadVersion
    ebml += MakeUInt(0x42F2, 4);  // EBMLMaxIDLength
    ebml += MakeUInt(0x42F3, 8);  // EBMLMaxSizeLength
    ebml += MakeString(0x4282, "matroska");
    ebml += MakeUInt(0x4287, 4);  // DocTypeVersion
    ebml += MakeUInt(0x4285, 2);  // DocTypeReadVersion
    WriteElement(0x1A45DFA3, ebml);

    WriteId(out_, 0x18538067);  // Segment
    WriteSize(out_, 0x01FFFFFFFFFFFFFFull);  // unknown size, streamed

    std::string info;
    info += MakeUInt(0x2AD7B1, 1000000);  // TimecodeScale: 1 ms
    info += MakeString(0x4D80, "wxr-vid-cap");
    info += MakeString(0x5741, "wxr-vid-cap");
    WriteElement(0x1549A966, info);

    std::string video;
    video += MakeUInt(0xB0, static_cast<std::uint64_t>(width_));
    video += MakeUInt(0xBA, static_cast<std::uint64_t>(height_));

    std::string track;
    track += MakeUInt(0xD7, 1);     // TrackNumber
    track += MakeUInt(0x73C5, 1);   // TrackUID
    track += MakeUInt(0x83, 1);     // TrackType: video
    track += MakeUInt(0x9C, 0);     // FlagLacing
    track += MakeString(0x86, "V_MPEG4/ISO/AVC");
    track += MakeElement(0x63A2, BuildAvcC(sps, pps));
    track += MakeUInt(0x23E383, static_cast<std::uint64_t>(1000000000ull / static_cast<unsigned>(fps_)));
    track += MakeElement(0xE0, video);

    std::string tracks;
    tracks += MakeElement(0xAE, track);

    if (audioChannels_ > 0 && audioSampleRate_ > 0 && !audioCodecPrivate_.empty()) {
        std::string audioInfo;
        audioInfo += MakeFloat(0xB5, static_cast<float>(audioSampleRate_));
        audioInfo += MakeUInt(0x9F, static_cast<std::uint64_t>(audioChannels_));

        std::string audioTrack;
        audioTrack += MakeUInt(0xD7, 2);  // TrackNumber
        audioTrack += MakeUInt(0x73C5, 2);  // TrackUID
        audioTrack += MakeUInt(0x83, 2);    // TrackType: audio
        audioTrack += MakeUInt(0x9C, 0);    // FlagLacing
        audioTrack += MakeString(0x86, "A_OPUS");
        audioTrack += MakeElement(0x63A2, std::string(reinterpret_cast<const char*>(audioCodecPrivate_.data()),
                                                      audioCodecPrivate_.size()));
        audioTrack += MakeUInt(0x23E383, 20000000);  // 20 ms
        audioTrack += MakeElement(0xE1, audioInfo);
        tracks += MakeElement(0xAE, audioTrack);
    }

    WriteElement(0x1654AE6B, tracks);

    headerWritten_ = ok_;
    return headerWritten_;
}

void MkvWriter::FlushCluster()
{
    if (!clusterOpen_) {
        return;
    }
    WriteElement(0x1F43B675, cluster_);
    cluster_.clear();
    clusterOpen_ = false;
}

void MkvWriter::WriteElement(std::uint32_t id, const std::string& payload)
{
    WriteId(out_, id);
    WriteSize(out_, payload.size());
    WriteRaw(payload.data(), payload.size());
}

void MkvWriter::WriteRaw(const void* data, std::size_t size)
{
    if (size == 0) {
        return;
    }
    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out_.good()) {
        ok_ = false;
    }
}
