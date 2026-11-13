#include "udp_sender.h"

#include <cstring>

UdpSender::~UdpSender()
{
    Close();
}

bool UdpSender::Open(const std::wstring& host, unsigned short port, std::wstring& error)
{
    if (Active()) {
        error = L"A cast is already running.";
        return false;
    }
    if (host.empty() || port == 0) {
        error = L"No receiver address was configured.";
        return false;
    }

    if (!winsockStarted_) {
        WSADATA wsaData = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            error = L"Winsock failed to initialize.";
            return false;
        }
        winsockStarted_ = true;
    }

    ADDRINFOW hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    wchar_t service[8] = {};
    swprintf_s(service, L"%hu", port);

    ADDRINFOW* result = nullptr;
    if (GetAddrInfoW(host.c_str(), service, &hints, &result) != 0 || result == nullptr) {
        if (result != nullptr) {
            FreeAddrInfoW(result);
        }
        error = L"Could not resolve \"" + host + L"\" as an IPv4 address.";
        return false;
    }

    const SOCKET socket = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket == INVALID_SOCKET) {
        FreeAddrInfoW(result);
        error = L"Could not open a UDP socket.";
        return false;
    }

    std::memcpy(&target_, result->ai_addr, result->ai_addrlen);
    FreeAddrInfoW(result);

    socket_ = socket;
    return true;
}

void UdpSender::Close()
{
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (winsockStarted_) {
        WSACleanup();
        winsockStarted_ = false;
    }
    target_ = {};
}

bool UdpSender::SendTsPackets(const unsigned char* packets, std::size_t packetCount)
{
    if (socket_ == INVALID_SOCKET || packets == nullptr) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < packetCount) {
        const std::size_t batch = (packetCount - offset < kPacketsPerDatagram) ? (packetCount - offset)
                                                                              : kPacketsPerDatagram;
        const int bytes = static_cast<int>(batch * kPacketSize);
        const char* data = reinterpret_cast<const char*>(packets + offset * kPacketSize);

        const int sent = sendto(socket_, data, bytes, 0, reinterpret_cast<const sockaddr*>(&target_),
                                static_cast<int>(sizeof(target_)));
        if (sent == SOCKET_ERROR) {
            return false;
        }
        offset += batch;
    }
    return true;
}
