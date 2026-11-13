// Structural tests for the Matroska writer: EBML header, track description, cluster timing, and
// AVCC conversion. Uses synthetic NAL units, so no media files are required.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "mkv_writer.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

struct Element {
    std::uint32_t id = 0;
    std::size_t data = 0;
    std::size_t size = 0;
    bool unknownSize = false;
};

bool ReadElement(const std::vector<std::uint8_t>& bytes, std::size_t& position, Element& element)
{
    if (position >= bytes.size()) {
        return false;
    }

    const std::uint8_t first = bytes[position];
    int idLength = 0;
    if ((first & 0x80) != 0) {
        idLength = 1;
    } else if ((first & 0x40) != 0) {
        idLength = 2;
    } else if ((first & 0x20) != 0) {
        idLength = 3;
    } else if ((first & 0x10) != 0) {
        idLength = 4;
    } else {
        return false;
    }
    if (position + static_cast<std::size_t>(idLength) >= bytes.size()) {
        return false;
    }

    std::uint32_t id = 0;
    for (int i = 0; i < idLength; ++i) {
        id = (id << 8) | bytes[position + static_cast<std::size_t>(i)];
    }

    std::size_t cursor = position + static_cast<std::size_t>(idLength);
    const std::uint8_t sizeFirst = bytes[cursor];
    std::uint8_t marker = 0x80;
    int sizeLength = 0;
    while (sizeLength < 8 && (sizeFirst & marker) == 0) {
        marker = static_cast<std::uint8_t>(marker >> 1);
        ++sizeLength;
    }
    if (sizeLength >= 8) {
        return false;
    }
    ++sizeLength;

    if (cursor + static_cast<std::size_t>(sizeLength) > bytes.size()) {
        return false;
    }

    std::uint64_t size = sizeFirst & static_cast<std::uint8_t>(marker - 1);
    for (int i = 1; i < sizeLength; ++i) {
        size = (size << 8) | bytes[cursor + static_cast<std::size_t>(i)];
    }

    const std::uint64_t allOnes = (1ull << (7 * sizeLength)) - 1;
    const bool unknown = (size == allOnes);

    element.id = id;
    element.unknownSize = unknown;
    element.data = cursor + static_cast<std::size_t>(sizeLength);
    element.size = unknown ? (bytes.size() - element.data) : static_cast<std::size_t>(size);
    if (element.data + element.size > bytes.size()) {
        return false;
    }

    position = element.data;
    return true;
}

bool FindChild(const std::vector<std::uint8_t>& bytes, std::size_t start, std::size_t end, std::uint32_t id,
               Element& result)
{
    std::size_t position = start;
    while (position < end) {
        Element element;
        if (!ReadElement(bytes, position, element)) {
            return false;
        }
        if (element.id == id) {
            result = element;
            return true;
        }
        position = element.data + element.size;
        if (element.unknownSize) {
            break;
        }
    }
    return false;
}

std::vector<Element> CollectChildren(const std::vector<std::uint8_t>& bytes, std::size_t start, std::size_t end,
                                     std::uint32_t id)
{
    std::vector<Element> result;
    std::size_t position = start;
    while (position < end) {
        Element element;
        if (!ReadElement(bytes, position, element)) {
            break;
        }
        if (element.id == id) {
            result.push_back(element);
        }
        position = element.data + element.size;
        if (element.unknownSize) {
            break;
        }
    }
    return result;
}

std::vector<std::uint8_t> MakeAccessUnit(const std::vector<std::vector<std::uint8_t>>& nals)
{
    std::vector<std::uint8_t> result;
    for (const std::vector<std::uint8_t>& nal : nals) {
        static const std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
        result.insert(result.end(), kStartCode, kStartCode + 4);
        result.insert(result.end(), nal.begin(), nal.end());
    }
    return result;
}

std::string AsString(const std::vector<std::uint8_t>& bytes, const Element& element)
{
    return std::string(reinterpret_cast<const char*>(bytes.data() + element.data), element.size);
}

std::uint64_t AsUInt(const std::vector<std::uint8_t>& bytes, const Element& element)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < element.size; ++i) {
        value = (value << 8) | bytes[element.data + i];
    }
    return value;
}

void TestMkvWriter()
{
    const std::vector<std::uint8_t> sps = {0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD9, 0x40};
    const std::vector<std::uint8_t> pps = {0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0};
    const std::vector<std::uint8_t> idr = {0x65, 0x88, 0x84, 0x21, 0xA0};
    const std::vector<std::uint8_t> inter = {0x41, 0x9A, 0x24, 0x6C};

    std::ostringstream stream(std::ios::binary);
    MkvWriter writer(stream, 640, 480, 30);

    const std::vector<std::uint8_t> keyFrame = MakeAccessUnit({sps, pps, idr});
    const std::vector<std::uint8_t> deltaFrame = MakeAccessUnit({inter});

    CHECK(writer.WriteVideoFrame(keyFrame.data(), keyFrame.size(), 0, true));
    CHECK(writer.WriteVideoFrame(deltaFrame.data(), deltaFrame.size(), 3000, false));
    CHECK(writer.WriteVideoFrame(deltaFrame.data(), deltaFrame.size(), 6000, false));
    CHECK(writer.WriteVideoFrame(keyFrame.data(), keyFrame.size(), 9000, true));
    CHECK(writer.Finalize());
    CHECK(writer.Ok());

    const std::string text = stream.str();
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());

    std::size_t position = 0;
    Element ebml;
    CHECK(ReadElement(bytes, position, ebml));
    CHECK(ebml.id == 0x1A45DFA3);

    Element docType;
    CHECK(FindChild(bytes, ebml.data, ebml.data + ebml.size, 0x4282, docType));
    CHECK(AsString(bytes, docType) == "matroska");

    Element docTypeVersion;
    CHECK(FindChild(bytes, ebml.data, ebml.data + ebml.size, 0x4287, docTypeVersion));
    CHECK(docTypeVersion.size == 1);
    CHECK(AsUInt(bytes, docTypeVersion) == 4);

    position = ebml.data + ebml.size;
    Element segment;
    CHECK(ReadElement(bytes, position, segment));
    CHECK(segment.id == 0x18538067);
    CHECK(segment.unknownSize);

    Element tracks;
    CHECK(FindChild(bytes, segment.data, segment.data + segment.size, 0x1654AE6B, tracks));

    Element trackEntry;
    CHECK(FindChild(bytes, tracks.data, tracks.data + tracks.size, 0xAE, trackEntry));

    Element codecId;
    CHECK(FindChild(bytes, trackEntry.data, trackEntry.data + trackEntry.size, 0x86, codecId));
    CHECK(AsString(bytes, codecId) == "V_MPEG4/ISO/AVC");

    Element codecPrivate;
    CHECK(FindChild(bytes, trackEntry.data, trackEntry.data + trackEntry.size, 0x63A2, codecPrivate));
    CHECK(codecPrivate.size == 11 + sps.size() + pps.size() + 4);  // + High-profile extension
    CHECK(bytes[codecPrivate.data] == 0x01);
    CHECK(bytes[codecPrivate.data + 1] == sps[1]);
    CHECK(bytes[codecPrivate.data + 2] == sps[2]);
    CHECK(bytes[codecPrivate.data + 3] == sps[3]);

    const std::vector<Element> clusters =
        CollectChildren(bytes, segment.data, segment.data + segment.size, 0x1F43B675);
    CHECK(clusters.size() == 2);  // a keyframe starts a new cluster

    Element timecode;
    CHECK(FindChild(bytes, clusters[0].data, clusters[0].data + clusters[0].size, 0xE7, timecode));
    CHECK(AsUInt(bytes, timecode) == 0);

    const std::vector<Element> blocks =
        CollectChildren(bytes, clusters[0].data, clusters[0].data + clusters[0].size, 0xA3);
    CHECK(blocks.size() == 3);

    // First block: track 1, relative 0, keyframe flag, AVCC with SPS, PPS, IDR.
    const Element& first = blocks[0];
    CHECK(bytes[first.data] == 0x81);
    CHECK(bytes[first.data + 1] == 0x00 && bytes[first.data + 2] == 0x00);
    CHECK((bytes[first.data + 3] & 0x80) != 0);

    std::size_t cursor = first.data + 4;
    const std::size_t end = first.data + first.size;
    // SPS and PPS are carried in CodecPrivate only; the block holds the IDR.
    CHECK(cursor + 4 <= end);
    const std::size_t length = (static_cast<std::size_t>(bytes[cursor]) << 24) |
                               (static_cast<std::size_t>(bytes[cursor + 1]) << 16) |
                               (static_cast<std::size_t>(bytes[cursor + 2]) << 8) |
                               static_cast<std::size_t>(bytes[cursor + 3]);
    CHECK(length == idr.size());
    cursor += 4 + length;
    CHECK(cursor == end);

    // Second block is a delta frame: flags must not carry the keyframe bit.
    const Element& second = blocks[1];
    CHECK(bytes[second.data] == 0x81);
    CHECK((bytes[second.data + 3] & 0x80) == 0);
}

}  // namespace

int RunMkvWriterTests()
{
    std::printf("wxr-vid-cap mkv tests\n");
    TestMkvWriter();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
