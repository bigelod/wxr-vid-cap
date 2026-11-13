#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstddef>
#include <string>

// Sends finished TS packets over UDP to one receiver. Datagrams are capped at seven packets
// (1316 bytes) so they fit a 1500-byte MTU without IP fragmentation.
class UdpSender {
public:
    static constexpr std::size_t kPacketSize = 188;
    static constexpr std::size_t kPacketsPerDatagram = 7;

    UdpSender() = default;
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool Open(const std::wstring& host, unsigned short port, std::wstring& error);
    void Close();
    bool Active() const { return socket_ != INVALID_SOCKET; }

    bool SendTsPackets(const unsigned char* packets, std::size_t packetCount);

private:
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in target_ = {};
    bool winsockStarted_ = false;
};
