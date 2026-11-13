#include "ts_muxer.h"

#include <cstring>
#include <limits>

namespace {

constexpr std::uint8_t kSyncByte = 0x47;
constexpr std::int64_t kPsiInterval90k = 45000;  // 0.5 s
constexpr std::int64_t kPcrInterval90k = 9000;   // 0.1 s
constexpr std::size_t kPesHeaderSize = 14;       // start code + flags + PTS

std::uint32_t Crc32Mpeg(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
        }
    }
    return crc;
}

void EncodePts(std::uint8_t* out, std::int64_t pts)
{
    const auto value = static_cast<std::uint64_t>(pts);
    out[0] = static_cast<std::uint8_t>(0x21 | ((value >> 29) & 0x0E));
    out[1] = static_cast<std::uint8_t>((value >> 22) & 0xFF);
    out[2] = static_cast<std::uint8_t>(0x01 | ((value >> 14) & 0xFE));
    out[3] = static_cast<std::uint8_t>((value >> 7) & 0xFF);
    out[4] = static_cast<std::uint8_t>(0x01 | ((value << 1) & 0xFE));
}

void EncodePcr(std::uint8_t* out, std::int64_t pts)
{
    const auto base = static_cast<std::uint64_t>(pts) & 0x1FFFFFFFFull;
    out[0] = static_cast<std::uint8_t>(base >> 25);
    out[1] = static_cast<std::uint8_t>(base >> 17);
    out[2] = static_cast<std::uint8_t>(base >> 9);
    out[3] = static_cast<std::uint8_t>(base >> 1);
    out[4] = static_cast<std::uint8_t>(((base & 1) << 7) | 0x7E);  // reserved bits set, ext = 0
    out[5] = 0x00;
}

}  // namespace

TsMuxer::TsMuxer(TsSink& sink, TsAudioCodec audioCodec, int audioChannels)
    : sink_(sink), audioCodec_(audioCodec), audioChannels_(audioChannels) {}

void TsMuxer::WriteVideo(const std::uint8_t* data, std::size_t size, std::int64_t pts90k, bool keyframe)
{
    if (data == nullptr || size == 0) {
        return;
    }

    MaybeWritePsi(pts90k);
    MaybeWritePcr(pts90k);
    WritePes(kVideoPid, 0xE0, data, size, pts90k, keyframe);
}

void TsMuxer::WriteAudio(const std::uint8_t* data, std::size_t size, std::int64_t pts90k)
{
    if (data == nullptr || size == 0 || audioCodec_ == TsAudioCodec::None) {
        return;
    }
    // PES_packet_length is 16 bits; anything larger cannot be represented.
    if (size > 65527) {
        return;
    }

    MaybeWritePsi(pts90k);
    // Opus is carried as private_stream_1, as the Opus-in-TS specification requires. AAC keeps
    // the conventional audio stream id.
    if (audioCodec_ == TsAudioCodec::Opus) {
        // OpusTS framing: 0x7FE0 syncword with no trim/control flags, then the access-unit size
        // as a sequence of 255s plus a terminator, then the raw Opus packet. VLC's TS demux
        // silently drops Opus PES payloads that do not start with this syncword.
        const std::size_t sizeBytes = size / 255 + 1;
        const std::size_t framedSize = 2 + sizeBytes + size;
        if (framedSize > 65527) {
            return;
        }
        opusFrame_.resize(framedSize);
        std::uint8_t* out = opusFrame_.data();
        *out++ = 0x7F;
        *out++ = 0xE0;
        std::size_t remaining = size;
        while (remaining >= 255) {
            *out++ = 0xFF;
            remaining -= 255;
        }
        *out++ = static_cast<std::uint8_t>(remaining);
        std::memcpy(out, data, size);
        WritePes(kAudioPid, 0xBD, opusFrame_.data(), framedSize, pts90k, false);
        return;
    }

    WritePes(kAudioPid, 0xC0, data, size, pts90k, false);
}

void TsMuxer::MaybeWritePsi(std::int64_t pts90k)
{
    const bool due = !wrotePsi_ || pts90k < lastPsiPts_ || (pts90k - lastPsiPts_) >= kPsiInterval90k;
    if (!due) {
        return;
    }

    WritePat();
    WritePmt();
    wrotePsi_ = true;
    lastPsiPts_ = pts90k;
}

void TsMuxer::MaybeWritePcr(std::int64_t pts90k)
{
    const bool due = !wrotePcr_ || pts90k < lastPcrPts_ || (pts90k - lastPcrPts_) >= kPcrInterval90k;
    if (!due) {
        return;
    }

    std::uint8_t packet[kPacketSize] = {};
    packet[0] = kSyncByte;
    packet[1] = static_cast<std::uint8_t>((kPcrPid >> 8) & 0x1F);
    packet[2] = static_cast<std::uint8_t>(kPcrPid & 0xFF);
    packet[3] = static_cast<std::uint8_t>(0x20 | NextCc(kPcrPid));  // adaptation field only
    packet[4] = 183;
    packet[5] = 0x10;  // PCR flag
    EncodePcr(packet + 6, pts90k);
    std::memset(packet + 12, 0xFF, 176);

    sink_.OnTsPacket(packet);
    wrotePcr_ = true;
    lastPcrPts_ = pts90k;
}

void TsMuxer::WritePat()
{
    // table_id, section_length 13, tsid 1, version 0, program 1 -> PMT PID.
    std::uint8_t section[12] = {0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1,
                                0x00, 0x00, 0x00, 0x01, 0xF0, 0x00};
    WriteSection(kPatPid, section, sizeof(section));
}

void TsMuxer::WritePmt()
{
    // table_id, then a variable stream loop: H.264 video plus optional AAC or Opus audio.
    std::uint8_t section[64] = {};
    std::size_t size = 0;

    section[size++] = 0x02;
    const std::size_t lengthOffset = size;
    size += 2;

    section[size++] = 0x00;  // program_number
    section[size++] = 0x01;
    section[size++] = 0xC1;  // version 0, current_next_indicator
    section[size++] = 0x00;  // section_number
    section[size++] = 0x00;  // last_section_number

    section[size++] = static_cast<std::uint8_t>(0xE0 | ((kPcrPid >> 8) & 0x1F));
    section[size++] = static_cast<std::uint8_t>(kPcrPid & 0xFF);
    section[size++] = 0xF0;  // program_info_length 0
    section[size++] = 0x00;

    // H.264 video.
    section[size++] = 0x1B;
    section[size++] = static_cast<std::uint8_t>(0xE0 | ((kVideoPid >> 8) & 0x1F));
    section[size++] = static_cast<std::uint8_t>(kVideoPid & 0xFF);
    section[size++] = 0xF0;  // ES_info_length 0
    section[size++] = 0x00;

    if (audioCodec_ != TsAudioCodec::None) {
        const bool opus = (audioCodec_ == TsAudioCodec::Opus);
        section[size++] = opus ? 0x06 : 0x0F;  // private data (Opus) or AAC ADTS
        section[size++] = static_cast<std::uint8_t>(0xE0 | ((kAudioPid >> 8) & 0x1F));
        section[size++] = static_cast<std::uint8_t>(kAudioPid & 0xFF);

        if (opus) {
            // ES_info_length 10: registration descriptor "Opus" and the Opus audio descriptor.
            section[size++] = 0xF0;
            section[size++] = 0x0A;

            section[size++] = 0x05;  // registration_descriptor
            section[size++] = 0x04;
            section[size++] = 'O';
            section[size++] = 'p';
            section[size++] = 'u';
            section[size++] = 's';

            section[size++] = 0x7F;  // DVB extension descriptor
            section[size++] = 0x02;
            section[size++] = 0x80;  // Opus audio descriptor, RTP mapping family
            section[size++] = static_cast<std::uint8_t>(audioChannels_ < 1 ? 2 : (audioChannels_ > 2 ? 2 : audioChannels_));
        } else {
            section[size++] = 0xF0;  // ES_info_length 0
            section[size++] = 0x00;
        }
    }

    // section_length counts everything after its own field, including the 4-byte CRC.
    const std::size_t sectionLength = size - 3 + 4;
    section[lengthOffset] = static_cast<std::uint8_t>(0xB0 | ((sectionLength >> 8) & 0x0F));
    section[lengthOffset + 1] = static_cast<std::uint8_t>(sectionLength & 0xFF);

    WriteSection(kPmtPid, section, size);
}

void TsMuxer::WriteSection(std::uint16_t pid, const std::uint8_t* section, std::size_t size)
{
    if (size + 4 > 183) {  // 184 payload bytes minus the pointer field
        return;
    }

    std::uint8_t packet[kPacketSize];
    std::memset(packet, 0xFF, sizeof(packet));

    packet[0] = kSyncByte;
    packet[1] = static_cast<std::uint8_t>(0x40 | ((pid >> 8) & 0x1F));  // payload_unit_start
    packet[2] = static_cast<std::uint8_t>(pid & 0xFF);
    packet[3] = static_cast<std::uint8_t>(0x10 | NextCc(pid));
    packet[4] = 0x00;  // pointer_field

    std::memcpy(packet + 5, section, size);
    const std::uint32_t crc = Crc32Mpeg(section, size);
    const std::size_t crcOffset = 5 + size;
    packet[crcOffset + 0] = static_cast<std::uint8_t>(crc >> 24);
    packet[crcOffset + 1] = static_cast<std::uint8_t>(crc >> 16);
    packet[crcOffset + 2] = static_cast<std::uint8_t>(crc >> 8);
    packet[crcOffset + 3] = static_cast<std::uint8_t>(crc);

    sink_.OnTsPacket(packet);
}

void TsMuxer::WritePes(std::uint16_t pid, std::uint8_t streamId, const std::uint8_t* payload,
                       std::size_t size, std::int64_t pts90k, bool randomAccess)
{
    std::uint8_t header[kPesHeaderSize] = {};
    header[0] = 0x00;
    header[1] = 0x00;
    header[2] = 0x01;
    header[3] = streamId;

    // Video length 0 means "unbounded", which is what players expect for a live stream.
    const std::size_t pesLength = (streamId == 0xE0) ? 0 : (8 + size);
    header[4] = static_cast<std::uint8_t>((pesLength >> 8) & 0xFF);
    header[5] = static_cast<std::uint8_t>(pesLength & 0xFF);
    header[6] = 0x80;  // marker bits
    header[7] = 0x80;  // PTS only
    header[8] = 5;
    EncodePts(header + 9, pts90k);

    const std::size_t total = kPesHeaderSize + size;
    std::size_t offset = 0;
    bool first = true;

    while (offset < total) {
        const std::size_t remaining = total - offset;
        const bool needRandomAccess = first && randomAccess;
        const bool useAdaptation = needRandomAccess || remaining < 184;

        std::size_t payloadLength = 0;
        std::size_t adaptationLength = 0;

        if (needRandomAccess) {
            payloadLength = (remaining >= 182) ? 182 : remaining;
            adaptationLength = 183 - payloadLength;
        } else if (remaining >= 184) {
            payloadLength = 184;
        } else {
            payloadLength = remaining;
            adaptationLength = 183 - payloadLength;
        }

        std::uint8_t packet[kPacketSize] = {};
        packet[0] = kSyncByte;
        packet[1] = static_cast<std::uint8_t>(((pid >> 8) & 0x1F) | (first ? 0x40 : 0x00));
        packet[2] = static_cast<std::uint8_t>(pid & 0xFF);

        std::size_t payloadStart = 4;
        if (useAdaptation) {
            packet[3] = static_cast<std::uint8_t>(0x30 | NextCc(pid));
            packet[4] = static_cast<std::uint8_t>(adaptationLength);
            if (adaptationLength > 0) {
                packet[5] = static_cast<std::uint8_t>(needRandomAccess ? 0x40 : 0x00);
                if (adaptationLength > 1) {
                    std::memset(packet + 6, 0xFF, adaptationLength - 1);
                }
            }
            payloadStart = 5 + adaptationLength;
        } else {
            packet[3] = static_cast<std::uint8_t>(0x10 | NextCc(pid));
        }

        for (std::size_t i = 0; i < payloadLength; ++i) {
            const std::size_t index = offset + i;
            packet[payloadStart + i] = (index < kPesHeaderSize) ? header[index] : payload[index - kPesHeaderSize];
        }

        sink_.OnTsPacket(packet);
        offset += payloadLength;
        first = false;
    }
}

std::uint8_t TsMuxer::NextCc(std::uint16_t pid)
{
    std::uint8_t* counter = nullptr;
    switch (pid) {
    case kPatPid:
        counter = &ccPat_;
        break;
    case kPmtPid:
        counter = &ccPmt_;
        break;
    case kVideoPid:
        counter = &ccVideo_;
        break;
    case kAudioPid:
        counter = &ccAudio_;
        break;
    case kPcrPid:
        counter = &ccPcr_;
        break;
    default:
        return 0;
    }

    const std::uint8_t value = *counter;
    *counter = static_cast<std::uint8_t>((*counter + 1) & 0x0F);
    return value;
}
