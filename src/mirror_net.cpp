#include "mirror_net.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "hl_common.h"
#include <windows.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <mutex>
#include <sstream>
#include <vector>

int32_t ReadBE32(const uint8_t* p) {
    return (int32_t)((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
}

int64_t ReadBE64(const uint8_t* p) {
    uint64_t v =
        (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) | (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
        (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) << 8) | uint64_t(p[7]);
    return static_cast<int64_t>(v);
}

FrameHeader ParseHeader(const uint8_t* buf) {
    FrameHeader h{};
    h.magic = ReadBE32(buf + 0);
    h.version = ReadBE32(buf + 4);
    h.width = ReadBE32(buf + 8);
    h.height = ReadBE32(buf + 12);
    h.jpegSize = ReadBE32(buf + 16);
    h.frameProducedNs = ReadBE64(buf + 20);
    h.callbackStartNs = ReadBE64(buf + 28);
    h.encodeStartNs = ReadBE64(buf + 36);
    h.encodeEndNs = ReadBE64(buf + 44);
    h.sendStartNs = ReadBE64(buf + 52);
    h.sendStartWallMs = ReadBE64(buf + 60);
    return h;
}

bool RecvAll(SOCKET s, uint8_t* dst, int size) {
    int got = 0;
    while (got < size) {
        int n = recv(s, reinterpret_cast<char*>(dst + got), size - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

int SocketPendingBytes(SOCKET s) {
    u_long available = 0;
    if (ioctlsocket(s, FIONREAD, &available) != 0) return 0;
    return static_cast<int>(available);
}

int GetSocketOptInt(SOCKET s, int level, int optName) {
    int value = 0;
    int len = sizeof(value);
    if (getsockopt(s, level, optName, reinterpret_cast<char*>(&value), &len) != 0) return 0;
    return value;
}

void ConfigureVideoSocketForLowLatency(SOCKET s) {
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    int rcvbuf = 32 * 1024 * 1024;
    int sndbuf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
}

// ---------- 新增的 TCP Server 实现 ----------
SOCKET StartTcpServer(int port, std::wstring& error) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = L"socket() failed";
        return INVALID_SOCKET;
    }

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        error = L"bind() failed";
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, 1) == SOCKET_ERROR) {
        error = L"listen() failed";
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET AcceptTcpClient(SOCKET listenSock, std::wstring& error) {
    sockaddr_in clientAddr{};
    int addrLen = sizeof(clientAddr);
    SOCKET client = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);
    if (client == INVALID_SOCKET) {
        error = L"accept() failed";
        return INVALID_SOCKET;
    }

    // 打印客户端 IP
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
    std::wstring ipW = ToWide(ipStr);
    error = L"客户端已连接: " + ipW;

    ConfigureVideoSocketForLowLatency(client);
    return client;
}