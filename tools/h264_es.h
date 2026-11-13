#pragma once

// Shared H.264 Annex B parsing for the test tools. Splits an elementary stream into access units,
// which is what both the cast test source and the MKV test source need.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace h264es {

struct NalUnit {
    const std::uint8_t* data = nullptr;  // payload after the start code
    std::size_t size = 0;
};

struct AccessUnit {
    std::vector<std::uint8_t> bytes;  // Annex B, start codes included
    bool keyframe = false;
    bool hasVcl = false;
};

inline bool IsVcl(std::uint8_t nalType)
{
    return nalType >= 1 && nalType <= 5;
}

// Returns true when the slice's first_mb_in_slice is 0, i.e. it starts a picture.
inline bool StartsPicture(const NalUnit& nal)
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

inline std::size_t FindStartCode(const std::vector<std::uint8_t>& data, std::size_t from)
{
    for (std::size_t i = from; i + 3 <= data.size(); ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            return i;
        }
    }
    return data.size();
}

inline std::vector<NalUnit> SplitNalUnits(const std::vector<std::uint8_t>& data)
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

inline std::vector<AccessUnit> GroupAccessUnits(const std::vector<NalUnit>& units)
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

}  // namespace h264es
