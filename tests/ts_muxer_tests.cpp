// Structural tests for the MPEG-TS muxer and UDP sender. Console app; exit code is the number of
// failed checks. No external test framework or media files required.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ts_muxer.h"
#include "udp_sender.h"

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

using Packet = std::array<std::uint8_t, TsMuxer::kPacketSize>;

struct MemorySink final : TsSink {
    std::vector<Packet> packets;

    void OnTsPacket(const std::uint8_t* packet) override
    {
        Packet copy = {};
        std::memcpy(copy.data(), packet, copy.size());
        packets.push_back(copy);
    }
};

struct Bytes {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

int Pid(const Packet& packet)
{
    return ((packet[1] & 0x1F) << 8) | packet[2];
}

bool Pusi(const Packet& packet)
{
    return (packet[1] & 0x40) != 0;
}

int Afc(const Packet& packet)
{
    return (packet[3] >> 4) & 0x03;
}

int Cc(const Packet& packet)
{
    return packet[3] & 0x0F;
}

Bytes Payload(const Packet& packet)
{
    std::size_t start = 4;
    if ((Afc(packet) & 0x02) != 0) {
        start = 5 + packet[4];
    }
    return {packet.data() + start, TsMuxer::kPacketSize - start};
}

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

Bytes Section(const Packet& packet)
{
    const Bytes payload = Payload(packet);
    const std::size_t pointer = payload.data[0];
    const std::uint8_t* section = payload.data + 1 + pointer;
    const std::size_t size = 3 + (((section[1] & 0x0F) << 8) | section[2]);
    return {section, size};
}

std::int64_t DecodePts(const std::uint8_t* data)
{
    return (static_cast<std::int64_t>(data[0] & 0x0E) << 29) |
           (static_cast<std::int64_t>(data[1]) << 22) |
           (static_cast<std::int64_t>(data[2] & 0xFE) << 14) |
           (static_cast<std::int64_t>(data[3]) << 7) |
           (static_cast<std::int64_t>(data[4] & 0xFE) >> 1);
}

std::uint64_t DecodePcr(const Packet& packet)
{
    return (static_cast<std::uint64_t>(packet[6]) << 25) |
           (static_cast<std::uint64_t>(packet[7]) << 17) |
           (static_cast<std::uint64_t>(packet[8]) << 9) |
           (static_cast<std::uint64_t>(packet[9]) << 1) |
           (static_cast<std::uint64_t>(packet[10]) >> 7);
}

std::vector<std::uint8_t> MakeAccessUnit(std::size_t size)
{
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 7) & 0xFF);
    }
    return data;
}

std::vector<Packet> PacketsForPid(const MemorySink& sink, int pid)
{
    std::vector<Packet> result;
    for (const Packet& packet : sink.packets) {
        if (Pid(packet) == pid) {
            result.push_back(packet);
        }
    }
    return result;
}

std::vector<std::uint8_t> Reassemble(const std::vector<Packet>& packets)
{
    std::vector<std::uint8_t> bytes;
    for (const Packet& packet : packets) {
        const Bytes payload = Payload(packet);
        bytes.insert(bytes.end(), payload.data, payload.data + payload.size);
    }
    return bytes;
}

void TestPatAndPmt()
{
    std::printf("PAT/PMT\n");
    MemorySink sink;
    TsMuxer muxer(sink);
    const std::vector<std::uint8_t> au = MakeAccessUnit(400);
    muxer.WriteVideo(au.data(), au.size(), 90000, true);

    int patCount = 0;
    int pmtCount = 0;
    for (const Packet& packet : sink.packets) {
        if (Pid(packet) == TsMuxer::kPatPid) {
            ++patCount;
            const Bytes section = Section(packet);
            CHECK(section.size == 16);
            CHECK(section.data[0] == 0x00);
            CHECK(Crc32Mpeg(section.data, section.size - 4) ==
                  ((static_cast<std::uint32_t>(section.data[section.size - 4]) << 24) |
                   (static_cast<std::uint32_t>(section.data[section.size - 3]) << 16) |
                   (static_cast<std::uint32_t>(section.data[section.size - 2]) << 8) |
                   static_cast<std::uint32_t>(section.data[section.size - 1])));
            CHECK((((section.data[10] << 8) | section.data[11]) & 0x1FFF) == TsMuxer::kPmtPid);
        } else if (Pid(packet) == TsMuxer::kPmtPid) {
            ++pmtCount;
            const Bytes section = Section(packet);
            CHECK(section.size == 26);
            CHECK(section.data[0] == 0x02);
            CHECK(Crc32Mpeg(section.data, section.size - 4) ==
                  ((static_cast<std::uint32_t>(section.data[section.size - 4]) << 24) |
                   (static_cast<std::uint32_t>(section.data[section.size - 3]) << 16) |
                   (static_cast<std::uint32_t>(section.data[section.size - 2]) << 8) |
                   static_cast<std::uint32_t>(section.data[section.size - 1])));
            CHECK((((section.data[8] & 0x1F) << 8) | section.data[9]) == TsMuxer::kPcrPid);
            CHECK(section.data[12] == 0x1B);  // H.264
            CHECK(section.data[17] == 0x0F);  // AAC ADTS
        }
    }

    CHECK(patCount == 1);
    CHECK(pmtCount == 1);
}

void TestVideoPes()
{
    std::printf("video PES\n");
    MemorySink sink;
    TsMuxer muxer(sink);
    const std::vector<std::uint8_t> au = MakeAccessUnit(400);
    muxer.WriteVideo(au.data(), au.size(), 90000, true);

    const std::vector<Packet> video = PacketsForPid(sink, TsMuxer::kVideoPid);
    CHECK(video.size() == 3);
    CHECK(Pusi(video[0]));
    CHECK(!Pusi(video[1]));
    CHECK(!Pusi(video[2]));
    CHECK(Cc(video[1]) == ((Cc(video[0]) + 1) & 0x0F));
    CHECK(Cc(video[2]) == ((Cc(video[1]) + 1) & 0x0F));

    const std::vector<std::uint8_t> pes = Reassemble(video);
    CHECK(pes.size() == 414);
    CHECK(pes[0] == 0x00 && pes[1] == 0x00 && pes[2] == 0x01 && pes[3] == 0xE0);
    CHECK(pes[4] == 0x00 && pes[5] == 0x00);  // unbounded video PES
    CHECK(pes[6] == 0x80);
    CHECK(pes[7] == 0x80);
    CHECK(pes[8] == 0x05);
    CHECK(DecodePts(pes.data() + 9) == 90000);
    CHECK(std::memcmp(pes.data() + 14, au.data(), au.size()) == 0);
}

void TestAudioPes()
{
    std::printf("audio PES\n");
    MemorySink sink;
    TsMuxer muxer(sink);
    const std::vector<std::uint8_t> frame(200, 0xAA);
    muxer.WriteAudio(frame.data(), frame.size(), 12345);

    const std::vector<Packet> audio = PacketsForPid(sink, TsMuxer::kAudioPid);
    CHECK(!audio.empty());

    const std::vector<std::uint8_t> pes = Reassemble(audio);
    CHECK(pes.size() == 214);
    CHECK(pes[3] == 0xC0);
    CHECK(((pes[4] << 8) | pes[5]) == 208);
    CHECK(DecodePts(pes.data() + 9) == 12345);
    CHECK(std::memcmp(pes.data() + 14, frame.data(), frame.size()) == 0);
}

void TestOpusTs()
{
    std::printf("Opus-in-TS\n");
    MemorySink sink;
    TsMuxer muxer(sink, TsAudioCodec::Opus, 2);
    const std::vector<std::uint8_t> frame = MakeAccessUnit(300);
    muxer.WriteAudio(frame.data(), frame.size(), 4687);

    bool sawPmt = false;
    for (const Packet& packet : sink.packets) {
        if (Pid(packet) != TsMuxer::kPmtPid) {
            continue;
        }
        sawPmt = true;
        const Bytes section = Section(packet);
        CHECK(section.size == 36);
        CHECK(Crc32Mpeg(section.data, section.size - 4) ==
              ((static_cast<std::uint32_t>(section.data[section.size - 4]) << 24) |
               (static_cast<std::uint32_t>(section.data[section.size - 3]) << 16) |
               (static_cast<std::uint32_t>(section.data[section.size - 2]) << 8) |
               static_cast<std::uint32_t>(section.data[section.size - 1])));
        CHECK(section.data[12] == 0x1B);  // H.264
        CHECK(section.data[17] == 0x06);  // private data
        CHECK((((section.data[20] & 0x0F) << 8) | section.data[21]) == 10);
        CHECK(section.data[22] == 0x05);  // registration descriptor
        CHECK(section.data[23] == 4);
        CHECK(std::memcmp(section.data + 24, "Opus", 4) == 0);
        CHECK(section.data[28] == 0x7F);  // Opus audio descriptor
        CHECK(section.data[29] == 2);
        CHECK(section.data[30] == 0x80);
        CHECK(section.data[31] == 2);     // stereo
    }
    CHECK(sawPmt);

    const std::vector<Packet> audio = PacketsForPid(sink, TsMuxer::kAudioPid);
    CHECK(!audio.empty());
    const std::vector<std::uint8_t> pes = Reassemble(audio);
    CHECK(pes.size() == 318);           // 14-byte PES header + 304-byte framed Opus
    CHECK(pes[3] == 0xBD);              // private_stream_1
    CHECK(((pes[4] << 8) | pes[5]) == 312);
    CHECK(DecodePts(pes.data() + 9) == 4687);

    const std::uint8_t* body = pes.data() + 14;
    CHECK(body[0] == 0x7F);             // OpusTS syncword
    CHECK(body[1] == 0xE0);
    CHECK(body[2] == 0xFF);             // 255 + 45 = 300
    CHECK(body[3] == 45);
    CHECK(std::memcmp(body + 4, frame.data(), frame.size()) == 0);
}

void TestPcr()
{
    std::printf("PCR\n");
    MemorySink sink;
    TsMuxer muxer(sink);
    const std::vector<std::uint8_t> au = MakeAccessUnit(50);
    muxer.WriteVideo(au.data(), au.size(), 0, false);
    muxer.WriteVideo(au.data(), au.size(), 9000, false);

    std::vector<std::uint64_t> pcrs;
    for (const Packet& packet : sink.packets) {
        if (Pid(packet) == TsMuxer::kPcrPid) {
            CHECK(Afc(packet) == 2);
            pcrs.push_back(DecodePcr(packet));
        }
    }

    CHECK(pcrs.size() == 2);
    CHECK(pcrs[0] == 0);
    CHECK(pcrs[1] == 9000);
}

void TestPsiRepeat()
{
    std::printf("PSI repeat\n");
    MemorySink sink;
    TsMuxer muxer(sink);
    const std::vector<std::uint8_t> au = MakeAccessUnit(50);
    muxer.WriteVideo(au.data(), au.size(), 0, false);
    muxer.WriteVideo(au.data(), au.size(), 45000, false);

    int patCount = 0;
    for (const Packet& packet : sink.packets) {
        if (Pid(packet) == TsMuxer::kPatPid) {
            ++patCount;
        }
    }
    CHECK(patCount == 2);
}

void TestContinuity()
{
    std::printf("continuity\n");
    MemorySink sink;
    TsMuxer muxer(sink);
    const std::vector<std::uint8_t> au = MakeAccessUnit(400);
    for (int i = 0; i < 5; ++i) {
        muxer.WriteVideo(au.data(), au.size(), static_cast<std::int64_t>(i) * 3000, false);
    }

    const std::vector<Packet> video = PacketsForPid(sink, TsMuxer::kVideoPid);
    CHECK(video.size() > 10);
    for (std::size_t i = 1; i < video.size(); ++i) {
        CHECK(Cc(video[i]) == ((Cc(video[i - 1]) + 1) & 0x0F));
    }
}

void TestUdpBatching()
{
    std::printf("UDP batching\n");
    WSADATA wsaData = {};
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);

    const SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(receiver != INVALID_SOCKET);

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(bind(receiver, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    int addressSize = sizeof(address);
    CHECK(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);

    DWORD timeout = 2000;
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    UdpSender sender;
    std::wstring error;
    CHECK(sender.Open(L"127.0.0.1", ntohs(address.sin_port), error));

    std::vector<std::uint8_t> packets(UdpSender::kPacketSize * 20, 0x47);
    CHECK(sender.SendTsPackets(packets.data(), 20));

    int datagrams = 0;
    std::size_t bytes = 0;
    for (;;) {
        char buffer[2048];
        const int received = recv(receiver, buffer, sizeof(buffer), 0);
        if (received == SOCKET_ERROR) {
            break;
        }
        ++datagrams;
        bytes += static_cast<std::size_t>(received);
        CHECK(received <= 1316);
    }

    CHECK(datagrams == 3);  // 7 + 7 + 6 packets
    CHECK(bytes == UdpSender::kPacketSize * 20);

    sender.Close();
    closesocket(receiver);
    WSACleanup();
}

void TestInvalidTarget()
{
    std::printf("invalid target\n");
    UdpSender sender;
    std::wstring error;
    CHECK(!sender.Open(L"", 1234, error));
    CHECK(!error.empty());
}

}  // namespace

int RunTsMuxerTests()
{
    std::printf("wxr-vid-cap muxer tests\n");
    TestPatAndPmt();
    TestVideoPes();
    TestAudioPes();
    TestOpusTs();
    TestPcr();
    TestPsiRepeat();
    TestContinuity();
    TestUdpBatching();
    TestInvalidTarget();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
